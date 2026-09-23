#include "SynthEngine.h"
#include <cmath>

namespace arploom
{
static constexpr double PI = 3.14159265358979323846;

//==============================================================================
int waveFromName (const juce::String& n)
{
    static const char* names[] = { "sine", "triangle", "sawtooth", "square", "pulse25", "pulse12", "glass", "organ", "hollow",
                                   "pulse40", "sid_combo", "nes_tri", "gb_wave", "gb_saw", "bit_sine", "ym_buzz", "ym_saw",
                                   "fm_brass", "fm_lead", "fm_organ", "fm_hollow", "fm_bass" };
    for (int i = 0; i < W_COUNT; ++i)
        if (n == names[i]) return i;
    return W_TRI;
}

// --- single-cycle shapes for the chip / FM waves (same formulas as WAVEFN in the page) ---
static double q4 (double v, int bits)
{
    const double L = (1 << bits) - 1;
    v = juce::jlimit (-1.0, 1.0, v);
    return std::round ((v * .5 + .5) * L) / L * 2 - 1;
}
static double stepd (double x, int n) { return std::floor (x * n) / n; }
static double waveFn (int w, double x)
{
    const double T = 2 * PI;
    switch (w)
    {
        case W_PULSE40:   return x < .4 ? 1 : -1;
        case W_SID_COMBO: { int t = (int) std::floor (x * 256), tri = t < 128 ? t * 2 : 511 - t * 2, saw = t; return ((tri & saw) & 255) / 127.5 - 1; }
        case W_NES_TRI:   return q4 (x < .5 ? 4 * x - 1 : 3 - 4 * x, 4);
        case W_GB_WAVE:   return q4 (std::sin (T * stepd (x, 32)) * .95, 4);
        case W_GB_SAW:    return q4 (2 * stepd (x, 32) - 1, 4);
        case W_BIT_SINE:  return q4 (std::sin (T * stepd (x, 32)), 3);
        case W_YM_BUZZ:   return 2 * std::pow (2.0, (std::floor (x * 16) - 15) / 2) - 1;
        case W_YM_SAW:    return q4 (2 * stepd (x, 16) - 1, 4);
        case W_FM_BRASS:  return std::sin (T * x + 2.5 * std::sin (T * x));
        case W_FM_LEAD:   return std::sin (T * x + 3.2 * std::sin (T * x + .8 * std::sin (T * x)));
        case W_FM_ORGAN:  return .6 * std::sin (T * x + .7 * std::sin (T * 3 * x)) + .4 * std::sin (T * 2 * x);
        case W_FM_HOLLOW: return std::sin (T * x + 1.8 * std::sin (T * 2 * x));
        case W_FM_BASS:   return std::sin (T * x + 1.6 * std::sin (T * x) + .6 * std::sin (T * 2 * x));
        default: return 0;
    }
}

const Wavetables& Wavetables::get() { static Wavetables w; return w; }

Wavetables::Wavetables()
{
    constexpr int N = tableSize, HMAX = 512;
    data.assign ((size_t) W_COUNT * numLevels * N, 0.f);
    std::vector<double> cosT (N), sinT (N);
    for (int k = 0; k < N; ++k) { cosT[(size_t) k] = std::cos (2 * PI * k / N); sinT[(size_t) k] = std::sin (2 * PI * k / N); }

    for (int w = 0; w < W_COUNT; ++w)
    {
        std::vector<double> re (HMAX + 1, 0.0), im (HMAX + 1, 0.0);
        int H = HMAX;
        switch (w)
        {
            case W_SINE:   im[1] = 1; H = 1; break;
            case W_TRI:    for (int n = 1; n <= HMAX; n += 2) im[(size_t) n] = 8 * std::sin (n * PI / 2) / ((n * PI) * (n * PI)); break;
            case W_SAW:    for (int n = 1; n <= HMAX; ++n) im[(size_t) n] = ((n % 2) ? 1.0 : -1.0) * 2 / (n * PI); break;
            case W_SQUARE: for (int n = 1; n <= HMAX; n += 2) im[(size_t) n] = 4 / (n * PI); break;
            case W_PULSE25: case W_PULSE12:
            {
                const double d = w == W_PULSE25 ? .25 : .125; H = 63;
                for (int n = 1; n <= H; ++n) re[(size_t) n] = 2 / (n * PI) * std::sin (n * PI * d);
                break;
            }
            case W_GLASS: { H = 63; const double hs[][2] = { {1,1},{2,.55},{3,.08},{4,.35},{6,.18},{8,.12},{12,.05} }; for (auto& h : hs) im[(size_t) h[0]] = h[1]; break; }
            case W_ORGAN: { H = 63; const double hs[][2] = { {1,1},{2,.7},{3,.5},{4,.35},{6,.25},{8,.2},{10,.1} }; for (auto& h : hs) im[(size_t) h[0]] = h[1]; break; }
            case W_HOLLOW: H = 63; for (int n = 1; n <= H; n += 2) im[(size_t) n] = 1 / std::pow ((double) n, 1.6); break;
            default:
            {
                // sampled shape -> 127 harmonics (DFT), like the page does
                H = 127; const int M = 2048;
                std::vector<double> sm (M);
                for (int k = 0; k < M; ++k) sm[(size_t) k] = waveFn (w, (double) k / M);
                for (int n = 1; n <= H; ++n)
                {
                    double a = 0, b = 0;
                    for (int k = 0; k < M; ++k) { const double ph = 2 * PI * n * k / M; a += sm[(size_t) k] * std::cos (ph); b += sm[(size_t) k] * std::sin (ph); }
                    re[(size_t) n] = 2 * a / M; im[(size_t) n] = 2 * b / M;
                }
            }
        }
        // build each mip level, then normalise every level by the full table's peak (like PeriodicWave)
        double peak = 0;
        for (int L = 0; L < numLevels; ++L)
        {
            const int maxH = juce::jmin (H, HMAX >> L);
            float* t = data.data() + ((size_t) w * numLevels + (size_t) L) * N;
            for (int k = 0; k < N; ++k)
            {
                double x = 0;
                for (int n = 1; n <= maxH; ++n)
                {
                    const int idx = (int) (((int64_t) n * k) % N);
                    x += re[(size_t) n] * cosT[(size_t) idx] + im[(size_t) n] * sinT[(size_t) idx];
                }
                t[k] = (float) x;
                if (L == 0) peak = juce::jmax (peak, std::abs (x));
            }
        }
        const float g = peak > 0 ? (float) (1.0 / peak) : 1.f;
        float* all = data.data() + (size_t) w * numLevels * N;
        for (int k = 0; k < numLevels * N; ++k) all[k] *= g;
    }
}

const float* Wavetables::table (int wave, double f, double sr) const
{
    const double allowed = juce::jmax (1.0, std::floor (sr * .5 / juce::jmax (1.0, f)));
    int L = (int) std::ceil (std::log2 (512.0 / allowed));
    L = juce::jlimit (0, numLevels - 1, L);
    return data.data() + ((size_t) juce::jlimit (0, W_COUNT - 1, wave) * numLevels + (size_t) L) * tableSize;
}

static inline float readTable (const float* t, double ph)
{
    const double x = ph * Wavetables::tableSize;
    const int i0 = (int) x;
    const float f = (float) (x - i0);
    return t[i0 & (Wavetables::tableSize - 1)] * (1 - f) + t[(i0 + 1) & (Wavetables::tableSize - 1)] * f;
}

//==============================================================================
static float num (const juce::var& o, const char* k, float d)
{
    const auto v = o.getProperty (k, juce::var());
    return v.isVoid() ? d : (float) (double) v;
}

SynthParams SynthParams::fromVar (const juce::var& v)
{
    SynthParams p;
    auto osc = [] (const juce::var& o, Osc& out)
    {
        out.wave = waveFromName (o.getProperty ("wave", "triangle").toString());
        out.oct = (int) num (o, "oct", 0); out.semi = (int) num (o, "semi", 0);
        out.det = num (o, "det", 0); out.lvl = num (o, "lvl", .7f);
        out.uni = juce::jlimit (1, 7, (int) num (o, "uni", 1));
        out.spread = num (o, "spread", 0); out.width = num (o, "width", 0);
    };
    auto env = [] (const juce::var& o, Env& out)
    { out.a = num (o, "a", .01f); out.d = num (o, "d", .5f); out.s = num (o, "s", .5f); out.r = num (o, "r", 1); };
    osc (v.getProperty ("osc1", {}), p.osc1);
    osc (v.getProperty ("osc2", {}), p.osc2);
    p.sub = num (v, "sub", 0); p.noise = num (v, "noise", 0);
    const auto f = v.getProperty ("filt", {});
    const auto type = f.getProperty ("type", "lowpass").toString();
    p.filtType = type == "highpass" ? 1 : type == "bandpass" ? 2 : 0;
    p.cut = num (f, "cut", 2600); p.res = num (f, "res", 1.2f); p.env = num (f, "env", 1.5f); p.key = num (f, "key", .3f);
    env (v.getProperty ("amp", {}), p.amp);
    env (v.getProperty ("fenv", {}), p.fenv);
    const auto d = v.getProperty ("delay", {});
    const auto dt = d.getProperty ("time", "3/16").toString();
    p.delayBeats = dt == "1/16" ? .25f : dt == "1/8T" ? 1.f / 3 : dt == "1/8" ? .5f : dt == "3/16" ? .75f : dt == "1/4" ? 1.f : dt == "3/8" ? 1.5f : dt == "1/2" ? 2.f : .75f;
    p.delayFb = num (d, "fb", .35f); p.delayMix = num (d, "mix", .22f);
    const auto r = v.getProperty ("rev", {});
    p.revSize = num (r, "size", 3.5f); p.revMix = num (r, "mix", .3f);
    p.vol = num (v, "vol", .8f);
    return p;
}

//==============================================================================
void Voice::start (const NoteSpec& s, const SynthParams& p, double sampleRate, int startDelay)
{
    spec = s; P = p; sr = sampleRate; delay = juce::jmax (0, startDelay);
    active = true; sustained = false; ageSamples = 0;
    freqHz = 440.0 * std::pow (2.0, (spec.pitch - 69) / 12.0);
    offAt = spec.durSec >= 0 ? (int64_t) (spec.durSec * sr) : -1;
    killAt = offAt >= 0 ? offAt + (int64_t) ((juce::jmax (P.amp.r, .01f) * 1.6 + .05) * sr) : -1;
    aStage = fStage = Attack; aLev = fLev = 0; fadeMul = 1; fadeStep = 0;
    // random start phases stop unison stacks from phasing identically on every note
    juce::Random rnd;
    for (auto& ph : ph1) ph = rnd.nextDouble();
    for (auto& ph : ph2) ph = rnd.nextDouble();
    phSub = 0; noiseSeed = (uint32_t) rnd.nextInt() | 1u;
    const double kt = std::pow (2.0, P.key * (spec.pitch - 60) / 12.0);
    baseCut = juce::jlimit (20.0, 20000.0, (double) P.cut * kt * spec.filterMul);
    peakCut = juce::jlimit (20.0, 20000.0, baseCut * std::pow (2.0, (double) P.env));
    zL1 = zL2 = zR1 = zR2 = 0; coefCountdown = 0;
    updateFilter (baseCut);
}

void Voice::release()
{
    if (! active || offAt >= 0 && ageSamples >= offAt) return;
    offAt = ageSamples;
    killAt = offAt + (int64_t) ((juce::jmax (P.amp.r, .01f) * 1.6 + .05) * sr);
    aStage = fStage = Release;
}

void Voice::quickStop() { if (active) fadeStep = (float) (1.0 / (.02 * sr)); }

void Voice::updateFilter (double f)
{
    f = juce::jlimit (10.0, sr * .49, f);
    const double w0 = 2 * PI * f / sr, cw = std::cos (w0), sw = std::sin (w0);
    const double Q = P.res;
    double alpha = P.filtType == 2 ? sw / (2 * juce::jmax (Q, 1e-4)) : sw / (2 * std::pow (10.0, Q / 20.0));
    double B0, B1, B2;
    if (P.filtType == 1)      { B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = B0; }
    else if (P.filtType == 2) { B0 = alpha; B1 = 0; B2 = -alpha; }
    else                      { B0 = (1 - cw) / 2; B1 = 1 - cw; B2 = B0; }
    const double A0 = 1 + alpha;
    b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = -2 * cw / A0; a2 = (1 - alpha) / A0;
}

void Voice::stepEnv (Stage& st, float& lev, const SynthParams::Env& e, bool)
{
    switch (st)
    {
        case Attack:
            lev += (float) (1.0 / (juce::jmax (e.a, .001f) * sr));
            if (lev >= 1) { lev = 1; st = Decay; }
            break;
        case Decay:
        {
            const float k = (float) std::exp (-1.0 / (juce::jmax (e.d, .001f) / 4 * sr));
            lev = e.s + (lev - e.s) * k;
            break;
        }
        case Release:
        {
            const float k = (float) std::exp (-1.0 / (juce::jmax (e.r, .005f) / 4 * sr));
            lev *= k;
            break;
        }
    }
}

void Voice::render (float* L, float* R, int n, float bend)
{
    if (! active) return;
    const auto& WT = Wavetables::get();
    // per-chunk (16 samples) values
    double inc1[7] {}, inc2[7] {}, incSub = 0;
    float g1L[7] {}, g1R[7] {}, g2L[7] {}, g2R[7] {};
    const float* t1 = nullptr; const float* t2 = nullptr; const float* tSub = nullptr;
    const int n1 = juce::jlimit (1, 7, P.osc1.uni), n2 = juce::jlimit (1, 7, P.osc2.uni);
    const float lv1 = P.osc1.lvl > .001f ? P.osc1.lvl / std::sqrt ((float) n1) : 0.f;
    const float lv2 = P.osc2.lvl > .001f ? P.osc2.lvl / std::sqrt ((float) n2) : 0.f;
    const float mixG = .22f * spec.vel / 127.f;
    // envelope coefficients are recomputed inside stepEnv (cheap exp per sample avoided by caching below)
    const float kaD = (float) std::exp (-1.0 / (juce::jmax (P.amp.d, .001f) / 4 * sr));
    const float kaR = (float) std::exp (-1.0 / (juce::jmax (P.amp.r, .005f) / 4 * sr));
    const float kfD = (float) std::exp (-1.0 / (juce::jmax (P.fenv.d, .001f) / 4 * sr));
    const float kfR = (float) std::exp (-1.0 / (juce::jmax (P.fenv.r, .005f) / 4 * sr));
    const float aInc = (float) (1.0 / (juce::jmax (P.amp.a, .001f) * sr));
    const float fInc = (float) (1.0 / (juce::jmax (P.fenv.a, .001f) * sr));
    float panNow = 0;
    // the per-chunk values above live only for this call, so always refresh them on its first sample
    // (carrying the countdown over from the last block left the wavetable pointers null -> crash in hosts
    //  whose block sizes aren't a multiple of 16, or when a note starts part-way through a block)
    coefCountdown = 0;

    for (int i = 0; i < n; ++i)
    {
        if (delay > 0) { --delay; continue; }
        if (--coefCountdown <= 0)
        {
            coefCountdown = 16;
            const double t = ageSamples / sr;
            double cents = spec.live ? bend : 0.0;
            if (! spec.tune.empty())
            {
                const auto& tp = spec.tune;
                double c = tp[0].second;
                for (size_t k = 1; k < tp.size(); ++k)
                {
                    const double t1k = tp[k].first, t0k = tp[k - 1].first;
                    if (t >= t1k) { c = tp[k].second; continue; }
                    double from = t0k;
                    if (spec.tuneSteps) from = juce::jmax (t0k, t1k - .012);
                    if (t > from) c = tp[k - 1].second + (tp[k].second - tp[k - 1].second) * (t - from) / juce::jmax (1e-6, t1k - from);
                    else c = tp[k - 1].second;
                    break;
                }
                cents += c;
            }
            double gmul = 1;
            if (spec.glideFrom >= 0 && spec.glideFrom != spec.pitch && t < spec.glideSec)
                gmul = std::pow (2.0, (spec.glideFrom - spec.pitch) / 12.0 * (1.0 - t / juce::jmax (1e-4, spec.glideSec)));
            const double pmul = gmul * std::pow (2.0, cents / 1200.0);
            auto setupOsc = [&] (const SynthParams::Osc& o, int cnt, double* inc, float* gl, float* gr, const float*& tab)
            {
                const double fr = freqHz * std::pow (2.0, o.oct + o.semi / 12.0) * pmul;
                tab = WT.table (o.wave, fr * std::pow (2.0, (std::abs (o.det) + o.spread) / 1200.0), sr);
                for (int u = 0; u < cnt; ++u)
                {
                    const double sp = cnt > 1 ? (double) u / (cnt - 1) * 2 - 1 : 0;
                    inc[u] = fr * std::pow (2.0, (o.det + sp * o.spread) / 1200.0) / sr;
                    if (cnt > 1 && o.width > 0)
                    {   // equal-power StereoPanner on a mono source
                        const double x = (sp * o.width + 1) * .5;
                        gl[u] = (float) std::cos (x * PI / 2); gr[u] = (float) std::sin (x * PI / 2);
                    }
                    else { gl[u] = 1; gr[u] = 1; }
                }
            };
            if (lv1 > 0) setupOsc (P.osc1, n1, inc1, g1L, g1R, t1);
            if (lv2 > 0) setupOsc (P.osc2, n2, inc2, g2L, g2R, t2);
            if (P.sub > .001f) { incSub = freqHz * .5 * pmul / sr; tSub = WT.table (W_SINE, freqHz * .5, sr); }
            updateFilter (baseCut + (peakCut - baseCut) * fLev);
            if (spec.hasPan)
                panNow = (float) (spec.pan0 + (spec.pan1 - spec.pan0) * juce::jmin (1.0, t / juce::jmax (1e-4, spec.durSec)));
        }
        // release trigger for sequenced notes
        if (offAt >= 0 && ageSamples == offAt) { aStage = fStage = Release; }

        float sL = 0, sR = 0;
        if (lv1 > 0)
            for (int u = 0; u < n1; ++u)
            {
                const float v = readTable (t1, ph1[(size_t) u]) * lv1;
                sL += v * g1L[u]; sR += v * g1R[u];
                ph1[(size_t) u] += inc1[u]; if (ph1[(size_t) u] >= 1) ph1[(size_t) u] -= 1;
            }
        if (lv2 > 0)
            for (int u = 0; u < n2; ++u)
            {
                const float v = readTable (t2, ph2[(size_t) u]) * lv2;
                sL += v * g2L[u]; sR += v * g2R[u];
                ph2[(size_t) u] += inc2[u]; if (ph2[(size_t) u] >= 1) ph2[(size_t) u] -= 1;
            }
        if (tSub != nullptr) { const float v = readTable (tSub, phSub) * P.sub; sL += v; sR += v; phSub += incSub; if (phSub >= 1) phSub -= 1; }
        if (P.noise > .001f)
        {
            noiseSeed = noiseSeed * 1664525u + 1013904223u;
            const float v = ((float) (noiseSeed >> 8) / 8388608.f - 1.f) * P.noise * .5f;
            sL += v; sR += v;
        }
        sL *= mixG; sR *= mixG;
        // biquad (transposed direct form II)
        const double yL = b0 * sL + zL1; zL1 = b1 * sL - a1 * yL + zL2; zL2 = b2 * sL - a2 * yL;
        const double yR = b0 * sR + zR1; zR1 = b1 * sR - a1 * yR + zR2; zR2 = b2 * sR - a2 * yR;
        // envelopes
        switch (aStage) { case Attack: aLev += aInc; if (aLev >= 1) { aLev = 1; aStage = Decay; } break;
                          case Decay: aLev = P.amp.s + (aLev - P.amp.s) * kaD; break;
                          case Release: aLev *= kaR; break; }
        switch (fStage) { case Attack: fLev += fInc; if (fLev >= 1) { fLev = 1; fStage = Decay; } break;
                          case Decay: fLev = P.fenv.s + (fLev - P.fenv.s) * kfD; break;
                          case Release: fLev *= kfR; break; }
        if (fadeStep > 0) { fadeMul -= fadeStep; if (fadeMul <= 0) { active = false; return; } }
        float oL = (float) yL * aLev * fadeMul, oR = (float) yR * aLev * fadeMul;
        if (spec.hasPan)
        {   // StereoPanner on a stereo source
            if (panNow <= 0) { const double x = panNow + 1; const float gl = (float) std::cos (x * PI / 2), gr = (float) std::sin (x * PI / 2); const float nl = oL + oR * gl; oR = oR * gr; oL = nl; }
            else             { const double x = panNow;     const float gl = (float) std::cos (x * PI / 2), gr = (float) std::sin (x * PI / 2); const float nr = oR + oL * gr; oL = oL * gl; oR = nr; }
        }
        L[i] += oL; R[i] += oR;
        ++ageSamples;
        if ((killAt >= 0 && ageSamples >= killAt) || (aStage == Release && aLev < 1e-5f)) { active = false; return; }
    }
}

//==============================================================================
void Engine::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    Wavetables::get();
    voiceBus.setSize (2, maxBlock); revSend.setSize (2, maxBlock);
    dlyL.assign ((size_t) (sr * 5.2), 0.f); dlyR.assign ((size_t) (sr * 5.2), 0.f); dlyW = 0;
    {   // delay feedback tone: 4.5 kHz low-pass
        const double w0 = 2 * PI * 4500 / sr, cw = std::cos (w0), sw = std::sin (w0), alpha = sw / (2 * std::pow (10.0, 0.0 / 20.0) * .7071 * 1.4142);
        const double A0 = 1 + alpha;
        toneB[0] = (1 - cw) / 2 / A0; toneB[1] = (1 - cw) / A0; toneB[2] = toneB[0]; toneA[1] = -2 * cw / A0; toneA[2] = (1 - alpha) / A0;
    }
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxBlock, 2 };
    convolution.prepare (spec);
    comp.prepare (spec); comp.setThreshold (-8.f); comp.setRatio (6.f); comp.setAttack (4.f); comp.setRelease (200.f);
    irSize = -1;
    rebuildIR (std::atomic_load (&params)->revSize);
    for (auto& v : voices) v.quickStop();
}

