// KALI SMASH offline harness: runs the exact firmware DSP (../dsp) on the Mac.
//
//   render test                      latency/null test + loudness-vs-drive table for every type
//   render make-loop out.wav         writes a synthetic stereo drum/bass/pad loop (8 s, 48 kHz)
//   render in.wav out.wav key=value  renders a file through the chain
//
// Keys: mo mo.type mo.rate mo.fb mo.spread mo.chaos mo.pre re re.type re.note re.color re.damp re.detune
//       re.freeze re.pre hpf res hdrv hfm hfm.src hspread hpf24 lpf lres lpf24 all(0/1) os(0/1) mix eqpow trim
//       a.type a.amt a.shape a.tone a.bias a.stereo a.mix a.trim  (same for b.)
//       sq sq.comp sq.clip sq.ceil sq.out
// Types by name: a.type=TANH, sq.comp=GLUE, sq.clip=TANH, a.stereo=SIDE
#include "../dsp/chain.h"
#include "../dsp/outs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ks;

// ---------- WAV I/O ----------
static bool ReadWav(const char* path, std::vector<float>& l, std::vector<float>& r, int& sr)
{
    FILE* f = fopen(path, "rb");
    if(!f)
        return false;
    char     id[4];
    uint32_t sz;
    fread(id, 1, 4, f);
    fread(&sz, 4, 1, f);
    fread(id, 1, 4, f);
    int      ch = 0, bits = 0, fmt = 0;
    while(fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1)
    {
        if(!memcmp(id, "fmt ", 4))
        {
            uint16_t a, c, ba, b;
            uint32_t s, br;
            fread(&a, 2, 1, f);
            fread(&c, 2, 1, f);
            fread(&s, 4, 1, f);
            fread(&br, 4, 1, f);
            fread(&ba, 2, 1, f);
            fread(&b, 2, 1, f);
            fseek(f, sz - 16, SEEK_CUR);
            fmt  = a;
            ch   = c;
            sr   = s;
            bits = b;
            if(fmt == 0xFFFE)
                fmt = bits == 32 ? 3 : 1; // extensible: guess from width (fine for our files)
        }
        else if(!memcmp(id, "data", 4))
        {
            int                  bps = bits / 8;
            size_t               frames = sz / (bps * ch);
            std::vector<uint8_t> raw(sz);
            fread(raw.data(), 1, sz, f);
            l.resize(frames);
            r.resize(frames);
            for(size_t i = 0; i < frames; i++)
                for(int c = 0; c < ch && c < 2; c++)
                {
                    const uint8_t* p = &raw[(i * ch + c) * bps];
                    float          v = 0.f;
                    if(fmt == 3 && bits == 32)
                        memcpy(&v, p, 4);
                    else if(bits == 16)
                        v = (int16_t)(p[0] | (p[1] << 8)) / 32768.f;
                    else if(bits == 24)
                        v = (int32_t)((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) / 2147483648.f;
                    else if(bits == 32)
                        v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) / 2147483648.f;
                    (c == 0 ? l : r)[i] = v;
                }
            if(ch == 1)
                r = l;
            fclose(f);
            return true;
        }
        else
            fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    fclose(f);
    return false;
}

static void WriteWav(const char* path, const std::vector<float>& l, const std::vector<float>& r, int sr)
{
    FILE*    f      = fopen(path, "wb");
    uint32_t frames = l.size(), data = frames * 6, riff = 36 + data, s = sr, br = sr * 6, fmtsz = 16;
    uint16_t pcm = 1, ch = 2, ba = 6, bits = 24;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtsz, 4, 1, f);
    fwrite(&pcm, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&s, 4, 1, f);
    fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    for(uint32_t i = 0; i < frames; i++)
        for(int c = 0; c < 2; c++)
        {
            float   v = clampf((c ? r : l)[i], -1.f, 1.f);
            int32_t q = (int32_t)lrintf(v * 8388607.f);
            uint8_t b[3] = {(uint8_t)q, (uint8_t)(q >> 8), (uint8_t)(q >> 16)};
            fwrite(b, 1, 3, f);
        }
    fclose(f);
}

