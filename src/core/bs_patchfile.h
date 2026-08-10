/*
 * BENCsynth - saving and loading a rack
 *
 * A plain text format, one line per module and one per cable. Text because a
 * patch is small, because a diff of two of them is readable, and because a
 * binary format would need a version negotiation the first time a module
 * gained a knob - here, a module that has grown one simply finds fewer values
 * on the line than it has parameters and leaves the rest at their defaults.
 *
 * The string forms are the real ones and the file forms wrap them. That is not
 * symmetry for its own sake: a plugin host does not hand a plugin a path, it
 * hands it a blob and expects one back, and a serialiser that can only reach a
 * disk cannot be saved inside somebody's song. Keeping the file out of it is
 * what lets the same code serve both.
 */

#ifndef BS_PATCHFILE_H
#define BS_PATCHFILE_H

#include "bs_engine.h"

#include <string>

/* The extension a rack is saved with, without the dot. */
#define BS_PATCH_EXT  "bencsynth"
#define BS_PATCH_DESC "BENCsynth rack"

/* The whole rack as text. Cannot fail: an empty rack is an empty document. */
std::string bs_patch_to_string(bs::Engine *eng);

/* Replaces whatever is loaded. Returns nonzero on success and fills `status`
 * with a line fit to show a person - which for a failure is the reason, not
 * "error". */
int bs_patch_from_string(bs::Engine *eng, const char *text,
                         char *status, int cap);

/* The same, through a file.
 *
 * A saved file carries the audio its rack refers to, appended after the text
 * and a NUL: the loader in every version ever shipped parses the text as a C
 * string and stops at the NUL, so an old build opens a new file and simply
 * has no audio in it. Loading restores any sample whose path no longer opens
 * into the user sample folder - which is what makes a .bencsynth something
 * you can hand to somebody. */
int bs_patch_save(bs::Engine *eng, const char *path, char *status, int cap);
int bs_patch_load(bs::Engine *eng, const char *path, char *status, int cap);

/* The audio-embedding machinery those two and the plugin's state share.
 *
 * bs_embed_audio walks the rack TEXT for X records naming files, and appends
 * "BSAU" u32 count, then { u32 pathLen, path, u64 dataLen, data } per file,
 * to `out`. Anything that does not open, or does not start "RIFF", is simply
 * not embedded. Returns how many files it carried, capped at `budget` bytes
 * of audio in total.
 *
 * bs_restore_audio is the other end: any SAMPLE module whose path did not
 * open gets its file written into userSampleDir() and reloaded from there.
 * Returns how many it brought back. */
int bs_embed_audio(const std::string &rackText, std::string &out,
                   unsigned long long budget);
int bs_restore_audio(bs::Engine *eng, const char *p, size_t n);

#endif /* BS_PATCHFILE_H */
