// KALI SMASH phase 0: hardware test firmware for the dsp.coffee Kali (Daisy Patch SM).
// Shows every input on the OLED, tests every output, and passes audio through.
//
// Left encoder = D6/D7, click D5 (enc_y). Right encoder = D2/D3, click D4 (enc_x).
//   Left click: next page.  Right click: change the mode on the current page.
#include "daisy_patch_sm.h"
#include "dev/oled_ssd130x.h"
#include <cmath>
#include <cstdio>

using namespace daisy;
using namespace patch_sm;

DaisyPatchSM hw;

// Kali's screen is a 128x32 SSD1306 on I2C1 (0x3C). libDaisy's init for 128x32 matches the stock
// firmware byte for byte, but its Update() starts each page at column 32 (0x12, meant for 64x32
// panels). Stock Kali starts at column 0, so override Update.
class KaliOledDriver : public SSD130xDriver<128, 32, SSD130xI2CTransport>
{
  public:
    void Update()
    {
        for(uint8_t i = 0; i < 4; i++)
        {
            transport_.SendCommand(0xB0 + i);
            transport_.SendCommand(0x00);
            transport_.SendCommand(0x10);
            transport_.SendData(&buffer_[128 * i], 128);
        }
    }
};
using Oled = OledDisplay<KaliOledDriver>;
Oled      oled;
Encoder   enc_x, enc_y;
SpiHandle dac_spi;

// ---------- DAC7554 (4 CV outs) ----------
static void Dac7554Init()
{
    SpiHandle::Config c;
    c.periph          = SpiHandle::Config::Peripheral::SPI_2;
    c.mode            = SpiHandle::Config::Mode::MASTER;
    c.direction       = SpiHandle::Config::Direction::TWO_LINES_TX_ONLY;
    c.datasize        = 8;
    c.clock_polarity  = SpiHandle::Config::ClockPolarity::HIGH;
    c.clock_phase     = SpiHandle::Config::ClockPhase::ONE_EDGE;
    c.nss             = SpiHandle::Config::NSS::HARD_OUTPUT;
    c.baud_prescaler  = SpiHandle::Config::BaudPrescaler::PS_8;
    c.pin_config.sclk = DaisyPatchSM::D10;
    c.pin_config.mosi = DaisyPatchSM::D9;
    c.pin_config.miso = Pin(PORTX, 0); // D8 stays analog (possible small knob)
    c.pin_config.nss  = DaisyPatchSM::D1;
    dac_spi.Init(c);
}

static void Dac7554Write(int ch, uint16_t code)
{
    if(code > 4095)
        code = 4095;
    uint16_t cmd    = (2u << 14) | ((uint16_t)ch << 12) | code;
    uint8_t  buf[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xff)};
    dac_spi.BlockingTransmit(buf, 2, 5);
}

// ---------- audio ----------
enum AudioMode
{
    AUDIO_THRU,
    AUDIO_TONE,
    AUDIO_MUTE,
    AUDIO_HOT, // passthrough +12 dB, to provoke clipping
    AUDIO_MODES
};
volatile int   audio_mode = AUDIO_THRU;
volatile float peak_in[2], peak_out[2];
volatile uint32_t blocks = 0;
// controls are read here at 1 kHz (48-sample blocks), so the blocking OLED update can't drop encoder steps
volatile int enc_x_count = 0, enc_y_count = 0, click_x = 0, click_y = 0;
float          ph[2]     = {0.f, 0.f};

// ---------- clock-out LED meters ----------
// The clock out LEDs sit on the gate-out signal, so they're dimmed by PWM on the gate pins:
// a 32 kHz timer interrupt gives 1 kHz PWM with 32 brightness steps. The jacks carry the same PWM.
TimerHandle    pwm_tim;
volatile int   meter_duty[2] = {0, 0}; // 0..32
volatile bool  meters_on     = true;
volatile uint32_t clip_count = 0;
float          env[2]        = {0.f, 0.f};
static const float kEnvAtt   = 0.0671f;   // ~0.3 ms attack at 48 kHz
static const float kEnvRel   = 0.99917f;  // ~25 ms release (was 120 ms: hi-hats blurred together)

