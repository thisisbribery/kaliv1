// KALI SMASH DSP: drive stages. Both stages run inside the 4x oversampled section, a block at a time.
//
// DRV (amount): fully CCW bypasses the stage. Above that it's input gain into the curve, slow at the
// bottom and steep at the top (Bribes v1.5 law): up to +36 dB, or +48 dB for FUZZ / DIST / METAL.
// No level compensation: more drive is always louder, and SQUASH catches the top.
// SHP (shape): each type's own big character control, audible at any drive setting. On the saturating
// types SHAPE also pushes harder into the curve, so its character shows even with DRV low.
//
// The crush family (VINT, ULAW, JITR) is the Bribes v1.27 crusher: zero-order hold so low rates
// alias instead of sounding filtered, and continuous bit depth. DRV = bits, SHP = sample rate.
#pragma once
#include "util.h"

namespace ks
{
enum DriveType
{
    DRIVE_OFF,
    DRIVE_TAPE,
    DRIVE_TANH,
    DRIVE_TUBE,
    DRIVE_SEAL,
    DRIVE_HARD,
    DRIVE_FOLD,
    DRIVE_FUZZ,
    DRIVE_DIST,
    DRIVE_METAL,
    DRIVE_RECT,
    DRIVE_WRAP,
    DRIVE_XOR,
    DRIVE_VINTAGE, // old sampler: soft input, 12-bit floor, 26 kHz-ish clock, one soft pole after the hold
    DRIVE_ULAW,    // companded (mu-law style segments, after MI Clouds/Beads): quiet tails stay grainy
    DRIVE_JITTER,  // unstable clock and a dithered threshold: noisy, crackly, sputtering
    DRIVE_NUM_TYPES
};
static const char* const kDriveTypeNames[DRIVE_NUM_TYPES] = {
    "OFF",   "TAPE", "TANH", "TUBE", "SEAL",  "HARD",    "FOLD", "FUZZ",  "DIST",
    "METAL", "RECT", "WRAP", "XOR",  "VINT", "ULAW", "JITR"};
inline bool IsCrushType(int t) { return t >= DRIVE_VINTAGE; }

enum StereoMode
{
    STEREO_LINK, // both channels through the curve
    STEREO_MID,  // only the mid (centre) is driven
    STEREO_SIDE, // only the sides are driven: dirtier and wider, kick and bass stay clean
    STEREO_NUM_MODES
};
static const char* const kStereoModeNames[STEREO_NUM_MODES] = {"LINK", "MID", "SIDE"};

struct DriveParams
{
    int   type     = DRIVE_OFF;
    float amount   = 0.f;  // 0..1
    float shape    = 0.5f; // 0..1
    float tone     = 0.f;  // -1 dark .. +1 bright (tilt after the curve)
    float bias     = 0.f;  // 0..1 extra asymmetry
    int   stereo   = STEREO_LINK;
    float stageMix = 1.f;  // parallel blend inside the stage
    float trimDb   = 0.f;  // OUTPUT, -12..+24 dB
    float boostDb  = 0.f;  // extra gain into the curve (SMASH)
};

class DriveStage
{
  public:
    static constexpr int kBufLen = 1024; // XOR copy and TAPE wobble, per channel (5.3 ms at 4x)

