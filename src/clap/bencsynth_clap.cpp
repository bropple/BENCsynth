/*
 * BENCsynth as a CLAP instrument.
 *
 * The second wrapper around the same core, and shorter than the LV2 because
 * there is no Turtle to write: a CLAP describes itself in C, so the port list
 * and the code that uses it are the same declaration rather than two that have
 * to be kept in agreement by hand.
 *
 * Why CLAP at all, when the LV2 already works: LMMS is the only host that
 * prefers LV2 and it is the host that implements the least of it. CLAP is MIT,
 * needs no SDK licence, and clap-wrapper turns one of these into VST3 and AU -
 * which is how this reaches the DAWs people actually use. See docs/PLUGIN.md.
 *
 * Still headless. But unlike the LV2 this one is not stuck on a single rack:
 * CLAP has a stepped parameter type, so every preset is reachable from the
 * host's own parameter list, and the choice is saved with the project.
 */

#include "bs_engine.h"
#include "bs_patchfile.h"
#include "bs_shm.h"
#include "bs_sync.h"
#include "bs_log.h"
#if defined(__APPLE__)
#  include "bs_cocoa.h"
#endif

#include <clap/clap.h>

#include <atomic>
#include <cstdio>
#include <vector>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#define BS_CLAP_ID "net.ropple.bencsynth"

#include "bs_version.h"
#include "bs_racklib.h"
#include "bs_midimsg.h"

/* Where the .clap itself lives, filled in by entry_init. The editor is a
 * different file and nothing guarantees the two were installed together, so
 * this is a hint rather than an answer - see editorCandidates() in
 * bs_shm.cpp. */
static std::string g_bundleDir;

/* Defined with the rest of the GUI, below: destroy has to reach the editor
 * teardown, and get_extension has to reach the table. */
static void gui_destroy(const clap_plugin_t *p);
static const clap_plugin_gui_t *guiExtension();

struct BencSynthClap;
/* Defined beside on_main_thread, but reached from state loading and from the
 * preset rebuild, both of which happen above it. */
static void tellHostAboutSlots(BencSynthClap *s);

/* Parameters. Eight macros, then the rack selector. The macros exist because a
 * host needs a fixed parameter list before it knows anything about the patch,
 * and a modular's knobs come and go with its modules - what each one does is
 * decided in the rack, by cable, on the MACRO module. */
enum {
    PARAM_MACRO_0 = 0,
    PARAM_PRESET  = bs::BS_MACROS,
    /* Sixteen slots a rack can point at any knob it likes. Their ids never
     * move: a host reads the parameter list once and every automation lane
     * refers to a parameter by id, so a list that renumbered itself as knobs
     * were exposed would leave lanes driving whatever landed in their place.
     * The name changes and the host is told to re-read it; the id does not. */
    PARAM_SLOT_0  = bs::BS_MACROS + 1,
    PARAM_COUNT   = PARAM_SLOT_0 + bs::BS_EXPOSED
};

struct BencSynthClap {
    clap_plugin_t     plugin;   /* must be first - the host holds a pointer to it */
    const clap_host_t *host;

    bs::Engine engine;

    /* The rack the audio thread has been told to switch to, and the one it is
     * actually running. Building a rack allocates, so process() may not do it:
     * it records the request and asks the host for a main-thread callback,
     * which is precisely what request_callback is for. */
    bs::MpeState     mpe;
    std::atomic<int> wantPreset;
    int              havePreset;

    /* Set when a loaded state replaced the rack, so the preset parameter stops
     * claiming to describe what is loaded. */
    bool custom;

    /* The authoritative macro values.
     *
     * Engine::setMacro writes into the MACRO module's knobs and does nothing
     * at all when the rack has no MACRO module - which most racks do not. If
     * get_value read back from the rack, a host would set a parameter to 0.75,
     * read 0.0, and draw automation that visibly does nothing. So the wrapper
     * keeps the value, reports it, and pushes it into the rack whenever there
     * is something there to receive it - including after a rack change, so
     * switching presets does not silently reset eight host parameters. */
    float macro[bs::BS_MACROS];

    std::string saved;          /* scratch for state save, never freed in RT */

    /* ---- the editor, when there is one ---- */
    bs::ShmMap shm;
    void      *editorProc;
    bool       floating;        /* false: reparent into the host's window */
    uint64_t   parentHandle;
    uint32_t   guiW, guiH;
#if defined(__APPLE__)
    bs::CocoaView *cocoaView;   /* our view inside the host's */
#else
    void      *cocoaView;
#endif
    uint32_t   fbSeen;          /* last frame copied to the screen */
    float      rate;            /* the host's, told to the editor */

    /* Told to re-read the parameter list when a slot changes what it points
     * at. Names and text only - never the ids. */
    const clap_host_params_t *hostParams;
    unsigned    exposedRev;     /* patch revision the host was last told about */
    bool       shmOpen;
    bool       guiCreated;
    std::string bundleDir;      /* where this binary lives, to find the editor */

    /* Sequences already consumed, so a repeat does not rebuild for nothing. */
    uint32_t appliedEditSeq;
    uint32_t appliedSnapSeq;
    uint32_t appliedParamSeq;

    /* Set by process() when the editor sent a structural change, which cannot
     * be applied on the audio thread. Cleared by on_main_thread. */
    std::atomic<uint32_t> pendingEdit;

    /* The rack the editor last published in full. This, not the plugin's own
     * serialisation, is what the host saves while an editor is open: the
     * plugin never rebuilds for a knob turn or a module drag, so its own copy
     * would have the right sound and the wrong layout. */
    std::string snapshot;

    /* Scratch, sized once, so nothing on the audio thread allocates. */
    std::vector<char>  textBuf;
    std::vector<float> paramBuf;
};

#if defined(__APPLE__)
/* Every click, drag and scroll the host's view received, straight into the
 * block. Main thread, and nothing here can block: an input path that waits is
 * a pointer that stutters. */
static void sinkInput(void *ctx, int kind, int button, int x, int y, float value)
{
    BencSynthClap *s = (BencSynthClap *)ctx;
    if (!s->shmOpen) return;
    bs::ShmInput e;
    e.kind   = (uint8_t)kind;
    e.button = (uint8_t)button;
    e.x      = (int16_t)x;
    e.y      = (int16_t)y;
    e.value  = value;
    bs::bs_shm_push_input(s->shm.block, e);
}

/* One frame, from the editor's shared buffer into the surface the host is
 * showing. Main thread, on a timer, because nothing in CLAP drives drawing and
 * a CALayer may not be touched anywhere else. */