static void PwmTick(void*)
{
    static uint32_t phase = 0;
    phase = (phase + 1) & 31;
    if(meters_on)
    {
        hw.gate_out_1.Write((int)phase < meter_duty[0]);
        hw.gate_out_2.Write((int)phase < meter_duty[1]);
    }
}

static int EnvToDuty(float e)
{
    float db   = 20.f * log10f(e + 1e-9f);
    float norm = (db + 42.f) / 42.f; // -42 dBFS = off, 0 dBFS = full
    norm       = norm < 0.f ? 0.f : (norm > 1.f ? 1.f : norm);
    return (int)lrintf(norm * norm * 32.f); // squared for perceived brightness
}

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    hw.ProcessAllControls();
    enc_x.Debounce();
    enc_y.Debounce();
    enc_x_count = enc_x_count + enc_x.Increment();
    enc_y_count = enc_y_count + enc_y.Increment();
    if(enc_x.RisingEdge())
        click_x = click_x + 1;
    if(enc_y.RisingEdge())
        click_y = click_y + 1;

    float pi[2] = {0, 0}, po[2] = {0, 0};
    bool  clipped = false;
    const float inc[2] = {440.f / 48000.f, 660.f / 48000.f};
    for(size_t i = 0; i < size; i++)
    {
        for(int c = 0; c < 2; c++)
        {
            float x = in[c][i];
            pi[c]   = fmaxf(pi[c], fabsf(x));
            float y;
            switch(audio_mode)
            {
                case AUDIO_TONE:
                    ph[c] += inc[c];
                    if(ph[c] >= 1.f)
                        ph[c] -= 1.f;
                    y = 0.25f * sinf(6.2831853f * ph[c]); // -12 dBFS
                    break;
                case AUDIO_MUTE: y = 0.f; break;
                case AUDIO_HOT: y = x * 3.981f; break;
                default: y = x; break;
            }
            if(fabsf(y) >= 0.999f)
            {
                clipped = true;
                y       = y > 0.f ? 0.999f : -0.999f;
            }
            float a = fabsf(y);
            env[c]  = a > env[c] ? env[c] + (a - env[c]) * kEnvAtt : env[c] * kEnvRel;
            out[c][i] = y;
            po[c]     = fmaxf(po[c], fabsf(y));
        }
    }
    for(int c = 0; c < 2; c++)
    {
        peak_in[c]  = fmaxf(pi[c], peak_in[c] * 0.95f);
        peak_out[c] = fmaxf(po[c], peak_out[c] * 0.95f);
    }
    if(clipped)
        clip_count = clip_count + 1;
    meter_duty[0] = EnvToDuty(env[0]);
    meter_duty[1] = EnvToDuty(env[1]);
    blocks = blocks + 1;
}

// ---------- outputs ----------
enum OutMode
{
    OUT_CHASE,
    OUT_TRI_SLOW,
    OUT_TRI_FAST,
    OUT_STEPS,
    OUT_MAX,
    OUT_MID,
    OUT_MIN,
    OUT_MODES
};
const char* out_mode_names[OUT_MODES] = {"CHASE", "TRI 0.25Hz", "TRI 2Hz", "STEPS", "ALL MAX", "ALL MID", "ALL MIN"};
const char* out_names[8] = {"LFO1 DAC7554 ch0", "LFO2 DAC7554 ch1", "LFO3 DAC7554 ch2", "LFO4 DAC7554 ch3",
                            "LFO5 PatchSM CV1", "LFO6 PatchSM CV2", "CLOCK L (B5)", "CLOCK R (B6)"};
int out_mode  = OUT_CHASE;
float out_level = 0.f; // level shown on screen for the CV outs
int chase_idx = 0;

