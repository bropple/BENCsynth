/*
 * BENCsynth - saving and loading a rack
 *
 * A plain text format, one line per module and one per cable. Text because a
 * patch is small, because a diff of two of them is readable, and because a
 * binary format would need a version negotiation the first time a module
 * gained a knob - here, a module that has grown one simply finds fewer values
 * on the line than it has parameters and leaves the rest at their defaults.
 */

#ifndef BS_PATCHFILE_H
#define BS_PATCHFILE_H

#include "bs_engine.h"

/* Both return nonzero on success and fill `status` with a line fit to show
 * the user - which for a failure is the reason, not "error". */
int bs_patch_save(bs::Engine *eng, const char *path, char *status, int cap);
int bs_patch_load(bs::Engine *eng, const char *path, char *status, int cap);

/* Where slot n lives. Slots rather than a file dialog: a modular is a thing
 * you keep reaching back into, and eight numbered racks answer that better
 * than a file picker, with no platform code behind it. */
const char *bs_patch_slot_path(int slot);

#endif /* BS_PATCHFILE_H */
