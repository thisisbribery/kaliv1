// KALI SMASH DSP: the whole phase-1 chain.
//
//   IN -> HPF -> (MOTION, RESO if PRE) -> [4x up] -> DRIVE A -> DRIVE B -> [4x down] -> LPF
//      -> (MOTION, RESO if POST) -> MIX -> SQUASH -> OUT
//                 \__ dry branch (delayed by the oversampler latency, gets its own LPF when FILTERS = ALL)
#pragma once
#include "drive.h"
#include "motion.h"
#include "reso.h"
#include "oversampler.h"
#include "squash.h"
#include "svf.h"

// Big delay memory. The firmware defines KS_BIG_BUFFER as DSY_SDRAM_BSS to put it in SDRAM.
#ifndef KS_BIG_BUFFER
#define KS_BIG_BUFFER
#endif

namespace ks
{
enum HpfFmSource
{
    HPF_FM_SELF,  // L audio moves the L cutoff, R the R: an uneven stereo input pushes them apart
    HPF_FM_CROSS, // L moves R and R moves L: wilder
    HPF_FM_MONO,  // the sum moves both: stays centred
    HPF_FM_NUM
};
static const char* const kHpfFmNames[HPF_FM_NUM] = {"SELF", "CROSS", "MONO"};

enum HpfSpreadSource
{
    HPF_SPR_MONO, // the sum: throws the stereo field around even on a mono mix
    HPF_SPR_SIDE, // the stereo difference: only the wide parts move it
    HPF_SPR_NUM
};
static const char* const kHpfSprNames[HPF_SPR_NUM] = {"MONO", "SIDE"};

KS_BIG_BUFFER static float g_ksMotionMem[2 * Motion::kLen];
KS_BIG_BUFFER static float g_ksResoMem[Reso::kMemLen];

struct ChainParams
{
    // filters (cutoffs are 0..1 knob positions)
    float hpfCut    = 0.f; // 20 Hz (off) .. 16 kHz, silent at the very end
    float hpfRes    = 0.f;
    float hpfDrive  = 0.f; // 0..1: 0 to +24 dB into the MS-20 HPF
    float hpfFm     = 0.f; // 0..1: the input audio into the HPF cutoff, up to +-6 octaves at full scale
    int   hpfFmSrc  = 0;   // HPF_FM_SELF / CROSS / MONO
    float hpfSpread = 0.f; // -1..1: L cutoff down and R up (or the reverse), up to 2 octaves each way
    float hpfSprFm  = 0.f; // 0..1: the input audio into the spread: L and R cutoffs pushed in opposite
                           // directions, up to +-4 octaves each per full-scale swing (Dual Dagger style)
    int   hpfSprSrc = 0;   // HPF_SPR_MONO / HPF_SPR_SIDE
    bool  hpf24     = false;
    bool  filtersAll = true; // dry path gets the filters too
    float lpfCut    = 1.f;
    float lpfRes    = 0.f;
    bool  lpf24     = false;

    DriveParams drive[2];
    bool        oversample = true;

    float mix      = 1.f;   // 0 dry .. 1 wet
    bool  mixEqPow = false; // linear by default: no level bump when the drives are clean

    MotionParams motion;
    bool         motionPre = false; // MOTION before the drives (else after the LPF)
    ResoParams   reso;
    bool         resoPre = false;

    float inTrimDb = 0.f;
    SquashParams squash;

    bool bypass = false; // crossfade to the untouched input (still through SQUASH, so levels compare)
    bool mute   = false;
};

// Knob position to cutoff. Both filters close all the way: the last 3% of travel fades to silence.
inline float HpfHz(float knob) { return 20.f * powf(16000.f / 20.f, clampf(knob, 0.f, 1.f)); }
inline float LpfHz(float knob) { return 20.f * powf(20000.f / 20.f, clampf(knob, 0.f, 1.f)); }
inline float HpfClose(float knob) { return clampf((1.f - knob) / 0.03f, 0.f, 1.f); }
inline float LpfClose(float knob) { return clampf(knob / 0.03f, 0.f, 1.f); }

struct ChainMeters
{
    float bite      = 0.f; // 0..1
    float grDb      = 0.f;
    float lfo       = 0.f; // MOTION's LFO, -1..1
};

class Chain
{
  public:
    static constexpr int kMaxBlock = Oversampler4x::kMaxBlock;
    static constexpr int kLatency  = Oversampler4x::kLatency;