static void pumpFrame(void *ctx)
{
    BencSynthClap *s = (BencSynthClap *)ctx;
    if (!s->shmOpen || !s->cocoaView) return;
    bs::ShmBlock *b = s->shm.block;

    const uint32_t seq = b->fbSeq.load(std::memory_order_acquire);
    if (seq & 1u) return;                    /* a frame is going in */
    if (seq == s->fbSeen) return;            /* nothing new */
    std::atomic_thread_fence(std::memory_order_acquire);

    const uint32_t w = b->fbW, h = b->fbH, srcStride = b->fbStride;
    if (!w || !h || w > bs::BS_SHM_FB_MAX_W || h > bs::BS_SHM_FB_MAX_H) return;

    int sw = 0, sh = 0, dstStride = 0;
    uint8_t *dst = bs::bs_cocoa_lock(s->cocoaView, &sw, &sh, &dstStride);
    if (!dst) return;

    /* The surface is whatever size the host last asked for and the frame is
     * whatever the editor last managed; during a resize they disagree for a
     * frame or two. Copy the overlap rather than refusing - a stale corner for
     * one frame is invisible, and a black flash is not. */
    const uint32_t rows = (h < (uint32_t)sh) ? h : (uint32_t)sh;
    const uint32_t cols = (w < (uint32_t)sw) ? w : (uint32_t)sw;
    for (uint32_t y = 0; y < rows; y++)
        std::memcpy(dst + (size_t)y * (size_t)dstStride,
                    b->fb + (size_t)y * (size_t)srcStride,
                    (size_t)cols * 4u);

    bs::bs_cocoa_unlock(s->cocoaView);

    /* Re-read: if the editor started another frame while we copied, this one
     * is torn and the next tick will replace it anyway. */
    if (b->fbSeq.load(std::memory_order_acquire) == seq) s->fbSeen = seq;
}
#endif

/* Publishes the plugin's current rack to the editor, which then catches up.
 * Main thread only - it serialises, which allocates. */
static void pushRackToEditor(BencSynthClap *s)
{
    if (!s->shmOpen) return;
    const std::string t = bs_patch_to_string(&s->engine);
    bs::bs_shm_write(&s->shm.block->host, t.c_str(), (uint32_t)t.size());
}

/* Editor -> plugin, on the audio thread. Knob values only: this writes floats
 * into modules that already exist and never allocates. Structural changes are
 * noticed here but handed to the main thread. */
static void pumpEditorRealtime(BencSynthClap *s)
{
    if (!s->shmOpen) return;
    bs::ShmBlock *b = s->shm.block;

    /* Under the graph lock, because this walks the module list.
     *
     * Engine::render takes the same lock, but everything else the audio thread
     * does to the patch was going in unguarded - and the main thread rebuilds
     * the patch on a preset change or a state load, which deletes modules and
     * reallocates the vector holding them. Automating the rack parameter while
     * the editor streams knob values was a use-after-free waiting for a busy
     * enough session. The lock is already on this thread's critical path once
     * per block; these traversals are shorter than a render. */
    std::lock_guard<std::mutex> guard(s->engine.graphLock());

    /* Structure first. Until the rebuild has happened the param vector belongs
     * to a rack this instance does not have, and applying it would write knob
     * values into whatever module happens to occupy that slot. */
    const uint32_t editSeq = b->edit.seq.load(std::memory_order_acquire);
    if (!(editSeq & 1u) && editSeq != s->appliedEditSeq) {
        s->pendingEdit.store(editSeq, std::memory_order_release);
        if (s->host && s->host->request_callback) s->host->request_callback(s->host);
        return;
    }

    /* Keys pressed in the editor window. They arrive with no frame offset -
     * a keystroke has no meaningful position inside a buffer - so they land at
     * the start of this block, which is the soonest honest answer. */
    bs::ShmNote nv;
    while (bs::bs_shm_pop_note(b, &nv)) {
        switch (nv.kind) {
        case bs::NE_NOTE_ON:  s->engine.noteOn(nv.note, nv.value, 0); break;
        case bs::NE_NOTE_OFF: s->engine.noteOff(nv.note, 0);          break;
        case bs::NE_ALL_OFF:  s->engine.panic();                      break;
        case bs::NE_SUSTAIN:  s->engine.setSustain(nv.value >= 0.5f, 0); break;
        case bs::NE_BEND:     s->engine.setBend(nv.value, 0);         break;
        case bs::NE_MOD:      s->engine.setMod(nv.value, 0);          break;
        default: break;
        }
    }

    const uint32_t pseq = b->paramSeq.load(std::memory_order_acquire);
    if (pseq == s->appliedParamSeq) return;
    if (b->paramStructSeq.load(std::memory_order_acquire) != s->appliedEditSeq) return;

    const uint32_t n = b->paramCount;
    if (n == 0 || n > bs::BS_SHM_PARAM_MAX) return;
    if (bs::bs_apply_params(&s->engine, b->params, n))
        s->appliedParamSeq = pseq;
}

/* The knob a slot drives, or null. */
static bs::Param *slotParam(BencSynthClap *s, int slot)
{
    if (slot < 0 || slot >= bs::BS_EXPOSED) return 0;
    const bs::Exposed &e = s->engine.patch.exposed[slot];
    if (e.module < 0) return 0;
    bs::Module *m = s->engine.patch.module(e.module);
    if (!m || e.param < 0 || e.param >= m->paramCount()) return 0;
    return &m->params[(size_t)e.param];
}

static bs::Module *slotModule(BencSynthClap *s, int slot)
{
    if (slot < 0 || slot >= bs::BS_EXPOSED) return 0;
    const bs::Exposed &e = s->engine.patch.exposed[slot];
    return e.module < 0 ? 0 : s->engine.patch.module(e.module);
}

static BencSynthClap *self_of(const clap_plugin_t *p)
{
    return (BencSynthClap *)p->plugin_data;
}

/* ------------------------------------------------------------------ *
 * Audio ports - stereo out, nothing in
 * ------------------------------------------------------------------ */

static uint32_t ap_count(const clap_plugin_t *, bool is_input)
{
    return is_input ? 0 : 1;
}

static bool ap_get(const clap_plugin_t *, uint32_t index, bool is_input,
                   clap_audio_port_info_t *info)
{
    if (is_input || index != 0) return false;
    info->id = 0;
    std::snprintf(info->name, sizeof info->name, "Out");
    info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type     = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t EXT_AUDIO_PORTS = { ap_count, ap_get };

/* ------------------------------------------------------------------ *
 * Note ports - one in, CLAP notes preferred, raw MIDI accepted
 * ------------------------------------------------------------------ */

static uint32_t np_count(const clap_plugin_t *, bool is_input)
{
    return is_input ? 1 : 0;
}

static bool np_get(const clap_plugin_t *, uint32_t index, bool is_input,
                   clap_note_port_info_t *info)
{
    if (!is_input || index != 0) return false;
    info->id = 0;
    /* Both dialects: CLAP notes carry a note id and a floating velocity, which
     * is the better source, but plenty of hosts still send raw MIDI and a
     * plugin that only accepts one of the two is silent in half of them. */
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect  = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof info->name, "MIDI In");
    return true;
}

static const clap_plugin_note_ports_t EXT_NOTE_PORTS = { np_count, np_get };