// ---------- synthetic loop: kick, snare, hats, bass, pad at 120 bpm ----------
static void MakeLoop(std::vector<float>& l, std::vector<float>& r, float seconds = 8.f)
{
    const int   n    = (int)(seconds * kSampleRate);
    const float step = kSampleRate * 60.f / 120.f / 4.f; // 16ths
    l.assign(n, 0.f);
    r.assign(n, 0.f);
    uint32_t rng = 12345;
    auto     noise = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return (int32_t)rng / 2147483648.f;
    };
    float hpz = 0.f, bassPh = 0.f, bassLp = 0.f, padPh[3] = {0, 0, 0};
    const float padHz[3] = {220.f, 261.63f, 329.63f};
    for(int i = 0; i < n; i++)
    {
        int   s16 = (int)(i / step);
        float t   = (i - s16 * step) / kSampleRate; // time since this 16th
        int   pos = s16 % 16;
        float L = 0.f, R = 0.f;
        // kick on 1, 5, 9, 13 (and 11)
        if(pos % 4 == 0 || pos == 10)
        {
            float f = 45.f + 120.f * expf(-t * 40.f);
            float k = sinf(2.f * kPi * f * t) * expf(-t * 7.f) * 0.55f;
            L += k;
            R += k;
        }
        // snare on 5 and 13
        if(pos == 4 || pos == 12)
        {
            float sn = (noise() * 0.5f + sinf(2.f * kPi * 190.f * t) * 0.3f) * expf(-t * 18.f) * 0.4f;
            L += sn * 0.9f;
            R += sn;
        }
        // hats every 16th, panned
        {
            float nz = noise();
            float hp = nz - hpz;
            hpz      = nz;
            float h  = hp * expf(-t * (pos % 2 ? 60.f : 30.f)) * 0.12f;
            L += h * 0.6f;
            R += h;
        }
        // bass: saw following the kick, A1
        {
            bassPh += 55.f / kSampleRate;
            if(bassPh >= 1.f)
                bassPh -= 1.f;
            float saw = 2.f * bassPh - 1.f;
            bassLp += 0.05f * (saw - bassLp);
            float env = (pos % 2 == 0) ? expf(-t * 6.f) : 0.3f * expf(-t * 10.f);
            L += bassLp * env * 0.25f;
            R += bassLp * env * 0.25f;
        }
        // pad: soft triangle chord, slightly wide
        for(int v = 0; v < 3; v++)
        {
            padPh[v] += padHz[v] * (1.f + 0.002f * v) / kSampleRate;
            if(padPh[v] >= 1.f)
                padPh[v] -= 1.f;
            float tri = 4.f * fabsf(padPh[v] - 0.5f) - 1.f;
            (v == 1 ? L : R) += tri * 0.035f;
            (v == 1 ? R : L) += tri * 0.02f;
        }
        l[i] = L;
        r[i] = R;
    }
}

// ---------- params from key=value ----------
static int Lookup(const char* v, const char* const* names, int count)
{
    for(int i = 0; i < count; i++)
        if(!strcasecmp(v, names[i]))
            return i;
    return atoi(v);
}

