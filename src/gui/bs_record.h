/*
 * BENCsynth - recording the master bus to a .wav.
 *
 * The audio callback must not open files, allocate, or block, so it does the
 * only thing it can afford: copy the block it just rendered into a ring and
 * carry on. A thread on the other side drains that ring and writes.
 *
 * Same discipline as everything else here - the note queue, the editor's
 * frames, the plugin's parameter channel. If the disk stalls, the ring fills
 * and samples are dropped; the count says how many, and the audio never
 * hesitates. Dropping a sample is a flaw in a recording. Blocking the callback
 * is a click in the monitors, and there is no undo for that.
 */

#ifndef BS_RECORD_H
#define BS_RECORD_H

namespace bs {

/* Opens `path` and starts a writer thread. Returns false if the file cannot
 * be created, which is the only failure worth reporting to a person. */
bool recStart(const char *path, int sampleRate);

/* Finishes the file - drains what is left, then goes back and writes the
 * sizes into the header, which cannot be known until the end. */
void recStop();

bool recActive();

/* Interleaved stereo, straight from the audio callback. Wait-free. */
void recPush(const float *interleaved, int frames);

/* For the interface: how long, and whether the disk ever fell behind. */
double recSeconds();
int    recDropped();

}  /* namespace bs */

#endif /* BS_RECORD_H */