    // os = oversampling factor the stage runs at (4, or 1 with oversampling off)
    void Configure(const DriveParams& p, int os)
    {
        if(p.type != type_)
            ResetState();
        type_         = p.type;
        stereo_       = p.stereo;
        const float a = p.amount, s = clampf(p.shape, 0.f, 1.f);
        const float rate = kSampleRate * (float)os;
        invOs_           = 1.f / (float)os;
        smooth_          = 0.002f * invOs_;
        wetTarget_       = (type_ == DRIVE_OFF) ? 0.f : clampf(a / 0.02f, 0.f, 1.f);
        const float t    = clampf((a - 0.02f) / 0.98f, 0.f, 1.f);
        const bool  hiGain = type_ == DRIVE_FUZZ || type_ == DRIVE_DIST || type_ == DRIVE_METAL;
        gainTarget_        = dbToGain((hiGain ? 48.f : 36.f) * powf(t, 1.7f) + p.boostDb);
        shape_             = s;
        stageMix_          = p.stageMix;
        trim_              = dbToGain(p.trimDb);
        extraBias_         = p.bias * 0.5f;
        float tt           = clampf(p.tone, -1.f, 1.f);
        toneLo_            = dbToGain(-4.5f * tt);
        toneHi_            = dbToGain(4.5f * tt);
        toneA_             = onePoleCoef(800.f, rate);
        bias_              = 0.f;
        preG_              = 1.f;
        postG_             = 1.f;
        wobTarget_         = 0.f;

        switch(type_)
        {
            case DRIVE_TAPE:
                // SHP = wear: clean pro deck -> worn-out cassette. Pushes harder into a harder knee, leaves
                // a head bump in the lows, rolls the top off to ~3 kHz (12 dB/oct) and adds wow and flutter.
                bias_     = 0.06f;
                hardness_ = s * 0.8f;
                preG_     = dbToGain(6.f * s);
                postG_    = dbToGain(-3.f * s);
                splitA_   = onePoleCoef(120.f, rate);
                emphLo_   = dbToGain(10.f * s);                // into the curve
                undoLo_   = dbToGain(4.f * s) / emphLo_;       // after: +4 dB bump stays
                hfA_      = onePoleCoef(20000.f * powf(0.15f, s), rate);
                wobTarget_ = s * s;
                wowDepth_  = 0.0011f * rate;  // samples, at full wear
                flutDepth_ = 0.00012f * rate;
                wowInc_    = 0.55f / rate;
                flutInc_   = 6.3f / rate;
                driftInc_  = 1.7f / rate;
                break;
            case DRIVE_TANH:
                // SHP = what gets driven: CCW pushes the lows (fat, woolly), noon is flat, CW pushes the
                // highs (fizzy, crisp). Up to 18 dB of emphasis into the curve; only half is undone after,
                // so the tilt is heard even when nothing clips.
                splitA_ = onePoleCoef(700.f, rate);
                emphLo_ = dbToGain(s < 0.5f ? 36.f * (0.5f - s) : 0.f);
                emphHi_ = dbToGain(s > 0.5f ? 36.f * (s - 0.5f) : 0.f);
                undoLo_ = 1.f / sqrtf(emphLo_);
                undoHi_ = 1.f / sqrtf(emphHi_);
                break;
            case DRIVE_TUBE:
                // SHP = bias and push: clean push-pull -> one-sided single-ended -> starved and spitting
                bias_  = 0.05f + 1.6f * s * s;
                preG_  = dbToGain(12.f * s);
                postG_ = dbToGain(-5.f * s);
                break;
            case DRIVE_SEAL:
                // SHP = push: up to +18 dB extra into an ever harder knee
                bias_     = 0.12f;
                preG_     = dbToGain(18.f * s);
                postG_    = dbToGain(-6.f * s);
                hardness_ = s + (1.f - s) * smoothstep(t, 0.5f, 1.f);
                break;
            case DRIVE_HARD:
                // SHP = asymmetry and push: symmetric clip -> the negative half clipped almost flat, +12 dB
                negClip_ = lerpf(1.f, 0.06f, s);
                preG_    = dbToGain(12.f * s);
                postG_   = dbToGain(-5.f * s);
                break;
            case DRIVE_FOLD:
                // SHP = fold density: 1x to 6x extra into the folder, slight symmetry shift
                preG_  = 1.f + 5.f * s * s;
                bias_  = 0.25f * s;
                postG_ = dbToGain(4.f * t * t); // folding thins the level at the top; keep it climbing
                break;
            case DRIVE_FUZZ:
                // SHP = starve: full fuzz -> a gate eats the quiet parts -> sputtering velcro. The gate sits on
                // the input (up to -18 dBFS), so it bites at any DRV.
                deadZone_ = 0.12f * s * s;
                bias_     = 0.15f + 0.3f * s;
                break;
            case DRIVE_DIST:
                // SHP = filter, RAT style: CCW tight and cutting, CW thick and dark
                splitA_ = onePoleCoef(700.f * powf(40.f / 700.f, s), rate);    // pre high-pass 700 -> 40 Hz
                hfA_    = onePoleCoef(12000.f * powf(900.f / 12000.f, s), rate); // post low-pass 12k -> 900 Hz
                break;
            case DRIVE_METAL:
                // SHP = mid scoop: 0 to -30 dB across 180 Hz .. 3 kHz after two cascaded clip stages
                splitA_ = onePoleCoef(180.f, rate);
                hfA_    = onePoleCoef(3000.f, rate);
                scoop_  = dbToGain(-30.f * s);
                break;
            case DRIVE_RECT:
                // SHP = rectifier: plain clip -> half-wave -> full-wave (octave up)
                break;
            case DRIVE_WRAP:
                // SHP = wrap point: full scale down to 1/4 (more tearing per volt)
                preG_  = 1.f / lerpf(1.f, 0.25f, s);
                postG_ = 1.f / preG_;
                break;
            case DRIVE_XOR:
                // SHP = delay of the copy it's XORed against: 1 sample to 5 ms (at 4x)
                xorDelay_   = 1 + (int)(s * s * (float)(kBufLen - 2) * (float)os * 0.25f);
                gainTarget_ = dbToGain(18.f * t); // DRV = how hard it hits the bits
                break;
            case DRIVE_VINTAGE:
            case DRIVE_ULAW:
            case DRIVE_JITTER: ConfigureCrush(t, s, rate); break;
            default: break;
        }
        biasOffset_ = Curve(type_, bias_ + extraBias_);
        slopeComp_  = 1.f;
        if(type_ == DRIVE_TUBE) // unity slope at rest so the stage never starts quieter than bypass
        {
            float b    = bias_ + extraBias_;
            slopeComp_ = powf(1.f + b * b, 1.5f);
        }
        fuzzOffset_ = Curve(DRIVE_FUZZ, bias_ + extraBias_);
    }