static void SetParam(ChainParams& p, const char* kv)
{
    std::string s(kv);
    size_t      eq = s.find('=');
    if(eq == std::string::npos)
        return;
    std::string k = s.substr(0, eq);
    const char* v = s.c_str() + eq + 1;
    float       f = atof(v);
    if(k == "hpf") p.hpfCut = f;
    else if(k == "res") p.hpfRes = f;
    else if(k == "hdrv") p.hpfDrive = f;
    else if(k == "hfm") p.hpfFm = f;
    else if(k == "hfm.src") p.hpfFmSrc = Lookup(v, kHpfFmNames, HPF_FM_NUM);
    else if(k == "hspread") p.hpfSpread = f;
    else if(k == "mo") p.motion.amount = f;
    else if(k == "mo.type") p.motion.type = Lookup(v, kMotionTypeNames, MOTION_NUM_TYPES);
    else if(k == "mo.rate") p.motion.rate = f;
    else if(k == "mo.fb") p.motion.feedback = f;
    else if(k == "mo.spread") p.motion.spread = f;
    else if(k == "mo.chaos") p.motion.chaos = f;
    else if(k == "mo.pre") p.motionPre = f > 0;
    else if(k == "re") p.reso.amount = f;
    else if(k == "re.type") p.reso.type = Lookup(v, kResoTypeNames, RESO_NUM_TYPES);
    else if(k == "re.note") p.reso.note = f;
    else if(k == "re.color") p.reso.color = f;
    else if(k == "re.damp") p.reso.damp = f;
    else if(k == "re.detune") p.reso.detune = f;
    else if(k == "re.freeze") p.reso.freeze = f > 0;
    else if(k == "re.pre") p.resoPre = f > 0;
    else if(k == "hpf24") p.hpf24 = f > 0;
    else if(k == "lpf") p.lpfCut = f;
    else if(k == "lres") p.lpfRes = f;
    else if(k == "lpf24") p.lpf24 = f > 0;
    else if(k == "all") p.filtersAll = f > 0;
    else if(k == "os") p.oversample = f > 0;
    else if(k == "mix") p.mix = f;
    else if(k == "eqpow") p.mixEqPow = f > 0;
    else if(k == "trim") p.inTrimDb = f;
    else if(k == "sq") p.squash.amount = f;
    else if(k == "sq.comp") p.squash.comp = Lookup(v, kCompTypeNames, COMP_NUM_TYPES);
    else if(k == "sq.clip") p.squash.clip = Lookup(v, kClipTypeNames, CLIP_NUM_TYPES);
    else if(k == "sq.ceil") p.squash.ceilingDb = f;
    else if(k == "sq.out") p.squash.outDb = f;
    else if(k.size() > 2 && (k[0] == 'a' || k[0] == 'b') && k[1] == '.')
    {
        DriveParams& d = p.drive[k[0] == 'b'];
        std::string  sub = k.substr(2);
        if(sub == "type") d.type = Lookup(v, kDriveTypeNames, DRIVE_NUM_TYPES);
        else if(sub == "amt") d.amount = f;
        else if(sub == "shape") d.shape = f;
        else if(sub == "tone") d.tone = f;
        else if(sub == "bias") d.bias = f;
        else if(sub == "stereo") d.stereo = Lookup(v, kStereoModeNames, STEREO_NUM_MODES);
        else if(sub == "mix") d.stageMix = f;
        else if(sub == "trim") d.trimDb = f;
        else fprintf(stderr, "unknown key %s\n", kv);
    }
    else
        fprintf(stderr, "unknown key %s\n", kv);
}

static void Run(const ChainParams& p, const std::vector<float>& l, const std::vector<float>& r,
                std::vector<float>& ol, std::vector<float>& orr, int* clipBlocks = nullptr)
{
    static Chain chain; // big; reset by re-construction
    chain = Chain();
    chain.Init();
    ol.resize(l.size());
    orr.resize(r.size());
    ChainMeters m;
    int         clips = 0;
    for(size_t i = 0; i < l.size(); i += 48)
    {
        int n = (int)std::min<size_t>(48, l.size() - i);
        chain.Process(p, &l[i], &r[i], &ol[i], &orr[i], n, m);
    }
    if(clipBlocks)
        *clipBlocks = clips;
}

static float Rms(const std::vector<float>& a, size_t from = 0)
{
    double s = 0;
    for(size_t i = from; i < a.size(); i++)
        s += (double)a[i] * a[i];
    return sqrtf(s / (a.size() - from));
}

// Level in one octave band around fc (two cascaded 2-pole bandpasses), dB
static float BandDb(const std::vector<float>& a, float fc)
{
    float  g = tanf(3.14159265f * fc / 48000.f), k = 1.41421356f; // Q ~0.7: about an octave wide
    float  a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float  s[2][2] = {};
    double e       = 0;
    for(size_t i = 0; i < a.size(); i++)
    {
        float v = a[i];
        for(int st = 0; st < 2; st++)
        {
            float v3 = v - s[st][1], v1 = a1 * s[st][0] + a2 * v3, v2 = s[st][1] + a2 * s[st][0] + a3 * v3;
            s[st][0] = 2.f * v1 - s[st][0];
            s[st][1] = 2.f * v2 - s[st][1];
            v        = k * v1; // bandpass, unity peak
        }
        if(i >= 4800)
            e += (double)v * v;
    }
    return 10.f * log10f((float)(e / (a.size() - 4800)) + 1e-20f);
}

