/*
 * BENCsynth - one MIDI message, applied.
 *
 * Shared, because there are now two things receiving MIDI - the plugin, from
 * its host, and the standalone, from a cable - and two copies of "note on with
 * velocity zero is a note off" is two places for it to be wrong. The MPE
 * decode in particular is not obvious enough to write twice.
 *
 * The plugin handles the messages it has opinions about (a CC assigned to a
 * macro) before calling this, and everything else lands here.
 */

#ifndef BS_MIDIMSG_H
#define BS_MIDIMSG_H

#include "bs_engine.h"
#include "bs_keys.h"

namespace bs {

/* Told about every event that reached the engine, so the plugin can pass it to
 * its editor - the editor has no audio device and would otherwise show a
 * keyboard that never lights up. May be null. */
typedef void (*MidiMirror)(void *user, int kind, int note, float value);

/* Returns true if the message meant something. `len` may be 0 for a running
 * status message this does not attempt to reassemble - a host and a driver
 * both hand over complete messages. */
bool applyMidi(Engine &eng, MpeState &mpe, const unsigned char *m, int len,
               int atFrame, MidiMirror mirror, void *user);

}  /* namespace bs */

#endif /* BS_MIDIMSG_H */