/* ------------------------------------------------------------------ *
 * Parameters
 * ------------------------------------------------------------------ */

static uint32_t pa_count(const clap_plugin_t *) { return PARAM_COUNT; }

/* Everything the host sees is 0..1, whatever the knob underneath happens to
 * be. A slot that reported the knob's own range would have to tell the host
 * the range changed every time it was reassigned, and hosts vary in how well
 * they take that; a normalised slot never changes shape. */
static float slotToPlain(const bs::Param *p, double v)
{
    return p->lo + (float)v * (p->hi - p->lo);
}

static double slotToNorm(const bs::Param *p, float v)
{
    const float span = p->hi - p->lo;
    return span > 1e-9f ? (double)((v - p->lo) / span) : 0.0;
}

/* The Rack chooser covers the built-in presets and then the racks a person
 * saved. Built-in indices never move, so a project that stored one still finds
 * it; user racks are appended in sorted order after them.
 *
 * A user rack that has been deleted since the project was saved leaves the
 * index pointing at a different name, or past the end. Neither loses any
 * sound: the plugin's state carries the whole rack as text, so what a project
 * reopens with is what it was saved with, and the chooser is only a label. */
static int rackTotal()
{
    return bs::rackPresetCount() + bs::userRackCount();
}

static const char *rackNameAt(int i)
{
    if (i >= 0 && i < bs::rackPresetCount()) {
        const bs::RackPreset *rp = bs::rackPresetAt(i);
        return rp ? rp->name : 0;
    }
    const bs::UserRack *ur = bs::userRackAt(i - bs::rackPresetCount());
    return ur ? ur->name.c_str() : 0;
}

static bool pa_get_info(const clap_plugin_t *p, uint32_t index,
                        clap_param_info_t *info)
{
    if (index >= PARAM_COUNT) return false;
    std::memset(info, 0, sizeof *info);
    info->id     = index;
    info->cookie = 0;

    if (index == PARAM_PRESET) {
        /* Stepped, so hosts draw it as a discrete chooser rather than a
         * continuous knob, and value_to_text names the rack. */
        info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
        std::snprintf(info->name, sizeof info->name, "Rack");
        info->min_value     = 0;
        info->max_value     = rackTotal() - 1;
        info->default_value = 0;
        return true;
    }

    if (index >= (uint32_t)PARAM_SLOT_0) {
        const int slot = (int)index - PARAM_SLOT_0;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        info->min_value     = 0.0;
        info->max_value     = 1.0;
        info->default_value = 0.0;

        /* Named for what it drives, so the host's automation lane says
         * "VCF CUTOFF" rather than "Slot 4". This is the part that changes on
         * reassignment, and the only part. */
        BencSynthClap *s = self_of(p);
        bs::Module *m = slotModule(s, slot);
        const bs::Param *tp = slotParam(s, slot);
        if (m && tp)
            std::snprintf(info->name, sizeof info->name, "%s %s",
                          m->typeId.c_str(), tp->name);
        else
            std::snprintf(info->name, sizeof info->name, "Slot %d", slot + 1);
        return true;
    }

    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof info->name, "Macro %u", index + 1);
    info->min_value     = 0.0;
    info->max_value     = 1.0;
    info->default_value = 0.0;
    return true;
}

static bool pa_get_value(const clap_plugin_t *p, clap_id id, double *out)
{
    BencSynthClap *s = self_of(p);
    if (id == PARAM_PRESET) { *out = s->wantPreset.load(); return true; }
    if (id >= (uint32_t)PARAM_SLOT_0) {
        const bs::Param *tp = slotParam(s, (int)id - PARAM_SLOT_0);
        *out = tp ? slotToNorm(tp, tp->value) : 0.0;
        return true;
    }
    if (id >= bs::BS_MACROS) return false;
    *out = s->macro[id];
    return true;
}

static bool pa_value_to_text(const clap_plugin_t *p, clap_id id, double value,
                             char *out, uint32_t cap)
{
    if (id == PARAM_PRESET) {
        const char *name = rackNameAt((int)(value + 0.5));
        if (self_of(p)->custom)
            std::snprintf(out, cap, "%s (edited)", name ? name : "Custom");
        else
            std::snprintf(out, cap, "%s", name ? name : "?");
        return true;
    }
    if (id >= (uint32_t)PARAM_SLOT_0) {
        /* In the knob's own units, with the knob's own format string - so a
         * cutoff reads "1200 Hz" in the host and not "0.37". */
        const bs::Param *tp = slotParam(self_of(p), (int)id - PARAM_SLOT_0);
        if (!tp) { std::snprintf(out, cap, "-"); return true; }
        std::snprintf(out, cap, tp->fmt ? tp->fmt : "%.2f",
                      (double)slotToPlain(tp, value));
        return true;
    }
    if (id >= bs::BS_MACROS) return false;
    std::snprintf(out, cap, "%.1f %%", value * 100.0);
    return true;
}

static bool pa_text_to_value(const clap_plugin_t *, clap_id id,
                             const char *text, double *out)
{
    if (id == PARAM_PRESET) {
        for (int i = 0; i < rackTotal(); i++) {
            const char *n = rackNameAt(i);
            if (n && std::strcmp(n, text) == 0) { *out = i; return true; }
        }
        return false;
    }
    if (id >= bs::BS_MACROS) return false;
    *out = std::atof(text) / 100.0;
    return true;
}

/* Applying one parameter event. Shared by process() and flush(), because a
 * host may move a knob while the plugin is not processing and expects it to
 * take effect anyway. */
static void applyParam(BencSynthClap *s, clap_id id, double value)
{
    /* Same reasoning as pumpEditorRealtime: setMacro walks every module, and
     * a slot dereferences one by id. */
    std::lock_guard<std::mutex> guard(s->engine.graphLock());
    if (id == PARAM_PRESET) {
        const int want = (int)(value + 0.5);
        if (want != s->wantPreset.load()) {
            s->wantPreset.store(want);
            /* Not built here - process() is the audio thread and building a
             * rack allocates. Ask for a main-thread callback and do it there. */
            if (s->host && s->host->request_callback)
                s->host->request_callback(s->host);
        }
        return;
    }
    if (id >= (uint32_t)PARAM_SLOT_0) {
        /* Straight onto the knob. No allocation, no rebuild - a knob value is
         * a float and the graph does not care that it moved. */
        bs::Param *tp = slotParam(s, (int)id - PARAM_SLOT_0);
        if (tp) tp->value = slotToPlain(tp, value);
        return;
    }
    if (id < bs::BS_MACROS) {
        s->macro[id] = (float)value;
        s->engine.setMacro((int)id, (float)value);
    }
}

static void pa_flush(const clap_plugin_t *p, const clap_input_events_t *in,
                     const clap_output_events_t *)
{
    BencSynthClap *s = self_of(p);
    const uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID &&
            h->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
            applyParam(s, e->param_id, e->value);
        }
    }
}