static int Test()
{
    std::vector<float> l, r, ol, orr;
    MakeLoop(l, r, 4.f);
    int fails = 0;

    // 1. Null test: with the drives off, the wet path (through the oversampler) must line up with the dry
    //    path (a plain kLatency delay) sample for sample, or blending them would comb-filter. Compare
    //    mix=1 against mix=0 and also search +-4 samples for the best-matching lag (must be 0).
    for(int os : {1, 0})
    {
        ChainParams p;
        p.oversample       = os;
        p.squash.ceilingDb = 0.f;
        p.squash.clip      = CLIP_HARD;
        std::vector<float> dl, dr;
        p.mix = 0.f;
        Run(p, l, r, dl, dr);
        p.mix = 1.f;
        Run(p, l, r, ol, orr);
        int    bestLag = 0;
        double best    = 1e30;
        for(int lag = -4; lag <= 4; lag++)
        {
            double err = 0, sig = 0;
            for(size_t i = 4800; i < l.size() - 8; i++)
            {
                float d = ol[i + lag] - dl[i];
                err += d * d;
                sig += dl[i] * dl[i];
            }
            double db = 10.0 * log10(err / sig + 1e-20);
            if(db < best)
            {
                best    = db;
                bestLag = lag;
            }
        }
        bool ok = bestLag == 0 && best < -40.0;
        printf("null  os=%d  wet vs dry: best lag %d, error %6.1f dB %s\n", os, bestLag, best,
               ok ? "ok" : "FAIL");
        fails += !ok;
    }

    // 2. Loudness vs drive: each type must never get quieter as the amount goes up (the Bribes rule).
    //    Measured before SQUASH by setting the ceiling to 0 dB with a HARD clip (so only overs count).
    printf("\nRMS dB vs drive amount (wet only, squash transparent)\n%-6s", "type");
    const float amts[] = {0.f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.f};
    for(float a : amts)
        printf(" %5.1f", a);
    printf("\n");
    for(int t = 1; t < DRIVE_NUM_TYPES; t++)
    {
        printf("%-8s", kDriveTypeNames[t]);
        float prev = -999.f;
        bool  dropped = false;
        for(float a : amts)
        {
            ChainParams p;
            p.drive[0].type    = t;
            p.drive[0].amount  = a;
            p.squash.ceilingDb = 0.f;
            p.squash.clip      = CLIP_HARD;
            Run(p, l, r, ol, orr);
            float db = gainToDb(Rms(ol, 4800));
            printf(" %5.1f", db);
            if(!IsCrushType(t) && t != DRIVE_XOR && t != DRIVE_WRAP && db < prev - 0.5f)
                dropped = true;
            prev = db;
        }
        printf("%s\n", dropped ? "  <- gets quieter" : "");
        fails += dropped;
    }
    // 3. SHAPE must be a deliberate control. Measured as the change in tonal balance: level in eight
    //    octave bands (63 Hz .. 8 kHz) for SHP fully CCW vs fully CW, averaged |dB| across bands, plus
    //    the waveform difference. At DRV 30% and 60%; under 3 dB average band change counts as too subtle
    //    unless the waveform difference is big (above -6 dB: time-based or gating changes).
    printf("\nSHAPE effect (avg octave-band change dB / waveform difference dB), SHP CCW vs CW\n");
    printf("%-8s %16s %16s\n", "type", "DRV 30%", "DRV 60%");
    for(int t = 1; t < DRIVE_NUM_TYPES; t++)
    {
        printf("%-8s", kDriveTypeNames[t]);
        bool weak = false;
        for(float amt : {0.3f, 0.6f})
        {
            std::vector<float> a0l, a0r, a1l, a1r;
            ChainParams        p;
            p.drive[0].type    = t;
            p.drive[0].amount  = amt;
            p.squash.ceilingDb = 0.f;
            p.squash.clip      = CLIP_HARD;
            p.drive[0].shape   = 0.f;
            Run(p, l, r, a0l, a0r);
            p.drive[0].shape = 1.f;
            Run(p, l, r, a1l, a1r);
            double d = 0, sg = 0;
            for(size_t i = 4800; i < l.size(); i++)
            {
                d += (a1l[i] - a0l[i]) * (a1l[i] - a0l[i]);
                sg += a0l[i] * a0l[i] + a1l[i] * a1l[i];
            }
            float wave = 10.f * log10f(2.f * d / sg + 1e-20);
            float band = 0.f;
            for(int k = 0; k < 8; k++)
            {
                float fc = 63.f * (float)(1 << k);
                band += fabsf(BandDb(a1l, fc) - BandDb(a0l, fc));
            }
            band /= 8.f;
            bool w = band < 3.f && wave < -6.f;
            weak |= w;
            printf("   %5.1f / %5.1f%s", band, wave, w ? "*" : " ");
        }
        printf("%s\n", weak ? "  <- too subtle" : "");
        fails += weak;
    }

    // 4. MS-20 HPF: self-oscillates from silence at full RES with a bounded level and a pitch that follows
    //    the cutoff; stays quiet below the threshold; never blows up anywhere on the knobs.
    printf("\nMS-20 HPF\n");
    {
        std::vector<float> zl(48000 * 2, 0.f), zr(48000 * 2, 0.f);
        for(float cut : {0.3f, 0.5f, 0.7f})
            for(float res : {0.7f, 0.85f, 1.f})
            {
                Ms20Hpf f;
                ol.assign(zl.size(), 0.f);
                for(size_t i = 0; i < zl.size(); i++)
                    ol[i] = f.Process(0.f, CutoffToW(HpfHz(cut)), Ms20Feedback(res), 1.f, 0.f, false);
                float pk = 0.f;
                int   zc = 0;
                for(size_t i = 48000; i < ol.size(); i++)
                {
                    pk = fmaxf(pk, fabsf(ol[i]));
                    zc += (ol[i] >= 0.f) != (ol[i - 1] >= 0.f);
                }
                float hz = zc * 0.5f; // one second of crossings / 2
                bool  ok = res < 0.8f ? pk < 0.01f : (pk > 0.2f && pk < 1.2f && hz > HpfHz(cut) * 0.5f && hz < HpfHz(cut) * 1.6f);
                printf("cut %.1f (%5.0f Hz) res %.1f: peak %.3f, pitch %6.0f Hz %s\n", cut, HpfHz(cut), res, pk,
                       hz, ok ? "ok" : "FAIL");
                fails += !ok;
            }
        float worst = 0.f;
        bool  bad   = false;
        for(float cut = 0.f; cut <= 1.f; cut += 0.1f)
            for(float res : {0.f, 0.5f, 0.85f, 1.f})
                for(float drv : {0.f, 0.5f, 1.f})
                    for(int s24 : {0, 1})
                    {
                        ChainParams p;
                        p.hpfCut = cut;
                        p.hpfRes = res;
                        p.hpfDrive = drv;
                        p.hpf24  = s24;
                        p.squash.ceilingDb = 0.f;
                        p.squash.clip      = CLIP_HARD;
                        p.squash.outDb     = -24.f; // look at the level before the converter clamp
                        Run(p, l, r, ol, orr);
                        for(float v : ol)
                        {
                            if(!std::isfinite(v))
                                bad = true;
                            worst = fmaxf(worst, fabsf(v));
                        }
                    }
        printf("sweep of cutoff x res x drive x slope: %s, peak %.2f (at -24 dB out)\n", bad ? "NaN FAIL" : "finite ok",
               worst);
        fails += bad;
    }

    // 5. MOTION and RESO: every type at a few settings stays finite and bounded, and does something.
    //    Levels measured before SQUASH (ceiling 0 dB, HARD clip, OUT -24 dB so nothing is clamped).
    printf("\nMOTION / RESO: RMS change vs off, peak (pre-squash), effect (difference vs off)\n");
    {
        auto runFx = [&](ChainParams p, float& rmsDb, float& peak, float& effDb) {
            p.squash.ceilingDb = 0.f;
            p.squash.clip      = CLIP_HARD;
            p.squash.outDb     = -24.f;
            std::vector<float> ref_l, ref_r;
            ChainParams        q = p;
            q.motion.amount = 0.f;
            q.reso.amount   = 0.f;
            Run(q, l, r, ref_l, ref_r);
            Run(p, l, r, ol, orr);
            double e = 0, s0 = 0, s1 = 0;
            peak     = 0.f;
            bool nan = false;
            for(size_t i = 4800; i < ol.size(); i++)
            {
                if(!std::isfinite(ol[i]) || !std::isfinite(orr[i]))
                    nan = true;
                peak = fmaxf(peak, fmaxf(fabsf(ol[i]), fabsf(orr[i])) * 15.85f);
                e += (ol[i] - ref_l[i]) * (ol[i] - ref_l[i]);
                s0 += ref_l[i] * ref_l[i];
                s1 += ol[i] * ol[i];
            }
            rmsDb = 10.f * log10f((float)(s1 / s0));
            effDb = 10.f * log10f((float)(e / s0) + 1e-20f);
            return !nan;
        };
        for(int t = 0; t < MOTION_NUM_TYPES; t++)
            for(float amt : {0.5f, 1.f})
                for(float fb : {0.f, -1.f, 1.f})
                {
                    if(amt < 1.f && fb != 0.f)
                        continue;
                    ChainParams p;
                    p.motion.type     = t;
                    p.motion.amount   = amt;
                    p.motion.feedback = fb;
                    float rms, pk, eff;
                    bool  ok = runFx(p, rms, pk, eff);
                    ok       = ok && pk < 4.f && eff > -25.f;
                    printf("MOTION %-8s amt %.1f fb %+.0f: %+5.1f dB, peak %4.2f, effect %6.1f dB %s\n",
                           kMotionTypeNames[t], amt, fb, rms, pk, eff, ok ? "ok" : "FAIL");
                    fails += !ok;
                }
        for(int t = 0; t < RESO_NUM_TYPES; t++)
            for(float amt : {0.5f, 0.9f, 1.f})
                for(float note : {28.f, 48.f, 84.f})
                {
                    if(amt != 0.9f && note != 48.f)
                        continue;
                    ChainParams p;
                    p.reso.type   = t;
                    p.reso.amount = amt;
                    p.reso.note   = note;
                    float rms, pk, eff;
                    bool  ok = runFx(p, rms, pk, eff);
                    ok       = ok && pk < 4.f && eff > -25.f;
                    printf("RESO   %-8s amt %.1f note %2.0f: %+5.1f dB, peak %4.2f, effect %6.1f dB %s\n",
                           kResoTypeNames[t], amt, note, rms, pk, eff, ok ? "ok" : "FAIL");
                    fails += !ok;
                }
        // freeze: 2 s of loop, then 2 s of silence; the frozen ring must still be there in the last second
        std::vector<float> fl(l.begin(), l.begin() + 96000), fr(r.begin(), r.begin() + 96000);
        fl.resize(192000, 0.f);
        fr.resize(192000, 0.f);
        for(int t : {RESO_COMB, RESO_CHORD, RESO_MODAL, RESO_KARPLUS})
        {
            ChainParams p;
            p.reso.type        = t;
            p.reso.amount      = 0.9f;
            p.squash.ceilingDb = 0.f;
            p.squash.clip      = CLIP_HARD;
            p.squash.outDb     = -24.f;
            // play at 0.9, then freeze for the silent half
            static Chain ch;
            ch = Chain();
            ch.Init();
            ChainMeters m;
            std::vector<float> o1(fl.size()), o2(fl.size());
            for(size_t i = 0; i < fl.size(); i += 48)
            {
                p.reso.freeze = i >= 90000;
                ch.Process(p, &fl[i], &fr[i], &o1[i], &o2[i], 48, m);
            }
            double e = 0;
            for(size_t i = 144000; i < fl.size(); i++)
                e += o1[i] * o1[i];
            float db = 10.f * log10f((float)(e / 48000.0) * 251.f + 1e-20f); // +24 dB back
            bool  ok = db > -40.f && std::isfinite(db) && db < 6.f;
            printf("RESO   %-8s freeze: last second of silence at %6.1f dB RMS %s\n", kResoTypeNames[t], db,
                   ok ? "ok" : "FAIL");
            fails += !ok;
        }
    }

    // 6. BYPASS: with everything cranked, bypass must land on the untouched input (SQUASH transparent).
    printf("\nBYPASS and output sources\n");
    {
        ChainParams p;
        p.hpfCut = 0.5f; p.hpfRes = 0.9f; p.lpfCut = 0.4f;
        p.drive[0].type = DRIVE_FUZZ; p.drive[0].amount = 0.9f;
        p.motion.amount = 0.8f; p.reso.amount = 0.8f; p.reso.type = RESO_KARPLUS;
        p.squash.ceilingDb = 0.f; p.squash.clip = CLIP_HARD;
        p.bypass = true;
        Run(p, l, r, ol, orr);
        std::vector<float> dl(l.size(), 0.f); // the input, delayed by the chain latency and clamped like the DAC
        for(size_t i = Chain::kLatency; i < l.size(); i++)
            dl[i] = clampf(l[i - Chain::kLatency], -0.999f, 0.999f);
        double e = 0, sg = 0;
        for(size_t i = 9600; i < l.size(); i++)
        {
            e += (ol[i] - dl[i]) * (ol[i] - dl[i]);
            sg += dl[i] * dl[i];
        }
        float db = 10.f * log10f((float)(e / sg) + 1e-20f);
        bool  ok = db < -40.f;
        printf("bypass vs clean input: %.1f dB %s\n", db, ok ? "ok" : "FAIL");
        fails += !ok;

        OutSources o;
        OutParams  op;
        o.Configure(op);
        int   onsets = 0;
        float mn[SRC_NUM], mx[SRC_NUM];
        for(int k = 0; k < SRC_NUM; k++)
        {
            mn[k] = 9.f;
            mx[k] = -9.f;
        }
        for(size_t i = 0; i + 48 <= l.size(); i += 48)
        {
            o.ProcessBlock(&l[i], &r[i], 48, 0.3f, 4.f, 0.5f);
            onsets += o.Gate(GATE_TRANSIENT) ? 1 : 0;
            for(int k = 0; k < SRC_NUM; k++)
            {
                float v = o.Value(k);
                mn[k]   = fminf(mn[k], v);
                mx[k]   = fmaxf(mx[k], v);
            }
        }
        bool okr = true;
        for(int k = 1; k < SRC_NUM; k++)
        {
            bool inRange = mn[k] >= -1.f && mx[k] <= 1.f;
            bool moves   = mx[k] - mn[k] > 0.05f || k == SRC_BITE || k == SRC_GR || k == SRC_MOTION_LFO;
            okr &= inRange && moves;
            printf("  %-9s %+5.2f .. %+5.2f%s\n", kOutSourceNames[k], mn[k], mx[k], inRange && moves ? "" : "  FAIL");
        }
        printf("transient gate blocks over %.0f s of loop: %d %s\n", l.size() / 48000.f, onsets,
               onsets > 10 ? "ok" : "FAIL");
        fails += !okr + (onsets <= 10);
    }

    // 7. HPF audio FM and spread: finite and bounded everywhere; SELF on a mono input stays centred,
    //    SELF on an uneven input moves L and R apart; it changes the sound.
    printf("\nHPF audio FM\n");
    {
        auto run = [&](float fm, int src, float spr, const std::vector<float>& a, const std::vector<float>& b,
                       std::vector<float>& oa, std::vector<float>& ob) {
            ChainParams p;
            p.hpfCut = 0.35f; p.hpfRes = 0.7f; p.hpfFm = fm; p.hpfFmSrc = src; p.hpfSpread = spr;
            p.squash.ceilingDb = 0.f; p.squash.clip = CLIP_HARD; p.squash.outDb = -24.f;
            Run(p, a, b, oa, ob);
        };
        bool bad = false;
        float pk = 0.f;
        for(float fm : {0.3f, 0.7f, 1.f})
            for(int src = 0; src < HPF_FM_NUM; src++)
                for(float spr : {-1.f, 0.f, 1.f})
                {
                    run(fm, src, spr, l, r, ol, orr);
                    for(size_t i = 4800; i < ol.size(); i++) // after SQUASH's output gain has settled
                    {
                        if(!std::isfinite(ol[i]) || !std::isfinite(orr[i]))
                            bad = true;
                        pk = fmaxf(pk, fmaxf(fabsf(ol[i]), fabsf(orr[i])) * 15.85f);
                    }
                }
        printf("sweep fm x src x spread: %s, peak %.2f (pre-squash)\n", bad ? "NaN FAIL" : "finite ok", pk);
        fails += bad || pk > 4.f;
        std::vector<float> m(l.size()), o0l, o0r;
        for(size_t i = 0; i < l.size(); i++)
            m[i] = 0.5f * (l[i] + r[i]);
        run(0.7f, HPF_FM_SELF, 0.f, m, m, ol, orr);
        double d = 0, sg = 0;
        for(size_t i = 4800; i < ol.size(); i++)
        {
            d += (ol[i] - orr[i]) * (ol[i] - orr[i]);
            sg += ol[i] * ol[i];
        }
        float monoDb = 10.f * log10f((float)(d / sg) + 1e-20f);
        std::vector<float> lr(l), rr2(r);
        for(size_t i = 0; i < l.size(); i++)
            rr2[i] = 0.8f * m[i]; // the same mix, right side 2 dB down: uneven
        for(size_t i = 0; i < l.size(); i++)
            lr[i] = m[i];
        run(0.7f, HPF_FM_SELF, 0.f, lr, rr2, ol, orr);
        d = sg = 0;
        for(size_t i = 4800; i < ol.size(); i++)
        {
            float e = ol[i] - orr[i] / 0.8f;
            d += e * e;
            sg += ol[i] * ol[i];
        }
        float unevenDb = 10.f * log10f((float)(d / sg) + 1e-20f);
        run(0.f, 0, 0.f, l, r, o0l, o0r);
        run(0.7f, HPF_FM_SELF, 0.f, l, r, ol, orr);
        d = sg = 0;
        for(size_t i = 4800; i < ol.size(); i++)
        {
            d += (ol[i] - o0l[i]) * (ol[i] - o0l[i]);
            sg += o0l[i] * o0l[i];
        }
        float effDb = 10.f * log10f((float)(d / sg) + 1e-20f);
        bool  ok    = monoDb < -60.f && unevenDb > -30.f && effDb > -15.f;
        printf("mono in: L-R %.1f dB (stays centred) | 2 dB uneven in: L vs R shape %.1f dB (pushed apart) | "
               "FM 70%% vs off: %.1f dB %s\n", monoDb, unevenDb, effDb, ok ? "ok" : "FAIL");
        fails += !ok;
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all tests pass", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv)
{
    if(argc >= 2 && !strcmp(argv[1], "test"))
        return Test();
    if(argc >= 3 && !strcmp(argv[1], "make-loop"))
    {
        std::vector<float> l, r;
        MakeLoop(l, r);
        WriteWav(argv[2], l, r, 48000);
        return 0;
    }
    if(argc < 3)
    {
        fprintf(stderr, "usage: render test | make-loop out.wav | in.wav out.wav key=value...\n");
        return 2;
    }
    std::vector<float> l, r, ol, orr;
    int                sr = 48000;
    if(!ReadWav(argv[1], l, r, sr))
    {
        fprintf(stderr, "can't read %s\n", argv[1]);
        return 1;
    }
    if(sr != 48000)
        fprintf(stderr, "warning: %s is %d Hz; Kali runs at 48 kHz, so filter and time settings will be off\n",
                argv[1], sr);
    ChainParams p;
    for(int i = 3; i < argc; i++)
        SetParam(p, argv[i]);
    int clips = 0;
    Run(p, l, r, ol, orr, &clips);
    WriteWav(argv[2], ol, orr, sr);
    printf("%s: in %.1f dB RMS, out %.1f dB RMS, %d clipped blocks\n", argv[2], gainToDb(Rms(l)),
           gainToDb(Rms(ol)), clips);
    return 0;
}
