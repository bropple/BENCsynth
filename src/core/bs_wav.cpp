/*
 * BENCsynth - reading a .wav. See bs_wav.h for what is and is not supported.
 */

#include "bs_wav.h"

#include <cstdio>
#include <cstring>

namespace bs {

namespace {

unsigned rd32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* One sample, whatever it is stored as, as a float in -1..1. */
float readOne(const unsigned char *p, int bits, int isFloat)
{
    if (isFloat) {
        if (bits == 32) { float f; std::memcpy(&f, p, 4); return f; }
        if (bits == 64) { double d; std::memcpy(&d, p, 8); return (float)d; }
        return 0.0f;
    }
    switch (bits) {
    case 8:
        /* Eight-bit WAV is unsigned, and every other width is signed. */
        return ((float)p[0] - 128.0f) / 128.0f;
    case 16: {
        const short v = (short)(unsigned short)rd16(p);
        return (float)v / 32768.0f;
    }
    case 24: {
        int v = (int)p[0] | ((int)p[1] << 8) | ((int)p[2] << 16);
        if (v & 0x800000) v -= 0x1000000;         /* sign extend */
        return (float)v / 8388608.0f;
    }
    case 32: {
        const int v = (int)rd32(p);
        return (float)v / 2147483648.0f;
    }
    default:
        return 0.0f;
    }
}

}  /* namespace */

bool wavLoad(const char *path, WavData *out, std::string *err, int maxSeconds)
{
    struct Fail {
        static bool no(std::string *e, const char *msg)
        { if (e) *e = msg; return false; }
    };

    if (!path || !*path || !out) return Fail::no(err, "no file");
    out->samples.clear();
    out->frames = 0;
    out->rate   = 0.0;

    std::FILE *f = std::fopen(path, "rb");
    if (!f) return Fail::no(err, "cannot open that file");

    unsigned char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        std::fclose(f);
        return Fail::no(err, "not a WAV file");
    }

    int channels = 0, bits = 0, isFloat = 0;
    double rate = 0.0;
    bool haveFmt = false;

    /* Walk the chunks. Anything unrecognised is skipped by its own length,
     * which is the point of the format - a file with a LIST or a cue in it is
     * perfectly ordinary and must not stop this. */
    for (;;) {
        unsigned char ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;
        const unsigned size = rd32(ch + 4);

        if (std::memcmp(ch, "fmt ", 4) == 0) {
            unsigned char fmt[40];
            const unsigned want = size < sizeof fmt ? size : (unsigned)sizeof fmt;
            if (std::fread(fmt, 1, want, f) != want) break;
            if (size > want) std::fseek(f, (long)(size - want), SEEK_CUR);

            unsigned tag = rd16(fmt);
            channels = (int)rd16(fmt + 2);
            rate     = (double)rd32(fmt + 4);
            bits     = (int)rd16(fmt + 14);
            /* WAVE_FORMAT_EXTENSIBLE puts the real tag in a GUID; its first
             * two bytes are the tag it stands in for. */
            if (tag == 0xfffe && want >= 26) tag = rd16(fmt + 24);
            isFloat = (tag == 3);
            if (tag != 1 && tag != 3) {
                std::fclose(f);
                return Fail::no(err, "compressed WAV files are not supported");
            }
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            if (!haveFmt) { std::fclose(f); return Fail::no(err, "WAV has no format chunk"); }
            if (channels < 1 || rate < 1.0) { std::fclose(f); return Fail::no(err, "WAV header makes no sense"); }
            const int bytes = bits / 8;
            if (bytes < 1 || bytes > 8) { std::fclose(f); return Fail::no(err, "unsupported sample width"); }

            long frames = (long)(size / (unsigned)(bytes * channels));
            const long cap = (long)(rate * (double)maxSeconds);
            bool truncated = false;
            if (frames > cap) { frames = cap; truncated = true; }
            if (frames < 1) { std::fclose(f); return Fail::no(err, "the file has no audio in it"); }

            out->samples.resize((size_t)frames * 2);
            out->frames = (int)frames;
            out->rate   = rate;

            /* A frame at a time through a small buffer rather than the whole
             * file at once: a long file at 32 bits and eight channels is a lot
             * of memory to hold twice. */
            std::vector<unsigned char> row((size_t)(bytes * channels));
            for (long i = 0; i < frames; i++) {
                if (std::fread(&row[0], 1, row.size(), f) != row.size()) {
                    out->frames = (int)i;
                    out->samples.resize((size_t)i * 2);
                    break;
                }
                if (channels == 1) {
                    const float v = readOne(&row[0], bits, isFloat);
                    out->samples[(size_t)i * 2 + 0] = v;
                    out->samples[(size_t)i * 2 + 1] = v;
                } else {
                    /* Two jacks out, so anything wider folds down. */
                    float l = 0.0f, r = 0.0f;
                    for (int c = 0; c < channels; c++) {
                        const float v = readOne(&row[(size_t)(c * bytes)], bits, isFloat);
                        if ((c & 1) == 0) l += v; else r += v;
                    }
                    const float nl = (float)((channels + 1) / 2);
                    const float nr = (float)(channels / 2);
                    out->samples[(size_t)i * 2 + 0] = l / (nl > 0 ? nl : 1.0f);
                    out->samples[(size_t)i * 2 + 1] = r / (nr > 0 ? nr : 1.0f);
                }
            }
            std::fclose(f);
            if (truncated && err) *err = "loaded, but the file was longer than "
                                         "this will hold and was cut short";
            return true;
        } else {
            /* Chunks are word aligned; an odd size is followed by a pad byte. */
            std::fseek(f, (long)(size + (size & 1)), SEEK_CUR);
        }
    }

    std::fclose(f);
    return Fail::no(err, haveFmt ? "WAV has no audio in it" : "not a WAV file");
}

}  /* namespace bs */
