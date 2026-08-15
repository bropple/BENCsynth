/*
 * Shared bits for building a track out of BENCsynth racks offline:
 * WAV write, WAV read, and enough analysis to tell whether a sound is the
 * sound I meant when I cannot listen to it.
 */
#ifndef BSTRACK_H
#define BSTRACK_H

#include "bs_engine.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const int RATE = 48000;

/* ---------------- WAV ---------------- */

static void put32(FILE *f, unsigned v)
{
    std::fputc((int)(v & 0xff), f);        std::fputc((int)((v >> 8) & 0xff), f);
    std::fputc((int)((v >> 16) & 0xff), f); std::fputc((int)((v >> 24) & 0xff), f);
}
static void put16(FILE *f, unsigned v)
{
    std::fputc((int)(v & 0xff), f); std::fputc((int)((v >> 8) & 0xff), f);
}

/* Interleaved stereo floats out to 16-bit. */
static int writeWav(const std::string &path, const std::vector<float> &s)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return 0;
    const unsigned frames = (unsigned)(s.size() / 2);
    const unsigned data   = frames * 4;

    std::fwrite("RIFF", 1, 4, f); put32(f, 36 + data);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); put32(f, 16);
    put16(f, 1); put16(f, 2);
    put32(f, (unsigned)RATE); put32(f, (unsigned)RATE * 4);
    put16(f, 4); put16(f, 16);
    std::fwrite("data", 1, 4, f); put32(f, data);

    for (size_t i = 0; i < s.size(); i++) {
        float v = s[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        put16(f, (unsigned)(short)std::lrintf(v * 32000.0f));
    }
    std::fclose(f);
    return 1;
}

static int readWav(const std::string &path, std::vector<float> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    unsigned char h[44];
    if (std::fread(h, 1, 44, f) != 44) { std::fclose(f); return 0; }
    out.clear();
    short s[2];
    while (std::fread(s, 2, 2, f) == 2) {
        out.push_back((float)s[0] / 32000.0f);
        out.push_back((float)s[1] / 32000.0f);
    }
    std::fclose(f);
    return 1;
}

/* ---------------- analysis ---------------- */

struct Stats { double peak, rms, crest, dc; };

static Stats measure(const std::vector<float> &b, size_t a = 0, size_t z = 0)
{
    if (z == 0 || z > b.size()) z = b.size();
    Stats s = { 0.0, 0.0, 0.0, 0.0 };
    double sq = 0.0, sum = 0.0;
    size_t n = 0;
    for (size_t i = a; i < z; i++) {
        const double v = b[i];
        const double m = v < 0 ? -v : v;
        if (m > s.peak) s.peak = m;
        sq += v * v; sum += v; n++;
    }
    if (n) { s.rms = std::sqrt(sq / (double)n); s.dc = sum / (double)n; }
    s.crest = s.rms > 0 ? s.peak / s.rms : 0.0;
    return s;
}

/* Energy in a band, by direct sin/cos correlation - Goertzel drifts over a
 * long window and this is never a short one. Mono-summed, one channel stride. */
static double bandEnergy(const std::vector<float> &b, double f, size_t a, size_t z)
{
    if (z > b.size()) z = b.size();
    double re = 0.0, im = 0.0;
    const double w = 2.0 * 3.14159265358979 * f / RATE;
    for (size_t i = a; i + 1 < z; i += 2) {
        const double v = 0.5 * ((double)b[i] + (double)b[i + 1]);
        const double p = w * (double)(i / 2);
        re += v * std::cos(p); im += v * std::sin(p);
    }
    const double n = (double)((z - a) / 2);
    return n > 0 ? 2.0 * std::sqrt(re * re + im * im) / n : 0.0;
}

/* The strongest frequency in a range, found by search. */
static double dominant(const std::vector<float> &b, double lo, double hi,
                       size_t a, size_t z)
{
    double best = 0.0, bf = 0.0;
    for (double f = lo; f <= hi; f *= 1.01) {
        const double e = bandEnergy(b, f, a, z);
        if (e > best) { best = e; bf = f; }
    }
    return bf;
}

/* ---------------- a real spectrum ----------------
 *
 * Correlating at a handful of chosen frequencies does not work here and cost
 * an hour proving it: a 55 Hz saw has partials every 55 Hz, so a probe at
 * 63 Hz or 1000 Hz sits between two of them and reads the null. Measured that
 * way a raw saw appears to have almost nothing above 500 Hz, and a filter
 * sweep appears to do nothing. Welch-average a windowed FFT instead - every
 * partial lands in some bin, and no bin has to be guessed in advance. */