static const clap_plugin_params_t EXT_PARAMS = {
    pa_count, pa_get_info, pa_get_value, pa_value_to_text, pa_text_to_value, pa_flush
};

/* ------------------------------------------------------------------ *
 * State - the whole rack, as text, inside the host's project
 * ------------------------------------------------------------------ */

static bool st_save(const clap_plugin_t *p, const clap_ostream_t *stream)
{
    BencSynthClap *s = self_of(p);

    /* An open editor owns the layout. The plugin rebuilds only for structural
     * changes, so its own copy has every knob right and every module in
     * whatever position it was last rebuilt at - which is not where the person
     * dragged it to. The snapshot is the editor's whole rack, and it is the
     * honest thing to save. */
    if (s->shmOpen) {
        uint32_t len = 0;
        if (s->textBuf.size() < bs::BS_SHM_RACK_MAX)
            s->textBuf.resize(bs::BS_SHM_RACK_MAX);
        const uint32_t seq = bs::bs_shm_read(&s->shm.block->snap,
                                             s->textBuf.data(), &len);
        if (seq && len) s->snapshot.assign(s->textBuf.data(), len);
    }
    s->saved = s->snapshot.empty() ? bs_patch_to_string(&s->engine) : s->snapshot;

    const char *b = s->saved.c_str();
    uint64_t left = s->saved.size() + 1;   /* including the terminator */
    while (left > 0) {
        /* A short write is normal, not an error - only a negative return is a
         * failure. Treating "wrote some of it" as done truncates the rack. */
        const int64_t n = stream->write(stream, b, left);
        if (n <= 0) return false;
        b += n;
        left -= (uint64_t)n;
    }
    return true;
}

static bool st_load(const clap_plugin_t *p, const clap_istream_t *stream)
{
    BencSynthClap *s = self_of(p);

    std::string text;
    char buf[4096];
    for (;;) {
        const int64_t n = stream->read(stream, buf, sizeof buf);
        if (n < 0) return false;
        if (n == 0) break;                 /* end of stream */
        text.append(buf, (size_t)n);
    }
    if (text.empty()) return false;
    /* The terminator was written; the parser wants a C string and text may or
     * may not still carry it depending on how the host stored the bytes. */
    while (!text.empty() && text.back() == '\0') text.pop_back();

    char status[192] = "";
    if (!bs_patch_from_string(&s->engine, text.c_str(), status, (int)sizeof status))
        return false;

    /* The rack no longer necessarily corresponds to any preset. */
    s->custom = true;

    /* The loaded rack carries its own macro knob positions, and those are now
     * the truth - the host will ask for them and should be told what it just
     * restored, not what was set before. */
    for (int i = 0; i < bs::BS_MACROS; i++) s->macro[i] = s->engine.macroValue(i);
    s->snapshot = text;
    pushRackToEditor(s);
    /* A restored rack brings its own assignments, so the names the host is
     * showing are for whatever was loaded before this one. */
    tellHostAboutSlots(s);
    return true;
}

static const clap_plugin_state_t EXT_STATE = { st_save, st_load };

/* ------------------------------------------------------------------ *
 * Lifecycle and audio
 * ------------------------------------------------------------------ */

static bool pl_init(const clap_plugin_t *) { return true; }

static void pl_destroy(const clap_plugin_t *p)
{
    BencSynthClap *s = self_of(p);
    /* A host is entitled to destroy an instance without closing the editor
     * first, and an orphaned editor holding a mapping of freed memory is a
     * crash with someone's session attached to it. */
    if (s->guiCreated) gui_destroy(p);
    delete s;
}



static bool pl_activate(const clap_plugin_t *p, double sample_rate,
                        uint32_t, uint32_t)
{
    BencSynthClap *s = self_of(p);

    /* setSampleRate, not init.
     *
     * Engine::init clears the patch. Activation happens AFTER the host has
     * loaded its project state in most hosts, so initialising here threw away
     * the rack that had just been restored and rebuilt nothing in its place -
     * the project reopened silent, and state.save afterwards still returned
     * the rack, so the file looked intact. The same happened on any sample
     * rate change or freeze/unfreeze, permanently.
     *
     * The plugin's own test missed it by activating before loading, which is
     * the one order that works. */
    s->rate = (float)sample_rate;
    if (s->shmOpen)
        s->shm.block->sampleRate.store(s->rate, std::memory_order_relaxed);

    if (s->engine.patch.slotCount() == 0) {
        /* Nothing loaded yet - a fresh instance the host has not restored. */
        s->engine.init((float)sample_rate);
        s->engine.buildPreset(s->havePreset);
    } else {
        s->engine.setSampleRate((float)sample_rate);
    }
    return true;
}

static void pl_deactivate(const clap_plugin_t *) {}
static bool pl_start_processing(const clap_plugin_t *) { return true; }
static void pl_stop_processing(const clap_plugin_t *) {}

static void pl_reset(const clap_plugin_t *p)
{
    self_of(p)->engine.panic();
}

/* Everything the host played, mirrored to the editor so its keyboard lights up
 * and its rack shows something happening. Only host events go through here -
 * events that arrived *from* the editor are applied directly and never echoed,
 * or a held key would bounce between the two processes forever. */
static inline void mirror(BencSynthClap *s, uint8_t kind, uint8_t note, float v)
{
    if (s->shmOpen) bs::bs_shm_push_host_note(s->shm.block, kind, note, v);
}

/* The shared MIDI decoder reports what it did; the editor needs to know so its
 * keyboard lights up, since it has no audio device of its own. */
static void mirrorThunk(void *user, int kind, int note, float value)
{
    mirror((BencSynthClap *)user, kind, (uint8_t)note, value);
}

