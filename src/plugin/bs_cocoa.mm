#include "bs_cocoa.h"
#include "bs_log.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>

#include <cstdlib>
#include <cstring>
#include <ctype.h>

/* No ARC. The ownership here is small enough to state outright: alloc gives
 * +1, addSubview retains, detach removes and releases our one. */

typedef void (*BsInputSink)(void *ctx, int kind, int button, int x, int y, float value);

/* Mirrors bs::ShmInputKind. Kept as plain ints across this boundary so the
 * Objective-C++ side does not need the shared block's header. */
enum { K_DOWN = 0, K_UP, K_MOVE, K_WHEEL, K_KEYDOWN, K_KEYUP, K_TEXT };

namespace bs {

struct CocoaView {
    NSView       *view;
    CALayer      *content;      /* shows the surface */
    CATextLayer  *status;
    IOSurfaceRef  surface;
    int           w, h;
    void         *timer;        /* NSTimer, retained */
    BsInputSink   sink;
    void         *sinkCtx;
};

} /* namespace bs - an Objective-C class cannot be declared inside one */

/* The view that actually receives the pointer.
 *
 * isFlipped is the important one: Cocoa's origin is bottom-left and every
 * interface in this project counts from the top, so flipping here means the
 * coordinates arriving in the editor need no correction and no one has to
 * remember which way up they are.
 *
 * acceptsFirstMouse matters in a DAW specifically - an FX window is often not
 * the frontmost window, and without this the click that focuses it is eaten
 * rather than delivered, so the first grab of a cable does nothing. */
/* raylib's key numbers, restated.
 *
 * This file is the plugin's, and the plugin does not link raylib - the editor
 * does. Rather than pull the whole header in for twenty-six integers, the ones
 * that cross the boundary are written out. They are part of the protocol
 * between the two processes now, so a change in raylib would have to be
 * matched here deliberately, which is the honest situation either way. */
enum {
    KEY_SPACE_ = 32,   KEY_ESCAPE_ = 256, KEY_ENTER_ = 257, KEY_TAB_ = 258,
    KEY_BACKSPACE_ = 259, KEY_DELETE_ = 261,
    KEY_RIGHT_ = 262,  KEY_LEFT_ = 263,   KEY_DOWN_ = 264,  KEY_UP_ = 265,
    KEY_PAGE_UP_ = 266, KEY_PAGE_DOWN_ = 267, KEY_HOME_ = 268, KEY_END_ = 269,
    KEY_F1_ = 290, KEY_F2_ = 291, KEY_F3_ = 292, KEY_F4_ = 293,
    KEY_F5_ = 294, KEY_F6_ = 295, KEY_F7_ = 296, KEY_F8_ = 297,
    KEY_LSHIFT_ = 340, KEY_LCTRL_ = 341, KEY_LALT_ = 342, KEY_LSUPER_ = 343
};

/* macOS virtual key codes to raylib's key numbers.
 *
 * Two halves, because the two kinds of key are identified differently. Letters
 * and digits come from the characters the event produced, uppercased - raylib
 * numbers those as ASCII, so 'Z' is KEY_Z and musical typing works whatever
 * the layout says the key in that position is. Everything else is a physical
 * key with no character, and those are looked up by virtual code.
 *
 * Doing letters by character rather than by position is deliberate: someone on
 * AZERTY pressing the key marked Z should get Z. */
static int bsKeyFromVirtual(unsigned short vk)
{
    switch (vk) {
    case 53:  return KEY_ESCAPE_;
    case 36:  return KEY_ENTER_;
    case 76:  return KEY_ENTER_;        /* keypad enter */
    case 48:  return KEY_TAB_;
    case 51:  return KEY_BACKSPACE_;
    case 117: return KEY_DELETE_;
    case 49:  return KEY_SPACE_;
    case 123: return KEY_LEFT_;
    case 124: return KEY_RIGHT_;
    case 125: return KEY_DOWN_;
    case 126: return KEY_UP_;
    case 115: return KEY_HOME_;
    case 119: return KEY_END_;
    case 116: return KEY_PAGE_UP_;
    case 121: return KEY_PAGE_DOWN_;
    case 122: return KEY_F1_;
    case 120: return KEY_F2_;
    case 99:  return KEY_F3_;
    case 118: return KEY_F4_;
    case 96:  return KEY_F5_;
    case 97:  return KEY_F6_;
    case 98:  return KEY_F7_;
    case 100: return KEY_F8_;
    case 27:  return '-';               /* octave down, as the rack marks it */
    case 24:  return '=';               /* octave up */
    default:  return 0;
    }
}

@interface BsInputView : NSView
{
@public
    bs::CocoaView       *owner;
    NSEventModifierFlags lastFlags;
}
@end

@implementation BsInputView

