// KALI SMASH DSP: linear-phase 4x oversampling (two halfband FIR stages, 48k -> 96k -> 192k and back).
// Linear phase means the wet path's delay is a whole number of 48 kHz samples (kLatency), so the dry path
// can be delayed to match exactly and parallel blends never comb-filter.
//
// Stereo and block-based: L and R share each coefficient load, and the symmetric taps are folded so each
// coefficient is used once per pair of history samples.
#pragma once
#include "halfband_coefs.h"

namespace ks
{
// Interpolate by 2. N = 4k+3 taps; odd outputs are the centre tap (a pure delay of k samples),
// even outputs are the even-index taps.
template <int N>
struct HalfbandUp2
{
    static constexpr int kTaps = (N + 1) / 2; // even-index taps (symmetric)
    static constexpr int kHalf = kTaps / 2;
    static constexpr int kK    = (N - 3) / 4;
    float                hl[2 * kTaps] = {}, hr[2 * kTaps] = {};
    int                  pos = 0;

    // n frames in, 2n frames out
    inline void Process(const float* inL, const float* inR, float* __restrict outL, float* __restrict outR,
                        int n, const float* __restrict h)
    {
        for(int i = 0; i < n; i++)
        {
            pos             = pos == 0 ? kTaps - 1 : pos - 1;
            hl[pos]         = inL[i];
            hl[pos + kTaps] = inL[i]; // mirrored so the window is contiguous
            hr[pos]         = inR[i];
            hr[pos + kTaps] = inR[i];
            const float* wl = &hl[pos];
            const float* wr = &hr[pos];
            float        al = 0.f, ar = 0.f;
#pragma GCC unroll 16
            for(int j = 0; j < kHalf; j++)
            {
                const float c = h[j];
                al += c * (wl[j] + wl[kTaps - 1 - j]);
                ar += c * (wr[j] + wr[kTaps - 1 - j]);
            }
            outL[2 * i]     = al;
            outR[2 * i]     = ar;
            outL[2 * i + 1] = wl[kK];
            outR[2 * i + 1] = wr[kK];
        }
    }
};

// Decimate by 2 with the same filter (gain 1: taps are stored doubled, so halve).
template <int N>
struct HalfbandDown2
{
    static constexpr int kTaps = (N + 1) / 2;
    static constexpr int kHalf = kTaps / 2;
    static constexpr int kK    = (N - 3) / 4;
    float                el[2 * kTaps] = {}, er[2 * kTaps] = {}; // even-phase input history
    float                ol[2 * kTaps] = {}, orr[2 * kTaps] = {}; // odd phase (centre tap)
    int                  pos = 0;

    // 2n frames in, n frames out
    inline void Process(const float* inL, const float* inR, float* __restrict outL, float* __restrict outR,
                        int n, const float* __restrict h)
    {
        for(int i = 0; i < n; i++)
        {
            pos              = pos == 0 ? kTaps - 1 : pos - 1;
            el[pos]          = inL[2 * i];
            el[pos + kTaps]  = inL[2 * i];
            er[pos]          = inR[2 * i];
            er[pos + kTaps]  = inR[2 * i];
            ol[pos]          = inL[2 * i + 1];
            ol[pos + kTaps]  = inL[2 * i + 1];
            orr[pos]         = inR[2 * i + 1];
            orr[pos + kTaps] = inR[2 * i + 1];
            const float* wl  = &el[pos];
            const float* wr  = &er[pos];
            float        al = 0.f, ar = 0.f;
#pragma GCC unroll 16
            for(int j = 0; j < kHalf; j++)
            {
                const float c = h[j];
                al += c * (wl[j] + wl[kTaps - 1 - j]);
                ar += c * (wr[j] + wr[kTaps - 1 - j]);
            }
            // centre tap h[c] = 0.5 sits on the odd sample of pair (p - k - 1)
            outL[i] = 0.5f * (al + ol[pos + kK + 1]);
            outR[i] = 0.5f * (ar + orr[pos + kK + 1]);
        }
    }
};

struct Oversampler4x
{
    static constexpr int kMaxBlock = 64;
    HalfbandUp2<kHb1Len>   up1;
    HalfbandUp2<kHb2Len>   up2;
    HalfbandDown2<kHb2Len> down2;
    HalfbandDown2<kHb1Len> down1;
    float                  padL[2] = {}, padR[2] = {}; // 2 samples at 192k = half a 48k sample, makes kLatency whole

    // Total latency up + down, in 48 kHz samples. Checked by the harness's null test.
    static constexpr int kLatency = ((kHb1Len - 1) / 2) + ((kHb2Len - 1) + 2) / 4;

    // n samples in -> 4n out
    inline void Up(const float* inL, const float* inR, float* outL, float* outR, int n)
    {
        float ml[2 * kMaxBlock], mr[2 * kMaxBlock];
        up1.Process(inL, inR, ml, mr, n, kHb1);
        up2.Process(ml, mr, outL, outR, 2 * n, kHb2);
    }
    // 4n samples in (modified) -> n out
    inline void Down(float* inL, float* inR, float* outL, float* outR, int n)
    {
        // two-sample delay at 192 kHz
        const int m = 4 * n;
        float     tl0 = inL[m - 2], tl1 = inL[m - 1], tr0 = inR[m - 2], tr1 = inR[m - 1];
        for(int i = m - 1; i >= 2; i--)
        {
            inL[i] = inL[i - 2];
            inR[i] = inR[i - 2];
        }
        inL[0] = padL[0];
        inL[1] = padL[1];
        inR[0] = padR[0];
        inR[1] = padR[1];
        padL[0] = tl0;
        padL[1] = tl1;
        padR[0] = tr0;
        padR[1] = tr1;
        float ml[2 * kMaxBlock], mr[2 * kMaxBlock];
        down2.Process(inL, inR, ml, mr, 2 * n, kHb2);
        down1.Process(ml, mr, outL, outR, n, kHb1);
    }
};

} // namespace ks
