/*
 * BENCsynth - a file browser drawn in the window.
 *
 * There is already a file dialog, and on Windows and macOS it is the system's
 * own, which is the right thing when it exists. On Linux it shells out to
 * zenity or kdialog, and on a box with neither there is no dialog at all - so
 * SAVE fell back to writing beside the program and LOAD SAMPLE could only say
 * "install zenity". Inside a DAW it is worse: a plugin editor spawning a
 * desktop dialog is at best rude and at worst blocked.
 *
 * So this one is ours. It draws with the same font and the same panels as
 * everything else, works identically on every platform, needs nothing
 * installed, and is fine inside a host because it is just more of the rack's
 * own window.
 *
 * Modal while it is open: bs_ui_blocked keeps the rack underneath from
 * reacting to clicks that are meant for the list.
 */

#ifndef BS_BROWSER_H
#define BS_BROWSER_H

#include "bs_gui.h"

/* Opens at `startDir` (or the home directory if that will not open), showing
 * directories and files whose extension matches `ext` - "wav", or null for
 * everything. `title` is the line across the top. */
void bs_browser_open(const char *startDir, const char *ext, const char *title);

/* The same list with a name to type into, for choosing where to write. The
 * suggestion is what the field starts with; the extension is added if the
 * typed name lacks it. */
void bs_browser_save(const char *startDir, const char *ext, const char *title,
                     const char *suggest);

int  bs_browser_active(void);

/* Draw and handle one frame. Returns 1 when a file has been chosen and its
 * path written to `out`, -1 when cancelled, and 0 while it is still open. */
int  bs_browser_frame(bs_ui *ui, Rectangle screen, char *out, int cap);

#endif /* BS_BROWSER_H */
