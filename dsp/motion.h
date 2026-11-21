// KALI SMASH DSP: MOTION (the FEEDBACK knob). Stereo modulated-delay and phase effects, at 48 kHz.
//
// Knob: fully CCW is off. The first half brings depth and wet level up to full; the second half pushes
// the feedback from its page setting towards the maximum, so flangers scream and choruses smear.
// All our own code. The feedback paths go through a soft clipper, so maximum feedback can't run away.
#pragma once
#include "util.h"

namespace ks
{
enum MotionType
{
    MOTION_FLANGER,  // wide stereo flanger, triangle sweep
    MOTION_CHAOS,    // 3-voice chorus per side, moved by a Lorenz attractor: never repeats
    MOTION_ENSEMBLE, // string-machine ensemble: 3 taps, slow + fast LFOs 120 degrees apart
    MOTION_PHASER,   // 6-stage stereo phaser
    MOTION_WARP,     // tape: wow, flutter, drift and dropouts
    MOTION_SHIFT,    // three-band frequency shifter, each band spun by its own offset
    MOTION_NUM_TYPES
};
static const char* const kMotionTypeNames[MOTION_NUM_TYPES] = {"FLANGER", "CHAOS", "ENSEMBLE", "PHASER", "WARP", "SHIFT"};
static const char* const kMotionShort[MOTION_NUM_TYPES]     = {"FLANG", "CHAOS", "ENSMB", "PHASE", "WARP", "SHIFT"};

struct MotionParams
{
    int   type     = MOTION_FLANGER;
    float amount   = 0.f;  // the knob, 0..1
    float rate     = 0.3f; // 0..1 = 0.02..20 Hz
    float feedback = 0.f;  // -1..1, the page setting
    float spread   = 0.5f; // 0..1 = 0..180 degrees between L and R
    float chaos    = 0.5f; // 0..1 randomness
};

inline float MotionRateHz(float r) { return 0.02f * powf(1000.f, clampf(r, 0.f, 1.f)); }
// SHIFT uses RATE as the glide time instead: 0..1 = 30 ms .. 2.4 s
inline float MotionGlideSec(float r) { return 0.03f * powf(80.f, clampf(r, 0.f, 1.f)); }

// SHIFT offsets in Hz for the low / mid / high bands. CHAOS morphs from set A to set B.
static const float kShiftA[3] = {90.f, -300.f, -1500.f};
static const float kShiftB[3] = {-190.f, 900.f, 2500.f};

// 4-point Hermite read, d samples behind the write position (d >= 1)
template <int kLen>
inline float ReadHermite(const float* buf, int w, float d)
{
    float rp = (float)w - d;
    float fl = fastFloor(rp);
    float t  = rp - fl;
    int   i  = (int)fl;
    float y0 = buf[(i - 1) & (kLen - 1)], y1 = buf[i & (kLen - 1)], y2 = buf[(i + 1) & (kLen - 1)],
          y3 = buf[(i + 2) & (kLen - 1)];
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

class Motion
{
  public:
    static constexpr int kLen = 4096; // 85 ms per channel

    // mem: 2 * kLen floats (SDRAM on the module)
    void Init(float* mem)
    {
        buf_[0] = mem;
        buf_[1] = mem + kLen;
        for(int i = 0; i < 2 * kLen; i++)
            mem[i] = 0.f;
        xoLoC_.Set(250.f);
        xoHiC_.Set(2500.f);
        ResetShift();
    }

    void Configure(const MotionParams& p)
    {
        if(p.type != type_)
        {
            type_ = p.type;
            for(int c = 0; c < 2; c++)
            {
                fbState_[c] = 0.f;
                for(int k = 0; k < 6; k++)
                    ap_[c][k] = 0.f;
            }
            ResetShift();
        }
        const float a = clampf(p.amount, 0.f, 1.f);
        wetTarget_    = a < 0.01f ? 0.f : clampf((a - 0.01f) * 2.04f, 0.f, 1.f);
        float push    = clampf(a * 2.f - 1.f, 0.f, 1.f);
        float base    = clampf(p.feedback, -1.f, 1.f) * 0.9f;
        float top     = base < 0.f ? -0.97f : 0.97f;
        fbTarget_     = base + (top - base) * push;
        depthTarget_  = 0.3f + 0.7f * clampf(a * 2.f, 0.f, 1.f);
        rateHz_       = MotionRateHz(p.rate);
        inc_          = rateHz_ / kSampleRate;
        spread_       = 0.5f * clampf(p.spread, 0.f, 1.f); // in cycles
        chaos_        = clampf(p.chaos, 0.f, 1.f);
        if(type_ == MOTION_SHIFT)
        {
            // the knob is the lever: shift 0..full offsets over the whole travel, wet in over the first quarter,
            // FEEDBACK comes in with the knob but is never pushed past the page setting
            wetTarget_ = a < 0.01f ? 0.f : clampf((a - 0.01f) * 4.f, 0.f, 1.f);
            fbTarget_  = clampf(p.feedback, -1.f, 1.f) * 0.9f * clampf(a * 2.f, 0.f, 1.f);
            for(int b = 0; b < 3; b++)
                shiftTgt_[b] = a * (kShiftA[b] + (kShiftB[b] - kShiftA[b]) * chaos_);
            glideCoef_ = 1.f - expf(-1.f / (MotionGlideSec(p.rate) * kSampleRate));
            float ph   = 2.f * kPi * spread_; // R oscillator phase offset, 0..180 degrees
            rotC_      = cosf(ph);
            rotS_      = sinf(ph);
        }
    }

    bool Active() const { return wetTarget_ > 0.f || wet_ > 1e-4f; }

    void ProcessBlock(float* l, float* r, int n)
    {
        if(!Active())
        {
            wet_ = 0.f;
            return;
        }
        switch(type_)
        {
            case MOTION_FLANGER: Flanger(l, r, n); break;
            case MOTION_CHAOS: Chaos(l, r, n); break;
            case MOTION_ENSEMBLE: Ensemble(l, r, n); break;
            case MOTION_PHASER: Phaser(l, r, n); break;
            case MOTION_SHIFT: Shift(l, r, n); break;
            default: Warp(l, r, n); break;
        }
    }

    // The LFO value (-1..1) the CV outs can follow later
    float Lfo() const { return lfoOut_; }

  private:
    inline void Smooth()
    {
        wet_ += (wetTarget_ - wet_) * 0.002f;
        fb_ += (fbTarget_ - fb_) * 0.002f;
        depth_ += (depthTarget_ - depth_) * 0.002f;
    }
    inline void Advance(float& ph, float inc)
    {
        ph += inc;
        ph -= (float)(ph >= 1.f);
    }
    static inline float Tri(float ph) // -1..1
    {
        float t = ph + 0.25f;
        t -= (float)(t >= 1.f);
        return 4.f * fabsf(t - 0.5f) - 1.f;
    }
    inline float Rand()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (float)(int32_t)rng_ * (1.f / 2147483648.f);
    }

    // FLANGER: 0.15 ms to ~7 ms, triangle sweep, SPREAD offsets R, CHAOS adds a random wander to the sweep
    void Flanger(float* l, float* r, int n)
    {
        const float minD = 0.00015f * kSampleRate, span = 0.007f * kSampleRate;
        for(int i = 0; i < n; i++)
        {
            Smooth();
            Advance(ph_, inc_);
            Advance(ph2_, inc_ * 0.37f);
            float wander = chaos_ * 0.3f * fastSin01(ph2_);
            float x[2]   = {l[i], r[i]};
            for(int c = 0; c < 2; c++)
            {
                float p = ph_ + (c ? spread_ : 0.f);
                p -= (float)(p >= 1.f);
                float m = clampf(0.5f + 0.5f * Tri(p) + wander, 0.f, 1.f);
                if(c == 0)
                    lfoOut_ = 2.f * m - 1.f;
                float d  = minD + span * depth_ * m;
                float dl = ReadHermite<kLen>(buf_[c], w_, d + 1.f);
                buf_[c][w_] = x[c] + fb_ * fastTanh(dl);
                float e     = 0.7f * (x[c] + dl);
                float o     = x[c] + wet_ * (e - x[c]);
                c ? r[i] = o : l[i] = o;
            }
            w_ = (w_ + 1) & (kLen - 1);
        }
    }

    // Lorenz system, stepped once per sample; RATE sets its speed. Normalised outputs in X_, Y_, Z_.
    inline void LorenzStep()
    {
        float dt = 1e-7f + inc_ * 0.5f; // RATE 1 Hz: about one lobe swap a second
        float dx = 10.f * (ly_ - lx_);
        float dy = lx_ * (28.f - lz_) - ly_;
        float dz = lx_ * ly_ - 2.6666667f * lz_;
        lx_ += dx * dt;
        ly_ += dy * dt;
        lz_ += dz * dt;
    }

    // CHAOS: 3 voices per side at 8..20 ms. CHAOS crossfades their movement from three steady sines to the
    // Lorenz attractor's three axes (L and R read the axes in a different order).
    void Chaos(float* l, float* r, int n)
    {
        const float base = 0.012f * kSampleRate, span = 0.0065f * kSampleRate;
        for(int i = 0; i < n; i++)
        {
            Smooth();
            LorenzStep();
            Advance(ph_, inc_);
            float ax[3] = {lx_ * 0.05f, ly_ * 0.037f, (lz_ - 25.f) * 0.04f};
            float sn[3];
            for(int k = 0; k < 3; k++)
            {
                float p = ph_ + k * 0.3333333f;
                p -= (float)(p >= 1.f);
                sn[k] = fastSin01(p);
            }
            lfoOut_    = clampf(ax[0], -1.f, 1.f);
            float x[2] = {l[i], r[i]};
            for(int c = 0; c < 2; c++)
            {
                float sum = 0.f;
                for(int k = 0; k < 3; k++)
                {
                    int   ak = (k + c * (1 + (spread_ > 0.125f))) % 3;
                    float m  = sn[(k + c) % 3] + chaos_ * (clampf(ax[ak], -1.f, 1.f) - sn[(k + c) % 3]);
                    float d  = base + span * depth_ * m;
                    sum += ReadHermite<kLen>(buf_[c], w_, d);
                }
                sum *= 0.3333333f;
                buf_[c][w_] = x[c] + 0.6f * fb_ * fastTanh(sum);
                float e     = 0.7f * x[c] + 0.9f * sum;
                float o     = x[c] + wet_ * (e - x[c]);
                c ? r[i] = o : l[i] = o;
            }
            w_ = (w_ + 1) & (kLen - 1);
        }
    }

    // ENSEMBLE: three taps around 6 ms, each moved by a slow (RATE) and fast (10x RATE) sine, 120 degrees
    // apart. L hears taps 1+2, R taps 2+3 (SPREAD blends towards fully separate taps).
    void Ensemble(float* l, float* r, int n)
    {
        const float base = 0.006f * kSampleRate;
        const float slow = 0.0018f * kSampleRate, fast = 0.00025f * kSampleRate;
        for(int i = 0; i < n; i++)
        {
            Smooth();
            Advance(ph_, inc_);
            Advance(ph2_, inc_ * 10.f);
            float x  = 0.5f * (l[i] + r[i]);
            float tp[3];
            for(int k = 0; k < 3; k++)
            {
                float p1 = ph_ + k * 0.3333333f, p2 = ph2_ + k * 0.3333333f;
                p1 -= (float)(p1 >= 1.f);
                p2 -= (float)(p2 >= 1.f);
                float d = base + depth_ * (slow * fastSin01(p1) + fast * fastSin01(p2)) * (1.f + 0.3f * chaos_ * Rand());
                tp[k]   = ReadHermite<kLen>(buf_[0], w_, d);
            }
            lfoOut_       = fastSin01(ph_);
            buf_[0][w_]   = x + 0.5f * fb_ * fastTanh(tp[0]);
            w_            = (w_ + 1) & (kLen - 1);
            float sep     = 2.f * spread_; // 0..1
            float el      = 0.5f * (tp[0] + tp[1]) + sep * 0.5f * (tp[0] - tp[1]);
            float er      = 0.5f * (tp[1] + tp[2]) + sep * 0.5f * (tp[2] - tp[1]);
            l[i] += wet_ * (0.5f * l[i] + 0.9f * el - l[i]);
            r[i] += wet_ * (0.5f * r[i] + 0.9f * er - r[i]);
        }
    }

    // PHASER: 6 first-order allpasses per side swept 150 Hz .. 3.4 kHz (sine), feedback around the chain
    void Phaser(float* l, float* r, int n)
    {
        for(int i = 0; i < n; i++)
        {
            Smooth();
            Advance(ph_, inc_);
            float x[2] = {l[i], r[i]};
            for(int c = 0; c < 2; c++)
            {
                float p = ph_ + (c ? spread_ : 0.f);
                p -= (float)(p >= 1.f);
                float s = fastSin01(p) * depth_;
                if(c == 0)
                    lfoOut_ = s;
                float f  = 715.f * fastExp2(2.2f * s); // ~150 .. 3.4k at full depth
                float wc = kPi * f / kSampleRate;
                float g  = wc * (1.f + wc * wc * 0.3333333f); // tan, small angles
                float a  = (g - 1.f) / (g + 1.f);
                float v  = x[c] + fb_ * fastTanh(fbState_[c]);
                for(int k = 0; k < 6; k++)
                {
                    float y    = a * v + ap_[c][k];
                    ap_[c][k]  = v - a * y;
                    v          = y;
                }
                fbState_[c] = v;
                float e     = 0.5f * (x[c] + v);
                float o     = x[c] + wet_ * (e - x[c]);
                c ? r[i] = o : l[i] = o;
            }
        }
    }

    // WARP: a worn tape transport. Wow (RATE), flutter (~7x RATE, up to 12 Hz), a random drift, and CHAOS
    // sets how often dropouts dip and dull the sound. Fully wet it's the warped signal alone.
    void Warp(float* l, float* r, int n)
    {
        const float base = 0.004f * kSampleRate;
        const float wow = 0.0022f * kSampleRate, flut = 0.00018f * kSampleRate;
        const float flInc = fminf(inc_ * 7.f, 12.f / kSampleRate);
        for(int i = 0; i < n; i++)
        {
            Smooth();
            Advance(ph_, inc_);
            Advance(ph2_, flInc);
            // drift: a slow random walk, pulled back to the centre
            if(--driftCount_ <= 0)
            {
                driftCount_  = (int)(0.7f * kSampleRate);
                driftTarget_ = Rand();
            }
            drift_ += (driftTarget_ - drift_) * 0.00005f;
            // dropouts
            if(dropLeft_ > 0)
                dropLeft_--;
            else if(Rand() > 1.f - chaos_ * chaos_ * 0.00006f)
                dropLeft_ = (int)((0.03f + 0.12f * fabsf(Rand())) * kSampleRate);
            float dropT = dropLeft_ > 0 ? 0.25f : 1.f;
            drop_ += (dropT - drop_) * 0.004f;
            float m = depth_ * (wow * (0.8f * fastSin01(ph_) + 0.5f * chaos_ * drift_) + flut * fastSin01(ph2_));
            lfoOut_    = fastSin01(ph_);
            float lpA  = 0.08f + 0.9f * drop_ * drop_; // dull during dropouts
            float x[2] = {l[i], r[i]};
            for(int c = 0; c < 2; c++)
            {
                float d     = base + m * (c ? 1.f - spread_ : 1.f);
                buf_[c][w_] = x[c] + 0.5f * fb_ * fastTanh(fbState_[c]);
                float y     = ReadHermite<kLen>(buf_[c], w_, d);
                tone_[c] += lpA * (y - tone_[c]);
                y           = tone_[c] * drop_;
                fbState_[c] = y;
                float o     = x[c] + wet_ * (y - x[c]);
                c ? r[i] = o : l[i] = o;
            }
            w_ = (w_ + 1) & (kLen - 1);
        }
    }


    // ---- SHIFT ----
    // Per side: DC blocker -> Hilbert allpass pair (the analytic signal, re + j im) -> two crossovers applied
    // to re and im alike (250 Hz, 2.5 kHz; low = LP, high = HP of the rest, mid = what's left, so the bands sum
    // back exactly) -> each band multiplied by its own complex oscillator -> real part. A positive offset moves
    // every frequency in the band up by that many Hz, so harmonics stop lining up; a negative one moves them down
    // and past 0 Hz they fold back. FEEDBACK runs the output back in through 30 ms, so each pass shifts again.
    // After the "Self-Care" lever in Freakshow MISHBY (studied, not copied).
    struct Hilbert
    {
        // Olli Niemitalo's 4+4 allpass 90-degree pair, coefficients squared. Path A carries a 1-sample delay.
        float ax1[4] = {}, ax2[4] = {}, ay1[4] = {}, ay2[4] = {};
        float bx1[4] = {}, bx2[4] = {}, by1[4] = {}, by2[4] = {};
        float aDel = 0.f;
        inline void Process(float x, float& re, float& im)
        {
            static const float ca[4] = {0.4794009f, 0.8762185f, 0.9765976f, 0.9974993f};
            static const float cb[4] = {0.1617585f, 0.7330289f, 0.9453497f, 0.9905992f};
            float a = x, b = x;
            for(int k = 0; k < 4; k++)
            {
                float ya = ca[k] * (a + ay2[k]) - ax2[k];
                ax2[k]   = ax1[k];
                ax1[k]   = a;
                ay2[k]   = ay1[k];
                ay1[k]   = ya;
                a        = ya;
                float yb = cb[k] * (b + by2[k]) - bx2[k];
                bx2[k]   = bx1[k];
                bx1[k]   = b;
                by2[k]   = by1[k];
                by1[k]   = yb;
                b        = yb;
            }
            re   = aDel;
            aDel = a;
            im   = b;
        }
    };
    // 2-pole TPT state variable filter (Butterworth), lowpass and highpass outputs
    struct Svf2
    {
        float ic1 = 0.f, ic2 = 0.f;
        inline void Process(float x, float a1, float a2, float a3, float& lp, float& hp)
        {
            float v3 = x - ic2;
            float v1 = a1 * ic1 + a2 * v3;
            float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1      = 2.f * v1 - ic1;
            ic2      = 2.f * v2 - ic2;
            lp       = v2;
            hp       = x - 1.4142136f * v1 - v2;
        }
    };
    struct SvfCoefs
    {
        float a1, a2, a3;
        void Set(float hz)
        {
            float g = tanf(kPi * hz / kSampleRate);
            a1      = 1.f / (1.f + g * (g + 1.4142136f));
            a2      = g * a1;
            a3      = g * a2;
        }
    };

    void ResetShift()
    {
        for(int c = 0; c < 2; c++)
        {
            hil_[c]  = Hilbert();
            dcX_[c]  = 0.f;
            dcY_[c]  = 0.f;
            for(int k = 0; k < 2; k++)
            {
                xoLo_[c][k] = Svf2();
                xoHi_[c][k] = Svf2();
            }
        }
        for(int b = 0; b < 3; b++)
        {
            oscC_[b]     = 1.f;
            oscS_[b]     = 0.f;
            shiftCur_[b] = 0.f;
        }
    }

    void Shift(float* l, float* r, int n)
    {
        const SvfCoefs& lo = xoLoC_;
        const SvfCoefs& hi = xoHiC_;
        const int   fbLen = (int)(0.03f * kSampleRate);
        const float wInc  = 2.f * kPi / kSampleRate;
        for(int i = 0; i < n; i++)
        {
            Smooth();
            // glide each band towards its offset, then step its oscillator (sin/cos of the step by series,
            // renormalised every sample so it never drifts)
            for(int b = 0; b < 3; b++)
            {
                shiftCur_[b] += (shiftTgt_[b] - shiftCur_[b]) * glideCoef_;
                float d  = shiftCur_[b] * wInc, d2 = d * d;
                float cd = 1.f - d2 * (0.5f - d2 * 0.0416667f);
                float sd = d * (1.f - d2 * (0.1666667f - d2 * 0.0083333f));
                float c  = oscC_[b] * cd - oscS_[b] * sd;
                float s  = oscC_[b] * sd + oscS_[b] * cd;
                float g  = 1.5f - 0.5f * (c * c + s * s);
                oscC_[b] = c * g;
                oscS_[b] = s * g;
            }
            lfoOut_    = clampf(shiftCur_[1] * (1.f / 900.f), -1.f, 1.f);
            // shifted copies don't cancel like a comb does, so feedback piles up power: take it back out
            const float fbQ    = sqrtf(1.f - fb_ * fb_);
            const float fbComp = fbQ * sqrtf(fbQ); // (1 - fb^2)^0.75
            // dry and shifted are unrelated, so a straight crossfade dips 3 dB halfway: hold the power instead
            const float blendG = 1.f / sqrtf(1.f - 2.f * wet_ + 2.f * wet_ * wet_);
            float x[2] = {l[i], r[i]};
            const int rd = (w_ - fbLen) & (kLen - 1);
            for(int c = 0; c < 2; c++)
            {
                // DC blocker (~10 Hz): DC would otherwise come out as a steady tone at the low band's offset
                float xd = x[c] - dcX_[c] + 0.9987f * dcY_[c];
                dcX_[c]  = x[c];
                dcY_[c]  = xd;
                float u  = xd + fb_ * fastTanh(buf_[c][rd]);
                float re, im;
                hil_[c].Process(u, re, im);
                float lr, li, rr, ri, hr, hi2, dummy;
                xoLo_[c][0].Process(re, lo.a1, lo.a2, lo.a3, lr, dummy);
                xoLo_[c][1].Process(im, lo.a1, lo.a2, lo.a3, li, dummy);
                rr = re - lr;
                ri = im - li;
                xoHi_[c][0].Process(rr, hi.a1, hi.a2, hi.a3, dummy, hr);
                xoHi_[c][1].Process(ri, hi.a1, hi.a2, hi.a3, dummy, hi2);
                float br[3] = {lr, rr - hr, hr}, bi[3] = {li, ri - hi2, hi2};
                float y     = 0.f;
                for(int b = 0; b < 3; b++)
                {
                    float oc = oscC_[b], os = oscS_[b];
                    if(c) // R: the same oscillator turned by SPREAD
                    {
                        float t = oc * rotC_ - os * rotS_;
                        os      = oc * rotS_ + os * rotC_;
                        oc      = t;
                    }
                    y += br[b] * oc + bi[b] * os;
                }
                buf_[c][w_] = y;
                y *= fbComp;
                float o     = (x[c] + wet_ * (y - x[c])) * blendG;
                c ? r[i] = o : l[i] = o;
            }
            w_ = (w_ + 1) & (kLen - 1);
        }
    }

    int      type_ = -1;
    float*   buf_[2] = {nullptr, nullptr};
    int      w_      = 0;
    float    wet_ = 0.f, wetTarget_ = 0.f, fb_ = 0.f, fbTarget_ = 0.f, depth_ = 0.3f, depthTarget_ = 0.3f;
    float    rateHz_ = 0.1f, inc_ = 0.f, spread_ = 0.25f, chaos_ = 0.5f;
    float    ph_ = 0.f, ph2_ = 0.f, lfoOut_ = 0.f;
    float    lx_ = 0.1f, ly_ = 0.f, lz_ = 20.f;
    float    ap_[2][6] = {}, fbState_[2] = {0.f, 0.f}, tone_[2] = {0.f, 0.f};
    float    drift_ = 0.f, driftTarget_ = 0.f, drop_ = 1.f;
    int      driftCount_ = 0;
    int      dropLeft_ = 0;
    uint32_t rng_      = 0x1234567u;
    // SHIFT
    Hilbert  hil_[2];
    Svf2     xoLo_[2][2], xoHi_[2][2];
    SvfCoefs xoLoC_ = {}, xoHiC_ = {};
    float    dcX_[2] = {0.f, 0.f}, dcY_[2] = {0.f, 0.f};
    float    oscC_[3] = {1.f, 1.f, 1.f}, oscS_[3] = {0.f, 0.f, 0.f};
    float    shiftCur_[3] = {0.f, 0.f, 0.f}, shiftTgt_[3] = {0.f, 0.f, 0.f};
    float    glideCoef_ = 0.001f, rotC_ = 1.f, rotS_ = 0.f;
};

} // namespace ks