/* One event, already known to be due. */
static void handleEvent(BencSynthClap *s, const clap_event_header_t *h, int at,
                        const clap_output_events_t *out)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;

    switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
        const clap_event_note_t *e = (const clap_event_note_t *)h;
        if (e->key >= 0) {
            s->engine.noteOn(e->key, (float)e->velocity, at);
            mirror(s, bs::NE_NOTE_ON, (uint8_t)e->key, (float)e->velocity);
        }
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        const clap_event_note_t *e = (const clap_event_note_t *)h;
        /* key -1 is a wildcard: every note on the port. */
        if (e->key < 0) { s->engine.panic(); mirror(s, bs::NE_ALL_OFF, 0, 0.0f); }
        else { s->engine.noteOff(e->key, at);
               mirror(s, bs::NE_NOTE_OFF, (uint8_t)e->key, 0.0f); }
        break;
    }
    /* Per-note expression - what a Seaboard, a Linnstrument or a Push sends,
     * and what MPE exists to carry. CLAP hands it over directly rather than
     * as a channel-per-note convention, so there is nothing to decode.
     *
     * Aimed at a note, not the keyboard: two fingers bending in opposite
     * directions is the whole point, and a voltage model handles it without
     * anything special, because a cable already carries all eight voices. */
    case CLAP_EVENT_NOTE_EXPRESSION: {
        const clap_event_note_expression_t *e =
            (const clap_event_note_expression_t *)h;
        if (e->key < 0) break;
        switch (e->expression_id) {
        case CLAP_NOTE_EXPRESSION_TUNING:
            /* Already in semitones, and can exceed an octave on a controller
             * that allows it. */
            s->engine.noteExpression(e->key, 0, (float)e->value, at);
            break;
        case CLAP_NOTE_EXPRESSION_PRESSURE:
            s->engine.noteExpression(e->key, 1, (float)e->value, at);
            break;
        case CLAP_NOTE_EXPRESSION_BRIGHTNESS:
            s->engine.noteExpression(e->key, 2, (float)e->value, at);
            break;
        default: break;   /* volume, pan, vibrato, expression: not ours yet */
        }
        break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
        const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
        applyParam(s, e->param_id, e->value);
        break;
    }
    case CLAP_EVENT_MIDI: {
        const clap_event_midi_t *e = (const clap_event_midi_t *)h;
        const uint8_t *m = e->data;

        /* One case this decides for itself, before the shared decoder sees
         * it: a MACRO panel can claim a run of eight CCs, which is how a
         * controller's knob row ends up driving a whole rack. If a CC has
         * been assigned, that is what it does, and the fixed meanings do not
         * also fire. */
        if ((m[0] & 0xf0) == 0xb0) {
            /* Learning takes precedence over playing: the CC that gets
             * assigned should not also move whatever it used to move. */
            if (s->shmOpen) {
                const int learn =
                    s->shm.block->learnMacro.load(std::memory_order_acquire);
                if (learn >= 0 && learn < bs::BS_MACROS && m[1] > 0) {
                    /* Reported, not applied. The editor owns the rack and
                     * turns the knob; see learnCc in bs_shm.h. */
                    s->shm.block->learnCc.store((int32_t)m[1],
                                                std::memory_order_release);
                    break;
                }
            }

            const int which = s->engine.macroForCc(m[1]);
            if (which >= 0) {
                const double v = (double)m[2] / 127.0;
                applyParam(s, (clap_id)which, v);
                s->macro[which] = (float)v;

                /* And tell the host, by putting the change in the output
                 * event list where a host reads its own automation back from.
                 * Without this the hardware moves the sound and the host's
                 * knob stays where it was - so the project saves the old
                 * value, and the next automation point snaps it back. */
                if (out) {
                    clap_event_param_value_t pe;
                    std::memset(&pe, 0, sizeof pe);
                    pe.header.size     = sizeof pe;
                    pe.header.time     = (uint32_t)(at < 0 ? 0 : at);
                    pe.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    pe.header.type     = CLAP_EVENT_PARAM_VALUE;
                    pe.header.flags    = 0;
                    pe.param_id        = (clap_id)which;
                    pe.cookie          = 0;
                    pe.note_id = -1; pe.port_index = -1;
                    pe.channel = -1; pe.key = -1;
                    pe.value           = v;
                    out->try_push(out, &pe.header);
                }
                break;
            }
        }

        bs::applyMidi(s->engine, s->mpe, m, 3, at, mirrorThunk, s);
        break;
    }
    default:
        break;
    }
}

static clap_process_status pl_process(const clap_plugin_t *p,
                                      const clap_process_t *proc)
{
    BencSynthClap *s = self_of(p);
    if (!proc || proc->audio_outputs_count < 1) return CLAP_PROCESS_ERROR;

    float *outL = proc->audio_outputs[0].data32[0];
    float *outR = proc->audio_outputs[0].data32[1];
    if (!outL || !outR) return CLAP_PROCESS_ERROR;

    /* The host's tempo, before anything reads it. Handed over every block and
     * previously thrown away - which is why every clocked rack drifted against
     * the session within a few bars. */
    if (proc->transport && (proc->transport->flags & CLAP_TRANSPORT_HAS_TEMPO)) {
        s->engine.patch.transport.bpm = (float)proc->transport->tempo;
        s->engine.patch.transport.playing =
            (proc->transport->flags & CLAP_TRANSPORT_IS_PLAYING) ? 1 : 0;
    } else {
        /* No transport is not the same as a tempo of zero: it means every
         * module should fall back to its own knob. */
        s->engine.patch.transport.bpm = 0.0f;
        s->engine.patch.transport.playing = 0;
    }

    /* Anything the editor changed since the last block, applied before a
     * sample is produced so a knob turn lands where it was made. */
    pumpEditorRealtime(s);

    const uint32_t nframes = proc->frames_count;

    /* Events are fed in as the buffer is walked, each rebased onto the chunk
     * that contains it. Pushing them all up front instead is wrong in a way
     * that sounds almost right: the engine's offsets are relative to one
     * render() call, this renders in chunks because de-interleaving needs
     * somewhere to put the result and process() must not allocate, so an event
     * at frame 2048 would be compared against a chunk that only reaches 256,
     * never come due, and fire at the start of the next one. That bug shipped
     * in the LV2 before a test caught it; the shape here is the fix. */
    float tmp[512];
    const uint32_t CHUNK = (uint32_t)(sizeof tmp / (2 * sizeof tmp[0]));

    const clap_input_events_t *in = proc->in_events;
    const uint32_t evCount = in ? in->size(in) : 0;
    uint32_t evIndex = 0;

    uint32_t done = 0;
    while (done < nframes) {
        uint32_t n = nframes - done;
        if (n > CHUNK) n = CHUNK;

        while (evIndex < evCount) {
            const clap_event_header_t *h = in->get(in, evIndex);
            if (!h || h->time >= done + n) break;
            /* An event stamped before where the buffer has reached is late
             * rather than invalid - it lands at the start of the chunk. */
            const int64_t rel = (int64_t)h->time - (int64_t)done;
            handleEvent(s, h, rel < 0 ? 0 : (int)rel, proc->out_events);
            evIndex++;
        }

        s->engine.render(tmp, (int)n);
        for (uint32_t i = 0; i < n; i++) {
            outL[done + i] = tmp[i * 2 + 0];
            outR[done + i] = tmp[i * 2 + 1];
        }
        done += n;
    }

    /* What only this side knows. Once per block, and never read here. */
    if (s->shmOpen) {
        bs::ShmBlock *sb = s->shm.block;
        sb->load.store(s->engine.load, std::memory_order_relaxed);
        sb->voices.store((uint32_t)s->engine.voicesSounding(), std::memory_order_relaxed);
        sb->voicesMax.store((uint32_t)s->engine.voicesAllocated(), std::memory_order_relaxed);
    }

    /* Never SLEEP: a rack can be self-playing - a filter past self-oscillation,
     * a sequence clocked by an LFO, a delay feeding itself - so silence now
     * says nothing about silence later. */
    return CLAP_PROCESS_CONTINUE;
}

/* The main thread, where allocating is allowed. */
/* Has the rack changed which knobs it offers? The patch bumps its revision
 * when a slot is assigned or freed, and the editor's edits arrive here as a
 * rebuild - so this is checked wherever the main thread has just touched the
 * rack, and the host is asked to re-read names and text. Never the list: the
 * ids are the same sixteen they always were. */
