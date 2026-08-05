/*
 * BENCsynth - note allocation
 *
 * Turns key presses into channels. Everything above this is voltages; this is
 * the one place that still thinks in note numbers, and it exists because a
 * polyphonic cable has to be filled from somewhere.
 *
 * Kept apart from the keyboard module that reads it so that the same
 * allocation serves a click on the on-screen keys, a computer keyboard, and -
 * when there is a plugin - a host's MIDI stream, without three copies of the
 * voice-stealing rules.
 *
 * KeyboardState belongs to the audio thread and nothing else touches it.
 * Whoever is playing posts events into the queue below instead; the engine
 * drains them at the top of each block. That is not ceremony - the alternative
 * was the interface thread writing the voice array while the audio thread read
 * and wrote the same fields, which is a data race however small the fields
 * are, and it is also precisely the shape a plugin needs to receive a host's
 * MIDI without a lock in the callback.
 */

#ifndef BS_KEYS_H
#define BS_KEYS_H

#include "bs_module.h"

#include <atomic>
#include <cstdint>

namespace bs {

enum KeyMode {
    KM_POLY = 0,
    KM_MONO,      /* one channel, last note wins, every note retriggers   */
    KM_LEGATO     /* one channel, last note wins, only the first triggers */
};

/* ------------------------------------------------------------------ *
 * Events
 * ------------------------------------------------------------------ */

enum NoteEventKind {
    NE_NOTE_ON = 0,
    NE_NOTE_OFF,
    NE_ALL_OFF,
    NE_SUSTAIN,   /* value 0 or 1        */
    NE_BEND,      /* value -1..1         */
    NE_MOD        /* value  0..1         */
};

struct NoteEvent {
    uint8_t  kind;
    uint8_t  note;
    float    value;
    /* Which frame of the current render call this belongs to, counted from
     * the start of that call. The standalone posts zero - a keystroke has no
     * meaningful position inside a buffer - but a plugin host hands over an
     * event list with exactly this on every entry, and honouring it is the
     * difference between a note landing where it was written and landing
     * wherever the buffer boundary happened to be. */
    uint32_t offset;
};

/* A wait-free ring for one producer and one consumer.
 *
 * Wait-free on the producer is the point: whoever is playing must not be able
 * to block behind a block of audio, and the audio thread must not be able to
 * block behind whoever is playing.
 *
 * Events carry a frame offset within the render call they belong to, and the
 * engine works through them as it works through the buffer rather than
 * applying them all at the top. The rack processes whole blocks, so the
 * granularity that actually lands is one block - 32 frames, under a
 * millisecond - which is as fine as this arrangement can be without splitting
 * the module graph mid-block.
 */
class NoteQueue {
public:
    NoteQueue() : head(0), tail(0) {}

    /* False when the ring is full, which means an event was dropped. With 256
     * slots, a sixty-hertz interface and a thousand-odd blocks a second, that
     * cannot happen short of the audio thread having stopped entirely - in
     * which case a lost note is not the problem. */
    bool push(const NoteEvent &e)
    {
        const unsigned h = head.load(std::memory_order_relaxed);
        const unsigned n = (h + 1u) & MASK;
        if (n == tail.load(std::memory_order_acquire)) return false;
        buf[h] = e;
        head.store(n, std::memory_order_release);
        return true;
    }

    bool pop(NoteEvent *e)
    {
        const unsigned t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        *e = buf[t];
        tail.store((t + 1u) & MASK, std::memory_order_release);
        return true;
    }

private:
    enum { SIZE = 256, MASK = SIZE - 1 };
    NoteEvent buf[SIZE];
    std::atomic<unsigned> head, tail;

    NoteQueue(const NoteQueue &);
    NoteQueue &operator=(const NoteQueue &);
};

struct KeyVoice {
    int      note;     /* -1 when the channel has never been used         */
    float    vel;      /* 0..1                                            */
    bool     gate;
    bool     retrig;   /* set for exactly one block when a note lands      */
    uint64_t stamp;    /* press order, for stealing the oldest             */
};

class KeyboardState {
public:
    KeyboardState() { reset(); }

    void reset()
    {
        for (int i = 0; i < BS_MAX_POLY; i++) {
            v[i].note = -1; v[i].vel = 0.0f; v[i].gate = false;
            v[i].retrig = false; v[i].stamp = 0;
        }
        heldCount = 0;
        clock     = 1;
        poly      = 4;
        mode      = KM_POLY;
        bend      = 0.0f;
        mod       = 0.0f;
        sustain   = false;
        sounding.store(0, std::memory_order_relaxed);
        allocated.store(1, std::memory_order_relaxed);
    }

    /* The one entry point the audio thread uses on an event drained from the
     * queue. Everything else here is called from inside process(). */
    void apply(const NoteEvent &e)
    {
        switch (e.kind) {
        case NE_NOTE_ON:  noteOn(e.note, e.value); break;
        case NE_NOTE_OFF: noteOff(e.note);         break;
        case NE_ALL_OFF:  allNotesOff();           break;
        case NE_SUSTAIN:  setSustain(e.value >= 0.5f); break;
        case NE_BEND:     bend = clampf(e.value, -1.0f, 1.0f); break;
        case NE_MOD:      mod  = clampf(e.value,  0.0f, 1.0f); break;
        default: break;
        }
    }

