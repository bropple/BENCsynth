/*
 * BENCsynth - MIDI in, for the standalone.
 *
 * Until now MIDI only reached this program through a host. A keyboard plugged
 * into the machine running the standalone did nothing at all, which surprises
 * everybody who tries it.
 *
 * No new dependency: ALSA's sequencer on Linux (already linked for audio),
 * winmm on Windows (already linked), CoreMIDI on macOS (a system framework).
 * Nothing vendored, nothing to fetch.
 *
 * Messages arrive on someone else's thread - a callback on two of the three
 * platforms - and go into a ring. The interface drains it once a frame and
 * hands what it finds to the engine, which keeps the engine's note queue at
 * one producer, the thing its whole design rests on. The cost is up to a
 * frame of latency; the alternative is two producers on a queue built for
 * one, and that is not a trade worth making.
 */

#ifndef BS_MIDIIN_H
#define BS_MIDIIN_H

namespace bs {

/* Opens every input port there is. Returns how many were found - zero is not
 * an error, it is a machine with no MIDI on it. */
int  midiInOpen();
void midiInClose();

/* The name of what it is listening to, for the status line. "" if nothing. */
const char *midiInName();

/* Takes the next message, or returns 0 when the ring is empty. `out` gets
 * three bytes; running status and system messages are dropped upstream. */
int  midiInPop(unsigned char *out);

}  /* namespace bs */

#endif /* BS_MIDIIN_H */