// level 0..1 for each of the 8 outputs
static void WriteOutputs(const float lv[8])
{
    for(int ch = 0; ch < 4; ch++)
        Dac7554Write(ch, (uint16_t)(lv[ch] * 4095.f + 0.5f));
    hw.WriteCvOut(CV_OUT_1, lv[4] * 5.f);
    hw.WriteCvOut(CV_OUT_2, lv[5] * 5.f);
    if(!meters_on)
    {
        hw.gate_out_1.Write(lv[6] > 0.5f);
        hw.gate_out_2.Write(lv[7] > 0.5f);
    }
}

// ---------- display ----------
static void Line(int row, const char* s)
{
    oled.SetCursor(0, row * 8);
    oled.WriteString(s, Font_6x8, true);
}

static int Pct(float v) { return (int)lrintf(v * 100.f); }

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    Oled::Config oc; // defaults: I2C1, PB8/PB9, 0x3C
    oled.Init(oc);

    enc_x.Init(DaisyPatchSM::D2, DaisyPatchSM::D3, DaisyPatchSM::D4);
    enc_y.Init(DaisyPatchSM::D6, DaisyPatchSM::D7, DaisyPatchSM::D5);

    Dac7554Init(); // after hw.Init so D9 is SPI MOSI, not analog

    TimerHandle::Config tc;
    tc.periph     = TimerHandle::Config::Peripheral::TIM_5;
    tc.dir        = TimerHandle::Config::CounterDir::UP;
    tc.enable_irq = true;
    pwm_tim.Init(tc);
    pwm_tim.SetPeriod(pwm_tim.GetFreq() / 32000 - 1);
    pwm_tim.SetCallback(PwmTick);
    pwm_tim.Start();

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    int      page = 0, seen_x = 0, seen_y = 0;
    uint32_t t_out = 0, t_chase = 0, t_led = 0;
    uint32_t seen_clips = 0, flash_start = 0;
    bool     flashing   = false;
    bool     led = false;
    char     s[32];

    while(1)
    {
        uint32_t now = System::GetNow();

        // encoder clicks (counted in the audio callback)
        if(click_y != seen_y) // left encoder: page
        {
            seen_y = click_y;
            page   = (page + 1) % 4;
        }
        if(click_x != seen_x) // right encoder: mode
        {
            seen_x = click_x;
            {
                if(page == 2)
                {
                    out_mode = (out_mode + 1) % OUT_MODES;
                    chase_idx = 0;
                    t_chase   = now;
                }
                if(page == 3)
                    audio_mode = (audio_mode + 1) % AUDIO_MODES;
            }
        }

        // outputs at 500 Hz
        if(now - t_out >= 2)
        {
            t_out = now;
            float lv[8];
            float tri_slow = fabsf(fmodf(now * 0.0005f, 2.f) - 1.f); // 0.25 Hz
            float tri_fast = fabsf(fmodf(now * 0.004f, 2.f) - 1.f);  // 2 Hz
            float step     = (float)((now / 2000) % 5) * 0.25f;       // 0,25,50,75,100% for 2 s each
            float clk      = (now / 500) & 1 ? 1.f : 0.f;
            for(int i = 0; i < 8; i++)
            {
                switch(out_mode)
                {
                    case OUT_CHASE: lv[i] = (i == chase_idx) ? 1.f : 0.f; break;
                    case OUT_TRI_SLOW: lv[i] = (i < 6) ? tri_slow : clk; break;
                    case OUT_TRI_FAST: lv[i] = (i < 6) ? tri_fast : clk; break;
                    case OUT_STEPS: lv[i] = (i < 6) ? step : clk; break;
                    case OUT_MAX: lv[i] = 1.f; break;
                    case OUT_MID: lv[i] = 0.5f; break;
                    default: lv[i] = 0.f; break;
                }
            }
            if(out_mode == OUT_CHASE && now - t_chase >= 1500)
            {
                t_chase   = now;
                chase_idx = (chase_idx + 1) % 8;
            }
            // Off the output-test page: LFO outs rest at mid (0 V, LEDs dark) and flash
            // three times whenever the audio output clips; clock LEDs are level meters.
            meters_on = (page != 2);
            if(page != 2)
            {
                if(clip_count != seen_clips)
                {
                    seen_clips = clip_count;
                    if(!flashing)
                    {
                        flashing    = true;
                        flash_start = now;
                    }
                }
                float cv = 0.5f;
                if(flashing)
                {
                    uint32_t ph_ms = now - flash_start;
                    if(ph_ms >= 360)
                        flashing = false; // re-arms; continuous clipping keeps it flashing
                    else if(((ph_ms / 60) & 1) == 0)
                        cv = 1.f;
                }
                for(int i = 0; i < 6; i++)
                    lv[i] = cv;
            }
            out_level = lv[0];
            WriteOutputs(lv);
        }

        // heartbeat LED
        if(now - t_led >= 500)
        {
            t_led = now;
            led   = !led;
            hw.SetLed(led);
        }

        // screen at ~30 Hz
        static uint32_t t_scr = 0;
        if(now - t_scr >= 33)
        {
            t_scr = now;
            oled.Fill(false);
            if(page == 0)
            {
                Line(0, "1/4 KNOBS   L click>");
                for(int r = 0; r < 3; r++)
                {
                    int  n = 0;
                    char* p = s;
                    for(int k = r * 3; k < r * 3 + 3 && k < 8; k++, n++)
                        p += snprintf(p, sizeof(s) - (p - s), "K%d%+4d ", k + 1,
                                      Pct(hw.GetAdcValue(CV_1 + k)));
                    Line(1 + r, s);
                }
            }
            else if(page == 1)
            {
                Line(0, "2/4 SMALL/GATE/ENC");
                snprintf(s, sizeof(s), "A9 %3d A10%3d A12%3d", Pct(hw.GetAdcValue(ADC_9)),
                         Pct(hw.GetAdcValue(ADC_10)), Pct(hw.GetAdcValue(ADC_12)));
                Line(1, s);
                snprintf(s, sizeof(s), "GATE1 %d   GATE2 %d", hw.gate_in_1.State() ? 1 : 0,
                         hw.gate_in_2.State() ? 1 : 0);
                Line(2, s);
                snprintf(s, sizeof(s), "ENC L%+4d %c R%+4d %c", enc_y_count,
                         enc_y.Pressed() ? '*' : '-', enc_x_count, enc_x.Pressed() ? '*' : '-');
                Line(3, s);
            }
            else if(page == 2)
            {
                snprintf(s, sizeof(s), "3/4 OUT %s", out_mode_names[out_mode]);
                Line(0, s);
                if(out_mode == OUT_CHASE)
                {
                    snprintf(s, sizeof(s), "NOW #%d of 8", chase_idx + 1);
                    Line(1, s);
                    Line(2, out_names[chase_idx]);
                    Line(3, "note jack/LED for #");
                }
                else
                {
                    snprintf(s, sizeof(s), "CV level %3d%%", Pct(out_level));
                    Line(1, s);
                    snprintf(s, sizeof(s), "DAC code %4d", (int)(out_level * 4095.f + 0.5f));
                    Line(2, s);
                    Line(3, "R click: next mode");
                }
            }
            else
            {
                const char* am[AUDIO_MODES] = {"THRU", "TONE", "MUTE", "HOT+12dB"};
                snprintf(s, sizeof(s), "4/4 AUDIO %s", am[audio_mode]);
                Line(0, s);
                snprintf(s, sizeof(s), "IN  L %3d  R %3d", Pct(peak_in[0]), Pct(peak_in[1]));
                Line(1, s);
                snprintf(s, sizeof(s), "OUT L %3d  R %3d", Pct(peak_out[0]), Pct(peak_out[1]));
                Line(2, s);
                snprintf(s, sizeof(s), "clips %lu  LED %2d %2d", (unsigned long)clip_count,
                         meter_duty[0], meter_duty[1]);
                Line(3, s);
            }
            oled.Update();
        }
    }
}
