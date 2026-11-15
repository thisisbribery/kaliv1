// KALI SMASH: stereo mix-buss destroyer for the dsp.coffee Kali.
//
//   IN -> HPF -> DRIVE A -> DRIVE B (4x oversampled) -> LPF -> MOTION -> RESO -> MIX -> SQUASH -> OUT
//   (MOTION and RESO can each sit before the drives instead: POSITION PRE)
//
// Main knobs L->R: HPF, DRV A, SHP A, DRV B, SHP B, LPF (CUTOFF), MOTION (FEEDBACK), MIX
// Small knobs: RESO (left), SQUASH (right)
// Left encoder: turn = page, click = jump to the page's TYPE, hold = HOME
// Right encoder: turn = value, click = next setting, hold = reset setting
// Turning a knob switches to its page. Runs from SRAM under the Daisy bootloader (BOOT_SRAM).
#include "kali_hw.h"
#define KS_BIG_BUFFER DSY_SDRAM_BSS
#include "../dsp/chain.h"
#include "../dsp/outs.h"
#include "util/CpuLoadMeter.h"
#include <cstdio>
#include <cstring>

using namespace daisy;
using patch_sm::DaisyPatchSM;
using namespace kali;

DaisyPatchSM hw;
Oled         oled;
Encoder      encL, encR; // left = D6/D7 click D5, right = D2/D3 click D4
Dac7554      dac;
TimerHandle  pwmTim;
CpuLoadMeter cpu;
ks::Chain    chain;
ks::OutSources outs;

// ---------------------------------------------------------------- settings (encoder pages)
struct Settings
{
    int hpfRes = 0, hpfDrive = 0, hpfSlope = 0, filters = 0, hpfBase = 0;
    int drvType[2]  = {ks::DRIVE_TANH, ks::DRIVE_FOLD};
    int tone[2]     = {0, 0}, bias[2] = {0, 0}, stereo[2] = {0, 0};
    int stageMix[2] = {100, 100}, trim[2] = {0, 0}, amtBase[2] = {0, 0}, shpBase[2] = {0, 0};
    int lpfRes = 0, lpfSlope = 0, lpfBase = 0;
    int mixLaw = 0, mixBase = 0, inTrim = 0;
    int motType = ks::MOTION_FLANGER, motRate = 30, motFb = 0, motSpread = 90, motChaos = 50, motPos = 0, motBase = 0;
    int resType = ks::RESO_COMB, resNote = 48, resColor = 50, resDamp = 30, resDetune = 10, resPos = 0, resFreeze = 0;
    int comp = ks::COMP_GLUE, clip = ks::CLIP_TANH, ceiling = -1, out = 0;
    int meterSpeed = 0, meterSrc = 0, oversample = 1, knobPop = 1;
    int outSrc[6] = {ks::SRC_ENV_LOW, ks::SRC_ENV_FULL, ks::SRC_ENV_HIGH, ks::SRC_BITE, ks::SRC_MOTION_LFO, ks::SRC_CHAOS_X};
    int outPol[6] = {ks::POL_POS, ks::POL_POS, ks::POL_POS, ks::POL_POS, ks::POL_BIPOLAR, ks::POL_BIPOLAR};
    int outOfs[6] = {0, 0, 0, 0, 0, 0}; // tenths of a volt, -50..+50
    int clkSrc[2] = {ks::GATE_METER, ks::GATE_METER};
    int envRel = 2, envRange = 1, trigSens = 1, lfoRate = 40;
    int frzDo = 0, frzMode = 0, syncDo = 1, syncMode = 1; // FREEZE = SMASH while held, SYNC = BYPASS latched
    int presetSlot = 0;
    // added in phase 4 test3 (new settings go at the end, so older saves load with these at default)
    int hpfFm = 0, hpfFmSrc = ks::HPF_FM_SELF, hpfSpread = 0;
};
Settings S;

// One-shot actions (turn to GO); not saved
struct Actions
{
    int update = 0, load = 0, save = 0, init = 0;
} A;

enum ButtonAction
{
    ACT_SMASH,
    ACT_BYPASS,
    ACT_KILL_LOWS,
    ACT_MUTE,
    ACT_NUM
};

static const char* const kSlopeNames[]  = {"12 dB", "24 dB"};
static const char* const kFilterNames[] = {"ALL", "WET"};
static const char* const kLawNames[]    = {"LINEAR", "EQ POWER"};
static const char* const kOnOff[]       = {"OFF", "ON"};
static const char* const kMeterNames[]  = {"FAST", "MED", "SLOW"};
static const char* const kActNames[]    = {"SMASH", "BYPASS", "KILL LOWS", "MUTE"};
static const char* const kModeNames[]   = {"MOMENT", "LATCH"};
static const char* const kSrcNames[]    = {"INPUT", "OUTPUT"};
static const char* const kOsNames[]     = {"OFF", "4x"};
static const char* const kPosNames[]    = {"POST", "PRE"};
static const char* const kUpdNames[]    = {"-", ">", ">>", "GO"};

struct ParamDef
{
    const char*        name;
    int*               v;
    int                min, max, def;
    const char* const* names; // enum labels, or null for numbers
    const char*        unit;
    void (*fmt)(char*, int, int) = nullptr; // custom display
};

