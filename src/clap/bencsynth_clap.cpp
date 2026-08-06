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

#include <clap/clap.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#define BS_CLAP_ID "net.ropple.bencsynth"

/* Parameters. Eight macros, then the rack selector. The macros exist because a
 * host needs a fixed parameter list before it knows anything about the patch,
 * and a modular's knobs come and go with its modules - what each one does is
 * decided in the rack, by cable, on the MACRO module. */
enum {
    PARAM_MACRO_0 = 0,
    PARAM_PRESET  = bs::BS_MACROS,
    PARAM_COUNT
};

struct BencSynthClap {
    clap_plugin_t     plugin;   /* must be first - the host holds a pointer to it */
    const clap_host_t *host;

    bs::Engine engine;

    /* The rack the audio thread has been told to switch to, and the one it is
     * actually running. Building a rack allocates, so process() may not do it:
     * it records the request and asks the host for a main-thread callback,
     * which is precisely what request_callback is for. */
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
};

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

static bool pa_get_info(const clap_plugin_t *, uint32_t index,
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
        info->max_value     = bs::rackPresetCount() - 1;
        info->default_value = 0;
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
    if (id >= bs::BS_MACROS) return false;
    *out = s->macro[id];
    return true;
}

static bool pa_value_to_text(const clap_plugin_t *p, clap_id id, double value,
                             char *out, uint32_t cap)
{
    if (id == PARAM_PRESET) {
        const int i = (int)(value + 0.5);
        const bs::RackPreset *rp = bs::rackPresetAt(i);
        if (self_of(p)->custom)
            std::snprintf(out, cap, "%s (edited)", rp ? rp->name : "Custom");
        else
            std::snprintf(out, cap, "%s", rp ? rp->name : "?");
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
        for (int i = 0; i < bs::rackPresetCount(); i++) {
            const bs::RackPreset *rp = bs::rackPresetAt(i);
            if (rp && std::strcmp(rp->name, text) == 0) { *out = i; return true; }
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
    s->saved = bs_patch_to_string(&s->engine);

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
    return true;
}

static const clap_plugin_state_t EXT_STATE = { st_save, st_load };

/* ------------------------------------------------------------------ *
 * Lifecycle and audio
 * ------------------------------------------------------------------ */

static bool pl_init(const clap_plugin_t *) { return true; }

static void pl_destroy(const clap_plugin_t *p)
{
    delete self_of(p);
}

static bool pl_activate(const clap_plugin_t *p, double sample_rate,
                        uint32_t, uint32_t)
{
    BencSynthClap *s = self_of(p);
    s->engine.init((float)sample_rate);
    if (!s->custom) s->engine.buildPreset(s->havePreset);
    return true;
}

static void pl_deactivate(const clap_plugin_t *) {}
static bool pl_start_processing(const clap_plugin_t *) { return true; }
static void pl_stop_processing(const clap_plugin_t *) {}

static void pl_reset(const clap_plugin_t *p)
{
    self_of(p)->engine.panic();
}

/* One event, already known to be due. */
static void handleEvent(BencSynthClap *s, const clap_event_header_t *h, int at)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;

    switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
        const clap_event_note_t *e = (const clap_event_note_t *)h;
        if (e->key >= 0) s->engine.noteOn(e->key, (float)e->velocity, at);
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        const clap_event_note_t *e = (const clap_event_note_t *)h;
        /* key -1 is a wildcard: every note on the port. */
        if (e->key < 0) s->engine.panic();
        else            s->engine.noteOff(e->key, at);
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
        switch (m[0] & 0xf0) {
        case 0x90:
            /* Note-on with zero velocity is a note-off, and has been since
             * running status made that cheaper to send. */
            if (m[2] > 0) s->engine.noteOn(m[1], (float)m[2] / 127.0f, at);
            else          s->engine.noteOff(m[1], at);
            break;
        case 0x80:
            s->engine.noteOff(m[1], at);
            break;
        case 0xe0: {
            const int raw = ((int)m[2] << 7) | (int)m[1];   /* 0..16383 */
            s->engine.setBend((float)(raw - 8192) / 8192.0f, at);
            break;
        }
        case 0xb0:
            if (m[1] == 1)        s->engine.setMod((float)m[2] / 127.0f, at);
            else if (m[1] == 64)  s->engine.setSustain(m[2] >= 64, at);
            else if (m[1] == 123 || m[1] == 120) s->engine.panic();
            break;
        default:
            break;
        }
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
            handleEvent(s, h, rel < 0 ? 0 : (int)rel);
            evIndex++;
        }

        s->engine.render(tmp, (int)n);
        for (uint32_t i = 0; i < n; i++) {
            outL[done + i] = tmp[i * 2 + 0];
            outR[done + i] = tmp[i * 2 + 1];
        }
        done += n;
    }

    /* Never SLEEP: a rack can be self-playing - a filter past self-oscillation,
     * a sequence clocked by an LFO, a delay feeding itself - so silence now
     * says nothing about silence later. */
    return CLAP_PROCESS_CONTINUE;
}

/* The main thread, where allocating is allowed. */
static void pl_on_main_thread(const clap_plugin_t *p)
{
    BencSynthClap *s = self_of(p);
    const int want = s->wantPreset.load();
    if (want == s->havePreset && !s->custom) return;
    if (want < 0 || want >= bs::rackPresetCount()) return;

    /* Engine::clear() takes the graph lock, so this is safe against a render
     * in flight - it is the same path the standalone's preset menu uses. */
    s->engine.buildPreset(want);
    s->havePreset = want;
    s->custom     = false;

    /* The new rack's MACRO knobs are at whatever the preset built them at.
     * The host's parameters are the truth, so put them back. */
    for (int i = 0; i < bs::BS_MACROS; i++) s->engine.setMacro(i, s->macro[i]);
}

static const void *pl_get_extension(const clap_plugin_t *, const char *id)
{
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &EXT_AUDIO_PORTS;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS)  == 0) return &EXT_NOTE_PORTS;
    if (std::strcmp(id, CLAP_EXT_PARAMS)      == 0) return &EXT_PARAMS;
    if (std::strcmp(id, CLAP_EXT_STATE)       == 0) return &EXT_STATE;
    return 0;
}

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
    "0.1.0",
    "A polyphonic virtual modular synthesizer. Build a rack in the standalone, "
    "save a .bencsynth, and load it here.",
    FEATURES
};

static const clap_plugin_t *createPlugin(const clap_host_t *host)
{
    BencSynthClap *s = new (std::nothrow) BencSynthClap();
    if (!s) return 0;

    s->host       = host;
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

static bool entry_init(const char *) { return true; }
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
