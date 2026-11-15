// KALI SMASH DSP: RESO (the left mini knob). Tuned resonators, at 48 kHz.
//
// Knob: fully CCW is off. The first half fades the resonator in; the second half raises the decay /
// feedback, and the top goes past unity: the combs and strings self-oscillate (sing on their own) while
// the input keeps coming through. FREEZE (page setting) is the only freeze. Every feedback loop runs
// through a soft clipper, so it sings instead of exploding. All our own code.
#pragma once
#include "motion.h" // ReadHermite
#include "util.h"

namespace ks
{
enum ResoType
{
    RESO_COMB,    // one tuned feedback comb: COLOR < 50% negative feedback (hollow, octave down), above positive
    RESO_CHORD,   // three combs at chord intervals; COLOR picks the chord
    RESO_MODAL,   // 10 tuned resonators; COLOR morphs string -> stiff string -> bar -> bell
    RESO_FORMANT, // vowel filter bank; COLOR morphs A E I O U, PITCH shifts it
    RESO_KARPLUS, // transients pluck a string; COLOR is the pluck brightness
    RESO_NUM_TYPES
};
static const char* const kResoTypeNames[RESO_NUM_TYPES] = {"COMB", "CHORD", "MODAL", "FORMANT", "KARPLUS"};
static const char* const kResoShort[RESO_NUM_TYPES]     = {"COMB", "CHORD", "MODAL", "FORMA", "KARPL"};
static const char* const kChordNames[6] = {"OCT+5", "MAJ", "MIN", "SUS4", "MIN7", "5+9"};

struct ResoParams
{
    int   type   = RESO_COMB;
    float amount = 0.f;  // the knob, 0..1
    float note   = 48.f; // MIDI note (16..111 = 20 Hz .. 5 kHz)
    float color  = 0.5f; // 0..1, per type
    float damp   = 0.3f; // 0..1: high frequencies die faster
    float detune = 0.1f; // 0..1 = 0..50 cents, L down / R up
    bool  freeze = false;
};

inline float NoteHz(float note) { return 440.f * exp2f((note - 69.f) / 12.f); }

class Reso
{
  public:
    static constexpr int kLen    = 4096; // per comb line: down to 11.7 Hz
    static constexpr int kCombs  = 3;
    static constexpr int kModes  = 10;
    static constexpr int kMemLen = 2 * kCombs * kLen;

    // mem: kMemLen floats (SDRAM on the module)
    void Init(float* mem)
    {
        for(int c = 0; c < 2; c++)
            for(int k = 0; k < kCombs; k++)
                line_[c][k] = mem + (c * kCombs + k) * kLen;
        for(int i = 0; i < kMemLen; i++)
            mem[i] = 0.f;
    }

    void Configure(const ResoParams& p)
    {
        if(p.type != type_)
        {
            type_ = p.type;
            Clear();
            modeKey_[0] = -1.f;
        }
        const float a = clampf(p.amount, 0.f, 1.f);
        wetTarget_    = a < 0.01f ? 0.f : clampf((a - 0.01f) * 2.04f, 0.f, 1.f);
        const bool fz = p.freeze;
        fz_           = fz;
        inTarget_     = fz ? 0.f : 1.f;
        float push    = clampf(a * 2.f - 1.f, 0.f, 1.f); // 0..1 over the second half
        // feedback 0.6 .. 1.02: past unity at the top, so it self-oscillates; frozen = 1 (the clipper holds it)
        fbAmt_       = fz ? 1.f : 0.6f + 0.42f * sqrtf(push);
        float t60    = fz ? 1e4f : 0.15f * powf(800.f, push); // modal decay: 0.15 s .. 120 s
        color_       = clampf(p.color, 0.f, 1.f);
        damp_        = clampf(p.damp, 0.f, 1.f);
        dampA_       = onePoleCoef(20000.f * powf(0.04f, damp_), kSampleRate); // loop low-pass 20k .. 800 Hz
        float cents  = 50.f * clampf(p.detune, 0.f, 1.f);
        float hz     = NoteHz(clampf(p.note, 16.f, 111.f));
        hz_[0]       = hz * exp2f(-cents / 1200.f);
        hz_[1]       = hz * exp2f(cents / 1200.f);

        switch(type_)
        {
            case RESO_COMB:
            case RESO_KARPLUS:
                for(int c = 0; c < 2; c++)
                    delay_[c][0] = clampf(kSampleRate / hz_[c], 2.f, (float)kLen - 4.f);
                sign_ = (type_ == RESO_COMB && color_ < 0.5f) ? -1.f : 1.f;
                // KARPLUS: COLOR = pluck brightness (excitation low-pass 600 Hz .. 16 kHz)
                exA_ = onePoleCoef(600.f * powf(16000.f / 600.f, color_), kSampleRate);
                break;
            case RESO_CHORD:
            {
                static const float kIv[6][3] = {{0, 7, 12}, {0, 4, 7}, {0, 3, 7}, {0, 5, 7}, {0, 3, 10}, {0, 7, 14}};
                int ch = (int)(color_ * 5.99f);
                for(int c = 0; c < 2; c++)
                    for(int k = 0; k < kCombs; k++)
                        delay_[c][k] = clampf(kSampleRate / (hz_[c] * exp2f(kIv[ch][k] / 12.f)), 2.f, (float)kLen - 4.f);
                sign_ = 1.f;
                break;
            }
            case RESO_MODAL:
            {
                // only recompute the 20 resonators when something moved (knob noise stays below these steps)
                float key[5] = {t60, color_, damp_, hz_[0], hz_[1]};
                bool  moved  = false;
                for(int k = 0; k < 5; k++)
                    moved |= fabsf(key[k] - modeKey_[k]) > 0.002f * fabsf(key[k]) + 1e-4f;
                if(moved)
                {
                    for(int k = 0; k < 5; k++)
                        modeKey_[k] = key[k];
                    ConfigureModes(t60);
                }
                break;
            }
            case RESO_FORMANT: ConfigureFormants(a, fz, p.note); break;
            default: break;
        }
    }

