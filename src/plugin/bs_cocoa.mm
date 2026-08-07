#include "bs_cocoa.h"
#include "bs_log.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>

#include <cstdlib>
#include <cstring>

/* No ARC. The ownership here is small enough to state outright: alloc gives
 * +1, addSubview retains, detach removes and releases our one. */

namespace bs {

struct CocoaView {
    NSView       *view;
    CALayer      *content;      /* shows the surface */
    CATextLayer  *status;
    IOSurfaceRef  surface;
    int           w, h;
    void         *timer;        /* NSTimer, retained */
};

static void makeSurface(CocoaView *c, int w, int h)
{
    if (c->surface) { CFRelease(c->surface); c->surface = 0; }
    if (w <= 0 || h <= 0) return;

    /* BGRA because that is what CoreAnimation composites without converting,
     * and a format it has to convert costs a pass over every pixel of every
     * frame for nothing. */
    const int bpe = 4;
    NSDictionary *props = @{
        (id)kIOSurfaceWidth:           @(w),
        (id)kIOSurfaceHeight:          @(h),
        (id)kIOSurfaceBytesPerElement: @(bpe),
        (id)kIOSurfacePixelFormat:     @((unsigned)'BGRA')
    };
    c->surface = IOSurfaceCreate((CFDictionaryRef)props);
    c->w = w;
    c->h = h;

    if (!c->surface) { bs_log("  cocoa: IOSurfaceCreate failed at %dx%d", w, h); return; }
    bs_log("  cocoa: surface %dx%d, stride %zu", w, h,
           (size_t)IOSurfaceGetBytesPerRow(c->surface));
}

CocoaView *bs_cocoa_attach(void *parentNSView, int w, int h)
{
    if (!parentNSView) { bs_log("  cocoa: no parent view"); return 0; }
    if (![NSThread isMainThread]) {
        /* AppKit is main-thread-only, and a violation here crashes the host
         * rather than us - a bad way to be remembered. */
        bs_log("  cocoa: attach off the main thread, refusing");
        return 0;
    }

    CocoaView *c = (CocoaView *)std::calloc(1, sizeof(CocoaView));
    if (!c) return 0;

    c->view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
    [c->view setWantsLayer:YES];
    [c->view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

    CALayer *root = [c->view layer];
    CGColorRef bg = CGColorCreateGenericRGB(0.043, 0.063, 0.043, 1.0);
    [root setBackgroundColor:bg];
    CGColorRelease(bg);

    /* The surface goes in its own layer rather than on the root, so the status
     * text can sit above it and the background stays visible behind a surface
     * that has not been written to yet. */
    c->content = [CALayer layer];
    [c->content setFrame:CGRectMake(0, 0, w, h)];
    [c->content setAutoresizingMask:(kCALayerWidthSizable | kCALayerHeightSizable)];
    /* Resize rather than stretch-to-fill: the rack has a fixed aspect only by
     * habit, and letterboxing a synthesizer looks like a bug. */
    [c->content setContentsGravity:kCAGravityResize];
    [c->content setContentsScale:[[NSScreen mainScreen] backingScaleFactor]];
    [root addSublayer:c->content];

    c->status = [CATextLayer layer];
    [c->status setString:@"BENCsynth"];
    [c->status setFontSize:13.0];
    [c->status setForegroundColor:CGColorGetConstantColor(kCGColorWhite)];
    [c->status setAlignmentMode:kCAAlignmentCenter];
    [c->status setContentsScale:[[NSScreen mainScreen] backingScaleFactor]];
    [c->status setFrame:CGRectMake(0, (CGFloat)h * 0.5 - 12.0, (CGFloat)w, 24.0)];
    [root addSublayer:c->status];

    makeSurface(c, w, h);

    NSView *host = (NSView *)parentNSView;
    [host addSubview:c->view];
    bs_log("  cocoa: attached a %dx%d view to the host's", w, h);
    return c;
}

} /* namespace bs - the pump needs an Objective-C class at file scope */

/* A timer needs a target, and a target needs a class. This is the whole of it:
 * fire, call back into C, return. */
@interface BsPump : NSObject
{
@public
    void (*fn)(void *);
    void  *ctx;
}
- (void)tick:(NSTimer *)t;
@end

@implementation BsPump
- (void)tick:(NSTimer *)t
{
    (void)t;
    if (fn) fn(ctx);
}
@end

namespace bs {

void bs_cocoa_start_pump(CocoaView *c, void (*fn)(void *), void *ctx, double hz)
{
    if (!c || ![NSThread isMainThread]) return;
    bs_cocoa_stop_pump(c);
    if (hz <= 0.0) return;

    BsPump *p = [[BsPump alloc] init];
    p->fn = fn;
    p->ctx = ctx;

    NSTimer *t = [NSTimer timerWithTimeInterval:(1.0 / hz)
                                         target:p
                                       selector:@selector(tick:)
                                       userInfo:nil
                                        repeats:YES];
    /* Common modes, or the picture freezes the moment somebody holds a menu
     * open or drags the window - the default run loop mode stops during
     * tracking, and a synthesizer that stops redrawing while you use it looks
     * broken in a way that is hard to describe in a bug report. */
    [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
    [p release];                 /* the timer retains its target */
    c->timer = [t retain];
}

void bs_cocoa_stop_pump(CocoaView *c)
{
    if (!c || !c->timer) return;
    NSTimer *t = (NSTimer *)c->timer;
    [t invalidate];
    [t release];
    c->timer = 0;
}

void bs_cocoa_detach(CocoaView *c)
{
    if (!c) return;
    bs_cocoa_stop_pump(c);
    if ([NSThread isMainThread]) {
        [c->view removeFromSuperview];
        [c->view release];
    }
    if (c->surface) CFRelease(c->surface);
    std::free(c);
}

void bs_cocoa_resize(CocoaView *c, int w, int h)
{
    if (!c || ![NSThread isMainThread]) return;
    if (w <= 0 || h <= 0) return;
    [c->view setFrame:NSMakeRect(0, 0, w, h)];
    [c->status setFrame:CGRectMake(0, (CGFloat)h * 0.5 - 12.0, (CGFloat)w, 24.0)];
    if (w != c->w || h != c->h) {
        makeSurface(c, w, h);
        [c->content setContents:nil];
    }
}

void bs_cocoa_set_status(CocoaView *c, const char *s)
{
    if (!c || !s || ![NSThread isMainThread]) return;
    [c->status setString:[NSString stringWithUTF8String:s]];
}

uint8_t *bs_cocoa_lock(CocoaView *c, int *w, int *h, int *stride)
{
    if (!c || !c->surface) return 0;
    if (IOSurfaceLock(c->surface, 0, 0) != kIOReturnSuccess) return 0;
    if (w)      *w      = c->w;
    if (h)      *h      = c->h;
    if (stride) *stride = (int)IOSurfaceGetBytesPerRow(c->surface);
    return (uint8_t *)IOSurfaceGetBaseAddress(c->surface);
}

void bs_cocoa_unlock(CocoaView *c)
{
    if (!c || !c->surface) return;
    IOSurfaceUnlock(c->surface, 0, 0);

    /* Re-setting contents is what tells CoreAnimation the surface changed.
     * Assign it once and the first frame shows forever. */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];   /* no crossfade between frames */
    [c->content setContents:nil];
    [c->content setContents:(id)c->surface];
    [CATransaction commit];
}

void bs_cocoa_test_pattern(CocoaView *c)
{
    int w = 0, h = 0, stride = 0;
    uint8_t *p = bs_cocoa_lock(c, &w, &h, &stride);
    if (!p) { bs_log("  cocoa: could not lock the surface"); return; }

    for (int y = 0; y < h; y++) {
        uint8_t *row = p + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            /* Green rising downward and left to right, with a red bar across
             * the top. If the red is at the bottom the rows are flipped; if
             * the bar is blue the channel order is wrong. Both are mistakes
             * worth being able to see rather than deduce. */
            const uint8_t g = (uint8_t)((y * 255) / (h > 1 ? h - 1 : 1));
            const uint8_t b = (uint8_t)((x * 120) / (w > 1 ? w - 1 : 1));
            const int top = (y < h / 16);
            row[x * 4 + 0] = top ? 0   : b;      /* B */
            row[x * 4 + 1] = top ? 0   : g;      /* G */
            row[x * 4 + 2] = top ? 220 : 20;     /* R */
            row[x * 4 + 3] = 255;                /* A */
        }
    }
    bs_cocoa_unlock(c);
    bs_log("  cocoa: test pattern written (%dx%d stride %d)", w, h, stride);
}

} /* namespace bs */
