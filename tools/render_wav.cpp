/*
 * BENCsynth - offline renderer
 *
 * Plays a short phrase through a rack and writes a WAV. No window, no audio
 * device, no raylib: it drives exactly the objects the synthesizer drives, so
 * what comes out of here is what comes out of the speakers.
 *
 * That makes it the way to check how the thing actually sounds on a machine
 * with no sound card, and the way to hear the effect of a change to the filter
 * without patching anything by hand.
 *
 *   bencsynth-render out.wav
 */

#include "bs_engine.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

static void put32(FILE *f, unsigned v)
{
    std::fputc((int)(v & 0xff), f);
    std::fputc((int)((v >> 8) & 0xff), f);
    std::fputc((int)((v >> 16) & 0xff), f);
    std::fputc((int)((v >> 24) & 0xff), f);
}

static void put16(FILE *f, unsigned v)
{
    std::fputc((int)(v & 0xff), f);
    std::fputc((int)((v >> 8) & 0xff), f);
}

static int writeWav(const char *path, const std::vector<float> &s, int rate)
{
    FILE *f = std::fopen(path, "wb");
    if (!f) return 0;

    const unsigned frames = (unsigned)(s.size() / 2);
    const unsigned data   = frames * 2 * 2;      /* stereo, 16-bit */

    std::fwrite("RIFF", 1, 4, f);  put32(f, 36 + data);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);  put32(f, 16);
    put16(f, 1); put16(f, 2);
    put32(f, (unsigned)rate);
    put32(f, (unsigned)rate * 4);
    put16(f, 4); put16(f, 16);
    std::fwrite("data", 1, 4, f);  put32(f, data);

    for (size_t i = 0; i < s.size(); i++) {
        float v = s[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        put16(f, (unsigned)(short)(v * 32000.0f));
    }
    std::fclose(f);
    return 1;
}

/* Notes as (start, length) in seconds, so the phrase reads as a phrase rather
 * than as a table of sample offsets. */
struct Note { float at, len; int note; float vel; };

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "bencsynth.wav";
    const int RATE = 48000;

    bs::Engine eng;
    eng.init((float)RATE);
    eng.buildDefaultPatch();

    static const Note PHRASE[] = {
        { 0.00f, 0.45f, 45, 1.00f },
        { 0.50f, 0.45f, 52, 0.90f },
        { 1.00f, 0.45f, 57, 0.85f },
        { 1.50f, 1.60f, 60, 0.95f },
        { 1.50f, 1.60f, 64, 0.85f },
        { 1.50f, 1.60f, 67, 0.85f },
        { 1.50f, 1.60f, 71, 0.75f },
        { 3.30f, 0.30f, 72, 1.00f },
        { 3.70f, 0.30f, 74, 0.90f },
        { 4.10f, 1.80f, 76, 1.00f }
    };
    const int N = (int)(sizeof PHRASE / sizeof PHRASE[0]);
    const float SECONDS = 7.5f;

    std::vector<float> buf((size_t)(RATE * (int)(SECONDS + 1.0f)) * 2, 0.0f);

    /* Rendered in small chunks so a note can start or stop between them.
     * Sixty-fourth of a second is finer than any of the note boundaries and
     * coarse enough that the loop is not the cost. */
    const int CHUNK = RATE / 64;
    int done = 0;
    std::vector<char> playing((size_t)N, 0);

    while (done < (int)(SECONDS * RATE)) {
        const float t = (float)done / (float)RATE;
        for (int i = 0; i < N; i++) {
            const int should = (t >= PHRASE[i].at && t < PHRASE[i].at + PHRASE[i].len);
            if (should && !playing[(size_t)i]) {
                eng.noteOn(PHRASE[i].note, PHRASE[i].vel);
                playing[(size_t)i] = 1;
            } else if (!should && playing[(size_t)i]) {
                eng.noteOff(PHRASE[i].note);
                playing[(size_t)i] = 0;
            }
        }
        eng.render(&buf[(size_t)done * 2], CHUNK);
        done += CHUNK;
    }
    buf.resize((size_t)done * 2);

    if (!writeWav(out, buf, RATE)) {
        std::fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }

    float peak = 0.0f;
    double sq = 0.0;
    for (size_t i = 0; i < buf.size(); i++) {
        const float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        if (a > peak) peak = a;
        sq += (double)buf[i] * buf[i];
    }
    std::printf("%s - %.2f s, peak %.3f, rms %.4f\n", out,
                (double)done / RATE, (double)peak,
                buf.empty() ? 0.0 : std::sqrt(sq / (double)buf.size()));
    return 0;
}