    bool Active() const { return wetTarget_ > 0.f || wet_ > 1e-4f; }

    void ProcessBlock(float* l, float* r, int n)
    {
        if(!Active())
        {
            if(wet_ > 0.f)
                Clear();
            wet_ = 0.f;
            return;
        }
        switch(type_)
        {
            case RESO_COMB: Combs(l, r, n, 1); break;
            case RESO_CHORD: Combs(l, r, n, kCombs); break;
            case RESO_MODAL: Modal(l, r, n); break;
            case RESO_FORMANT: Formant(l, r, n); break;
            default: Karplus(l, r, n); break;
        }
    }

  private:
    void Clear()
    {
        for(int c = 0; c < 2; c++)
        {
            for(int k = 0; k < kCombs; k++)
            {
                if(line_[c][k])
                    for(int i = 0; i < kLen; i++)
                        line_[c][k][i] = 0.f;
                lp_[c][k] = 0.f;
            }
            for(int m = 0; m < kModes; m++)
                y1_[c][m] = y2_[c][m] = 0.f;
            for(int f = 0; f < 3; f++)
                f1_[c][f] = f2_[c][f] = 0.f;
            ex_[c] = 0.f;
        }
    }

    inline void Smooth()
    {
        wet_ += (wetTarget_ - wet_) * 0.002f;
        in_ += (inTarget_ - in_) * 0.002f;
    }

    // COMB and CHORD: feedback combs with a damping low-pass and a soft clipper in the loop.
    // Output is scaled down as the feedback rises so the ring doesn't simply get louder.
    void Combs(float* l, float* r, int n, int nc)
    {
        const float fb   = sign_ * fbAmt_;
        const float norm = (0.35f + 0.65f * sqrtf(1.f - fminf(fbAmt_, 0.999f))) * (nc > 1 ? 0.6f : 1.f);
        for(int i = 0; i < n; i++)
        {
            Smooth();
            float x[2] = {l[i], r[i]};
            for(int c = 0; c < 2; c++)
            {
                float sum = 0.f;
                for(int k = 0; k < nc; k++)
                {
                    float d = ReadHermite<kLen>(line_[c][k], w_, delay_[c][k]);
                    lp_[c][k] += dampA_ * (d - lp_[c][k]);
                    line_[c][k][w_] = x[c] * in_ + fb * 1.2f * fastTanh(lp_[c][k] * (1.f / 1.2f));
                    sum += d;
                }
                float e = (x[c] * in_ + sum) * norm;
                float o = x[c] + wet_ * (e - x[c]);
                c ? r[i] = o : l[i] = o;
            }
            w_ = (w_ + 1) & (kLen - 1);
        }
    }

