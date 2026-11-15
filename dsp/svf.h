// KALI SMASH DSP: resonant HPF / LPF, zero-delay-feedback state-variable filter
// (Simper/Cytomic trapezoidal SVF), 12 or 24 dB/oct, cutoff smoothed per sample.
#pragma once
#include "util.h"

namespace ks
{
struct SvfCore
{
    float ic1 = 0.f, ic2 = 0.f;
    void  Reset() { ic1 = ic2 = 0.f; }
    // returns low; hp written out
    inline float Process(float v0, float g, float k, float& hp)
    {
        float a1 = 1.f / (1.f + g * (g + k));
        float a2 = g * a1;
        float a3 = g * a2;
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1      = 2.f * v1 - ic1;
        ic2      = 2.f * v2 - ic2;
        hp       = v0 - k * v1 - v2;
        return v2;
    }
};

// One channel of a 12/24 dB filter. The 24 dB version is two Butterworth sections, resonance on the second.
struct ResFilter
{
    SvfCore s1, s2;
    float   g = 0.1f;
    inline float Process(float x, float gTarget, float res, bool slope24, bool highpass)
    {
        g += (gTarget - g) * 0.004f; // ~5 ms cutoff glide, kills zipper on knob/CV moves
        float hp, out;
        if(slope24)
        {
            float k2 = lerpf(0.765f, 0.04f, res);
            float l  = s1.Process(x, g, 1.848f, hp);
            float y  = highpass ? hp : l;
            l        = s2.Process(y, g, k2, hp);
            out      = highpass ? hp : l;
        }
        else
        {
            float k = lerpf(1.414f, 0.04f, res);
            float l = s1.Process(x, g, k, hp);
            out     = highpass ? hp : l;
        }
        return out;
    }
};

// MS-20 style high-pass (12 dB/oct, or 24 with a clean Butterworth section in front).
//
// A 2-pole zero-delay SVF whose resonance is positive feedback through an asymmetric diode-style clipper,
// like the MS-20's: damping k = 2 - R * (clipper gain). At rest k = 2 (a soft Q 0.5 knee). Small signals
// see the full feedback, so past R = 2 the filter self-oscillates, and the clipper sets how loud the howl
// gets (up to about 0.9 of full scale). Loud input pushes the clipper over, which squashes and dirties the
// resonance: DRIVE (up to +24 dB into the filter) is how you get there, and the output soft-clips too.
// The clipper is solved per sample with its secant gain at the current band-pass level (cheap, stable).
struct Ms20Hpf
{
    SvfCore  pre;
    float    ic1 = 0.f, ic2 = 0.f, bp = 0.f, w = 0.01f;
    uint32_t rng = 0x2545F491u;

    // Clipper in the feedback: tanh with a little offset (even harmonics), unity slope at 0
    static constexpr float kA    = 2.0f;   // 1 / clip level: full RES howls at about 0.9 of full scale
    static constexpr float kAsym = 0.25f;
    static inline float SecantGain(float v)
    {
        const float t0 = 0.24491866f;            // tanh(kAsym)
        const float d0 = 1.f / (1.f - t0 * t0);  // 1 / slope at 0
        float       u  = kA * v;
        if(fabsf(u) < 1e-4f)
            return 1.f;
        return (fastTanh(u + kAsym) - t0) * d0 / u;
    }

    // tan(x) for 0 < x < 1.45 (Pade, within ~3% at the very top, exact enough lower down): cheap enough
    // to run per sample under audio-rate FM
    static inline float TanW(float x)
    {
        x = clampf(x, 0.0006f, 1.44f);
        float x2 = x * x;
        return x * (15.f - x2) / (15.f - 6.f * x2);
    }

    // wTarget: cutoff as an angle (CutoffToW). R: resonance feedback 0..2.6 (self-oscillates past 2).
    // drvIn: input gain. sat: 0..1 output clip blend. fmMul: per-sample cutoff multiplier (audio-rate FM
    // and stereo spread); the glide is on the knob only, so the FM isn't smoothed away.
    inline float Process(float x, float wTarget, float R, float drvIn, float sat, bool slope24, float fmMul = 1.f)
    {
        w += (wTarget - w) * 0.004f; // ~5 ms cutoff glide
        const float g = TanW(w * fmMul);
        float u = x * drvIn;
        if(R > 1.9f) // a whisper of noise so it can start howling from silence
        {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            u += (float)(int32_t)rng * (1e-5f / 2147483648.f);
        }
        if(slope24)
        {
            float hp;
            pre.Process(u, g, 1.414f, hp);
            u = hp;
        }
        // two passes: solve with the clipper gain at the last band-pass level, then again at this
        // sample's estimate (one pass lags too much at high cutoffs and the howl runs away)
        const float v3 = u - ic2;
        float       k  = 2.f - R * SecantGain(bp);
        float       v1 = (ic1 + g * v3) / (1.f + g * (g + k));
        k              = 2.f - R * SecantGain(v1);
        float a1 = 1.f / (1.f + g * (g + k));
        float a2 = g * a1;
        float a3 = g * a2;
        v1       = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1      = 2.f * v1 - ic1;
        ic2      = 2.f * v2 - ic2;
        bp       = v1;
        float hp = u - k * v1 - v2;
        // output stage: soft clip at about +1.6 dBFS, blended in with DRIVE
        float c = 1.2f * fastTanh(hp * (1.f / 1.2f));
        return hp + sat * (c - hp);
    }
};

// Resonance knob (0..1) to MS-20 feedback: self-oscillation starts at about 80%
inline float Ms20Feedback(float res) { return 2.6f * powf(clampf(res, 0.f, 1.f), 1.2f); }

// Cutoff in Hz to the angle the MS-20 HPF glides and modulates
inline float CutoffToW(float hz) { return kPi * clampf(hz, 5.f, 0.46f * kSampleRate) / kSampleRate; }

// Cutoff in Hz to the prewarped SVF gain
inline float CutoffToG(float hz)
{
    hz = clampf(hz, 5.f, 0.46f * kSampleRate);
    return tanf(kPi * hz / kSampleRate);
}

} // namespace ks