static void tellHostAboutSlots(BencSynthClap *s)
{
    const unsigned rev = s->engine.patch.revision();
    if (rev == s->exposedRev) return;
    s->exposedRev = rev;
    if (s->hostParams && s->hostParams->rescan)
        s->hostParams->rescan(s->host,
                              CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT);
}

static void pl_on_main_thread(const clap_plugin_t *p)
{
    BencSynthClap *s = self_of(p);


    /* A structural change from the editor: a module or a cable appeared or
     * went away, so the rack has to be rebuilt. This allocates, which is why
     * process() only ever asked for this callback rather than doing it. */
    const uint32_t pend = s->pendingEdit.exchange(0, std::memory_order_acq_rel);
    if (pend && s->shmOpen) {
        if (s->textBuf.size() < bs::BS_SHM_RACK_MAX)
            s->textBuf.resize(bs::BS_SHM_RACK_MAX);
        uint32_t len = 0;
        const uint32_t seq = bs::bs_shm_read(&s->shm.block->edit,
                                             s->textBuf.data(), &len);
        if (seq && len) {
            char status[192] = "";
            if (bs_patch_from_string(&s->engine, s->textBuf.data(), status,
                                     (int)sizeof status)) {
                s->appliedEditSeq  = seq;
                /* The values that arrive next belong to this structure. */
                s->appliedParamSeq = 0;
                s->custom          = true;
                for (int i = 0; i < bs::BS_MACROS; i++)
                    s->macro[i] = s->engine.macroValue(i);
                tellHostAboutSlots(s);
            }
        }
        return;
    }

    const int want = s->wantPreset.load();
    if (want == s->havePreset && !s->custom) return;
    if (want < 0 || want >= rackTotal()) return;

    /* Engine::clear() takes the graph lock, so this is safe against a render
     * in flight - it is the same path the standalone's preset menu uses. */
    if (want < bs::rackPresetCount()) {
        s->engine.buildPreset(want);
    } else {
        /* A file this time. If it will not load - deleted, or moved, or
         * written by something else - say so and leave the rack alone rather
         * than dropping the player into silence mid-song. */
        const bs::UserRack *ur = bs::userRackAt(want - bs::rackPresetCount());
        char status[256] = "";
        if (!ur || !bs_patch_load(&s->engine, ur->path.c_str(),
                                  status, sizeof status)) {
            bs::bs_log("  rack %d (%s) did not load: %s", want,
                       ur ? ur->path.c_str() : "?", status);
            s->wantPreset.store(s->havePreset);
            return;
        }
        bs::bs_log("  loaded user rack %s", ur->path.c_str());
    }
    s->havePreset = want;
    s->custom     = false;

    /* The new rack's MACRO knobs are at whatever the preset built them at.
     * The host's parameters are the truth, so put them back. */
    for (int i = 0; i < bs::BS_MACROS; i++) s->engine.setMacro(i, s->macro[i]);

    /* The editor is showing the rack that just went away. */
    pushRackToEditor(s);
    tellHostAboutSlots(s);
}

static const void *pl_get_extension(const clap_plugin_t *, const char *id)
{
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &EXT_AUDIO_PORTS;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS)  == 0) return &EXT_NOTE_PORTS;
    if (std::strcmp(id, CLAP_EXT_PARAMS)      == 0) return &EXT_PARAMS;
    if (std::strcmp(id, CLAP_EXT_STATE)       == 0) return &EXT_STATE;
    if (std::strcmp(id, CLAP_EXT_GUI)         == 0) return guiExtension();
    return 0;
}

/* ------------------------------------------------------------------ *
 * GUI - a floating window, in another process
 *
 * The editor is the standalone binary in a mode where it makes no sound and
 * drives a rack in shared memory instead. It has to be a separate process:
 * raylib keeps its window and GL context in one file-scope global, so a
 * process gets exactly one raylib window however many plugin instances are
 * loaded. See src/plugin/bs_shm.h.
 *
 * Floating rather than embedded, because a raylib window is a top-level window
 * that GLFW created and owns. Embedding means reparenting it into a handle the
 * host supplies - SetParent on Windows, XReparentWindow on X11, and something
 * considerably less pleasant on macOS. CLAP supports floating outright, which
 * is why it is the first target rather than VST3.
 * ------------------------------------------------------------------ */

static bool gui_is_api_supported(const clap_plugin_t *, const char *api,
                                 bool is_floating)
{
    /* Logged because the answer to "why is there no window" is usually here:
     * a host that only embeds never asks for floating, and a plugin that only
     * floats is never asked for anything else. */
    bs::bs_log("gui.is_api_supported(%s, floating=%d)", api ? api : "?",
               is_floating ? 1 : 0);
#if defined(_WIN32)
    if (std::strcmp(api, CLAP_WINDOW_API_WIN32) != 0) return false;
    /* Both. Hosts built around an FX rack - REAPER is one - embed the plugin's
     * view in their own window and never ask for a floating one, so refusing
     * embedded means those hosts show their generic parameter list and no
     * plugin ever gets to draw itself. The editor's window is an HWND, and
     * SetParent works across processes. */
    return true;
#elif defined(__APPLE__)
    /* Embedded, because that is the only thing hosts here ask for. REAPER's
     * one question is is_api_supported(cocoa, floating=0) - answer no and it
     * never asks anything else, which is a plugin with no window and no
     * explanation. Floating stays available for hosts that prefer it. */
    (void)is_floating;
    return std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
#else
    if (std::strcmp(api, CLAP_WINDOW_API_X11) != 0) return false;
    /* Both, and embedded preferred - same as Windows, for the same reason:
     * every host built around an FX rack embeds and never asks for floating.
     * An X11 Window is a server-side object with an id, so it reparents across
     * processes exactly as an HWND does. */
    (void)is_floating;
    return true;
#endif
}

static bool gui_get_preferred_api(const clap_plugin_t *, const char **api,
                                  bool *is_floating)
{
#if defined(_WIN32)
    *api = CLAP_WINDOW_API_WIN32;
    /* Embedded, because that is what the hosts on this platform actually do. */
    *is_floating = false;
#elif defined(__APPLE__)
    *api = CLAP_WINDOW_API_COCOA;
    *is_floating = false;
#else
    *api = CLAP_WINDOW_API_X11;
    *is_floating = false;
#endif
    return true;
}