- (BOOL)isFlipped                          { return YES; }
- (BOOL)acceptsFirstResponder              { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e     { (void)e; return YES; }

- (void)send:(int)kind button:(int)b event:(NSEvent *)e value:(float)v
{
    if (!owner || !owner->sink) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    owner->sink(owner->sinkCtx, kind, b, (int)p.x, (int)p.y, v);
}

- (void)mouseDown:(NSEvent *)e
{
    /* Ask for the keyboard. AppKit will not offer it. */
    [[self window] makeFirstResponder:self];
    [self send:K_DOWN button:0 event:e value:0];
}
- (void)mouseUp:(NSEvent *)e          { [self send:K_UP   button:0 event:e value:0]; }
- (void)rightMouseDown:(NSEvent *)e   { [self send:K_DOWN button:1 event:e value:0]; }
- (void)rightMouseUp:(NSEvent *)e     { [self send:K_UP   button:1 event:e value:0]; }
- (void)otherMouseDown:(NSEvent *)e   { [self send:K_DOWN button:2 event:e value:0]; }
- (void)otherMouseUp:(NSEvent *)e     { [self send:K_UP   button:2 event:e value:0]; }

- (void)mouseMoved:(NSEvent *)e       { [self send:K_MOVE button:0 event:e value:0]; }
- (void)mouseDragged:(NSEvent *)e     { [self send:K_MOVE button:0 event:e value:0]; }
- (void)rightMouseDragged:(NSEvent *)e{ [self send:K_MOVE button:1 event:e value:0]; }
- (void)otherMouseDragged:(NSEvent *)e{ [self send:K_MOVE button:2 event:e value:0]; }

- (void)scrollWheel:(NSEvent *)e
{
    /* Trackpads report in points and a mouse wheel in lines; raylib's callers
     * expect something around one per notch, so the two are brought together
     * rather than one of them being twenty times the other. */
    CGFloat d = [e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) d /= 10.0;
    [self send:K_WHEEL button:0 event:e value:(float)d];
}

/* Tracking, or mouseMoved never arrives - Cocoa only sends it to a view that
 * asked, and a knob that only responds while a button is held is not obviously
 * a missing tracking area. */
/* ---- keys ----
 *
 * The view has to be first responder to receive any of this, and AppKit does
 * not hand that over on a click by itself - so mouseDown asks. Inside a DAW
 * that means the rack takes the keyboard while you are working in it, which is
 * what you want when typing into a scratchpad and what every plugin with a
 * text field does. */
- (void)sendKey:(int)kind code:(int)code
{
    if (!owner || !owner->sink || code <= 0) return;
    owner->sink(owner->sinkCtx, kind, 0, 0, 0, (float)code);
}

- (void)keyDown:(NSEvent *)e
{
    const int vk = bsKeyFromVirtual([e keyCode]);
    if (vk) [self sendKey:K_KEYDOWN code:vk];

    NSString *bare = [e charactersIgnoringModifiers];
    if ([bare length] > 0) {
        const unichar c = [bare characterAtIndex:0];
        if (c < 128 && (isalnum((int)c) || c == '-' || c == '=' ||
                        c == ',' || c == '.' || c == '/' || c == ';' ||
                        c == '\'' || c == '[' || c == ']' || c == '\\')) {
            /* raylib numbers letters and digits by their uppercase ASCII. */
            const int up = (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : (int)c;
            [self sendKey:K_KEYDOWN code:up];
        }
    }

    /* Text is separate from keys: a scratchpad wants what the layout produced,
     * with shift and dead keys already applied, and not while a command
     * shortcut is being pressed. */
    if (!([e modifierFlags] & NSEventModifierFlagCommand)) {
        NSString *typed = [e characters];
        for (NSUInteger i = 0; i < [typed length]; i++) {
            const unichar c = [typed characterAtIndex:i];
            if (c >= 32 && c != 127 && owner && owner->sink)
                owner->sink(owner->sinkCtx, K_TEXT, 0, 0, 0, (float)c);
        }
    }
}

- (void)keyUp:(NSEvent *)e
{
    const int vk = bsKeyFromVirtual([e keyCode]);
    if (vk) [self sendKey:K_KEYUP code:vk];

    NSString *bare = [e charactersIgnoringModifiers];
    if ([bare length] > 0) {
        const unichar c = [bare characterAtIndex:0];
        if (c < 128 && isalnum((int)c)) {
            const int up = (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : (int)c;
            [self sendKey:K_KEYUP code:up];
        }
    }
}

/* Modifiers arrive as a change of state rather than as key events, so the
 * edges have to be worked out from what changed. Ctrl-S in a rack depends on
 * this. */
- (void)flagsChanged:(NSEvent *)e
{
    const NSEventModifierFlags f = [e modifierFlags];
    struct { NSEventModifierFlags bit; int key; } M[] = {
        { NSEventModifierFlagShift,   KEY_LSHIFT_ },
        { NSEventModifierFlagControl, KEY_LCTRL_  },
        { NSEventModifierFlagOption,  KEY_LALT_   },
        { NSEventModifierFlagCommand, KEY_LSUPER_ },
    };
    for (int i = 0; i < 4; i++) {
        const bool now = (f & M[i].bit) != 0;
        const bool was = (lastFlags & M[i].bit) != 0;
        if (now != was) [self sendKey:(now ? K_KEYDOWN : K_KEYUP) code:M[i].key];
    }
    lastFlags = f;
}

- (void)updateTrackingAreas
{
    for (NSTrackingArea *a in [self trackingAreas]) [self removeTrackingArea:a];
    NSTrackingArea *t =
        [[NSTrackingArea alloc] initWithRect:[self bounds]
                                     options:(NSTrackingMouseMoved |
                                              NSTrackingActiveInKeyWindow |
                                              NSTrackingInVisibleRect)
                                       owner:self
                                    userInfo:nil];
    [self addTrackingArea:t];
    [t release];
    [super updateTrackingAreas];
}

@end

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

    BsInputView *iv = [[BsInputView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
    iv->owner = c;
    c->view = iv;
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

void bs_cocoa_set_input_sink(CocoaView *c, BsInputSink fn, void *ctx)
{
    if (!c) return;
    c->sink = fn;
    c->sinkCtx = ctx;
}

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
