#include "bs_cocoa.h"
#include "bs_log.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

/* No ARC: this is compiled as plain Objective-C++ alongside the rest of the
 * plugin, and the ownership here is small enough to state outright. alloc
 * gives us +1; addSubview retains; detach removes and releases our one. */

namespace bs {

void *bs_cocoa_attach(void *parentNSView, int w, int h)
{
    if (!parentNSView) { bs_log("  cocoa: no parent view"); return 0; }
    if (![NSThread isMainThread]) {
        /* AppKit is main-thread-only and a violation here is a crash in the
         * host, not in us - which is a bad way to be remembered. */
        bs_log("  cocoa: attach off the main thread, refusing");
        return 0;
    }

    NSView *host = (NSView *)parentNSView;
    NSRect frame = NSMakeRect(0, 0, w, h);

    NSView *view = [[NSView alloc] initWithFrame:frame];
    [view setWantsLayer:YES];
    [view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

    CALayer *layer = [view layer];
    /* BS_BG from the interface: near-black with a green cast. */
    CGColorRef bg = CGColorCreateGenericRGB(0.043, 0.063, 0.043, 1.0);
    [layer setBackgroundColor:bg];
    CGColorRelease(bg);

    CATextLayer *text = [CATextLayer layer];
    [text setName:@"status"];
    [text setString:@"BENCsynth"];
    [text setFontSize:13.0];
    [text setForegroundColor:CGColorGetConstantColor(kCGColorWhite)];
    [text setAlignmentMode:kCAAlignmentCenter];
    [text setContentsScale:[[NSScreen mainScreen] backingScaleFactor]];
    [text setFrame:CGRectMake(0, (CGFloat)h * 0.5 - 12.0, (CGFloat)w, 24.0)];
    [text setAutoresizingMask:(kCALayerWidthSizable | kCALayerMinYMargin |
                               kCALayerMaxYMargin)];
    [layer addSublayer:text];

    [host addSubview:view];
    bs_log("  cocoa: attached a %dx%d view to the host's", w, h);
    return view;
}

void bs_cocoa_detach(void *v)
{
    if (!v) return;
    if (![NSThread isMainThread]) return;
    NSView *view = (NSView *)v;
    [view removeFromSuperview];
    [view release];
}

void bs_cocoa_resize(void *v, int w, int h)
{
    if (!v || ![NSThread isMainThread]) return;
    NSView *view = (NSView *)v;
    [view setFrame:NSMakeRect(0, 0, w, h)];
}

void bs_cocoa_set_status(void *v, const char *s)
{
    if (!v || !s || ![NSThread isMainThread]) return;
    NSView *view = (NSView *)v;
    for (CALayer *l in [[view layer] sublayers]) {
        if ([[l name] isEqualToString:@"status"]) {
            [(CATextLayer *)l setString:[NSString stringWithUTF8String:s]];
            return;
        }
    }
}

} /* namespace bs */
