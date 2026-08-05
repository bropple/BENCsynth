/*
 * BENCsynth GUI - native open and save dialogs
 *
 * raylib has no file dialog, and the alternatives are all worse than this
 * file. Bundling one of the single-header dialog libraries would end the claim
 * that raylib is the only third-party dependency, over a feature that is a
 * system call on every platform this targets. Drawing a file browser out of
 * the widget set would work and look right, but it would be a home-made file
 * browser: no network shares, no sidebar, none of the places the operating
 * system already knows a person keeps things.
 *
 * So each platform gets the dialog it already has, and nothing is linked that
 * the system does not already ship:
 *
 *   Windows   GetSaveFileName / GetOpenFileName from comdlg32, part of the OS
 *   macOS     the Cocoa panels, reached through osascript
 *   Unix      zenity or kdialog, whichever is installed
 *
 * The Unix case is the only one that can come up empty, so the return value
 * distinguishes "the user said no" from "there is nothing here to ask with" -
 * a caller that cannot tell those apart either loses the file or nags about a
 * cancellation the person meant.
 *
 * Lifted from BENCmouth, which worked this out first.
 */

#ifndef BS_FILEDLG_H
#define BS_FILEDLG_H

#include <stddef.h>

enum {
    BS_DLG_CANCELLED   = 0,   /* the user dismissed it                     */
    BS_DLG_OK          = 1,   /* `out` holds a path                        */
    BS_DLG_UNAVAILABLE = -1   /* no dialog on this machine; caller decides  */
};

/* `owner` is the native window handle to parent the dialog to - raylib's
 * GetWindowHandle(), or null. Passed in rather than fetched here so this file
 * needs no raylib. `ext` has no leading dot. */
int bs_save_dialog(void *owner, const char *title, const char *defaultName,
                   const char *filterDesc, const char *ext,
                   char *out, size_t cap);

int bs_open_dialog(void *owner, const char *title, const char *startDir,
                   const char *filterDesc, const char *ext,
                   char *out, size_t cap);

#endif /* BS_FILEDLG_H */