static void FmtRate(char* s, int n, int v)
{
    int c = (int)lrintf(ks::MotionRateHz(v * 0.01f) * 100.f);
    if(c < 1000)
        snprintf(s, n, "%d.%02dHz", c / 100, c % 100);
    else
        snprintf(s, n, "%d.%dHz", c / 100, (c / 10) % 10);
}
static void FmtNote(char* s, int n, int v)
{
    static const char* const kN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(s, n, "%s%d %dHz", kN[v % 12], v / 12 - 1, (int)lrintf(ks::NoteHz((float)v)));
}
static void FmtColor(char* s, int n, int v);

#define P(n, var, lo, hi, d, names, unit) {n, &S.var, lo, hi, d, names, unit, nullptr}
#define PF(n, var, lo, hi, d, fmt) {n, &S.var, lo, hi, d, nullptr, "", fmt}
#define PA(n, var) {n, &A.var, 0, 3, 0, kUpdNames, "", nullptr}
ParamDef pHpf[] = {P("RES", hpfRes, 0, 100, 0, nullptr, "%"), P("DRIVE", hpfDrive, 0, 100, 0, nullptr, "%"),
                   P("AUDIO FM", hpfFm, 0, 100, 0, nullptr, "%"), P("FM SRC", hpfFmSrc, 0, ks::HPF_FM_NUM - 1, 0, ks::kHpfFmNames, ""),
                   P("SPREAD", hpfSpread, -100, 100, 0, nullptr, ""),
                   P("SLOPE", hpfSlope, 0, 1, 0, kSlopeNames, ""),
                   P("FILTERS", filters, 0, 1, 0, kFilterNames, ""), P("CV BASE", hpfBase, 0, 100, 0, nullptr, "%")};
ParamDef pDrv[2][8] = {
    {P("TYPE", drvType[0], 0, ks::DRIVE_NUM_TYPES - 1, ks::DRIVE_TANH, ks::kDriveTypeNames, ""),
     P("OUTPUT", trim[0], -12, 24, 0, nullptr, " dB"), P("TONE", tone[0], -100, 100, 0, nullptr, ""),
     P("BIAS", bias[0], 0, 100, 0, nullptr, "%"), P("STEREO", stereo[0], 0, 2, 0, ks::kStereoModeNames, ""),
     P("STAGE MIX", stageMix[0], 0, 100, 100, nullptr, "%"), P("DRV BASE", amtBase[0], 0, 100, 0, nullptr, "%"),
     P("SHP BASE", shpBase[0], 0, 100, 0, nullptr, "%")},
    {P("TYPE", drvType[1], 0, ks::DRIVE_NUM_TYPES - 1, ks::DRIVE_FOLD, ks::kDriveTypeNames, ""),
     P("OUTPUT", trim[1], -12, 24, 0, nullptr, " dB"), P("TONE", tone[1], -100, 100, 0, nullptr, ""),
     P("BIAS", bias[1], 0, 100, 0, nullptr, "%"), P("STEREO", stereo[1], 0, 2, 0, ks::kStereoModeNames, ""),
     P("STAGE MIX", stageMix[1], 0, 100, 100, nullptr, "%"), P("DRV BASE", amtBase[1], 0, 100, 0, nullptr, "%"),
     P("SHP BASE", shpBase[1], 0, 100, 0, nullptr, "%")}};
ParamDef pLpf[] = {P("RES", lpfRes, 0, 100, 0, nullptr, "%"), P("SLOPE", lpfSlope, 0, 1, 0, kSlopeNames, ""),
                   P("CV BASE", lpfBase, 0, 100, 0, nullptr, "%")};
ParamDef pMix[] = {P("LAW", mixLaw, 0, 1, 0, kLawNames, ""), P("IN TRIM", inTrim, -12, 12, 0, nullptr, " dB"),
                   P("CV BASE", mixBase, 0, 100, 0, nullptr, "%")};
ParamDef pSquash[] = {P("COMP", comp, 0, ks::COMP_NUM_TYPES - 1, ks::COMP_GLUE, ks::kCompTypeNames, ""),
                      P("CLIP", clip, 0, ks::CLIP_NUM_TYPES - 1, ks::CLIP_TANH, ks::kClipTypeNames, ""),
                      P("CEILING", ceiling, -12, 0, -1, nullptr, " dB"), P("OUT", out, -24, 6, 0, nullptr, " dB")};