    // In place, n samples per channel at the stage's rate
    void ProcessBlock(float* l, float* r, int n)
    {
        if(wetTarget_ == 0.f && sh_.wet < 1e-4f)
        {
            sh_.wet = 0.f;
            return;
        }
        if(stereo_ == STEREO_LINK)
        {
            Run(l, n, 0, sh_);
            sh_ = Run(r, n, 1, sh_);
            return;
        }
        for(int i = 0; i < n; i++)
        {
            float m = 0.5f * (l[i] + r[i]), sd = 0.5f * (l[i] - r[i]);
            l[i] = m;
            r[i] = sd;
        }
        if(stereo_ == STEREO_MID)
            sh_ = Run(l, n, 0, sh_);
        else
            sh_ = Run(r, n, 1, sh_);
        for(int i = 0; i < n; i++)
        {
            float m = l[i], sd = r[i];
            l[i]    = m + sd;
            r[i]    = m - sd;
        }
    }

    float TakeBite()
    {
        float b  = biteIn_ > 1e-9f ? clampf(biteErr_ / biteIn_, 0.f, 1.f) : 0.f;
        biteIn_  = 0.f;
        biteErr_ = 0.f;
        return b * sh_.wet;
    }

  private:
    // State both channels must step through identically (smoothers, clocks, write index): each channel
    // starts the block from the same copy.
    struct Shared
    {
        float    gain = 1.f, wet = 0.f, wob = 0.f;
        float    countdown = 0.f;
        uint32_t rng       = 0x9E3779B9u;
        float    phWow = 0.f, phFlut = 0.25f, phDrift = 0.6f;
        int      w = 0;
    };
    // Per-channel filter and hold state
    struct Ch
    {
        float split = 0.f, mid1 = 0.f, mid2 = 0.f, hf = 0.f, hf2 = 0.f, tone = 0.f, held = 0.f, lp = 0.f;
    };

    void ResetState()
    {
        for(int c = 0; c < 2; c++)
        {
            ch_[c] = Ch();
            for(int i = 0; i < kBufLen; i++)
                buf_[c][i] = 0.f;
        }
        sh_.countdown = 0.f;
        sh_.wob       = 0.f;
    }

    void ConfigureCrush(float t, float s, float rate)
    {
        gainTarget_ = 1.f;
        float bits  = 12.f;
        if(type_ == DRIVE_VINTAGE)
            bits = 12.f * powf(3.f / 12.f, t); // always a 12-bit converter; DRV takes it down to 3
        else if(type_ == DRIVE_ULAW)
        {
            // bits in each segment: 5 (smooth), 4 (8-bit telephone), down to 0 (one level per octave);
            // towards the end the bottom segments merge too, so quiet parts get coarse and gate
            muStep_    = exp2f(-(5.f - 5.f * t));
            muBias_    = 33.f * exp2f(6.f * t * t);
            invMuStep_ = 1.f / muStep_;
        }
        else
            bits = 12.f * powf(1.f / 12.f, t);
        step_    = 2.f * exp2f(-bits);
        invStep_ = 1.f / step_;
        // Clock, in 48 kHz samples per held sample: up to 256x down (188 Hz), middle of the knob ~5 kHz
        if(type_ == DRIVE_VINTAGE)
        {
            period_  = (kSampleRate / 26040.f) * exp2f(5.5f * powf(s, 1.4f)); // ~26 kHz down to ~600 Hz
            float fc = fminf(kSampleRate / period_, 16000.f);
            lpCoef_  = 1.f - expf(-2.f * kPi * fc / rate);
        }
        else
            period_ = exp2f(8.f * powf(s, 1.4f));
    }

