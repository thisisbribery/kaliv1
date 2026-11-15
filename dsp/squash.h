// KALI SMASH DSP: SQUASH, the master section after MIX. Gain in, compressor, then a master clipper.
//
// Knob fully CCW: unity gain, the compressor's ratio is 1 (idle) and the clipper only catches overs
// above its knee, so it's transparent. Turning up pushes 0..+24 dB into the compressor (whose ratio
// ramps in over the first third) and then into the clipper. The output can't pass the ceiling.
#pragma once
#include "util.h"

namespace ks
{
enum CompType
{
    COMP_OFF,
    COMP_GLUE,
    COMP_SLAM,
    COMP_NUM_TYPES
};
static const char* const kCompTypeNames[COMP_NUM_TYPES] = {"OFF", "GLUE", "SLAM"};

enum ClipType
{
    CLIP_SOFT,
    CLIP_TANH,
    CLIP_HARD,
    CLIP_TAPE,
    CLIP_FOLD,
    CLIP_NUM_TYPES
};
static const char* const kClipTypeNames[CLIP_NUM_TYPES] = {"SOFT", "TANH", "HARD", "TAPE", "FOLD"};

struct SquashParams
{
    float amount    = 0.f; // 0..1, mini knob
    int   comp      = COMP_GLUE;
    int   clip      = CLIP_TANH;
    float ceilingDb = -1.f; // -12..0
    float outDb     = 0.f;  // -24..+6
};

class Squash
{
  public:
    void Configure(const SquashParams& p)
    {
        float s     = clampf(p.amount, 0.f, 1.f);
        gainTarget_ = dbToGain(24.f * powf(s, 1.5f));
        comp_       = p.comp;
        clip_       = p.clip;
        ceil_       = dbToGain(clampf(p.ceilingDb, -12.f, 0.f)) * 0.999f;
        invCeil_    = 1.f / ceil_;
        outTarget_  = dbToGain(p.outDb);
        float ramp  = clampf(s * 3.f, 0.f, 1.f);
        if(comp_ == COMP_GLUE)
        {
            thrDb_ = -10.f;
            slope_ = (1.f - 1.f / 2.5f) * ramp;
            att_   = expf(-1.f / (0.010f * kSampleRate));
            rel_   = expf(-1.f / (0.150f * kSampleRate));
        }
        else if(comp_ == COMP_SLAM)
        {
            thrDb_ = -18.f;
            slope_ = (1.f - 1.f / 20.f) * ramp;
            att_   = expf(-1.f / (0.0005f * kSampleRate));
            rel_   = expf(-1.f / (0.060f * kSampleRate));
        }
        else
            slope_ = 0.f;
        // Static makeup: what the compressor would take off a full-scale signal. Squashing makes things
        // denser and louder into the clipper, never quieter (the Bribes rule).
        makeupTarget_ = dbToGain(slope_ * -thrDb_ * 0.85f);
    }

    inline void Process(float& l, float& r)
    {
        gain_ += (gainTarget_ - gain_) * 0.002f;
        out_ += (outTarget_ - out_) * 0.002f;
        // Noise-floor guard: don't push gain or makeup into near-silence (that's where hiss and any
        // digital whine on the input live). Fades the extra gain out between -54 and -66 dBFS.
        float in   = fmaxf(fabsf(l), fabsf(r));
        guardEnv_  = in > guardEnv_ ? in : guardEnv_ * 0.99979f; // ~100 ms release
        float gdb  = fastGainToDb(guardEnv_);
        guard_    += (clampf((gdb + 66.f) / 12.f, 0.f, 1.f) - guard_) * 0.002f;
        float gin  = 1.f + (gain_ - 1.f) * guard_;
        l *= gin;
        r *= gin;

        if(slope_ > 0.f)
        {
            // stereo-linked peak detector with a sidechain high-pass (~80 Hz)
            float dl = l - scL_.Process(l, scA_);
            float dr = r - scR_.Process(r, scA_);
            float pk = fmaxf(fabsf(dl), fabsf(dr));
            float db = fastGainToDb(pk);
            float c  = db > envDb_ ? att_ : rel_;
            envDb_   = db + c * (envDb_ - db);
            // soft knee, 6 dB wide
            float over = envDb_ - thrDb_;
            float gr;
            if(over <= -3.f)
                gr = 0.f;
            else if(over < 3.f)
                gr = slope_ * (over + 3.f) * (over + 3.f) / 12.f;
            else
                gr = slope_ * over;
            makeup_ += (makeupTarget_ - makeup_) * 0.002f;
            float g = fastDbToGain(-gr) * (1.f + (makeup_ - 1.f) * guard_);
            l *= g;
            r *= g;
            grDb_ = gr;
        }
        else
            grDb_ = 0.f;

        l = Clip(l) * out_;
        r = Clip(r) * out_;
        // the converter's own limit
        l = clampf(l, -0.999f, 0.999f);
        r = clampf(r, -0.999f, 0.999f);
    }

    float GainReductionDb() const { return grDb_; }

  private:

    inline float Clip(float x)
    {
        float u  = x * invCeil_;
        float au = fabsf(u);
        float y;
        switch(clip_)
        {
            case CLIP_SOFT: // linear to 0.8, cubic knee into the ceiling
                y = au < 0.8f ? au : (au < 1.1f ? 0.8f + 0.2f * cubic((au - 0.8f) / 0.3f) : 1.f);
                break;
            case CLIP_TANH: // linear to 0.7, tanh shoulder
                y = au < 0.7f ? au : 0.7f + 0.3f * fastTanh((au - 0.7f) / 0.3f);
                break;
            case CLIP_HARD: y = au < 1.f ? au : 1.f; break;
            case CLIP_TAPE: // earlier, rounder bend
                y = au < 0.5f ? au : 0.5f + 0.5f * quinticClip((au - 0.5f) * 2.f);
                break;
            case CLIP_FOLD: // folds back from the ceiling
                y = au < 1.f ? au : fabsf(triangleFold(au));
                break;
            default: y = au; break;
        }
        return (u < 0.f ? -y : y) * ceil_;
    }

    int     comp_ = COMP_GLUE, clip_ = CLIP_TANH;
    float   gain_ = 1.f, gainTarget_ = 1.f, out_ = 1.f, outTarget_ = 1.f;
    float   ceil_ = 0.89f, invCeil_ = 1.f / 0.89f;
    float   makeup_ = 1.f, makeupTarget_ = 1.f, guardEnv_ = 0.f, guard_ = 0.f;
    float   thrDb_ = -10.f, slope_ = 0.f, att_ = 0.f, rel_ = 0.f, envDb_ = -120.f, grDb_ = 0.f;
    float   scA_ = onePoleCoef(80.f, kSampleRate);
    OnePole scL_, scR_;
};

} // namespace ks