void Engine::rebuildIR (float size)
{
    if (std::abs (size - irSize) < 1e-3f) return;
    irSize = size;
    const int len = juce::jmax (1, (int) (size * sr));
    juce::AudioBuffer<float> ir (2, len);
    // mulberry32(7), same generator as the page
    uint32_t a = 7;
    auto rnd = [&a]() { a += 0x6D2B79F5u; uint32_t t = (a ^ (a >> 15)) * (1u | a); t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t; return (double) (t ^ (t >> 14)) / 4294967296.0; };
    double power = 0;
    for (int c = 0; c < 2; ++c)
    {
        auto* d = ir.getWritePointer (c); double y = 0;
        for (int i = 0; i < len; ++i)
        {
            const double x = (rnd() * 2 - 1) * std::pow (1.0 - (double) i / len, 2.2);
            const double k = .9 - .75 * ((double) i / len);
            y += (x - y) * k;
            d[i] = (float) (y * (i < sr * .004 ? i / (sr * .004) : 1.0));
            power += (double) d[i] * d[i];
        }
    }
    // Web Audio ConvolverNode normalisation
    power = std::sqrt (power / (2.0 * len));
    if (! std::isfinite (power) || power < .000125) power = .000125;
    const float scale = (float) (1.0 / power * .00125 * 44100.0 / sr);
    ir.applyGain (scale);
    convolution.loadImpulseResponse (std::move (ir), sr, juce::dsp::Convolution::Stereo::yes,
                                     juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
}

void Engine::setParams (std::shared_ptr<const SynthParams> p)
{
    if (p == nullptr) return;
    std::atomic_store (&params, p);
    rebuildIR (p->revSize);
}

void Engine::startNote (const NoteSpec& spec, int startDelay)
{
    Voice* v = nullptr;
    for (auto& x : voices) if (! x.isActive()) { v = &x; break; }
    if (v == nullptr)
    {   // steal the oldest non-held voice
        int64_t best = -1;
        for (auto& x : voices) if (! x.isLive() && x.age() > best) { best = x.age(); v = &x; }
        if (v == nullptr) v = &voices[0];
    }
    const auto cur = std::atomic_load (&params);
    v->start (spec, spec.params ? *spec.params : *cur, sr, startDelay);
}

void Engine::noteOffLive (int pitch)
{
    for (auto& v : voices)
        if (v.isActive() && v.isLive() && v.pitch() == pitch && ! v.sustained)
        {
            if (sustain) v.sustained = true;
            else v.release();
        }
}

void Engine::sustainPedal (bool down)
{
    sustain = down;
    if (! down)
        for (auto& v : voices) if (v.isActive() && v.sustained) { v.sustained = false; v.release(); }
}

void Engine::stopSequenced() { for (auto& v : voices) if (v.isActive() && ! v.isLive()) v.quickStop(); }
void Engine::panic()         { for (auto& v : voices) v.quickStop(); }

void Engine::render (juce::AudioBuffer<float>& out, int start, int n)
{
    // hosts may send a bigger block than they promised in prepare: work through it in pieces that fit the buffers
    const int cap = voiceBus.getNumSamples();
    if (cap <= 0 || out.getNumChannels() < 1) return;
    while (n > cap) { renderChunk (out, start, cap); start += cap; n -= cap; }
    renderChunk (out, start, n);
}

void Engine::renderChunk (juce::AudioBuffer<float>& out, int start, int n)
{
    if (n <= 0) return;
    const auto pp = std::atomic_load (&params);
    const auto& P = *pp;
    voiceBus.clear (0, n);
    float* vL = voiceBus.getWritePointer (0); float* vR = voiceBus.getWritePointer (1);
    for (auto& v : voices) if (v.isActive()) v.render (vL, vR, n, bendCents);

    // reverb send
    revSend.clear (0, n);
    revSend.copyFrom (0, 0, voiceBus.getReadPointer (0), n, P.revMix);
    revSend.copyFrom (1, 0, voiceBus.getReadPointer (1), n, P.revMix);
    if (P.revMix > 0)
    {
        juce::dsp::AudioBlock<float> blk (revSend.getArrayOfWritePointers(), 2, 0, (size_t) n);
        convolution.process (juce::dsp::ProcessContextReplacing<float> (blk));
    }

    float* oL = out.getWritePointer (0, start);
    float* oR = out.getNumChannels() > 1 ? out.getWritePointer (1, start) : nullptr;
    const float* rL = revSend.getReadPointer (0); const float* rR = revSend.getReadPointer (1);
    const int dlen = (int) dlyL.size();
    const double dSamp = juce::jlimit (1.0, (double) dlen - 3, P.delayBeats * 60.0 / juce::jmax (20.0, tempo) * sr);
    for (int i = 0; i < n; ++i)
    {
        // delay: in -> send -> delay -> tone -> (feedback into delay, and out)
        double rp = dlyW - dSamp; while (rp < 0) rp += dlen;
        const int i0 = (int) rp; const float fr = (float) (rp - i0);
        const float dl = dlyL[(size_t) i0] * (1 - fr) + dlyL[(size_t) ((i0 + 1) % dlen)] * fr;
        const float dr = dlyR[(size_t) i0] * (1 - fr) + dlyR[(size_t) ((i0 + 1) % dlen)] * fr;
        const double tl = toneB[0] * dl + tzL1; tzL1 = toneB[1] * dl - toneA[1] * tl + tzL2; tzL2 = toneB[2] * dl - toneA[2] * tl;
        const double tr = toneB[0] * dr + tzR1; tzR1 = toneB[1] * dr - toneA[1] * tr + tzR2; tzR2 = toneB[2] * dr - toneA[2] * tr;
        dlyL[(size_t) dlyW] = vL[i] * P.delayMix + (float) tl * P.delayFb;
        dlyR[(size_t) dlyW] = vR[i] * P.delayMix + (float) tr * P.delayFb;
        if (++dlyW >= dlen) dlyW = 0;
        const float mL = (vL[i] + (float) tl + rL[i]) * P.vol;
        const float mR = (vR[i] + (float) tr + rR[i]) * P.vol;
        oL[i] += mL; if (oR) oR[i] += mR; else oL[i] += 0;
    }
    // bus compressor + the make-up gain Web Audio's DynamicsCompressor applies
    juce::dsp::AudioBlock<float> ob (out.getArrayOfWritePointers(), (size_t) juce::jmin (2, out.getNumChannels()), (size_t) start, (size_t) n);
    comp.process (juce::dsp::ProcessContextReplacing<float> (ob));
    ob.multiplyBy (1.59f);
}

} // namespace arploom
