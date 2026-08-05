/*
 * BENCsynth - engine
 *
 * The one object a host holds. It owns the rack, the note allocation and the
 * lock that keeps the two threads out of each other's way, and it turns the
 * whole arrangement into two channels of audio.
 *
 * The standalone program and any future plugin differ only in what calls
 * render() and where the note events come from. Nothing below this line knows
 * which it is.
 */

#ifndef BS_ENGINE_H
#define BS_ENGINE_H

#include "bs_patch.h"
#include "bs_keys.h"

#include <mutex>

namespace bs {

class Engine {
public:
    Engine();

    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    float sampleRate() const { return patch.sampleRate(); }

    /* ---- audio thread ---- */

    /* Interleaved stereo, `frames` frames. The block size the rack runs at is
     * fixed and has nothing to do with what the device asks for, so this
     * keeps whatever is left over from the last call. */
    void render(float *interleaved, int frames);

    /* ---- editing, from whichever thread the interface runs on ---- */

    /* Every structural change goes through these rather than through `patch`
     * directly, because each one has to hold the lock the audio callback
     * takes: adding a module reallocates, deleting one frees a buffer an
     * input may still be pointing at, and connecting rewrites a pointer that
     * process() is about to read. Parameter knobs need none of this - a
     * float written while the audio thread reads it is either the old value
     * or the new one, and both are fine. */
    int  addModule(const char *typeId, float x, float y);
    void removeModule(int moduleId);
    int  connect(int src, int srcPort, int dst, int dstPort);
    void disconnect(int cableId);
    void clear();

    /* ---- notes ---- */

    void noteOn(int note, float velocity);
    void noteOff(int note);
    void setBend(float b);          /* -1..1 */
    void setMod(float m);           /*  0..1 */
    void setSustain(bool on);
    void panic();

    /* A complete subtractive voice, patched the way the front of a Moog
     * manual patches one. Something has to be making a sound when the window
     * opens, or the first impression of a modular is an empty cabinet. */
    void buildDefaultPatch();

    Patch         patch;
    KeyboardState keys;

    /* Fraction of the available time the last block took. Read by the status
     * line; approximate, and deliberately not smoothed here. */
    float load;

    std::mutex &graphLock() { return mutex; }

private:
    void refreshOutputs();

    std::mutex mutex;
    std::vector<Module *> outputs;   /* cached, rebuilt when the graph changes */
    unsigned cachedRev;

    float blockL[BS_BLOCK], blockR[BS_BLOCK];
    int   blockPos;                  /* how much of the current block is spent */

    Engine(const Engine &);
    Engine &operator=(const Engine &);
};

} /* namespace bs */

#endif /* BS_ENGINE_H */