static void FmtSlot(char* s, int n, int v);
static void FmtVolts(char* s, int n, int v)
{
    int a = v < 0 ? -v : v;
    snprintf(s, n, "%c%d.%dV", v < 0 ? '-' : (v > 0 ? '+' : ' '), a / 10, a % 10);
}
#define OUT3(i, def, pol)                                                                        \
    P("OUT " #i, outSrc[i - 1], 0, ks::SRC_NUM - 1, def, ks::kOutSourceNames, ""),              \
        P(#i " POLARITY", outPol[i - 1], 0, ks::POL_NUM - 1, pol, ks::kPolNames, ""),            \
        PF(#i " OFFSET", outOfs[i - 1], -50, 50, 0, FmtVolts)
ParamDef pCvOuts[] = {OUT3(1, ks::SRC_ENV_LOW, ks::POL_POS),     OUT3(2, ks::SRC_ENV_FULL, ks::POL_POS),
                      OUT3(3, ks::SRC_ENV_HIGH, ks::POL_POS),    OUT3(4, ks::SRC_BITE, ks::POL_POS),
                      OUT3(5, ks::SRC_MOTION_LFO, ks::POL_BIPOLAR), OUT3(6, ks::SRC_CHAOS_X, ks::POL_BIPOLAR)};
ParamDef pOuts[] = {P("CLOCK L", clkSrc[0], 0, ks::GATE_NUM - 1, ks::GATE_METER, ks::kGateSourceNames, ""),
                    P("CLOCK R", clkSrc[1], 0, ks::GATE_NUM - 1, ks::GATE_METER, ks::kGateSourceNames, ""),
                    P("ENV REL", envRel, 0, 5, 2, ks::kEnvRelNames, ""),
                    P("ENV RANGE", envRange, 0, 2, 1, ks::kRangeNames, ""),
                    P("HIT SENS", trigSens, 0, 2, 1, ks::kSensNames, ""),
                    PF("LFO RATE", lfoRate, 0, 100, 40, FmtRate),
                    P("METER", meterSpeed, 0, 2, 0, kMeterNames, ""),
                    P("METER SRC", meterSrc, 0, 1, 0, kSrcNames, "")};
ParamDef pButtons[] = {P("FREEZE DO", frzDo, 0, ACT_NUM - 1, ACT_SMASH, kActNames, ""),
                       P("FREEZE", frzMode, 0, 1, 0, kModeNames, ""),
                       P("SYNC DO", syncDo, 0, ACT_NUM - 1, ACT_BYPASS, kActNames, ""),
                       P("SYNC", syncMode, 0, 1, 1, kModeNames, "")};
ParamDef pPresets[] = {PF("SLOT", presetSlot, 0, 7, 0, FmtSlot), PA("LOAD", load), PA("SAVE", save)};
ParamDef pSys[]  = {P("OVERSAMPLE", oversample, 0, 1, 1, kOsNames, ""), P("KNOB POP", knobPop, 0, 1, 1, kOnOff, ""),
                    PA("UPDATE", update), PA("INIT ALL", init)};
ParamDef pMotion[] = {P("TYPE", motType, 0, ks::MOTION_NUM_TYPES - 1, ks::MOTION_FLANGER, ks::kMotionTypeNames, ""),
                      PF("RATE", motRate, 0, 100, 30, FmtRate), P("FEEDBACK", motFb, -100, 100, 0, nullptr, ""),
                      P("SPREAD", motSpread, 0, 180, 90, nullptr, "deg"), P("CHAOS", motChaos, 0, 100, 50, nullptr, "%"),
                      P("POSITION", motPos, 0, 1, 0, kPosNames, ""), P("CV BASE", motBase, 0, 100, 0, nullptr, "%")};
ParamDef pReso[] = {P("TYPE", resType, 0, ks::RESO_NUM_TYPES - 1, ks::RESO_COMB, ks::kResoTypeNames, ""),
                    PF("PITCH", resNote, 16, 111, 48, FmtNote), PF("COLOR", resColor, 0, 100, 50, FmtColor),
                    P("DAMP", resDamp, 0, 100, 30, nullptr, "%"), P("DETUNE", resDetune, 0, 100, 10, nullptr, "%"),
                    P("POSITION", resPos, 0, 1, 0, kPosNames, ""), P("FREEZE", resFreeze, 0, 1, 0, kOnOff, "")};

// COLOR means something different on each RESO type
static void FmtColor(char* s, int n, int v)
{
    static const char* const kVowel[5] = {"A", "E", "I", "O", "U"};
    switch(S.resType)
    {
        case ks::RESO_COMB: snprintf(s, n, v < 50 ? "HOLLOW %d" : "FULL %d", v); break;
        case ks::RESO_CHORD: snprintf(s, n, "%s", ks::kChordNames[(int)(v * 0.01f * 5.99f)]); break;
        case ks::RESO_MODAL: snprintf(s, n, v < 33 ? "STRING %d" : (v < 66 ? "BAR %d" : "BELL %d"), v); break;
        case ks::RESO_FORMANT: snprintf(s, n, "VOWEL %s", kVowel[(int)(v * 0.01f * 4.999f)]); break;
        default: snprintf(s, n, "BRIGHT %d", v); break;
    }
}
#undef P

enum Block
{
    B_HOME,
    B_HPF,
    B_DRVA,
    B_DRVB,
    B_LPF,
    B_MOTION,
    B_RESO,
    B_MIX,
    B_SQUASH,
    B_CVOUTS,
    B_OUTS,
    B_BUTTONS,
    B_PRESETS,
    B_SYSTEM,
    B_COUNT
};
struct BlockDef
{
    const char* title;
    ParamDef*   params;
    int         count;
};
const BlockDef kBlocks[B_COUNT] = {{"HOME", nullptr, 0},
                                   {"HPF", pHpf, 8},
                                   {"DRIVE A", pDrv[0], 8},
                                   {"DRIVE B", pDrv[1], 8},
                                   {"LPF", pLpf, 3},
                                   {"MOTION", pMotion, 7},
                                   {"RESO", pReso, 7},
                                   {"MIX", pMix, 3},
                                   {"SQUASH", pSquash, 4},
                                   {"CV OUTS", pCvOuts, 18},
                                   {"OUT SETUP", pOuts, 8},
                                   {"BUTTONS", pButtons, 4},
                                   {"PRESETS", pPresets, 3},
                                   {"SYSTEM", pSys, 4}};

// ---------------------------------------------------------------- shared between audio and UI
volatile int   evTurnL = 0, evTurnR = 0, evClickL = 0, evClickR = 0, evHoldL = 0, evHoldR = 0;
volatile float knob[8], miniL = 0.f, miniR = 0.f; // final 0..1 values after BASE
volatile float bite = 0.f, grDb = 0.f;
volatile int   meterDuty[2] = {0, 0};
volatile int   activeActs   = 0; // bitmask of button actions currently on
float          outV[6]      = {};
float          env[2]       = {0.f, 0.f};

// ---------------------------------------------------------------- clock LED meters (PWM on the gate pins)
// First-order sigma-delta at 32 kHz instead of fixed 1 kHz PWM: the switching energy is spread out and
// pushed up high, so it can't sit on the jacks (or leak into the audio) as a steady 1 kHz whine.
static void PwmTick(void*)
{
    static int acc[2] = {0, 0};
    for(int c = 0; c < 2; c++)
    {
        acc[c] += meterDuty[c];
        bool on = acc[c] >= 32;
        if(on)
            acc[c] -= 32;
        (c ? hw.gate_out_2 : hw.gate_out_1).Write(on);
    }
}

static int EnvToDuty(float e)
{
    float db   = 20.f * log10f(e + 1e-9f);
    float norm = ks::clampf((db + 36.f) / 36.f, 0.f, 1.f);
    return (int)lrintf(norm * norm * 32.f);
}

// ---------------------------------------------------------------- encoder events at 1 kHz
struct EncEvents
{
    bool holdFired = false;
    void Run(Encoder& e, volatile int& turn, volatile int& click, volatile int& hold)
    {
        e.Debounce();
        turn = turn + e.Increment();
        if(e.RisingEdge())
            holdFired = false;
        if(e.Pressed() && !holdFired && e.TimeHeldMs() > 600.f)
        {
            holdFired = true;
            hold      = hold + 1;
        }
        if(e.FallingEdge() && !holdFired)
            click = click + 1;
    }
};
EncEvents eventsL, eventsR;

// ---------------------------------------------------------------- audio
static float Knob(int i, int basePct)
{
    float v = hw.GetAdcValue(pin::kKnob[i]) + basePct * 0.01f;
    return ks::clampf(v, 0.f, 1.f);
}

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    cpu.OnBlockStart();
    hw.ProcessAllControls();
    eventsL.Run(encL, evTurnL, evClickL, evHoldL);
    eventsR.Run(encR, evTurnR, evClickR, evHoldR);

    // knobs (CV adds on top of the encoder BASE; unpatched BASE 0 = plain knob)
    knob[0] = Knob(0, S.hpfBase);
    knob[1] = Knob(1, S.amtBase[0]);
    knob[2] = Knob(2, S.shpBase[0]);
    knob[3] = Knob(3, S.amtBase[1]);
    knob[4] = Knob(4, S.shpBase[1]);
    knob[5] = Knob(5, S.lpfBase); // CUTOFF knob = LPF
    knob[6] = Knob(6, S.motBase); // FEEDBACK knob = MOTION
    knob[7] = Knob(7, S.mixBase);
    miniL   = hw.GetAdcValue(pin::kMiniLeft);
    miniR   = hw.GetAdcValue(pin::kMiniRight);
#ifdef KS_PROFILE // emulator profiling: both drives well up, filters and SQUASH in play
    knob[0] = 0.2f; knob[1] = 0.8f; knob[2] = 0.6f; knob[3] = 0.8f; knob[4] = 0.6f;
    knob[5] = 0.7f; knob[7] = 1.f;  miniR = 0.5f;
    knob[6] = 0.8f; miniL = 0.8f; S.motType = KS_PROF_MOT; S.resType = KS_PROF_RES;
#endif

    ks::ChainParams p;
    p.hpfCut     = knob[0];
    p.hpfRes     = S.hpfRes * 0.01f;
    p.hpfDrive   = S.hpfDrive * 0.01f;
    p.hpfFm      = S.hpfFm * 0.01f;
    p.hpfFmSrc   = S.hpfFmSrc;
    p.hpfSpread  = S.hpfSpread * 0.01f;
    p.hpf24      = S.hpfSlope;
    p.filtersAll = S.filters == 0;
    p.lpfCut     = knob[5];
    p.lpfRes     = S.lpfRes * 0.01f;
    p.lpf24      = S.lpfSlope;
    for(int d = 0; d < 2; d++)
    {
        ks::DriveParams& dp = p.drive[d];
        dp.type             = S.drvType[d];
        dp.amount           = knob[1 + 2 * d];
        dp.shape            = knob[2 + 2 * d];
        dp.tone             = S.tone[d] * 0.01f;
        dp.bias             = S.bias[d] * 0.01f;
        dp.stereo           = S.stereo[d];
        dp.stageMix         = S.stageMix[d] * 0.01f;
        dp.trimDb           = (float)S.trim[d];
    }
    p.motion.type      = S.motType;
    p.motion.amount    = knob[6];
    p.motion.rate      = S.motRate * 0.01f;
    p.motion.feedback  = S.motFb * 0.01f;
    p.motion.spread    = S.motSpread / 180.f;
    p.motion.chaos     = S.motChaos * 0.01f;
    p.motionPre        = S.motPos == 1;
    p.reso.type        = S.resType;
    p.reso.amount      = miniL;
    p.reso.note        = (float)S.resNote;
    p.reso.color       = S.resColor * 0.01f;
    p.reso.damp        = S.resDamp * 0.01f;
    p.reso.detune      = S.resDetune * 0.01f;
    p.reso.freeze      = S.resFreeze == 1;
    p.resoPre          = S.resPos == 1;
    p.oversample       = S.oversample;
    p.mix              = knob[7];
    p.mixEqPow         = S.mixLaw == 1;
    p.inTrimDb         = (float)S.inTrim;
    p.squash.amount    = miniR;
    p.squash.comp      = S.comp;
    p.squash.clip      = S.clip;
    p.squash.ceilingDb = (float)S.ceiling;
    p.squash.outDb     = (float)S.out;

    // FREEZE and SYNC: each button and its gate jack share one input. MOMENT = on while high,
    // LATCH = each rising edge toggles.
    static bool lastG[2] = {false, false}, latch[2] = {false, false};
    const bool  g[2]     = {hw.gate_in_2.State(), hw.gate_in_1.State()}; // FREEZE, SYNC
    const int   mode[2]  = {S.frzMode, S.syncMode}, act[2] = {S.frzDo, S.syncDo};
    int         acts     = 0;
    for(int b = 0; b < 2; b++)
    {
        if(g[b] && !lastG[b])
            latch[b] = !latch[b];
        lastG[b] = g[b];
        if(mode[b] ? latch[b] : g[b])
            acts |= 1 << act[b];
    }
    activeActs = acts;
    if(acts & (1 << ACT_SMASH)) // +18 dB into Drive A, Drive B and SQUASH to full
    {
        p.drive[0].boostDb = 18.f;
        p.drive[0].amount  = fmaxf(p.drive[0].amount, 0.35f);
        p.drive[1].amount  = 1.f;
        p.squash.amount    = 1.f;
    }
    if(acts & (1 << ACT_KILL_LOWS))
        p.hpfCut = fmaxf(p.hpfCut, 0.38f); // ~250 Hz
    p.bypass = acts & (1 << ACT_BYPASS);
    p.mute   = acts & (1 << ACT_MUTE);

    ks::ChainMeters m;
    chain.Process(p, in[0], in[1], out[0], out[1], (int)size, m);
    bite = m.bite;
    grDb = m.grDb;

    // CV outs: LFO 1-4 on the DAC7554, 5-6 on the Patch SM's own DAC. -1..1 = -5..+5 V; only changed
    // values go out over SPI.
    ks::OutParams op;
    op.envRel  = S.envRel;
    op.range   = S.envRange;
    op.sens    = S.trigSens;
    op.lfoRate = S.lfoRate * 0.01f;
    outs.Configure(op);
    outs.ProcessBlock(in[0], in[1], (int)size, m.bite, m.grDb, m.lfo);
    // Kali's output stages invert (DAC code up = jack voltage down), so the volts are flipped here.
    static uint16_t lastCode[4] = {0xffff, 0xffff, 0xffff, 0xffff};
    for(int o = 0; o < 6; o++)
    {
        float v = ks::clampf(outs.Value(S.outSrc[o]), -1.f, 1.f);
        float V = ks::OutVolts(S.outSrc[o], v, S.outPol[o], S.outOfs[o] * 0.1f); // at the jack, -5..+5
        outV[o] = V;
        if(o < 4)
        {
            uint16_t code = (uint16_t)ks::clampf(2048.f - V * 409.4f + 0.5f, 0.f, 4095.f);
            if(code != lastCode[o])
            {
                lastCode[o] = code;
                dac.Write(o, code);
            }
        }
        else
            hw.WriteCvOut(o == 4 ? patch_sm::CV_OUT_1 : patch_sm::CV_OUT_2, 2.5f - 0.5f * V);
    }
    // clock outs: LED level meters (input by default: shows every hit), or gates
    static const float kRel[3] = {0.99861f, 0.99948f, 0.99983f}; // 15 / 40 / 120 ms
    const float        rel     = kRel[S.meterSpeed % 3];
    const float*       src[2]  = {S.meterSrc ? out[0] : in[0], S.meterSrc ? out[1] : in[1]};
    for(size_t i = 0; i < size; i++)
        for(int c = 0; c < 2; c++)
        {
            float a = fabsf(src[c][i]);
            env[c]  = a > env[c] ? env[c] + (a - env[c]) * 0.0671f : env[c] * rel;
        }
    for(int c = 0; c < 2; c++)
        meterDuty[c] = S.clkSrc[c] == ks::GATE_METER ? EnvToDuty(env[c]) : (outs.Gate(S.clkSrc[c]) ? 32 : 0);
    cpu.OnBlockEnd();
}

// ---------------------------------------------------------------- saving (QSPI)
// The current settings and 8 preset slots, in the last 64 KB of the 8 MB QSPI (the app sits at 0x90040000,
// well below). Saved a few seconds after the last change; a layout change (new firmware with different
// settings) is caught by the size check and starts from defaults.
struct Bank
{
    uint32_t magic, size, used;
    Settings cur;
    Settings slot[8];
};
static_assert(sizeof(Bank) < 4096, "the bank has to fit one 4 KB sector");
static constexpr uint32_t kBankAddr = 0x7F0000;
static constexpr uint32_t kMagic    = 0x4B534D31; // "KSM1"
Bank                      bank;

static void ClampAll(); // keeps every setting inside its range (defined after the page tables are used)

static void LoadBank()
{
    const Bank* q = reinterpret_cast<const Bank*>(hw.qspi.GetData(kBankAddr));
    dsy_dma_invalidate_cache_for_buffer((uint8_t*)q, sizeof(Bank));
    if(q->magic == kMagic && q->size == sizeof(Settings))
    {
        memcpy(&bank, q, sizeof(Bank));
        S = bank.cur;
        ClampAll();
    }
    else if(q->magic == kMagic && q->size >= 64 && q->size < sizeof(Settings) && (q->size & 3) == 0)
    {
        // saved by an older build: new settings were appended, so copy what's there over defaults
        const uint8_t* base = reinterpret_cast<const uint8_t*>(q) + 3 * sizeof(uint32_t);
        bank       = Bank();
        bank.magic = kMagic;
        bank.size  = sizeof(Settings);
        bank.used  = q->used;
        for(int i = 0; i < 9; i++)
        {
            Settings& dst = i == 0 ? bank.cur : bank.slot[i - 1];
            dst           = Settings();
            memcpy(&dst, base + i * q->size, q->size);
        }
        S = bank.cur;
        ClampAll();
        bank.cur = S;
        for(int i = 0; i < 8; i++)
        {
            Settings keep = S;
            S             = bank.slot[i];
            ClampAll();
            bank.slot[i] = S;
            S            = keep;
        }
    }
    else
    {
        bank       = Bank();
        bank.magic = kMagic;
        bank.size  = sizeof(Settings);
        bank.used  = 0;
        bank.cur   = S;
        for(int i = 0; i < 8; i++)
            bank.slot[i] = S;
    }
}

static void SaveBank()
{
    bank.cur = S;
    hw.qspi.Erase(kBankAddr, kBankAddr + sizeof(Bank));
    hw.qspi.Write(kBankAddr, sizeof(Bank), (uint8_t*)&bank);
}

static void FmtSlot(char* s, int n, int v) { snprintf(s, n, "%d %s", v + 1, (bank.used >> v) & 1 ? "SAVED" : "EMPTY"); }

static char     msg[22]  = "";
static uint32_t msgUntil = 0;
static void     Message(const char* m, uint32_t now)
{
    snprintf(msg, sizeof(msg), "%s", m);
    msgUntil = now + 1200;
}

// ---------------------------------------------------------------- UI
struct Ui
{
    int      block = B_HOME;
    int      sel[B_COUNT] = {};
    float    popRef[10]    = {};
    uint32_t lastMove[10]  = {};
    uint32_t moveStart[10] = {};
} ui;

// The type shown next to a page's title
static const char* TypeName(int b)
{
    switch(b)
    {
        case B_DRVA:
        case B_DRVB: return ks::kDriveTypeNames[S.drvType[b - B_DRVA]];
        case B_MOTION: return ks::kMotionTypeNames[S.motType];
        case B_RESO: return ks::kResoTypeNames[S.resType];
        case B_SQUASH: return ks::kCompTypeNames[S.comp];
        default: return "";
    }
}

static void ClampAll()
{
    for(int b = 0; b < B_COUNT; b++)
        for(int i = 0; i < kBlocks[b].count; i++)
        {
            ParamDef& p = kBlocks[b].params[i];
            *p.v        = *p.v < p.min ? p.min : (*p.v > p.max ? p.max : *p.v);
        }
}

static void FmtParam(char* s, int n, const ParamDef& d)
{
    if(d.fmt)
        d.fmt(s, n, *d.v);
    else if(d.names)
        snprintf(s, n, "%s", d.names[*d.v]);
    else if(d.min < 0 && d.unit[0] == '\0')
        snprintf(s, n, "%+d", *d.v);
    else
        snprintf(s, n, "%d%s", *d.v, d.unit);
}

static void FmtHz(char* s, int n, float hz)
{
    if(hz < 1000.f)
        snprintf(s, n, "%dHz", (int)hz);
    else
        snprintf(s, n, "%d.%dk", (int)(hz / 1000.f), ((int)(hz / 100.f)) % 10);
}

static int Pct(float v) { return (int)lrintf(v * 100.f); }

static void Line(int row, const char* s)
{
    oled.SetCursor(0, row * 8);
    oled.WriteString(s, Font_6x8, true);
}

static void HandleEvents(int& seenTL, int& seenTR, int& seenCL, int& seenCR, int& seenHL, int& seenHR)
{
    int dL = evTurnL - seenTL, dR = evTurnR - seenTR;
    seenTL += dL;
    seenTR += dR;
    const BlockDef& bd = kBlocks[ui.block];

    if(dL)
        ui.block = (ui.block + dL + B_COUNT * 8) % B_COUNT;
    if(dR && bd.count)
    {
        ParamDef& p = bd.params[ui.sel[ui.block]];
        int       v = *p.v + dR;
        *p.v        = v < p.min ? p.min : (v > p.max ? p.max : v);
    }
    if(evClickL != seenCL)
    {
        seenCL = evClickL;
        ui.sel[ui.block] = 0; // jump to the page's first setting (TYPE where there is one)
    }
    if(evClickR != seenCR)
    {
        seenCR = evClickR;
        if(bd.count)
            ui.sel[ui.block] = (ui.sel[ui.block] + 1) % bd.count;
    }
    if(evHoldL != seenHL)
    {
        seenHL   = evHoldL;
        ui.block = B_HOME;
    }
    if(evHoldR != seenHR)
    {
        seenHR = evHoldR;
        if(bd.count)
        {
            ParamDef& p = kBlocks[ui.block].params[ui.sel[ui.block]];
            *p.v        = p.def;
        }
    }
}

// Turning a knob switches to its page, and the page stays (the encoders then edit that page).
// A knob that has been moving nonstop for over 2 s is being driven by CV, not a hand: it stops switching
// pages until it has been still for a second.
static void KnobPop(uint32_t now)
{
    static const int kKnobBlock[10] = {B_HPF, B_DRVA, B_DRVA, B_DRVB, B_DRVB, B_LPF, B_MOTION, B_MIX, B_RESO, B_SQUASH};
    float            v[10];
    for(int i = 0; i < 8; i++)
        v[i] = knob[i];
    v[8] = miniL;
    v[9] = miniR;
    for(int i = 0; i < 10; i++)
    {
        if(fabsf(v[i] - ui.popRef[i]) <= 0.03f)
            continue;
        ui.popRef[i] = v[i];
        if(now - ui.lastMove[i] > 1000)
            ui.moveStart[i] = now; // a fresh touch
        ui.lastMove[i] = now;
        bool byCv      = now - ui.moveStart[i] > 2000;
        if(S.knobPop && kKnobBlock[i] >= 0 && !byCv && now > 1500)
        {
            ui.block = kKnobBlock[i];
        }
    }
}

static int cpuShown = 0; // refreshed twice a second so the screen isn't redrawn just for a flickering digit

static void HpfText(char* a)
{
    if(knob[0] < 0.01f)
        snprintf(a, 16, "OFF");
    else if(knob[0] > 0.97f)
        snprintf(a, 16, "CLOSED");
    else
        FmtHz(a, 16, ks::HpfHz(knob[0]));
}

static void LpfText(char* a)
{
    if(knob[5] > 0.99f)
        snprintf(a, 16, "OFF");
    else if(knob[5] < 0.03f)
        snprintf(a, 16, "CLOSED");
    else
        FmtHz(a, 16, ks::LpfHz(knob[5]));
}

static void DrawBlock(int b, char* s)
{
    char a[16], c[16];
    switch(b)
    {
        case B_HOME:
        {
            snprintf(s, 22, "A %-5s%3d B %-5s%3d", ks::kDriveTypeNames[S.drvType[0]], Pct(knob[1]),
                     ks::kDriveTypeNames[S.drvType[1]], Pct(knob[3]));
            Line(0, s);
            HpfText(a);
            LpfText(c);
            snprintf(s, 22, "HP %-6s  LP %s", a, c);
            Line(1, s);
            snprintf(s, 22, "M %-5s%3d R %-5s%3d", ks::kMotionShort[S.motType], Pct(knob[6]), ks::kResoShort[S.resType],
                     Pct(miniL));
            Line(2, s);
            int  nb = (int)lrintf(bite * 5.f);
            char bar[6];
            for(int i = 0; i < 5; i++)
                bar[i] = i < nb ? '#' : '.';
            bar[5] = 0;
            snprintf(s, 22, "MIX%3d SQ%3d %s %2d", Pct(knob[7]), Pct(miniR), bar, cpuShown);
            int acts = activeActs;
            if(acts)
            {
                char* q = s;
                q += snprintf(q, 22, ">>");
                for(int k = 0; k < ACT_NUM; k++)
                    if(acts & (1 << k))
                        q += snprintf(q, 22 - (q - s), " %s", kActNames[k]);
            }
            Line(3, s);
            return;
        }
        case B_HPF:
            HpfText(a);
            snprintf(s, 22, "CUT %s", a);
            break;
        case B_DRVA:
        case B_DRVB:
        {
            int d = b - B_DRVA;
            snprintf(s, 22, "DRV %3d%%  SHP %3d%%", Pct(knob[1 + 2 * d]), Pct(knob[2 + 2 * d]));
            break;
        }
        case B_LPF:
            LpfText(a);
            snprintf(s, 22, "CUT %s", a);
            break;
        case B_MOTION: snprintf(s, 22, "AMT %3d%%  %s", Pct(knob[6]), S.motPos ? "PRE" : "POST"); break;
        case B_RESO:
            snprintf(s, 22, "AMT %3d%%  %s%s", Pct(miniL), S.resFreeze ? "FROZEN " : "",
                     S.resPos ? "PRE" : "POST");
            break;
        case B_MIX: snprintf(s, 22, "WET %3d%%", Pct(knob[7])); break;
        case B_SQUASH: snprintf(s, 22, "AMT %3d%%  GR %2ddB", Pct(miniR), (int)lrintf(grDb)); break;
        case B_CVOUTS: // live volts at each jack, rounded
        {
            char* q = s;
            for(int o = 0; o < 6; o++)
                q += snprintf(q, 22 - (q - s), "%+2d ", (int)lrintf(outV[o]));
            break;
        }
        case B_OUTS: snprintf(s, 22, "CLOCKS, ENV, LFO"); break;
        case B_BUTTONS:
            snprintf(s, 22, "FRZ %-3s  SYNC %-3s", activeActs & (1 << S.frzDo) ? "ON" : "off",
                     activeActs & (1 << S.syncDo) ? "ON" : "off");
            break;
        case B_PRESETS:
            snprintf(s, 22, "SLOT %d  %s", S.presetSlot + 1, (bank.used >> S.presetSlot) & 1 ? "SAVED" : "EMPTY");
            break;
        default: snprintf(s, 22, "CPU %d%% max %d%%", cpuShown, (int)(cpu.GetMaxCpuLoad() * 100.f)); break;
    }
    Line(1, s);

    // title row: name + type
    snprintf(s, 22, "%-9s%s", kBlocks[b].title, TypeName(b));
    Line(0, s);

    const BlockDef& bd = kBlocks[b];
    if(!bd.count)
        return;
    for(int r = 0; r < 2 && r < bd.count; r++)
    {
        int             idx = (ui.sel[b] + r) % bd.count;
        const ParamDef& p   = bd.params[idx];
        FmtParam(a, 16, p);
        snprintf(s, 22, "%c%-8s%12s", r == 0 ? '>' : ' ', p.name, a);
        Line(2 + r, s);
    }
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    cpu.Init(hw.AudioSampleRate(), hw.AudioBlockSize());
    chain.Init(); // after hw.Init: the delay memory is in SDRAM
    LoadBank();   // the settings from last time

    Oled::Config oc;
    oled.Init(oc);
    encL.Init(DaisyPatchSM::D6, DaisyPatchSM::D7, DaisyPatchSM::D5);
    encR.Init(DaisyPatchSM::D2, DaisyPatchSM::D3, DaisyPatchSM::D4);
    dac.Init(); // after hw.Init so D9 is SPI MOSI, not analog

    TimerHandle::Config tc;
    tc.periph     = TimerHandle::Config::Peripheral::TIM_5;
    tc.dir        = TimerHandle::Config::CounterDir::UP;
    tc.enable_irq = true;
    pwmTim.Init(tc);
    pwmTim.SetPeriod(pwmTim.GetFreq() / 32000 - 1);
    pwmTim.SetCallback(PwmTick);
    pwmTim.Start();

    // LFO outs rest at 0 V (LEDs dark) until they get sources in a later phase
    for(int ch = 0; ch < 4; ch++)
        dac.Write(ch, 2048);
    hw.WriteCvOut(patch_sm::CV_OUT_1, 2.5f);
    hw.WriteCvOut(patch_sm::CV_OUT_2, 2.5f);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    int      seenTL = 0, seenTR = 0, seenCL = 0, seenCR = 0, seenHL = 0, seenHR = 0;
    uint32_t tScr = 0;
    char     s[32];

    while(1)
    {
        uint32_t now = System::GetNow();
        HandleEvents(seenTL, seenTR, seenCL, seenCR, seenHL, seenHR);
        KnobPop(now);

        static uint32_t tCpu = 0;
        if(now - tCpu >= 500)
        {
            tCpu     = now;
            cpuShown = (int)(cpu.GetAvgCpuLoad() * 100.f);
        }
        // one-shot actions
        if(A.save == 3)
        {
            A.save                     = 0;
            bank.slot[S.presetSlot]    = S;
            bank.used |= 1u << S.presetSlot;
            SaveBank();
            snprintf(s, 22, "SAVED TO SLOT %d", S.presetSlot + 1);
            Message(s, now);
        }
        if(A.load == 3)
        {
            A.load   = 0;
            int slot = S.presetSlot;
            if((bank.used >> slot) & 1)
            {
                S            = bank.slot[slot];
                S.presetSlot = slot;
                ClampAll();
                snprintf(s, 22, "LOADED SLOT %d", slot + 1);
            }
            else
                snprintf(s, 22, "SLOT %d IS EMPTY", slot + 1);
            Message(s, now);
        }
        if(A.init == 3)
        {
            A.init = 0;
            S      = Settings();
            Message("ALL SETTINGS RESET", now);
        }
        // autosave: 3 s after the last change
        static Settings seen     = S;
        static uint32_t tChanged = 0;
        if(memcmp(&seen, &S, sizeof(Settings)) != 0)
        {
            seen     = S;
            tChanged = now;
        }
        else if(now - tChanged > 3000 && memcmp(&bank.cur, &S, sizeof(Settings)) != 0)
            SaveBank();

        if(A.update == 3) // SYSTEM -> UPDATE turned to GO: reboot into the Daisy bootloader, which then waits
        {
            oled.Fill(false);
            Line(0, "UPDATE MODE");
            Line(1, "flash.daisy.audio");
            Line(2, "File Upload, pick");
            Line(3, "Daisy Bootloader");
            oled.Update();
            System::Delay(100);
            System::ResetToBootloader(System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
        }
        if(now - tScr >= 33)
        {
            tScr = now;
            oled.Fill(false);
            DrawBlock(ui.block, s);
            if(now < msgUntil) // a one-line notice over the bottom row
            {
                oled.DrawRect(0, 24, 127, 31, false, true);
                Line(3, msg);
            }
            oled.Update();
        }
    }
}