static void fftRadix2(std::vector<double> &re, std::vector<double> &im)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979 / (double)len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < len / 2; k++) {
                const double ur = re[i + k],           ui = im[i + k];
                const double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;             im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;   im[i + k + len / 2] = ui - vi;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

static const size_t FFTN = 8192;

/* Power per bin, Hann-windowed, half-overlapped, averaged over the window.
 * Bin k is k * RATE / FFTN Hz. */
static std::vector<double> spectrumOf(const std::vector<float> &b,
                                      size_t a, size_t z)
{
    if (z > b.size()) z = b.size();
    std::vector<double> acc(FFTN / 2, 0.0);
    const size_t nf = (z - a) / 2;
    if (nf < FFTN) return acc;

    int blocks = 0;
    for (size_t off = 0; off + FFTN <= nf; off += FFTN / 2) {
        std::vector<double> re(FFTN, 0.0), im(FFTN, 0.0);
        for (size_t i = 0; i < FFTN; i++) {
            const size_t k = a + (off + i) * 2;
            const double v = 0.5 * ((double)b[k] + (double)b[k + 1]);
            const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 *
                                                  (double)i / (double)FFTN);
            re[i] = v * w;
        }
        fftRadix2(re, im);
        for (size_t k = 0; k < FFTN / 2; k++)
            acc[k] += re[k] * re[k] + im[k] * im[k];
        blocks++;
    }
    if (blocks)
        for (size_t k = 0; k < FFTN / 2; k++) acc[k] /= blocks;
    return acc;
}

/* Summed power between two frequencies. */
static double bandOf(const std::vector<double> &spec, double flo, double fhi)
{
    const double hz = (double)RATE / (double)FFTN;
    size_t a = (size_t)(flo / hz), z = (size_t)(fhi / hz);
    if (z > spec.size()) z = spec.size();
    double s = 0.0;
    for (size_t k = a; k < z; k++) s += spec[k];
    return s;
}

/* Where the energy sits, in Hz. Bright things are high. */
static double centroid(const std::vector<float> &b, size_t a, size_t z)
{
    double num = 0.0, den = 0.0;
    for (double f = 40.0; f < 16000.0; f *= 1.12) {
        const double e = bandEnergy(b, f, a, z);
        num += e * f; den += e;
    }
    return den > 0 ? num / den : 0.0;
}

/* How long until the envelope falls to `frac` of its peak, in ms. */
static double decayMs(const std::vector<float> &b, double frac)
{
    const size_t n = b.size() / 2;
    std::vector<double> env(n, 0.0);
    double e = 0.0;
    const double k = 1.0 - std::exp(-1.0 / (0.005 * RATE));
    double pk = 0.0; size_t pi = 0;
    for (size_t i = 0; i < n; i++) {
        const double v = std::fabs(0.5 * ((double)b[i * 2] + (double)b[i * 2 + 1]));
        e += k * (v - e);
        env[i] = e;
        if (e > pk) { pk = e; pi = i; }
    }
    if (pk <= 0.0) return 0.0;
    for (size_t i = pi; i < n; i++)
        if (env[i] < pk * frac) return 1000.0 * (double)(i - pi) / RATE;
    return 1000.0 * (double)(n - pi) / RATE;
}

/* ---------------- knob access ---------------- */

/* Set a knob by module slot and param index, clamped to its own range so a
 * typo cannot push a filter past what the module will accept. */
static void knob(bs::Engine &eng, int slot, int param, float v)
{
    bs::Module *m = eng.patch.module(slot);
    if (!m || param < 0 || param >= (int)m->params.size()) {
        std::fprintf(stderr, "!! no knob %d/%d\n", slot, param);
        return;
    }
    bs::Param &p = m->params[(size_t)param];
    p.value = v < p.lo ? p.lo : (v > p.hi ? p.hi : v);
}

/* Drop a module into the rack and hand back its slot. A factory rack is a
 * starting point, not a fixed instrument - the VCF has no high-pass output,
 * so a hi-hat needs an SVF spliced in ahead of the VCA. */
static int addMod(bs::Engine &eng, const char *type)
{
    const int id = eng.addModule(type, 0.0f, 0.0f);
    if (id < 0) std::fprintf(stderr, "!! cannot add %s\n", type);
    return id;
}

/* Connecting to an input that already has a cable replaces it, the way a
 * patch bay does, so re-routing never needs an explicit disconnect. */
static void wire(bs::Engine &eng, int src, int sp, int dst, int dp)
{
    if (eng.connect(src, sp, dst, dp) < 0)
        std::fprintf(stderr, "!! cannot wire %d:%d -> %d:%d\n", src, sp, dst, dp);
}

#endif