    static inline float Noise(uint32_t& r)
    {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return (float)(int32_t)r * (1.f / 2147483648.f);
    }

    // The static curves (also used per block for bias offsets)
    inline float Curve(int type, float z) const
    {
        switch(type)
        {
            case DRIVE_TAPE:
            case DRIVE_SEAL:
            {
                float q = quinticClip(z);
                return q + hardness_ * (clampf(z, -1.f, 1.f) - q);
            }
            case DRIVE_TANH: return fastTanh(z);
            case DRIVE_TUBE: return algSoft(z);
            case DRIVE_HARD: return clampf(z, -negClip_, 1.f);
            case DRIVE_FOLD: return cubic(triangleFold(z * (2.f / 3.f)));
            case DRIVE_FUZZ: // transistor-ish: hard on top, softer and lower on the bottom
                return z > 0.f ? fastTanh(2.5f * z) : -0.7f * fastTanh(-1.2f * z);
            case DRIVE_DIST: return clampf(fastTanh(3.f * z) * 1.1f, -1.f, 1.f);
            case DRIVE_RECT: return fastTanh(z);
            case DRIVE_WRAP:
            {
                float u = z + 1.f;
                return u - 2.f * fastFloor(u * 0.5f) - 1.f;
            }
            default: return z;
        }
    }

    template <int T>
    inline float Quantise(float x, float d) const
    {
        if(T == DRIVE_ULAW)
        {
            // G.711-style segments on a 13-bit scale: each octave of level gets the same number of steps
            constexpr float kScale = 8159.f;
            if(x == 0.f)
                return 0.f; // otherwise silence turns into a tiny DC offset
            float    a       = fminf(fabsf(x), 8.f) * kScale + muBias_;
            uint32_t e       = (floatBits(a) >> 23) & 0xFF; // floor(log2) + 127
            float    seg     = bitsFloat(e << 23);          // 2^floor(log2 a)
            float    invSeg  = bitsFloat((254u - e) << 23);
            float    stp     = seg * muStep_;
            float    q       = (fastFloor(a * invSeg * invMuStep_) + 0.5f) * stp;
            q                = fmaxf(q - muBias_, 0.f) * (1.f / kScale);
            return x < 0.f ? -q : q;
        }
        // mid-tread rounding: no DC, quiet signals gate to zero at low bit depths
        return fastFloor(clampf(x, -64.f, 64.f) * invStep_ + 0.5f + d) * step_;
    }

    Shared Run(float* x, int n, int c, Shared sh)
    {
        switch(type_)
        {
            case DRIVE_TAPE: return RunT<DRIVE_TAPE>(x, n, c, sh);
            case DRIVE_TANH: return RunT<DRIVE_TANH>(x, n, c, sh);
            case DRIVE_TUBE: return RunT<DRIVE_TUBE>(x, n, c, sh);
            case DRIVE_SEAL: return RunT<DRIVE_SEAL>(x, n, c, sh);
            case DRIVE_HARD: return RunT<DRIVE_HARD>(x, n, c, sh);
            case DRIVE_FOLD: return RunT<DRIVE_FOLD>(x, n, c, sh);
            case DRIVE_FUZZ: return RunT<DRIVE_FUZZ>(x, n, c, sh);
            case DRIVE_DIST: return RunT<DRIVE_DIST>(x, n, c, sh);
            case DRIVE_METAL: return RunT<DRIVE_METAL>(x, n, c, sh);
            case DRIVE_RECT: return RunT<DRIVE_RECT>(x, n, c, sh);
            case DRIVE_WRAP: return RunT<DRIVE_WRAP>(x, n, c, sh);
            case DRIVE_XOR: return RunT<DRIVE_XOR>(x, n, c, sh);
            case DRIVE_VINTAGE: return RunT<DRIVE_VINTAGE>(x, n, c, sh);
            case DRIVE_ULAW: return RunT<DRIVE_ULAW>(x, n, c, sh);
            case DRIVE_JITTER: return RunT<DRIVE_JITTER>(x, n, c, sh);
            default: return RunT<DRIVE_OFF>(x, n, c, sh);
        }
    }