static bool gui_create(const clap_plugin_t *p, const char *api, bool is_floating)
{
    BencSynthClap *s = self_of(p);
    bs::bs_log("gui.create(%s, floating=%d)", api ? api : "?", is_floating ? 1 : 0);
    if (!gui_is_api_supported(p, api, is_floating)) {
        bs::bs_log("  refused - this platform build does not offer that mode");
        return false;
    }
    if (s->guiCreated) return true;
    s->floating     = is_floating;
    s->parentHandle = 0;

    /* The salt keeps two instances in one project from colliding on a name. */
    static std::atomic<unsigned long> counter(0);
    if (!bs::bs_shm_create(&s->shm, counter.fetch_add(1) + 1)) return false;
    s->shmOpen = true;

    s->appliedEditSeq  = 0;
    s->appliedSnapSeq  = 0;
    s->appliedParamSeq = 0;
    s->pendingEdit.store(0);

    s->shm.block->wantW.store(s->guiW);
    s->shm.block->wantH.store(s->guiH);
    s->shm.block->sampleRate.store(s->rate, std::memory_order_relaxed);

    /* The editor starts from what the plugin is currently playing. */
    pushRackToEditor(s);

    s->guiCreated = true;
    return true;
}

static void gui_destroy(const clap_plugin_t *p)
{
    BencSynthClap *s = self_of(p);
    if (!s->guiCreated) return;

#if defined(__APPLE__)
    if (s->cocoaView) {
        bs::bs_cocoa_stop_pump(s->cocoaView);
        bs::bs_cocoa_detach(s->cocoaView);
        s->cocoaView = 0;
    }
#endif
    if (s->shmOpen) {
        /* Ask first. A window that saves nothing on the way out loses whatever
         * was on screen, so the editor gets a moment to publish and exit. */
        s->shm.block->quit.store(1, std::memory_order_release);
        bs::bs_shm_wait_editor(s->editorProc, 1500);
        s->editorProc = 0;

        /* Last word from the editor before the shared block goes away. */
        if (s->textBuf.size() < bs::BS_SHM_RACK_MAX)
            s->textBuf.resize(bs::BS_SHM_RACK_MAX);
        uint32_t len = 0;
        if (bs::bs_shm_read(&s->shm.block->snap, s->textBuf.data(), &len) && len)
            s->snapshot.assign(s->textBuf.data(), len);

        bs::bs_shm_close(&s->shm);
        s->shmOpen = false;
    }
    s->guiCreated = false;
}

static bool gui_set_scale(const clap_plugin_t *, double) { return false; }

static bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
    BencSynthClap *s = self_of(p);
    *w = s->guiW;
    *h = s->guiH;
    return true;
}

static bool gui_can_resize(const clap_plugin_t *) { return true; }

static bool gui_get_resize_hints(const clap_plugin_t *, clap_gui_resize_hints_t *h)
{
    std::memset(h, 0, sizeof *h);
    h->can_resize_horizontally = true;
    h->can_resize_vertically   = true;
    return true;
}

static bool gui_adjust_size(const clap_plugin_t *, uint32_t *, uint32_t *)
{
    return true;
}

/* Embedded, the host owns the geometry - but the window it is resizing lives
 * in another process, so the size goes through the shared block and the editor
 * applies it on its own thread. */
static bool gui_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h)
{
    BencSynthClap *s = self_of(p);
    if (w < 640) w = 640;
    if (h < 480) h = 480;
    s->guiW = w;
    s->guiH = h;
#if defined(__APPLE__)
    if (s->cocoaView) bs::bs_cocoa_resize(s->cocoaView, (int)w, (int)h);
#endif
    if (s->shmOpen) {
        s->shm.block->wantW.store(w, std::memory_order_release);
        s->shm.block->wantH.store(h, std::memory_order_release);
    }
    return true;
}

/* Starting the editor, from whichever call gets here first.
 *
 * CLAP says a host calls gui.create, gui.set_parent, then gui.show, and show
 * is the natural place to start a process. Every host that calls show gets
 * that. clap-wrapper's AUv2 view does not call it - the line is there in
 * wrappedview.asinclude.mm, commented out, twice - so under Logic and
 * GarageBand the plugin was handed a parent view and then asked for nothing
 * else, and sat on "starting the rack" forever with the editor never spawned.
 *
 * Idempotent, because now two paths reach it. */
static bool ensureEditor(BencSynthClap *s)
{
    if (!s->guiCreated || !s->shmOpen) {
        bs::bs_log("  no editor yet (created=%d shm=%d)",
                   s->guiCreated ? 1 : 0, s->shmOpen ? 1 : 0);
        return false;
    }
    if (s->editorProc && bs::bs_shm_editor_running(s->editorProc)) return true;

    s->shm.block->quit.store(0, std::memory_order_release);
    s->shm.block->embedParent.store(s->floating ? 0 : s->parentHandle,
                                    std::memory_order_release);
    const char *hint = s->bundleDir.empty() ? 0 : s->bundleDir.c_str();
#if defined(__APPLE__)
    const bool offscreen = !s->floating;
#else
    const bool offscreen = false;
#endif
    if (!bs::bs_shm_spawn_editor(hint, s->shm.name, &s->editorProc, offscreen)) {
        bs::bs_log("  the editor could not be started - see the candidates above");
        /* Nothing to show and no way to say why through this interface. The
         * host will report a failed show; the message goes to stderr, which is
         * where a person looking for it will be. */
        std::fprintf(stderr,
                     "BENCsynth: could not start the editor. Set BENCSYNTH_EDITOR "
                     "to the path of the bencsynth executable.\n");
        return false;
    }
    bs::bs_log("  editor started");
#if defined(__APPLE__)
    if (s->cocoaView) bs::bs_cocoa_set_status(s->cocoaView, "");
#endif
    return true;
}

static bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *window)
{
    BencSynthClap *s = self_of(p);
    bs::bs_log("gui.set_parent(%s)", window && window->api ? window->api : "?");
    if (!window) return false;
#if defined(_WIN32)
    if (std::strcmp(window->api, CLAP_WINDOW_API_WIN32) != 0) return false;
    s->parentHandle = (uint64_t)(uintptr_t)window->win32;
#elif defined(__APPLE__)
    if (std::strcmp(window->api, CLAP_WINDOW_API_COCOA) != 0) return false;
    /* A view of our own inside the host's. Nothing renders into it yet - the
     * editor's pixels arrive over an IOSurface in the next stage - so for now
     * it says what it is rather than showing a blank rectangle. */
    if (s->cocoaView) bs::bs_cocoa_detach(s->cocoaView);
    s->cocoaView = bs::bs_cocoa_attach(window->cocoa, (int)s->guiW, (int)s->guiH);
    if (!s->cocoaView) return false;
    bs::bs_cocoa_set_status(s->cocoaView, "BENCsynth - starting the rack");
    /* Pixels, not a window: the editor has no way to put a view in this
     * process, so it renders offscreen and we show what it produced. */
    if (s->shmOpen) {
        s->shm.block->fbMode.store(1, std::memory_order_release);
        s->shm.block->wantW.store(s->guiW, std::memory_order_release);
        s->shm.block->wantH.store(s->guiH, std::memory_order_release);
    }
    bs::bs_cocoa_set_input_sink(s->cocoaView, sinkInput, s);
    bs::bs_cocoa_start_pump(s->cocoaView, pumpFrame, s, 60.0);
    s->parentHandle = (uint64_t)(uintptr_t)window->cocoa;
#else
    if (std::strcmp(window->api, CLAP_WINDOW_API_X11) != 0) return false;
    s->parentHandle = (uint64_t)window->x11;
#endif
    if (s->shmOpen)
        s->shm.block->embedParent.store(s->parentHandle, std::memory_order_release);
    /* A host that goes on to call show() finds it already running. One that
     * never calls show() - clap-wrapper's AUv2 view - gets a rack anyway. */
    ensureEditor(s);
    return true;
}
static bool gui_set_transient(const clap_plugin_t *, const clap_window_t *) { return false; }
static void gui_suggest_title(const clap_plugin_t *, const char *) {}

