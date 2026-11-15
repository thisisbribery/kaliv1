// KALI SMASH DSP: small shared helpers. Plain C++, no platform dependencies.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ks
{
static constexpr float kSampleRate = 48000.f;
static constexpr float kPi         = 3.14159265f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float dbToGain(float db) { return powf(10.f, db * 0.05f); }
inline float gainToDb(float g) { return 20.f * log10f(g + 1e-12f); }
inline float smoothstep(float t, float lo, float hi)
{
    float u = clampf((t - lo) / (hi - lo), 0.f, 1.f);
    return u * u * (3.f - 2.f * u);
}

// tanh, rational approximation, exact enough for audio and monotonic, clamps to +-1 past |x| = 3
inline float fastTanh(float x)
{
    if(x > 3.f)
        return 1.f;
    if(x < -3.f)
        return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// x / sqrt(1 + x^2): the softest knee (Bribes WARM curve)
inline float algSoft(float x) { return x / sqrtf(1.f + x * x); }

// (15z - 10z^3 + 3z^5) / 8 on [-1, 1]: tanh-like and smooth into the flat top (Bribes TAPE curve)
inline float quinticClip(float x)
{
    float z = x * (8.f / 15.f);
    if(z >= 1.f)
        return 1.f;
    if(z <= -1.f)
        return -1.f;
    float z2 = z * z;
    return z * (1.875f + z2 * (-1.25f + 0.375f * z2));
}

// 1.5z - 0.5z^3 on [-1, 1]
inline float cubic(float z) { return z * (1.5f - 0.5f * z * z); }

// Triangle wave of x, period 4, peaks of +1 at x = 1 + 4k (a wavefolder)
inline float triangleFold(float x)
{
    float u  = x + 1.f;
    float q  = u * 0.25f;
    float fl = floorf(q);
    u -= 4.f * fl;
    return u < 2.f ? u - 1.f : 3.f - u;
}

// Fast float helpers for per-sample work (libm's logf/expf/floorf are library calls on the M7)
inline float fastFloor(float v)
{
    int32_t i = (int32_t)v;
    return (float)(i - (v < (float)i));
}
inline uint32_t floatBits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
inline float bitsFloat(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}
// log2 to ~1e-4; x must be > 0
inline float fastLog2(float x)
{
    uint32_t u = floatBits(x);
    float    e = (float)(int32_t)((u >> 23) & 0xff) - 127.f;
    float    m = bitsFloat((u & 0x007fffffu) | 0x3f800000u); // 1..2
    float ln = -1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m;
    return e + 1.44269504f * ln; // the polynomial is ln(m)
}
// 2^x to ~0.1%, for |x| < 100
inline float fastExp2(float x)
{
    float   fl = fastFloor(x);
    float   f  = x - fl;
    float   p  = 1.f + f * (0.6960656f + f * (0.2244494f + f * 0.0794603f)); // 2^f on 0..1
    int32_t e  = (int32_t)fl + 127;
    if(e < 1)
        return 0.f;
    if(e > 254)
        e = 254;
    return p * bitsFloat((uint32_t)e << 23);
}
inline float fastGainToDb(float g) { return 6.0205999f * fastLog2(g + 1e-12f); }
inline float fastDbToGain(float db) { return fastExp2(db * 0.16609640f); }

// sin(2 pi p) for a phase p in 0..1, parabolic with one correction (~0.1% error)
inline float fastSin01(float p)
{
    float x = p < 0.5f ? p : p - 1.f; // -0.5..0.5
    float y = 8.f * x - 16.f * x * fabsf(x);
    return y * (0.775f + 0.225f * fabsf(y));
}

// One-pole lowpass coefficient for a cutoff at a given sample rate
inline float onePoleCoef(float hz, float sr) { return 1.f - expf(-2.f * kPi * hz / sr); }

struct OnePole
{
    float z = 0.f;
    float Process(float x, float a)
    {
        z += a * (x - z);
        return z;
    }
};

// DC blocker, ~10 Hz at 48 kHz
struct DcBlock
{
    float x1 = 0.f, y1 = 0.f;
    float Process(float x)
    {
        float y = x - x1 + 0.9987f * y1;
        x1      = x;
        y1      = y;
        return y;
    }
};

// Fixed integer delay line, power-of-two length
template <int kLen>
struct Delay
{
    float buf[kLen] = {};
    int   w         = 0;
    float Process(float x, int d)
    {
        buf[w]  = x;
        float y = buf[(w - d) & (kLen - 1)];
        w       = (w + 1) & (kLen - 1);
        return y;
    }
};

} // namespace ks