    template <int T>
    Shared RunT(float* __restrict x, int n, int c, Shared sh)
    {
        Ch               s   = ch_[c];
        float* __restrict db = buf_[c];
        const float gT = gainTarget_, wT = wetTarget_, sc = smooth_;
        const float wobT = wobTarget_;
        float       bIn = 0.f, bErr = 0.f;
        const float bias = bias_ + extraBias_;
        for(int i = 0; i < n; i++)
        {
            sh.gain += (gT - sh.gain) * sc;
            sh.wet += (wT - sh.wet) * sc;
            const float in = x[i];
            float       z  = in * sh.gain;
            float       y;
            if(T == DRIVE_TAPE)
            {
                // wow and flutter: a short delay, modulated (bypassed when there's no wear)
                sh.wob += (wobT - sh.wob) * sc;
                if(sh.wob > 1e-5f)
                {
                    db[sh.w] = z;
                    sh.phWow += wowInc_;
                    sh.phWow -= (float)(sh.phWow >= 1.f);
                    sh.phFlut += flutInc_;
                    sh.phFlut -= (float)(sh.phFlut >= 1.f);
                    sh.phDrift += driftInc_;
                    sh.phDrift -= (float)(sh.phDrift >= 1.f);
                    float mod = wowDepth_ * (0.7f * fastSin01(sh.phWow) + 0.3f * fastSin01(sh.phDrift))
                                + flutDepth_ * fastSin01(sh.phFlut);
                    float d  = sh.wob * (wowDepth_ + flutDepth_ + mod) + 1.f;
                    float rp = (float)sh.w - d;
                    float fl = fastFloor(rp);
                    float fr = rp - fl;
                    int   i0 = (int)fl & (kBufLen - 1);
                    float a0 = db[i0], a1 = db[(i0 + 1) & (kBufLen - 1)];
                    // d = 1 + wob*... : at wob -> 0 this reads 1 sample back; blend that last sample out
                    float rd = a0 + fr * (a1 - a0);
                    z        = z + (rd - z) * fminf(sh.wob * 50.f, 1.f);
                    sh.w     = (sh.w + 1) & (kBufLen - 1);
                }
                float lo  = (s.split += splitA_ * (z - s.split));
                float e   = (lo * emphLo_ + (z - lo)) * preG_;
                y         = Curve(T, e + bias) - biasOffset_;
                float lo2 = (s.mid1 += splitA_ * (y - s.mid1));
                y         = lo2 * undoLo_ + (y - lo2);
                s.hf += hfA_ * (y - s.hf);
                s.hf2 += hfA_ * (s.hf - s.hf2);
                y = s.hf2 * postG_;
            }
            else if(T == DRIVE_TANH)
            {
                float lo  = (s.split += splitA_ * (z - s.split));
                float e   = lo * emphLo_ + (z - lo) * emphHi_;
                y         = Curve(T, e + bias) - biasOffset_;
                float lo2 = (s.mid1 += splitA_ * (y - s.mid1));
                y         = lo2 * undoLo_ + (y - lo2) * undoHi_;
            }
            else if(T == DRIVE_DIST)
            {
                float hp = z - (s.split += splitA_ * (z - s.split));
                y        = Curve(T, hp + extraBias_);
                y        = (s.hf += hfA_ * (y - s.hf));
            }
            else if(T == DRIVE_METAL)
            {
                float s1 = fastTanh(4.f * z + extraBias_);
                float lo = (s.mid1 += splitA_ * (s1 - s.mid1)); // interstage: thin the lows, push the mids
                float s2 = clampf(2.5f * (s1 - 0.5f * lo), -1.f, 1.f);
                float l2 = (s.mid2 += splitA_ * (s2 - s.mid2)); // output mid scoop
                float h2 = s2 - (s.hf += hfA_ * (s2 - s.hf));
                y        = l2 + h2 + (s2 - l2 - h2) * scoop_;
            }
            else if(T == DRIVE_FUZZ)
            {
                float ai = fabsf(in) - deadZone_; // input-referred gate
                float g  = ai > 0.f ? (in > 0.f ? ai : -ai) : 0.f;
                y        = Curve(T, g * sh.gain + bias) - fuzzOffset_;
            }
            else if(T == DRIVE_RECT)
            {
                float cc   = Curve(T, z + extraBias_);
                float half = cc > 0.f ? cc : 0.f;
                // half-wave loses its DC to the blocker, so it's made up generously
                y = shape_ < 0.5f ? lerpf(cc, half * 2.f, shape_ * 2.f)
                                  : lerpf(half * 2.f, fabsf(cc) * 1.2f, shape_ * 2.f - 1.f);
            }
            else if(T == DRIVE_XOR)
            {
                db[sh.w]    = z;
                float old   = db[(sh.w - xorDelay_) & (kBufLen - 1)];
                sh.w        = (sh.w + 1) & (kBufLen - 1);
                int32_t a   = (int32_t)(clampf(z, -1.f, 1.f) * 32767.f);
                int32_t b   = (int32_t)(clampf(old, -1.f, 1.f) * 32767.f);
                y           = (float)(int16_t)(a ^ b) * (1.f / 32768.f);
            }
            else if(T == DRIVE_VINTAGE || T == DRIVE_ULAW || T == DRIVE_JITTER)
            {
                sh.countdown -= invOs_;
                if(sh.countdown <= 0.f)
                {
                    float period = period_;
                    if(T == DRIVE_JITTER)
                        period *= 1.f + 0.5f * Noise(sh.rng);
                    sh.countdown += period;
                    if(sh.countdown < 0.f) // the period got a lot shorter all at once
                        sh.countdown = period;
                    float v = in;
                    if(T == DRIVE_VINTAGE)
                    {
                        // soft converter input: unity for small signals, ceiling 1.33
                        float h = clampf(v * 0.5f, -1.f, 1.f);
                        v       = 2.f * (h - h * h * h * (1.f / 3.f));
                    }
                    float d = 0.f;
                    if(T == DRIVE_JITTER)
                        d = 0.4f * Noise(sh.rng);
                    s.held = Quantise<T>(v, d);
                }
                y = s.held;
                if(T == DRIVE_VINTAGE)
                    y = (s.lp += lpCoef_ * (y - s.lp));
                z = in;
            }
            else // TUBE, SEAL, HARD, FOLD, WRAP
            {
                y = (Curve(T, z * preG_ + bias) - biasOffset_) * slopeComp_ * postG_;
            }
            bIn += fabsf(z);
            bErr += fabsf(y - z);

            float lo = (s.tone += toneA_ * (y - s.tone)); // tilt EQ
            y        = lo * toneLo_ + (y - lo) * toneHi_;
            y        = (in + stageMix_ * (y - in)) * trim_; // stage blend and trim
            x[i]     = in + sh.wet * (y - in);              // fade in from bypass
        }
        ch_[c] = s;
        biteIn_ += bIn;
        biteErr_ += bErr;
        return sh;
    }

