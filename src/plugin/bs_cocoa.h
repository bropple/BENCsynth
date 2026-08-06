/*
 * The macOS half of embedding, which is a different problem from the others.
 *
 * On Windows a window is an HWND - a kernel object - and SetParent puts one
 * process's window inside another's with one call. X11 has XReparentWindow.
 * macOS has neither: an NSView is an Objective-C object in one process's
 * address space, and Apple does not intend view hierarchies to span
 * processes. Pointers do not cross that boundary.
 *
 * So the pixels do. The plugin owns an IOSurface in its own process, shows it
 * as the contents of a CALayer inside the host's view, and the editor writes
 * frames into it - by way of the shared block that already exists, rather than
 * by sharing the surface itself.
 *
 * That last choice is deliberate. Handing an IOSurface to another process
 * means sending a mach port, which means a bootstrap-registered service and a
 * pile of XPC machinery; IOSurfaceLookup by ID would skip it but is deprecated
 * and was always a security hole. A memcpy through shared memory we already
 * have is one copy more and several hundred lines less, and can be made
 * cleverer later without changing either end's interface.
 *
 * Plain C entry points, so the CLAP wrapper stays C++ and never sees an
 * Objective-C type.
 */
#ifndef BS_COCOA_H
#define BS_COCOA_H

#include <cstdint>

namespace bs {

/* Opaque: an NSView, the IOSurface behind it, and the layer showing it. */
struct CocoaView;

/* Creates our view inside the host's. Main thread only, which CLAP guarantees
 * for every gui.* call. Returns null rather than half a view. */
CocoaView *bs_cocoa_attach(void *parentNSView, int w, int h);
void       bs_cocoa_detach(CocoaView *v);
void       bs_cocoa_resize(CocoaView *v, int w, int h);
void       bs_cocoa_set_status(CocoaView *v, const char *text);

/* Locks the surface and hands back where to write. Pixels are BGRA, top row
 * first, `stride` bytes per row - which is not always width * 4, because the
 * kernel aligns rows and a writer that assumes otherwise produces a picture
 * with a diagonal shear in it. Returns null if the surface is unavailable. */
uint8_t *bs_cocoa_lock(CocoaView *v, int *w, int *h, int *stride);

/* Unlocks and tells the layer its contents changed. Without the second part
 * the first frame appears and nothing ever updates. */
void bs_cocoa_unlock(CocoaView *v);

/* A recognisable pattern, so the display path can be proved before anything
 * is asked to feed it. Colour tells you the format is right; the gradient's
 * direction tells you the rows are not upside down. */
void bs_cocoa_test_pattern(CocoaView *v);

} /* namespace bs */

#endif
