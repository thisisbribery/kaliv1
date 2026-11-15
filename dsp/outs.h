// KALI SMASH DSP: what the six CV outs and two clock outs carry. Sources follow the music coming in, so
// patching Kali's outs back into its own knob CVs (or other modules) animates the rig from the mix.
//
// Values are -1..1. Unipolar sources (envelopes, transient, BITE, GR) sit at 0..1 = 0..+5 V; bipolar
// ones (S&H, LFOs, chaos) swing the full -5..+5 V.
#pragma once
#include "motion.h" // MotionRateHz
#include "util.h"

namespace ks
{
enum OutSource
{
    SRC_OFF,
    SRC_ENV_FULL,
    SRC_ENV_LOW,  // below ~150 Hz: the kick
    SRC_ENV_MID,  // ~300 Hz .. 2 kHz
    SRC_ENV_HIGH, // above ~4 kHz: hats and air
    SRC_ENV_SIDE, // the stereo difference
    SRC_TRANSIENT, // an AD envelope fired by each hit
    SRC_BITE,      // how hard the drives are clipping
    SRC_GR,        // SQUASH gain reduction
    SRC_SH,        // a random value, new on each hit
    SRC_MOTION_LFO,
    SRC_FREE_LFO,
    SRC_CHAOS_X,
    SRC_CHAOS_Y,
    SRC_NUM
};
static const char* const kOutSourceNames[SRC_NUM] = {"OFF",  "ENV FULL", "ENV LOW", "ENV MID", "ENV HIGH",
                                                      "ENV SIDE", "TRANSNT", "BITE",  "SQ GR",  "S&H",
                                                      "MOT LFO", "FREE LFO", "CHAOS X", "CHAOS Y"};

enum GateSource
{
    GATE_METER, // LED level meter (the jack carries the dimming pulses)
    GATE_TRANSIENT,
    GATE_BITE, // high while the drives clip hard
    GATE_OFF,
    GATE_NUM
};
// Sources that only go 0..1 (the rest swing -1..1)
inline bool IsUnipolar(int src) { return src >= SRC_ENV_FULL && src <= SRC_GR; }

// Output polarity
enum OutPolarity
{
    POL_POS,     // 0 .. +5 V
    POL_NEG,     // 0 .. -5 V
    POL_BIPOLAR, // -5 .. +5 V
    POL_NUM
};
static const char* const kPolNames[POL_NUM] = {"POS", "NEG", "BIPOLAR"};

// Source value -> volts at the jack, -5..+5. OFF puts out just the offset.
inline float OutVolts(int src, float v, int pol, float offsetV)
{
    float out = 0.f;
    if(src != SRC_OFF)
    {
        float u = IsUnipolar(src) ? v : 0.5f * (v + 1.f); // 0..1
        out     = pol == POL_POS ? 5.f * u : (pol == POL_NEG ? -5.f * u : 10.f * u - 5.f);
    }
    return clampf(out + offsetV, -5.f, 5.f);
}

static const char* const kGateSourceNames[GATE_NUM] = {"METER", "TRANSNT", "BITE", "OFF"};

static const char* const kEnvRelNames[6] = {"10ms", "30ms", "60ms", "120ms", "250ms", "500ms"};
static const float       kEnvRelMs[6]    = {10.f, 30.f, 60.f, 120.f, 250.f, 500.f};
static const char* const kRangeNames[3]  = {"24 dB", "36 dB", "48 dB"};
static const char* const kSensNames[3]   = {"LOW", "MED", "HIGH"};

struct OutParams
{
    int   envRel   = 2;   // index into kEnvRelMs
    int   range    = 1;   // 24 / 36 / 48 dB of envelope range
    int   sens     = 1;   // transient sensitivity
    float lfoRate  = 0.4f; // FREE LFO and chaos speed, 0..1 (0.02..20 Hz)
};

class OutSources
{
  public:
    void Configure(const OutParams& p)
    {
        relA_    = 1.f - expf(-1.f / (kEnvRelMs[p.envRel % 6] * 0.001f * kSampleRate));
        range_   = (p.range == 0 ? 24.f : (p.range == 1 ? 36.f : 48.f));
        ratio_   = p.sens == 0 ? 2.6f : (p.sens == 1 ? 1.9f : 1.45f);
        lfoInc_  = MotionRateHz(p.lfoRate) / kSampleRate;
    }

