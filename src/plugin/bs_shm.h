/*
 * The shared block between a plugin instance and its editor process.
 *
 * The editor is a separate process rather than a thread, and that is not
 * squeamishness about locking - it is forced. raylib keeps its window, GL
 * context, input and timing in one file-scope `CoreData CORE` (rcore.c:373),
 * so a process gets exactly one raylib window no matter how many threads it
 * has. Two plugin instances in one project would fight over that single
 * global. Two processes have two of them by construction, which is also how
 * LMMS hosts VST2 and why it survives plugins that crash.
 *
 * Three channels, because the two directions have different costs:
 *
 *   edit    editor -> plugin, structural. A module or cable appeared or went
 *           away, so the plugin has to rebuild - which allocates, so it
 *           happens on the main thread, never in process().
 *
 *   param   editor -> plugin, knob values. Applied in place every block: no
 *           allocation, no rebuild, no voices cut off. This is the channel
 *           that makes turning a knob not sound like reloading a patch.
 *
 *   snap    editor -> plugin, the whole rack as text, published on any change.
 *           Never rebuilt from - it is what the host saves, so module
 *           positions and knob values survive a project reload even though the
 *           plugin never rebuilt for them.
 *
 * and one going back:
 *
 *   host    plugin -> editor. The host changed the rack underneath us - a
 *           preset was selected, or a project was loaded - and the editor has
 *           to catch up.
 *
 * Every channel is a seqlock: the writer bumps the sequence to odd, writes,
 * bumps it to even. A reader that sees an odd sequence, or a different one
 * after reading, tries again next frame. There is no lock anywhere, so a stuck
 * or killed editor can never block the audio thread - the worst it can do is
 * stop publishing.
 */
#ifndef BS_SHM_H
#define BS_SHM_H

#include <atomic>
#include <cstdint>
#include <cstring>

namespace bs {

static const uint32_t BS_SHM_MAGIC     = 0x42534831u;   /* "BSH1" */
/* 2: learnMacro. A mismatched pair refuses to attach rather than reading the
 * block at the wrong offsets, which is the whole reason this number exists. */
static const uint32_t BS_SHM_VERSION   = 2u;
static const uint32_t BS_SHM_RACK_MAX  = 192u * 1024u;  /* a rack as text     */
static const uint32_t BS_SHM_PARAM_MAX = 4096u;         /* knobs in one rack  */
static const uint32_t BS_SHM_NOTE_MAX  = 256u;          /* must be a power of 2 */

/* The framebuffer, for platforms where embedding means shipping pixels rather
 * than reparenting a window - which is macOS, and only macOS. Big enough for a
 * rack window somebody has dragged large, and no bigger: this is committed per
 * open editor, and the pages are only touched as they are used. */
static const uint32_t BS_SHM_FB_MAX_W = 2048u;
static const uint32_t BS_SHM_FB_MAX_H = 1280u;
static const uint32_t BS_SHM_FB_BYTES = BS_SHM_FB_MAX_W * BS_SHM_FB_MAX_H * 4u;

/* Input, travelling the opposite way to everything else on this channel: the
 * host's window has the pointer and the keyboard, and the editor - which has
 * no window at all when it is drawing offscreen - has the interface those
 * belong to. Every click on a cable arrives this way.
 *
 * Coordinates are in the editor's pixels, top-left origin, already converted
 * from Cocoa's bottom-left by the view that received them. */
enum ShmInputKind {
    SI_MOUSE_DOWN = 0,
    SI_MOUSE_UP,
    SI_MOUSE_MOVE,
    SI_WHEEL,
    SI_KEY_DOWN,
    SI_KEY_UP,
    SI_TEXT           /* a character, already through the keyboard layout */
};

struct ShmInput {
    uint8_t kind;
    uint8_t button;   /* 0 left, 1 right, 2 middle */
    int16_t x, y;
    float   value;    /* wheel delta, or a key code, or a character */
};

static const uint32_t BS_SHM_INPUT_MAX = 512u;   /* power of two */

/* A key pressed in the editor window. The editor makes no sound - it has no
 * audio device - so the only way its on-screen keyboard and musical typing can
 * be heard is to hand the events to the plugin, which does. Without this they
 * pile up in the editor's own event queue and are never drained by anything. */
struct ShmNote {
    uint8_t kind;      /* bs::NoteEventKind */
    uint8_t note;
    float   value;
};

struct ShmChannel {
    std::atomic<uint32_t> seq;      /* odd while being written */
    uint32_t              len;
    char                  text[BS_SHM_RACK_MAX];
};

struct ShmBlock {
    uint32_t magic;
    uint32_t version;

