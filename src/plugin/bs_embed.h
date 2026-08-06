/*
 * Reparenting the editor's window into a host's.
 *
 * This is its own file for one reason: <windows.h> and raylib cannot be
 * included together. Both define Rectangle, CloseWindow, ShowCursor and
 * several more, and the usual workaround - defining NOGDI and NOUSER before
 * windows.h - removes exactly the USER functions this needs. So raylib stays
 * on one side of the wall, windows.h on the other, and a void* crosses it.
 */
#ifndef BS_EMBED_H
#define BS_EMBED_H

#include <cstdint>

namespace bs {

/* `child` is what raylib's GetWindowHandle() returned - an HWND on Windows.
 * `parent` is the host's handle from clap gui.set_parent. Does nothing where
 * embedding is not implemented, which is every platform but Windows so far. */
void bs_embed_window(void *child, uint64_t parent, int w, int h);

} /* namespace bs */

#endif
