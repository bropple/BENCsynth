/*
 * BENCsynth GUI - native open and save dialogs.
 * See bs_filedlg.h for why this is three platform cases and not a library.
 */

/* popen/pclose are POSIX, and the C++ standard alone does not declare them. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "bs_filedlg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using std::snprintf;
using std::strlen;
using std::memset;
using std::system;


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
#if defined(_WIN32)

#include <windows.h>
#include <commdlg.h>

/* The filter is a run of NUL-terminated strings ending in a second NUL, which
 * is why it is assembled by hand rather than with one snprintf. */
static void build_filter(char *filter, size_t cap, const char *desc,
                         const char *ext)
{
    size_t n = 0;
    n += (size_t)snprintf(filter + n, cap - n, "%s (*.%s)", desc, ext);
    filter[n++] = '\0';
    n += (size_t)snprintf(filter + n, cap - n, "*.%s", ext);
    filter[n++] = '\0';
    n += (size_t)snprintf(filter + n, cap - n, "All files (*.*)");
    filter[n++] = '\0';
    n += (size_t)snprintf(filter + n, cap - n, "*.*");
    filter[n++] = '\0';
    filter[n++] = '\0';
}

static void init_ofn(OPENFILENAMEA *ofn, void *owner, const char *title,
                     const char *filter, char *path, size_t pathCap,
                     const char *ext)
{
    memset(ofn, 0, sizeof *ofn);
    ofn->lStructSize = sizeof *ofn;
    ofn->hwndOwner   = (HWND)owner;
    ofn->lpstrFilter = filter;
    ofn->lpstrFile   = path;
    ofn->nMaxFile    = (DWORD)pathCap;
    ofn->lpstrTitle  = title;
    ofn->lpstrDefExt = ext;
    /* NOCHANGEDIR because a file dialog that moves the process's working
     * directory turns every later relative path into a different path. */
    ofn->Flags       = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
}

int bs_save_dialog(void *owner, const char *title, const char *defaultName,
                   const char *filterDesc, const char *ext,
                   char *out, size_t cap)
{
    OPENFILENAMEA ofn;
    char path[1024];
    char filter[128];

    if (out == 0 || cap < 2) return BS_DLG_CANCELLED;

    build_filter(filter, sizeof filter, filterDesc, ext);
    snprintf(path, sizeof path, "%s", defaultName);
    init_ofn(&ofn, owner, title, filter, path, sizeof path, ext);
    ofn.Flags |= OFN_OVERWRITEPROMPT;

    if (!GetSaveFileNameA(&ofn)) return BS_DLG_CANCELLED;

    snprintf(out, cap, "%s", path);
    return BS_DLG_OK;
}

int bs_open_dialog(void *owner, const char *title, const char *startDir,
                   const char *filterDesc, const char *ext,
                   char *out, size_t cap)
{
    OPENFILENAMEA ofn;
    char path[1024];
    char filter[128];

    if (out == 0 || cap < 2) return BS_DLG_CANCELLED;

    build_filter(filter, sizeof filter, filterDesc, ext);
    path[0] = '\0';
    init_ofn(&ofn, owner, title, filter, path, sizeof path, ext);
    ofn.lpstrInitialDir = startDir;
    ofn.Flags |= OFN_FILEMUSTEXIST;

    if (!GetOpenFileNameA(&ofn)) return BS_DLG_CANCELLED;

    snprintf(out, cap, "%s", path);
    return BS_DLG_OK;
}

/* ------------------------------------------------------------------ */
#else

/* Reads one line of output from a command. Returns 0 if the command could not
 * be run or printed nothing - which for these dialogs means cancelled. */
static int read_line(const char *cmd, char *out, size_t cap)
{
    FILE *p = popen(cmd, "r");
    size_t n;

    if (p == 0) return 0;
    if (fgets(out, (int)cap, p) == 0) { pclose(p); return 0; }
    pclose(p);

    n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return n > 0;
}

static int have(const char *tool)
{
    char cmd[64];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
    return system(cmd) == 0;
}

int bs_save_dialog(void *owner, const char *title, const char *defaultName,
                   const char *filterDesc, const char *ext,
                   char *out, size_t cap)
{
    char cmd[1024];

    (void)owner;   /* X11 and Cocoa parent these dialogs themselves */
    if (out == 0 || cap < 2) return BS_DLG_CANCELLED;

#if defined(__APPLE__)
    (void)filterDesc;
    (void)ext;
    /* `choose file name` is the Cocoa save panel. AppleScript raises an error
     * on cancel, so stderr is dropped and an empty read means cancelled. */
    snprintf(cmd, sizeof cmd,
             "osascript -e 'POSIX path of (choose file name with prompt \"%s\" "
             "default name \"%s\")' 2>/dev/null",
             title, defaultName);
    return read_line(cmd, out, cap) ? BS_DLG_OK : BS_DLG_CANCELLED;
#else
    if (have("zenity")) {
        snprintf(cmd, sizeof cmd,
                 "zenity --file-selection --save --confirm-overwrite "
                 "--title='%s' --filename='%s' "
                 "--file-filter='%s | *.%s' --file-filter='All files | *' "
                 "2>/dev/null",
                 title, defaultName, filterDesc, ext);
        return read_line(cmd, out, cap) ? BS_DLG_OK : BS_DLG_CANCELLED;
    }
    if (have("kdialog")) {
        snprintf(cmd, sizeof cmd,
                 "kdialog --title '%s' --getsavefilename '%s' '*.%s|%s' "
                 "2>/dev/null",
                 title, defaultName, ext, filterDesc);
        return read_line(cmd, out, cap) ? BS_DLG_OK : BS_DLG_CANCELLED;
    }
    return BS_DLG_UNAVAILABLE;
#endif
}

int bs_open_dialog(void *owner, const char *title, const char *startDir,
                   const char *filterDesc, const char *ext,
                   char *out, size_t cap)
{
    char cmd[1024];

    (void)owner;
    if (out == 0 || cap < 2) return BS_DLG_CANCELLED;
    if (startDir == 0) startDir = ".";

#if defined(__APPLE__)
    (void)filterDesc;
    /* `of type` takes extensions, so the panel greys out everything else the
     * way a native application's open panel does. */
    snprintf(cmd, sizeof cmd,
             "osascript -e 'POSIX path of (choose file with prompt \"%s\" "
             "of type {\"%s\"} default location POSIX file \"%s\")' "
             "2>/dev/null",
             title, ext, startDir);
    return read_line(cmd, out, cap) ? BS_DLG_OK : BS_DLG_CANCELLED;
#else
    if (have("zenity")) {
        snprintf(cmd, sizeof cmd,
                 "zenity --file-selection --title='%s' --filename='%s/' "
                 "--file-filter='%s | *.%s' --file-filter='All files | *' "
                 "2>/dev/null",
                 title, startDir, filterDesc, ext);
        return read_line(cmd, out, cap) ? BS_DLG_OK : BS_DLG_CANCELLED;
    }
    if (have("kdialog")) {
        snprintf(cmd, sizeof cmd,
                 "kdialog --title '%s' --getopenfilename '%s' '*.%s|%s' "
                 "2>/dev/null",
                 title, startDir, ext, filterDesc);
        return read_line(cmd, out, cap) ? BS_DLG_OK : BS_DLG_CANCELLED;
    }
    return BS_DLG_UNAVAILABLE;
#endif
}

#endif