    // KARPLUS: an onset detector opens a short gate on the input (about 6 ms); what gets through, low-passed
    // by COLOR, plucks a string (comb with an averaging low-pass in the loop). The dry stays on top.
    void Karplus(float* l, float* r, int n)
    {
        const float fb = fz_ ? 1.f : 0.9f + 0.103f * (fbAmt_ - 0.6f) / 0.42f; // up to 1.003: endless
        for(int i = 0; i < n; i++)
        {
            Smooth();
            float x[2] = {l[i], r[i]};
            float a    = fmaxf(fabsf(x[0]), fabsf(x[1]));
            envFast_ += (a - envFast_) * (a > envFast_ ? 0.3f : 0.002f);
            envSlow_ += (a - envSlow_) * 0.0005f;
            if(gate_ > 0)
                gate_--;
            else if(envFast_ > 2.f * envSlow_ + 0.01f && holdOff_ == 0)
            {
                gate_    = (int)(0.006f * kSampleRate);
                holdOff_ = (int)(0.05f * kSampleRate);
            }
            if(holdOff_ > 0)
                holdOff_--;
            float g = gate_ > 0 ? in_ : 0.f;
            for(int c = 0; c < 2; c++)
            {
                ex_[c] += exA_ * (x[c] * g - ex_[c]);
                float d  = ReadHermite<kLen>(line_[c][0], w_, delay_[c][0]);
                float av = 0.5f * (d + lp_[c][0]); // the classic two-point average, then DAMP
                lp_[c][0] = d;
                lp_[c][1] += dampA_ * (av - lp_[c][1]);
                line_[c][0][w_] = ex_[c] * 2.f + fb * 1.2f * fastTanh(lp_[c][1] * (1.f / 1.2f));
                float e         = x[c] + 0.6f * d;
                float o         = x[c] + wet_ * (e - x[c]);
                c ? r[i] = o : l[i] = o;
            }
            w_ = (w_ + 1) & (kLen - 1);
        }
    }

    // MODAL: 10 two-pole resonators per side. Odd partials lean left, even right (wide, Rings-like).
    void ConfigureModes(float t60)
    {
        static const float kBar[kModes]  = {1.f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.81f, 31.87f, 39.81f, 48.63f};
        static const float kBell[kModes] = {0.5f, 1.f, 1.183f, 1.506f, 2.f, 2.514f, 2.662f, 3.011f, 4.166f, 5.433f};
        for(int m = 0; m < kModes; m++)
        {
            float k = (float)(m + 1), ratio;
            if(color_ < 0.33f) // string -> stiff string
            {
                float B = 0.02f * (color_ / 0.33f) * (color_ / 0.33f);
                ratio   = k * sqrtf(1.f + B * k * k);
            }
            else if(color_ < 0.66f) // stiff string -> bar
            {
                float u = (color_ - 0.33f) / 0.33f;
                ratio   = lerpf(k * sqrtf(1.f + 0.02f * k * k), kBar[m], u);
            }
            else // bar -> bell
                ratio = lerpf(kBar[m], kBell[m] * 2.f, (color_ - 0.66f) / 0.34f);
            float tm = t60 / (1.f + damp_ * 6.f * (float)m);
            float rr = expf(-6.9078f / (tm * kSampleRate));
            // short decays mean wide bands that catch more of a mix: same loudness across the knob
            float dg = powf(fminf(t60, 30.f) / 10.f, 0.22f);
            for(int c = 0; c < 2; c++)
            {
                float f = hz_[c] * ratio;
                if(f > 0.45f * kSampleRate)
                {
                    mGain_[c][m] = 0.f;
                    mA1_[c][m]   = 0.f;
                    mA2_[c][m]   = 0.f;
                    continue;
                }
                float w      = 2.f * kPi * f / kSampleRate;
                mA1_[c][m]   = 2.f * rr * cosf(w);
                mA2_[c][m]   = -rr * rr;
                float pan    = ((m & 1) == c) ? 1.f : 0.45f; // m even (odd partial) louder on L
                // broadband-normalised: white noise in comes out at about the same power, whatever the decay
                // tilted up with pitch, since a mix has far less energy up top
                mGain_[c][m] = sqrtf(2.f * (1.f - rr * rr)) * sinf(w) * powf(f / 130.f, 0.8f) / sqrtf(k) * pan * 0.24f * dg;
            }
        }
    }

