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

struct RackPreset {
    const char *name;    /* menu label                          */
    const char *blurb;   /* one line, for the status area       */
    /* Some racks make a sound with nothing held down - a filter past
     * self-oscillation, a sequence clocked by an LFO, a delay feeding itself.
     * Declared rather than inferred, so the test suite knows which silence is
     * a bug and which is the point. */
    int selfPlaying;
    /* What the rack is doing and what to turn, dropped into a TEXT panel when
     * it is built. A patch cannot explain itself - the cables say what is
     * connected and nothing says why - so this is the only place the intent
     * can live. */
    const char *notes;
};

int               rackPresetCount();
const RackPreset *rackPresetAt(int i);

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

    /* Wait-free. These post to a queue the audio thread drains as it works
     * through the block; nothing here touches the voice array, which belongs
     * to the audio thread. Safe to call from any single thread - the queue has
     * one producer, and in this program that is whoever is playing.
     *
     * `atFrame` is the offset into the next render call the event belongs at.
     * Whoever is playing by hand has no opinion about that and leaves it at
     * zero; a plugin passes what the host said. */
    void noteOn(int note, float velocity, int atFrame = 0);
    void noteOff(int note, int atFrame = 0);
    void setBend(float b, int atFrame = 0);          /* -1..1 */
    void setMod(float m, int atFrame = 0);           /*  0..1 */
    void setSustain(bool on, int atFrame = 0);

    /* Not wait-free: takes the lock, because it resets every module's state
     * as well. It is a button someone presses when things have gone wrong,
     * not something called per note. */
    void panic();

    /* What the interface may read while audio is running: published once a
     * block, and nothing else about the voice allocator is safe to look at. */
    int voicesSounding() const { return keys.sounding.load(std::memory_order_relaxed); }
    int voicesAllocated() const { return keys.allocated.load(std::memory_order_relaxed); }

    /* The macro controls, by index.
     *
     * These write to and read from the knobs of every MACRO module in the rack,
     * rather than keeping a separate copy: a host automating a parameter should
     * move the knob that parameter *is*, and one source of truth cannot
     * disagree with itself. Values are 0..1, which is what every plugin format
     * wants; the module turns that into volts.
     *
     * Safe to call while audio is running, for the same reason turning a knob
     * is: a float written while the audio thread reads it is either the old
     * value or the new one. */
    void  setMacro(int index, float value01);
    float macroValue(int index) const;

    /* A complete subtractive voice, patched the way the front of a Moog manual
     * patches one. Something has to be making a sound when the window opens,
     * or the first impression of a modular is an empty cabinet. Preset 0. */
    void buildDefaultPatch();

    /* Builds one of the factory racks over whatever is there now.
     *
     * They exist because a modular's problem is not that it cannot make a
     * sound, it is that it makes no sound until you already know what you are
     * doing - and the fastest way to learn what a ladder filter's resonance
     * does is to be handed a rack where it is already doing something and
     * turn the knob. Each one is a working instrument and a worked example. */
    void buildPreset(int index);

    Patch         patch;

    /* Audio-thread property. The interface reaches it only through the event
     * queue above and the two published counters. */
    KeyboardState keys;
    NoteQueue     events;

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

    /* One event held back because its offset has not been reached yet. The
     * queue cannot be looked into without taking from it, so the one that does
     * not belong yet waits here. */
    NoteEvent pending;
    bool      havePending;

    Engine(const Engine &);
    Engine &operator=(const Engine &);
};

} /* namespace bs */

#endif /* BS_ENGINE_H */