    ShmChannel edit;    /* editor -> plugin, structural: rebuild            */
    ShmChannel snap;    /* editor -> plugin, whole rack: for state save     */
    ShmChannel host;    /* plugin -> editor, the host changed the rack      */

    /* Knob values, flattened in module-slot order. Only meaningful when
     * structSeq matches the edit sequence the reader has actually applied -
     * otherwise the two sides disagree about which knob is which. */
    std::atomic<uint32_t> paramSeq;
    std::atomic<uint32_t> paramStructSeq;
    uint32_t              paramCount;
    float                 params[BS_SHM_PARAM_MAX];

    /* Notes played in the editor, on their way to the plugin that can sound
     * them. Single producer, single consumer, so two plain indices are enough
     * and no lock is needed anywhere near the audio thread. */
    std::atomic<uint32_t> noteHead;    /* written by the editor */
    std::atomic<uint32_t> noteTail;    /* written by the plugin */
    ShmNote               notes[BS_SHM_NOTE_MAX];

    /* Notes the *host* played, on their way to the editor. Without these the
     * editor's keyboard stays dark while the DAW plays a part through it, and
     * its scopes and meters show a rack nothing is going through - the plugin
     * has the audio and the editor has the picture. Same ring discipline as
     * the other direction, opposite ends. */
    std::atomic<uint32_t> hostNoteHead;   /* written by the plugin */
    std::atomic<uint32_t> hostNoteTail;   /* written by the editor */
    ShmNote               hostNotes[BS_SHM_NOTE_MAX];

    /* Numbers only the plugin knows. Plain relaxed atomics rather than a
     * seqlock: each is a single word, they are read once a frame for display,
     * and a reader that catches one an instant late has a status line that is
     * one frame stale. */
    std::atomic<float>    sampleRate;   /* the host's, not the editor's guess */
    std::atomic<float>    load;
    std::atomic<uint32_t> voices;
    std::atomic<uint32_t> voicesMax;

    /* Embedding. Zero means the editor owns a top-level window; otherwise
     * this is the host's native handle (an HWND on Windows, an X11 Window
     * elsewhere) that the editor reparents itself into. The host then owns the
     * geometry, so it also tells us what size to be. */
    /* MIDI learn, which needs a round trip.
     *
     * The knob is in the editor and the MIDI is in the plugin, so neither can
     * do this alone. The editor writes the macro's index here when someone
     * chooses LEARN; the plugin takes the next CC, writes the assignment into
     * the rack, and puts this back to -1. The editor sees the rack come back
     * through the host channel like any other change the plugin made.
     *
     * One integer, because the assignment itself is already an ordinary knob
     * and travels the way every other knob does. */
    std::atomic<int32_t>  learnMacro;

    /* And the answer coming back: the CC the plugin caught, or 0.
     *
     * The plugin does not write the assignment itself. It could - it has the
     * rack - but the editor also publishes every knob every frame, so a frame
     * already in flight would overwrite it and the learn would vanish with
     * nothing said. Two writers, one rack.
     *
     * So the plugin reports what it saw and the editor, which owns the rack,
     * turns the knob. That also makes the plugin's half free: one atomic
     * store on the audio thread, no lock and no main-thread callback for
     * something a person does once. */
    std::atomic<int32_t>  learnCc;