    int    type_ = -1, stereo_ = STEREO_LINK;
    Shared sh_;
    float  gainTarget_ = 1.f, wetTarget_ = 0.f, smooth_ = 0.0005f, invOs_ = 0.25f;
    float  shape_ = 0.5f, stageMix_ = 1.f, trim_ = 1.f;
    float  bias_ = 0.f, extraBias_ = 0.f, biasOffset_ = 0.f, hardness_ = 0.f, slopeComp_ = 1.f;
    float  preG_ = 1.f, postG_ = 1.f, negClip_ = 1.f, deadZone_ = 0.f, scoop_ = 1.f, fuzzOffset_ = 0.f;
    float  splitA_ = 0.f, emphLo_ = 1.f, emphHi_ = 1.f, undoLo_ = 1.f, undoHi_ = 1.f, hfA_ = 1.f;
    float  toneLo_ = 1.f, toneHi_ = 1.f, toneA_ = 0.f;
    float  wobTarget_ = 0.f, wowDepth_ = 0.f, flutDepth_ = 0.f, wowInc_ = 0.f, flutInc_ = 0.f, driftInc_ = 0.f;
    int    xorDelay_ = 1;
    float  step_ = 1.f, invStep_ = 1.f, muStep_ = 1.f, invMuStep_ = 1.f, muBias_ = 33.f, period_ = 1.f,
          lpCoef_ = 1.f;
    float buf_[2][kBufLen] = {};
    Ch    ch_[2];
    float biteIn_ = 0.f, biteErr_ = 0.f;
};

} // namespace ks