    // Run on the input block. bite 0..1, grDb >= 0, motionLfo -1..1.
    void ProcessBlock(const float* l, const float* r, int n, float bite, float grDb, float motionLfo)
    {
        onset_ = false;
        for(int i = 0; i < n; i++)
        {
            float m = 0.5f * (l[i] + r[i]), sd = 0.5f * (l[i] - r[i]);
            // bands: one-pole splits (cheap; these are control signals)
            lo1_ += 0.0195f * (m - lo1_); // 150 Hz
            lo2_ += 0.0195f * (lo1_ - lo2_);
            midLo_ += 0.0385f * (m - midLo_); // 300 Hz
            midHi_ += 0.2304f * (m - midHi_); // 2 kHz
            hiLp_ += 0.4085f * (m - hiLp_);   // 4 kHz
            float band[5] = {m, lo2_, midHi_ - midLo_, m - hiLp_, sd};
            for(int b = 0; b < 5; b++)
            {
                float a = fabsf(band[b]);
                env_[b] += (a > env_[b] ? 0.06f : relA_) * (a - env_[b]); // ~0.3 ms attack
            }
            // transients on the full band: fast envelope jumps over the slow one
            float a = fabsf(m);
            fast_ += (a > fast_ ? 0.2f : 0.0021f) * (a - fast_); // 10 ms fall
            slow_ += 0.00010f * (a - slow_);                     // ~200 ms
            if(holdOff_ > 0)
                holdOff_--;
            else if(fast_ > ratio_ * slow_ + 0.003f)
            {
                onset_   = true;
                holdOff_ = (int)(0.06f * kSampleRate);
                trans_   = 1.f;
                sh_      = Rand();
            }
            trans_ *= 0.99986f; // ~150 ms decay
            // free LFO and the Lorenz system
            lfoPh_ += lfoInc_;
            lfoPh_ -= (float)(lfoPh_ >= 1.f);
            float dt = 1e-7f + lfoInc_ * 2.f; // LFO RATE 1 Hz: a few lobe swaps a second
            float dx = 10.f * (ly_ - lx_), dy = lx_ * (28.f - lz_) - ly_, dz = lx_ * ly_ - 2.6666667f * lz_;
            lx_ += dx * dt;
            ly_ += dy * dt;
            lz_ += dz * dt;
        }
        bite_ = bite;
        gr_   = grDb;
        mlfo_ = motionLfo;
        if(onset_)
            pulse_ = (int)(0.01f * 1000.f); // 10 ms, counted in 1 ms blocks
        else if(pulse_ > 0)
            pulse_--;
    }

    float Value(int src) const
    {
        switch(src)
        {
            case SRC_ENV_FULL:
            case SRC_ENV_LOW:
            case SRC_ENV_MID:
            case SRC_ENV_HIGH:
            case SRC_ENV_SIDE: return EnvCv(env_[src - SRC_ENV_FULL]);
            case SRC_TRANSIENT: return trans_;
            case SRC_BITE: return bite_;
            case SRC_GR: return clampf(gr_ / 18.f, 0.f, 1.f);
            case SRC_SH: return sh_;
            case SRC_MOTION_LFO: return mlfo_;
            case SRC_FREE_LFO: return fastSin01(lfoPh_);
            case SRC_CHAOS_X: return clampf(lx_ * 0.05f, -1.f, 1.f);
            case SRC_CHAOS_Y: return clampf(ly_ * 0.037f, -1.f, 1.f);
            default: return 0.f;
        }
    }

    bool Gate(int src) const
    {
        switch(src)
        {
            case GATE_TRANSIENT: return pulse_ > 0;
            case GATE_BITE: return bite_ > 0.25f;
            default: return false;
        }
    }

  private:
    // envelope level in dB mapped onto 0..1 over the chosen range (so quiet parts still move it)
    float EnvCv(float e) const { return clampf(1.f + fastGainToDb(e * 1.4142f) / range_, 0.f, 1.f); }

    float Rand()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (float)(int32_t)rng_ * (1.f / 2147483648.f);
    }

    float    lo1_ = 0.f, lo2_ = 0.f, midLo_ = 0.f, midHi_ = 0.f, hiLp_ = 0.f, env_[5] = {};
    float    fast_ = 0.f, slow_ = 0.f, trans_ = 0.f, sh_ = 0.f;
    float    relA_ = 0.001f, range_ = 36.f, ratio_ = 1.9f, lfoInc_ = 0.f, lfoPh_ = 0.f;
    float    lx_ = 0.1f, ly_ = 0.f, lz_ = 20.f;
    float    bite_ = 0.f, gr_ = 0.f, mlfo_ = 0.f;
    bool     onset_   = false;
    int      holdOff_ = 0, pulse_ = 0;
    uint32_t rng_     = 0xC0FFEE11u;
};

} // namespace ks