    /* Published once a block for the interface to read. Two atomics rather
     * than letting the readout walk the voice array, which would put the
     * interface back inside data the audio thread owns - the whole thing this
     * arrangement exists to prevent. */
    void publish()
    {
        int n = 0;
        const int ch = channels();
        for (int i = 0; i < ch; i++) if (v[i].gate) n++;
        sounding.store(n, std::memory_order_relaxed);
        allocated.store(ch, std::memory_order_relaxed);
    }

    std::atomic<int> sounding;
    std::atomic<int> allocated;

    void setPolyphony(int n)
    {
        n = n < 1 ? 1 : (n > BS_MAX_POLY ? BS_MAX_POLY : n);
        if (n == poly) return;
        for (int i = n; i < BS_MAX_POLY; i++) { v[i].gate = false; v[i].note = -1; }
        poly = n;
    }

    int  polyphony() const { return mode == KM_POLY ? poly : 1; }
    int  channels()  const { return polyphony(); }

    void noteOn(int note, float velocity)
    {
        if (note < 0 || note > 127) return;
        pushHeld(note, velocity);

        if (mode != KM_POLY) {
            const bool wasHeld = heldCount > 1;
            v[0].note   = note;
            v[0].vel    = velocity;
            v[0].stamp  = clock++;
            v[0].retrig = !(mode == KM_LEGATO && wasHeld && v[0].gate);
            v[0].gate   = true;
            return;
        }

        /* A key pressed twice before its first press was released - which is
         * what a stuck computer-keyboard repeat looks like - should retrigger
         * the channel already holding it rather than consume a second one. */
        for (int i = 0; i < poly; i++) {
            if (v[i].gate && v[i].note == note) {
                v[i].vel = velocity; v[i].retrig = true; v[i].stamp = clock++;
                return;
            }
        }

        int slot = -1;
        for (int i = 0; i < poly; i++) {
            if (!v[i].gate) { slot = i; break; }
        }
        if (slot < 0) {
            /* Everything is held: take the channel that has been held
             * longest. Stealing the newest instead makes fast passages eat
             * their own last note, which is the more noticeable failure. */
            uint64_t oldest = ~(uint64_t)0;
            slot = 0;
            for (int i = 0; i < poly; i++)
                if (v[i].stamp < oldest) { oldest = v[i].stamp; slot = i; }
        }
        v[slot].note   = note;
        v[slot].vel    = velocity;
        v[slot].gate   = true;
        v[slot].retrig = true;
        v[slot].stamp  = clock++;
    }

    void noteOff(int note)
    {
        removeHeld(note);
        if (sustain) return;

        if (mode != KM_POLY) {
            if (heldCount > 0) {
                /* Fall back to whatever is still down, which is what makes a
                 * trill on a mono synth behave. */
                const int prev = held[heldCount - 1].note;
                v[0].note   = prev;
                v[0].vel    = held[heldCount - 1].vel;
                v[0].retrig = (mode == KM_MONO);
                v[0].stamp  = clock++;
            } else {
                v[0].gate = false;
            }
            return;
        }
        for (int i = 0; i < poly; i++)
            if (v[i].gate && v[i].note == note) { v[i].gate = false; return; }
    }

    void setSustain(bool on)
    {
        sustain = on;
        if (on) return;
        /* Releasing the pedal drops every channel whose key is not still down. */
        for (int i = 0; i < poly; i++) {
            if (!v[i].gate) continue;
            bool down = false;
            for (int h = 0; h < heldCount; h++)
                if (held[h].note == v[i].note) { down = true; break; }
            if (!down) v[i].gate = false;
        }
        if (mode != KM_POLY && heldCount == 0) v[0].gate = false;
    }

    void allNotesOff()
    {
        heldCount = 0;
        for (int i = 0; i < BS_MAX_POLY; i++) { v[i].gate = false; v[i].retrig = false; }
    }

    /* Called by the engine once per block, after every module has had a chance
     * to see the flags. The keyboard module used to do this itself, which was
     * wrong the moment a rack had two of them: the first to run cleared the
     * triggers before the second could read them. */
    void clearRetriggers()
    {
        for (int i = 0; i < BS_MAX_POLY; i++) v[i].retrig = false;
    }

    KeyVoice v[BS_MAX_POLY];
    int      mode;
    float    bend;     /* -1..1, scaled by the module's range knob */
    float    mod;      /*  0..1                                    */

private:
    struct Held { int note; float vel; };

    void pushHeld(int note, float vel)
    {
        removeHeld(note);
        if (heldCount >= 128) return;
        held[heldCount].note = note;
        held[heldCount].vel  = vel;
        heldCount++;
    }

    void removeHeld(int note)
    {
        for (int i = 0; i < heldCount; i++) {
            if (held[i].note != note) continue;
            for (int j = i; j < heldCount - 1; j++) held[j] = held[j + 1];
            heldCount--;
            return;
        }
    }

    Held     held[128];
    int      heldCount;
    uint64_t clock;
    int      poly;
    bool     sustain;
};

} /* namespace bs */

#endif /* BS_KEYS_H */