    std::atomic<uint64_t> embedParent;
    std::atomic<uint32_t> wantW;
    std::atomic<uint32_t> wantH;

    /* Pixels, when the editor cannot simply be reparented into the host's
     * window. Written by the editor, read by the plugin, BGRA and top row
     * first - already in the order CoreAnimation wants, so the plugin's copy
     * into the IOSurface is a memcpy per row rather than a per-pixel loop on
     * the main thread.
     *
     * A seqlock like the others: odd while a frame is going in. A reader that
     * arrives mid-write shows the previous frame again, which at sixty frames
     * a second nobody can see. */
    std::atomic<uint32_t> fbMode;    /* editor renders offscreen when nonzero */
    std::atomic<uint32_t> fbSeq;
    uint32_t              fbW, fbH, fbStride;
    unsigned char         fb[BS_SHM_FB_BYTES];

    /* Pointer and keyboard, host -> editor. Same ring discipline as the
     * notes, and generously sized: a fast drag produces a lot of move events
     * and dropping them mid-gesture is a cable that sticks to the pointer. */
    std::atomic<uint32_t> inHead;    /* written by the plugin */
    std::atomic<uint32_t> inTail;    /* written by the editor */
    ShmInput              in[BS_SHM_INPUT_MAX];

    /* The editor bumps this every frame. The plugin uses it to notice an
     * editor that died without saying goodbye. */
    std::atomic<uint32_t> alive;
    /* The plugin sets this to ask the editor to close its window. */
    std::atomic<uint32_t> quit;
};

/* ---- seqlock ---------------------------------------------------------- */

inline void bs_shm_write(ShmChannel *c, const char *data, uint32_t len)
{
    if (len > BS_SHM_RACK_MAX - 1) len = BS_SHM_RACK_MAX - 1;
    const uint32_t s = c->seq.load(std::memory_order_relaxed);
    c->seq.store(s + 1, std::memory_order_release);          /* odd: writing */
    std::atomic_thread_fence(std::memory_order_release);

    std::memcpy(c->text, data, len);
    c->text[len] = '\0';
    c->len = len;

    std::atomic_thread_fence(std::memory_order_release);
    c->seq.store(s + 2, std::memory_order_release);          /* even: stable */
}

/* Returns the sequence read, or 0 if the value was mid-write or changed under
 * us. `out` must have room for BS_SHM_RACK_MAX bytes. */
inline uint32_t bs_shm_read(const ShmChannel *c, char *out, uint32_t *outLen)
{
    const uint32_t before = c->seq.load(std::memory_order_acquire);
    if (before & 1u) return 0;                                /* being written */
    std::atomic_thread_fence(std::memory_order_acquire);

    uint32_t len = c->len;
    if (len > BS_SHM_RACK_MAX - 1) return 0;
    std::memcpy(out, c->text, len);
    out[len] = '\0';

    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t after = c->seq.load(std::memory_order_acquire);
    if (after != before) return 0;                            /* torn */

    *outLen = len;
    return before;
}

/* Returns false when the ring is full, which drops the event rather than
 * blocking - the alternative is an editor that stutters because a plugin is
 * slow, and a dropped note-on is recoverable where a stalled UI is not. */
inline bool bs_shm_push_note(ShmBlock *b, uint8_t kind, uint8_t note, float value)
{
    const uint32_t head = b->noteHead.load(std::memory_order_relaxed);
    const uint32_t tail = b->noteTail.load(std::memory_order_acquire);
    if (head - tail >= BS_SHM_NOTE_MAX) return false;
    ShmNote &n = b->notes[head & (BS_SHM_NOTE_MAX - 1)];
    n.kind  = kind;
    n.note  = note;
    n.value = value;
    b->noteHead.store(head + 1, std::memory_order_release);
    return true;
}

inline bool bs_shm_pop_note(ShmBlock *b, ShmNote *out)
{
    const uint32_t tail = b->noteTail.load(std::memory_order_relaxed);
    if (tail == b->noteHead.load(std::memory_order_acquire)) return false;
    *out = b->notes[tail & (BS_SHM_NOTE_MAX - 1)];
    b->noteTail.store(tail + 1, std::memory_order_release);
    return true;
}

/* The same ring the other way round: what the DAW played, for the editor to
 * mirror. Pushed from the audio thread, so it drops rather than waits. */
inline bool bs_shm_push_host_note(ShmBlock *b, uint8_t kind, uint8_t note, float value)
{
    const uint32_t head = b->hostNoteHead.load(std::memory_order_relaxed);
    const uint32_t tail = b->hostNoteTail.load(std::memory_order_acquire);
    if (head - tail >= BS_SHM_NOTE_MAX) return false;
    ShmNote &n = b->hostNotes[head & (BS_SHM_NOTE_MAX - 1)];
    n.kind  = kind;
    n.note  = note;
    n.value = value;
    b->hostNoteHead.store(head + 1, std::memory_order_release);
    return true;
}

inline bool bs_shm_pop_host_note(ShmBlock *b, ShmNote *out)
{
    const uint32_t tail = b->hostNoteTail.load(std::memory_order_relaxed);
    if (tail == b->hostNoteHead.load(std::memory_order_acquire)) return false;
    *out = b->hostNotes[tail & (BS_SHM_NOTE_MAX - 1)];
    b->hostNoteTail.store(tail + 1, std::memory_order_release);
    return true;
}

inline bool bs_shm_push_input(ShmBlock *b, const ShmInput &e)
{
    const uint32_t head = b->inHead.load(std::memory_order_relaxed);
    const uint32_t tail = b->inTail.load(std::memory_order_acquire);
    if (head - tail >= BS_SHM_INPUT_MAX) return false;
    b->in[head & (BS_SHM_INPUT_MAX - 1)] = e;
    b->inHead.store(head + 1, std::memory_order_release);
    return true;
}

inline bool bs_shm_pop_input(ShmBlock *b, ShmInput *out)
{
    const uint32_t tail = b->inTail.load(std::memory_order_relaxed);
    if (tail == b->inHead.load(std::memory_order_acquire)) return false;
    *out = b->in[tail & (BS_SHM_INPUT_MAX - 1)];
    b->inTail.store(tail + 1, std::memory_order_release);
    return true;
}

/* ---- the mapping itself ------------------------------------------------ */

struct ShmMap {
    ShmBlock *block;
    void     *handle;        /* HANDLE on Windows, unused elsewhere */
    int       fd;            /* POSIX shm fd, -1 elsewhere          */
    char      name[128];
    bool      owner;         /* the creator unlinks it              */
};

/* Creates a fresh block and initialises it. name is generated. */
bool bs_shm_create(ShmMap *m, unsigned long salt);
/* Opens an existing block by name. */
bool bs_shm_open(ShmMap *m, const char *name);
void bs_shm_close(ShmMap *m);

/* Starts the editor process. `exePath` may be null, in which case the usual
 * places are searched - see the implementation. Returns false if nothing could
 * be started, which the caller should report rather than hang waiting for an
 * editor that will never appear. */
/* `offscreen` is passed on the command line rather than read from the block,
 * because the editor has to decide whether to create a visible window before
 * it opens anything - and a flag whose value depends on which of two processes
 * got there first is a flag that works on one machine and not another. */
bool bs_shm_spawn_editor(const char *exePath, const char *shmName,
                         void **procOut, bool offscreen = false);
void bs_shm_wait_editor(void *proc, int millis);
bool bs_shm_editor_running(void *proc);

} /* namespace bs */

#endif