static bool gui_show(const clap_plugin_t *p)
{
    BencSynthClap *s = self_of(p);
#if defined(__APPLE__)
    /* Embedded here means the editor draws offscreen and its frames are
     * copied into the surface behind the host's view. It still has to be
     * started, and it still has to be found - ensureEditor does both. */
    if (!s->floating && !s->cocoaView) {
        bs::bs_log("gui.show  (macOS: no view to draw into)");
        return false;
    }
#endif
    bs::bs_log("gui.show  (created=%d shm=%d editorDir=%s)",
               s->guiCreated ? 1 : 0, s->shmOpen ? 1 : 0,
               s->bundleDir.empty() ? "(none)" : s->bundleDir.c_str());
    return ensureEditor(s);
}

static bool gui_hide(const clap_plugin_t *p)
{
    BencSynthClap *s = self_of(p);
    if (!s->shmOpen) return false;
    s->shm.block->quit.store(1, std::memory_order_release);
    bs::bs_shm_wait_editor(s->editorProc, 1500);
    s->editorProc = 0;
    return true;
}

static const clap_plugin_gui_t EXT_GUI = {
    gui_is_api_supported,
    gui_get_preferred_api,
    gui_create,
    gui_destroy,
    gui_set_scale,
    gui_get_size,
    gui_can_resize,
    gui_get_resize_hints,
    gui_adjust_size,
    gui_set_size,
    gui_set_parent,
    gui_set_transient,
    gui_suggest_title,
    gui_show,
    gui_hide
};

static const clap_plugin_gui_t *guiExtension() { return &EXT_GUI; }

/* ------------------------------------------------------------------ *
 * Descriptor and factory
 * ------------------------------------------------------------------ */

static const char *const FEATURES[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    0
};

static const clap_plugin_descriptor_t DESCRIPTOR = {
    CLAP_VERSION_INIT,
    BS_CLAP_ID,
    "BENCsynth",
    "BENCO",
    "https://github.com/bropple/BENCsynth",
    0,
    0,
    BS_VERSION_STRING,
    "A polyphonic virtual modular synthesizer. Build a rack in the standalone, "
    "save a .bencsynth, and load it here.",
    FEATURES
};

static const clap_plugin_t *createPlugin(const clap_host_t *host)
{
    BencSynthClap *s = new (std::nothrow) BencSynthClap();
    if (!s) return 0;

    s->host        = host;
    s->hostParams  = host && host->get_extension
        ? (const clap_host_params_t *)host->get_extension(host, CLAP_EXT_PARAMS)
        : 0;
    s->exposedRev  = 0;
    s->editorProc  = 0;
    s->shmOpen     = false;
    s->guiCreated  = false;
    s->appliedEditSeq = s->appliedSnapSeq = s->appliedParamSeq = 0;
    s->floating     = true;
    s->parentHandle = 0;
    s->guiW = 1280;
    s->guiH = 800;
    s->cocoaView = 0;
    s->fbSeen    = 0;
    s->rate = 48000.0f;
    s->pendingEdit.store(0);
    s->bundleDir   = g_bundleDir;

    /* The racks a person saved, found once per instance.
     *
     * Once, not on every list: a host asks for parameter info constantly, and
     * a chooser whose length changed under it would be worse than one that
     * needs the plugin reopening to notice a new file. Beside the plugin as
     * well as in the per-user folder, because a portable Windows install puts
     * the editor - and so its patches folder - next to the .clap. */
    {
        std::string beside = g_bundleDir;
        if (!beside.empty()) {
            const char sep = beside.find('\\') != std::string::npos ? '\\' : '/';
            if (beside[beside.size() - 1] != sep) beside += sep;
            beside += "patches";
        }
        const int n = bs::scanUserRacks(beside.empty() ? 0 : beside.c_str());
        bs::bs_log("  %d built-in racks, %d of the user's", bs::rackPresetCount(), n);
    }

    s->havePreset = 0;
    s->custom     = false;
    s->wantPreset.store(0);
    for (int i = 0; i < bs::BS_MACROS; i++) s->macro[i] = 0.0f;

    s->plugin.desc             = &DESCRIPTOR;
    s->plugin.plugin_data      = s;
    s->plugin.init             = pl_init;
    s->plugin.destroy          = pl_destroy;
    s->plugin.activate         = pl_activate;
    s->plugin.deactivate       = pl_deactivate;
    s->plugin.start_processing = pl_start_processing;
    s->plugin.stop_processing  = pl_stop_processing;
    s->plugin.reset            = pl_reset;
    s->plugin.process          = pl_process;
    s->plugin.get_extension    = pl_get_extension;
    s->plugin.on_main_thread   = pl_on_main_thread;

    /* A sample rate arrives at activate(); this makes the object valid before
     * then, so a host that asks for parameters first gets real answers. */
    s->engine.init(48000.0f);
    s->engine.buildPreset(0);
    return &s->plugin;
}

static uint32_t fa_count(const clap_plugin_factory_t *) { return 1; }

static const clap_plugin_descriptor_t *fa_get(const clap_plugin_factory_t *,
                                              uint32_t index)
{
    return index == 0 ? &DESCRIPTOR : 0;
}

static const clap_plugin_t *fa_create(const clap_plugin_factory_t *,
                                      const clap_host_t *host, const char *id)
{
    if (!id || std::strcmp(id, BS_CLAP_ID) != 0) return 0;
    return createPlugin(host);
}

static const clap_plugin_factory_t FACTORY = { fa_count, fa_get, fa_create };

static bool entry_init(const char *path)
{
    /* The directory only. What the editor is actually called differs by
     * platform - on macOS it is a binary inside BENCsynth.app rather than a
     * file of its own - so the naming belongs with the code that knows the
     * platform. See editorCandidates() in bs_shm.cpp. */
    bs::bs_log("---- entry_init(%s)", path ? path : "(null)");
    g_bundleDir.clear();
    if (path && *path) {
        g_bundleDir = path;
        const size_t cut = g_bundleDir.find_last_of("/\\");
        if (cut != std::string::npos) g_bundleDir.erase(cut);
    }
    return true;
}
static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id)
{
    if (std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0) return &FACTORY;
    return 0;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entry_init,
    entry_deinit,
    entry_get_factory
};