    // Call once after the memory is up (SDRAM on the module)
    void Init()
    {
        motion_.Init(g_ksMotionMem);
        reso_.Init(g_ksResoMem);
    }

    void Process(const ChainParams& p, const float* inL, const float* inR, float* outL, float* outR,
                 int n, ChainMeters& m)
    {
        // per-block configuration
        const int os = p.oversample ? 4 : 1;
        for(int s = 0; s < 2; s++)
            drv_[s].Configure(p.drive[s], os);
        squash_.Configure(p.squash);
        motion_.Configure(p.motion);
        reso_.Configure(p.reso);
        const bool  hpfOff = p.hpfCut < 0.01f;
        const float hpfW   = CutoffToW(hpfOff ? 10.f : HpfHz(p.hpfCut));
        const float fmOct  = 6.f * p.hpfFm * p.hpfFm; // octaves per unit of audio
        const float sprOct = 2.f * clampf(p.hpfSpread, -1.f, 1.f);
        const float sprFm  = 4.f * p.hpfSprFm * p.hpfSprFm; // octaves per unit of audio, each way
        const float hpfR   = hpfOff ? 0.f : Ms20Feedback(p.hpfRes);
        const float hpfDrv = dbToGain(24.f * clampf(p.hpfDrive, 0.f, 1.f));
        // the output soft clip comes in with DRIVE, and with audio FM (swept resonance throws big peaks)
        const float hpfSat = clampf(fmaxf(p.hpfDrive, fmaxf(p.hpfFm, p.hpfSprFm)) * 4.f, 0.f, 1.f);
        const bool  lpfOff = p.lpfCut > 0.99f;
        const float lpfG   = CutoffToG(lpfOff ? 22000.f : LpfHz(p.lpfCut));
        const float lres   = lpfOff ? 0.f : p.lpfRes;
        const float closeTarget = HpfClose(p.hpfCut) * LpfClose(p.lpfCut);
        const float inTrim = dbToGain(p.inTrimDb);
        float       dryG, wetG;
        if(p.mixEqPow)
        {
            dryG = cosf(p.mix * 0.5f * kPi);
            wetG = sinf(p.mix * 0.5f * kPi);
        }
        else
        {
            dryG = 1.f - p.mix;
            wetG = p.mix;
        }

        float hl[kMaxBlock], hr[kMaxBlock], dl[kMaxBlock], dr[kMaxBlock], rl[kMaxBlock], rr[kMaxBlock];
        float ul[4 * kMaxBlock], ur[4 * kMaxBlock];

        // HPF (fully CCW sits at 10 Hz: effectively off, and no click when it comes in), dry branch
        for(int i = 0; i < n; i++)
        {
            float xl = inL[i] * inTrim, xr = inR[i] * inTrim;
            // cutoff offsets in octaves: SPREAD (+ audio into it, L and R opposite), then AUDIO FM
            float spr = sprOct;
            if(sprFm > 0.f)
            {
                float sa = p.hpfSprSrc == HPF_SPR_SIDE ? 0.5f * (xl - xr) : 0.5f * (xl + xr);
                spr += clampf(sprFm * sa, -4.f, 4.f);
            }
            float el = -spr, er = spr;
            if(fmOct > 0.f)
            {
                float a = xl, b = xr;
                if(p.hpfFmSrc == HPF_FM_CROSS)
                {
                    a = xr;
                    b = xl;
                }
                else if(p.hpfFmSrc == HPF_FM_MONO)
                    a = b = 0.5f * (xl + xr);
                el += clampf(fmOct * a, -5.f, 5.f); // at most +-5 octaves
                er += clampf(fmOct * b, -5.f, 5.f);
            }
            float ml = 1.f, mr = 1.f;
            if(el != 0.f || er != 0.f)
            {
                ml = fastExp2(clampf(el, -7.f, 7.f));
                mr = fastExp2(clampf(er, -7.f, 7.f));
            }
            hl[i]    = hpf_[0].Process(xl, hpfW, hpfR, hpfDrv, hpfSat, p.hpf24, ml);
            hr[i]    = hpf_[1].Process(xr, hpfW, hpfR, hpfDrv, hpfSat, p.hpf24, mr);
            rl[i]    = rawDelay_[0].Process(xl, kLatency); // the untouched input, for BYPASS
            rr[i]    = rawDelay_[1].Process(xr, kLatency);
            float a  = p.filtersAll ? hl[i] : xl, b = p.filtersAll ? hr[i] : xr;
            dl[i]    = dcDry_[0].Process(dryDelay_[0].Process(a, kLatency)); // same DC blocker as wet: no
            dr[i]    = dcDry_[1].Process(dryDelay_[1].Process(b, kLatency)); // low-end phase shift between them
        }

        // MOTION / RESO set to PRE: on the wet path only, before the drives
        if(p.motionPre)
            motion_.ProcessBlock(hl, hr, n);
        if(p.resoPre)
            reso_.ProcessBlock(hl, hr, n);

        // wet: drives, 4x oversampled (or at 1x with the same delay, so the blend never shifts)
        if(p.oversample)
        {
            os_.Up(hl, hr, ul, ur, n);
            drv_[0].ProcessBlock(ul, ur, 4 * n);
            drv_[1].ProcessBlock(ul, ur, 4 * n);
            os_.Down(ul, ur, hl, hr, n);
        }
        else
        {
            drv_[0].ProcessBlock(hl, hr, n);
            drv_[1].ProcessBlock(hl, hr, n);
            for(int i = 0; i < n; i++)
            {
                hl[i] = wetDelay_[0].Process(hl[i], kLatency);
                hr[i] = wetDelay_[1].Process(hr[i], kLatency);
            }
        }

        // DC blocker and LPF on wet, LPF on dry too when FILTERS = ALL
        for(int i = 0; i < n; i++)
        {
            hl[i] = lpf_[0].Process(dc_[0].Process(hl[i]), lpfG, lres, p.lpf24, false);
            hr[i] = lpf_[1].Process(dc_[1].Process(hr[i]), lpfG, lres, p.lpf24, false);
            if(p.filtersAll)
            {
                dl[i] = lpfDry_[0].Process(dl[i], lpfG, lres, p.lpf24, false);
                dr[i] = lpfDry_[1].Process(dr[i], lpfG, lres, p.lpf24, false);
            }
        }

        // MOTION / RESO set to POST
        if(!p.motionPre)
            motion_.ProcessBlock(hl, hr, n);
        if(!p.resoPre)
            reso_.ProcessBlock(hl, hr, n);

        for(int i = 0; i < n; i++)
        {
            float wl = hl[i], wr = hr[i], al = dl[i], ar = dr[i];

            // filters fully closed = silence (smoothed so it never clicks)
            close_ += (closeTarget - close_) * 0.002f;
            wl *= close_;
            wr *= close_;
            if(p.filtersAll)
            {
                al *= close_;
                ar *= close_;
            }

            // MIX, then SQUASH
            mixDry_ += (dryG - mixDry_) * 0.002f;
            mixWet_ += (wetG - mixWet_) * 0.002f;
            float ol  = al * mixDry_ + wl * mixWet_;
            float orr = ar * mixDry_ + wr * mixWet_;
            byp_ += ((p.bypass ? 1.f : 0.f) - byp_) * 0.004f;
            ol += byp_ * (rl[i] - ol);
            orr += byp_ * (rr[i] - orr);
            squash_.Process(ol, orr);
            mute_ += ((p.mute ? 0.f : 1.f) - mute_) * 0.004f;
            outL[i] = ol * mute_;
            outR[i] = orr * mute_;
        }
        float b = fmaxf(drv_[0].TakeBite(), drv_[1].TakeBite());
        bite_ += (b - bite_) * 0.2f;
        m.bite = bite_;
        m.grDb = squash_.GainReductionDb();
        m.lfo  = motion_.Lfo();
    }

  private:
    Ms20Hpf       hpf_[2];
    ResFilter     lpf_[2], lpfDry_[2];
    Oversampler4x os_;
    DriveStage    drv_[2];
    Delay<64>     dryDelay_[2], wetDelay_[2], rawDelay_[2];
    DcBlock       dc_[2], dcDry_[2];
    Squash        squash_;
    Motion        motion_;
    Reso          reso_;
    float         mixDry_ = 0.f, mixWet_ = 1.f, bite_ = 0.f, close_ = 1.f, byp_ = 0.f, mute_ = 1.f;
};

} // namespace ks
