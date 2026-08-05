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
 */

#ifndef BS_KEYS_H
#define BS_KEYS_H

#include "bs_module.h"

namespace bs {

enum KeyMode {
    KM_POLY = 0,
    KM_MONO,      /* one channel, last note wins, every note retriggers   */
    KM_LEGATO     /* one channel, last note wins, only the first triggers */
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
    }

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

    /* True while any key is physically down - the on-screen keyboard uses it
     * to decide whether a key draws lit. */
    bool isHeld(int note) const
    {
        for (int i = 0; i < heldCount; i++) if (held[i].note == note) return true;
        return false;
    }

    /* Called by the keyboard module once per block, after it has consumed the
     * retrigger flags. */
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
