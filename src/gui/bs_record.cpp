/*
 * BENCsynth - the recorder. See bs_record.h for why it is shaped this way.
 */

#include "bs_record.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace bs {

namespace {

/* Eight seconds at 48 kHz. Large because the cost of being large is memory
 * nobody notices, and the cost of being small is a dropout the first time a
 * filesystem decides to think about something. */
const size_t RING_FRAMES = 48000 * 8;
const size_t RING_SAMPLES = RING_FRAMES * 2;

struct Rec {
    std::vector<float>  ring;
    std::atomic<size_t> writePos;   /* audio thread writes, disk thread reads */
    std::atomic<size_t> readPos;
    std::atomic<bool>   running;
    std::atomic<int>    dropped;
    std::atomic<long>   framesWritten;

    std::thread  worker;
    std::FILE   *file;
    int          rate;
    std::mutex   startStop;         /* the interface thread only */

    Rec() : writePos(0), readPos(0), running(false), dropped(0),
            framesWritten(0), file(0), rate(48000) {}
};

Rec g;

void put32(std::FILE *f, unsigned v)
{
    const unsigned char b[4] = { (unsigned char)(v),       (unsigned char)(v >> 8),
                                 (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    std::fwrite(b, 1, 4, f);
}
void put16(std::FILE *f, unsigned v)
{
    const unsigned char b[2] = { (unsigned char)(v), (unsigned char)(v >> 8) };
    std::fwrite(b, 1, 2, f);
}

/* Sizes are written twice: zero now, and the real thing at the end. A WAV
 * header cannot say how long the file is until the file stops growing. */
void writeHeader(std::FILE *f, int rate, unsigned dataBytes)
{
    std::fwrite("RIFF", 1, 4, f);  put32(f, 36 + dataBytes);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);  put32(f, 16);
    put16(f, 1); put16(f, 2);                   /* PCM, stereo */
    put32(f, (unsigned)rate);
    put32(f, (unsigned)rate * 4);               /* bytes per second */
    put16(f, 4); put16(f, 16);                  /* block align, bits */
    std::fwrite("data", 1, 4, f);  put32(f, dataBytes);
}

void drain(bool finishing)
{
    const size_t w = g.writePos.load(std::memory_order_acquire);
    size_t r = g.readPos.load(std::memory_order_relaxed);

    /* In chunks, converting to 16-bit on the way. A whole ring's worth of
     * shorts is 1.5 MB, so this is a fixed buffer rather than one per call. */
    static short out[4096];
    size_t n = 0;
    while (r != w) {
        float v = g.ring[r % RING_SAMPLES];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        out[n++] = (short)(v * 32000.0f);
        r++;
        if (n == 4096 || r == w) {
            std::fwrite(out, sizeof(short), n, g.file);
            n = 0;
        }
    }
    g.readPos.store(r, std::memory_order_release);
    if (finishing) std::fflush(g.file);
}

void workerMain()
{
    while (g.running.load(std::memory_order_acquire)) {
        drain(false);
        /* Nothing clever: the ring holds eight seconds and this wakes twenty
         * times a second. */
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    drain(true);
}

}  /* namespace */

bool recStart(const char *path, int sampleRate)
{
    std::lock_guard<std::mutex> lock(g.startStop);
    if (g.running.load()) return true;

    g.file = std::fopen(path, "wb");
    if (!g.file) return false;

    g.rate = sampleRate > 0 ? sampleRate : 48000;
    writeHeader(g.file, g.rate, 0);

    if (g.ring.size() != RING_SAMPLES) g.ring.assign(RING_SAMPLES, 0.0f);
    g.writePos.store(0, std::memory_order_relaxed);
    g.readPos.store(0, std::memory_order_relaxed);
    g.dropped.store(0, std::memory_order_relaxed);
    g.framesWritten.store(0, std::memory_order_relaxed);

    /* Published last: the audio thread starts pushing the moment it is true,
     * and everything it needs has to be in place first. */
    g.running.store(true, std::memory_order_release);
    g.worker = std::thread(workerMain);
    return true;
}

void recStop()
{
    std::lock_guard<std::mutex> lock(g.startStop);
    if (!g.running.load()) return;

    g.running.store(false, std::memory_order_release);
    if (g.worker.joinable()) g.worker.join();

    const unsigned dataBytes =
        (unsigned)(g.framesWritten.load() * 4);   /* stereo, 16-bit */
    std::fseek(g.file, 0, SEEK_SET);
    writeHeader(g.file, g.rate, dataBytes);
    std::fclose(g.file);
    g.file = 0;
}

bool recActive() { return g.running.load(std::memory_order_acquire); }

void recPush(const float *interleaved, int frames)
{
    if (!g.running.load(std::memory_order_acquire)) return;

    const size_t w = g.writePos.load(std::memory_order_relaxed);
    const size_t r = g.readPos.load(std::memory_order_acquire);
    const size_t free = RING_SAMPLES - (w - r);
    const size_t want = (size_t)frames * 2;

    if (want > free) {
        /* The disk is behind. Drop this block rather than overwrite what has
         * not been written yet, and say so - a recording with a gap in it is
         * worth knowing about. */
        g.dropped.fetch_add(frames, std::memory_order_relaxed);
        return;
    }

    for (size_t i = 0; i < want; i++) g.ring[(w + i) % RING_SAMPLES] = interleaved[i];
    g.writePos.store(w + want, std::memory_order_release);
    g.framesWritten.fetch_add(frames, std::memory_order_relaxed);
}

double recSeconds()
{
    return (double)g.framesWritten.load(std::memory_order_relaxed) / (double)g.rate;
}

int recDropped() { return g.dropped.load(std::memory_order_relaxed); }

}  /* namespace bs */