    void Modal(float* l, float* r, int n)
    {
        for(int i = 0; i < n; i++)
        {
            Smooth();
            float x[2] = {l[i], r[i]};
            float mono = 0.5f * (x[0] + x[1]) * in_;
            for(int c = 0; c < 2; c++)
            {
                float sum = 0.f;
                for(int m = 0; m < kModes; m++)
                {
                    float y   = mGain_[c][m] * mono + mA1_[c][m] * y1_[c][m] + mA2_[c][m] * y2_[c][m];
                    y2_[c][m] = y1_[c][m];
                    y1_[c][m] = y;
                    sum += y;
                }
                float e = 1.2f * fastTanh(sum * (1.f / 1.2f));
                float o = x[c] + wet_ * (e - x[c]);
                c ? r[i] = o : l[i] = o;
            }
        }
    }

    // FORMANT: three band-passes per side on the vowel's formants. COLOR walks A E I O U; PITCH shifts the
    // whole set (C3 = as sung, an octave either way); the knob's second half narrows them to whistling.
    void ConfigureFormants(float a, bool fz, float note)
    {
        static const float kF[5][3] = {{800, 1150, 2900}, {400, 1600, 2700}, {270, 2140, 2950}, {450, 800, 2830}, {325, 700, 2700}};
        float v  = color_ * 3.999f;
        int   i0 = (int)v;
        float u  = v - (float)i0;
        float sh = exp2f((note - 48.f) / 24.f);
        float q  = fz ? 80.f : 4.f + 76.f * clampf(a * 2.f - 1.f, 0.f, 1.f);
        fMakeup_ = 1.6f * sqrtf(q); // narrow bands pass less of a full mix: make it up
        for(int f = 0; f < 3; f++)
        {
            float hz = lerpf(kF[i0][f], kF[i0 + 1][f], u) * sh;
            float g  = tanf(kPi * clampf(hz, 50.f, 15000.f) / kSampleRate);
            for(int c = 0; c < 2; c++)
            {
                float gc  = g * (c ? 1.01f : 0.99f);
                fG_[c][f] = gc;
                fK_[f]    = 1.f / q;
            }
        }
    }

    void Formant(float* l, float* r, int n)
    {
        static const float kAmp[3] = {1.f, 0.7f, 0.45f};
        for(int i = 0; i < n; i++)
        {
            Smooth();
            float x[2] = {l[i], r[i]};
            for(int c = 0; c < 2; c++)
            {
                float sum = 0.f, v0 = x[c] * in_;
                for(int f = 0; f < 3; f++)
                {
                    float g = fG_[c][f], k = fK_[f];
                    float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
                    float v3 = v0 - f2_[c][f];
                    float v1 = a1 * f1_[c][f] + a2 * v3;
                    float v2 = f2_[c][f] + a2 * f1_[c][f] + a3 * v3;
                    f1_[c][f] = 2.f * v1 - f1_[c][f];
                    f2_[c][f] = 2.f * v2 - f2_[c][f];
                    sum += kAmp[f] * k * v1; // band-pass, unity peak
                }
                float e = 1.2f * fastTanh(sum * fMakeup_ * (1.f / 1.2f));
                float o = x[c] + wet_ * (e - x[c]);
                c ? r[i] = o : l[i] = o;
            }
        }
    }

    int    type_ = -1;
    float* line_[2][kCombs] = {};
    int    w_               = 0;
    float  wet_ = 0.f, wetTarget_ = 0.f, in_ = 1.f, inTarget_ = 1.f;
    bool   fz_    = false;
    float  fbAmt_ = 0.6f, sign_ = 1.f, color_ = 0.5f, damp_ = 0.3f, dampA_ = 1.f, exA_ = 1.f;
    float  hz_[2] = {130.f, 130.f}, delay_[2][kCombs] = {};
    float  lp_[2][kCombs] = {};
    float  mA1_[2][kModes] = {}, mA2_[2][kModes] = {}, mGain_[2][kModes] = {}, y1_[2][kModes] = {}, y2_[2][kModes] = {};
    float  fG_[2][3] = {}, fK_[3] = {}, f1_[2][3] = {}, f2_[2][3] = {}, fMakeup_ = 1.f;
    float  ex_[2] = {0.f, 0.f}, envFast_ = 0.f, envSlow_ = 0.f;
    int    gate_ = 0, holdOff_ = 0;
    float  modeKey_[5] = {-1.f, -1.f, -1.f, -1.f, -1.f};
};

} // namespace ks
