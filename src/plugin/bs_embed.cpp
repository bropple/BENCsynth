#include "bs_embed.h"

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace bs {

void bs_embed_window(void *child, uint64_t parent, int w, int h)
{
#if defined(_WIN32)
    HWND c = (HWND)child;
    HWND p = (HWND)(uintptr_t)parent;
    if (!c || !p) return;

    /* A window created as top-level keeps its caption and border when it is
     * reparented, which inside somebody's FX rack reads as a bug. WS_CHILD
     * drops both; WS_CLIPSIBLINGS keeps it from drawing over the host's own
     * controls. */
    SetWindowLongPtrA(c, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    SetParent(c, p);
    SetWindowPos(c, 0, 0, 0, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
#else
    (void)child; (void)parent; (void)w; (void)h;
#endif
}

} /* namespace bs */
