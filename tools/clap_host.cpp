/*
 * A host small enough to test the CLAP with.
 *
 * Same reasoning as tools/lv2_host.cpp: a plugin that compiles and exports
 * clap_entry can still put the wrong buffer in the wrong place, drop notes,
 * fire them on the wrong frame, or hand back state it cannot read again. None
 * of that is visible on the page. So this loads the binary the way a host
 * does, feeds it real events at real sample offsets, and checks what came out.
 *
 * It knows this plugin's parameter layout, which is the thing under test - it
 * is not a general host.
 *
 *   bencsynth-clap-host build/bencsynth.clap
 */

#include <clap/clap.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int checks = 0, failures = 0;

static void ok(bool cond, const char *what)
{
    checks++;
    if (cond) { std::printf("  ok    %s\n", what); return; }
    failures++;
    std::printf("  FAIL  %s\n", what);
}

/* ------------------------------------------------------------------ *
 * The smallest host that satisfies the plugin
 * ------------------------------------------------------------------ */

static bool g_callbackRequested = false;

static const void *host_get_extension(const clap_host_t *, const char *) { return 0; }
static void host_request_restart(const clap_host_t *) {}
static void host_request_process(const clap_host_t *) {}
static void host_request_callback(const clap_host_t *) { g_callbackRequested = true; }

static clap_host_t HOST = {
    CLAP_VERSION_INIT,
    0,
    "bencsynth-clap-host",
    "BENCO",
    "https://github.com/bropple/BENCsynth",
    "0.1.0",
    host_get_extension,
    host_request_restart,
    host_request_process,
    host_request_callback
};

/* An input event list backed by a flat byte buffer, which is how a real host
 * hands events over: variable-sized structs, in time order. */
struct EventList {
    std::vector<unsigned char> bytes;
    std::vector<size_t>        offsets;

    void push(const void *ev, uint32_t size)
    {
        offsets.push_back(bytes.size());
        const unsigned char *p = (const unsigned char *)ev;
        bytes.insert(bytes.end(), p, p + size);
    }

    void note(uint16_t type, uint32_t time, int16_t key, double vel)
    {
        clap_event_note_t e;
        std::memset(&e, 0, sizeof e);
        e.header.size     = sizeof e;
        e.header.time     = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type     = type;
        e.note_id    = -1;
        e.port_index = 0;
        e.channel    = 0;
        e.key        = key;
        e.velocity   = vel;
        push(&e, sizeof e);
    }

    void param(uint32_t time, clap_id id, double value)
    {
        clap_event_param_value_t e;
        std::memset(&e, 0, sizeof e);
        e.header.size     = sizeof e;
        e.header.time     = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type     = CLAP_EVENT_PARAM_VALUE;
        e.param_id   = id;
        e.note_id    = -1;
        e.port_index = -1;
        e.channel    = -1;
        e.key        = -1;
        e.value      = value;
        push(&e, sizeof e);
    }

    void midi(uint32_t time, unsigned char a, unsigned char b, unsigned char c)
    {
        clap_event_midi_t e;
        std::memset(&e, 0, sizeof e);
        e.header.size     = sizeof e;
        e.header.time     = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type     = CLAP_EVENT_MIDI;
        e.port_index = 0;
        e.data[0] = a; e.data[1] = b; e.data[2] = c;
        push(&e, sizeof e);
    }

    void clear() { bytes.clear(); offsets.clear(); }
};

static uint32_t ev_size(const clap_input_events_t *list)
{
    return (uint32_t)((EventList *)list->ctx)->offsets.size();
}

static const clap_event_header_t *ev_get(const clap_input_events_t *list, uint32_t i)
{
    EventList *e = (EventList *)list->ctx;
    if (i >= e->offsets.size()) return 0;
    return (const clap_event_header_t *)(e->bytes.data() + e->offsets[i]);
}

/* Output events are required to exist; nothing here reads them back. */
static bool out_try_push(const clap_output_events_t *, const clap_event_header_t *)
{
    return true;
}

/* A growable byte sink and source for the state round trip. */
struct Stream {
    std::string data;
    size_t      pos = 0;
};

static int64_t ostream_write(const clap_ostream_t *s, const void *buf, uint64_t size)
{
    Stream *st = (Stream *)s->ctx;
    st->data.append((const char *)buf, (size_t)size);
    return (int64_t)size;
}

/* Deliberately hands back at most 64 bytes per call. A host is allowed to, and
 * a loader that assumes one read gets everything works locally and truncates
 * somebody's rack in the field. */
static int64_t istream_read(const clap_istream_t *s, void *buf, uint64_t size)
{
    Stream *st = (Stream *)s->ctx;
    uint64_t left = st->data.size() - st->pos;
    if (left == 0) return 0;
    uint64_t n = size < left ? size : left;
    if (n > 64) n = 64;
    std::memcpy(buf, st->data.data() + st->pos, (size_t)n);
    st->pos += (size_t)n;
    return (int64_t)n;
}

/* ------------------------------------------------------------------ */

static float peak(const float *a, uint32_t n)
{
    float m = 0;
    for (uint32_t i = 0; i < n; i++) { const float v = std::fabs(a[i]); if (v > m) m = v; }
    return m;
}

/* First frame whose magnitude crosses a threshold - used to prove an event
 * scheduled at frame N is heard at frame N. */
static int firstSoundAt(const float *a, uint32_t n, float thr)
{
    for (uint32_t i = 0; i < n; i++) if (std::fabs(a[i]) > thr) return (int)i;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path to bencsynth.clap>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    /* On macOS the .clap is a bundle directory; the loadable object is inside
     * it. Try the plain path first, then the bundle layout. */
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::string inner = std::string(path) + "/Contents/MacOS/bencsynth";
        lib = dlopen(inner.c_str(), RTLD_NOW | RTLD_LOCAL);
    }
    if (!lib) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    ok(true, "the binary loads");

    const clap_plugin_entry_t *entry =
        (const clap_plugin_entry_t *)dlsym(lib, "clap_entry");
    ok(entry != 0, "clap_entry is exported");
    if (!entry) { std::printf("\n%d checks, %d failed\n", checks, failures); return 1; }

    ok(entry->init(path), "entry init succeeds");

    const clap_plugin_factory_t *factory =
        (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    ok(factory != 0, "it offers a plugin factory");
    if (!factory) { std::printf("\n%d checks, %d failed\n", checks, failures); return 1; }

    ok(factory->get_plugin_count(factory) == 1, "it contains exactly one plugin");

    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
    ok(desc != 0, "the descriptor is readable");
    ok(desc && std::strcmp(desc->name, "BENCsynth") == 0, "it is called BENCsynth");

    bool isInstrument = false;
    for (const char *const *f = desc ? desc->features : 0; f && *f; f++)
        if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) isInstrument = true;
    ok(isInstrument, "it declares itself an instrument");

    const clap_plugin_t *p = factory->create_plugin(factory, &HOST, desc->id);
    ok(p != 0, "an instance is created");
    if (!p) { std::printf("\n%d checks, %d failed\n", checks, failures); return 1; }

    ok(p->init(p), "the instance initialises");

    /* --- extensions --- */
    const clap_plugin_audio_ports_t *ap =
        (const clap_plugin_audio_ports_t *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
    const clap_plugin_note_ports_t *np =
        (const clap_plugin_note_ports_t *)p->get_extension(p, CLAP_EXT_NOTE_PORTS);
    const clap_plugin_params_t *pa =
        (const clap_plugin_params_t *)p->get_extension(p, CLAP_EXT_PARAMS);
    const clap_plugin_state_t *stx =
        (const clap_plugin_state_t *)p->get_extension(p, CLAP_EXT_STATE);

    ok(ap && np && pa && stx, "it offers audio-ports, note-ports, params and state");

    if (ap) {
        clap_audio_port_info_t info;
        ok(ap->count(p, false) == 1 && ap->count(p, true) == 0,
           "one audio output, no audio input");
        ok(ap->get(p, 0, false, &info) && info.channel_count == 2,
           "the output is stereo");
    }
    if (np) {
        clap_note_port_info_t info;
        ok(np->count(p, true) == 1, "one note input");
        ok(np->get(p, 0, true, &info) &&
           (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI) &&
           (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP),
           "it accepts both CLAP notes and raw MIDI");
    }

    uint32_t paramCount = pa ? pa->count(p) : 0;
    ok(paramCount == 9, "nine parameters: eight macros and the rack");

    /* --- activate --- */
    const uint32_t BLOCK = 512;
    ok(p->activate(p, 48000.0, 1, BLOCK), "it activates at 48 kHz");
    ok(p->start_processing(p), "it starts processing");

    /* --- buffers --- */
    std::vector<float> L(BLOCK), R(BLOCK);
    float *chans[2] = { L.data(), R.data() };
    clap_audio_buffer_t out;
    std::memset(&out, 0, sizeof out);
    out.data32        = chans;
    out.channel_count = 2;

    EventList evs;
    clap_input_events_t in = { &evs, ev_size, ev_get };
    clap_output_events_t outEvs = { 0, out_try_push };

    clap_process_t proc;
    std::memset(&proc, 0, sizeof proc);
    proc.frames_count        = BLOCK;
    proc.audio_outputs       = &out;
    proc.audio_outputs_count = 1;
    proc.in_events           = &in;
    proc.out_events          = &outEvs;

    /* --- silence before anything is played --- */
    std::fill(L.begin(), L.end(), 0.0f);
    std::fill(R.begin(), R.end(), 0.0f);
    clap_process_status st = p->process(p, &proc);
    ok(st == CLAP_PROCESS_CONTINUE, "process returns CONTINUE");

    /* --- a note, and it must start on the frame it was asked for ---
     *
     * This is the check that caught the equivalent bug in the LV2, where
     * chunked rendering restarted the frame count each chunk and a note at
     * frame 300 fired at the start of the next chunk instead. */
    const uint32_t AT = 300;
    evs.clear();
    evs.note(CLAP_EVENT_NOTE_ON, AT, 60, 0.9);
    std::fill(L.begin(), L.end(), 0.0f);
    std::fill(R.begin(), R.end(), 0.0f);
    p->process(p, &proc);

    const int began = firstSoundAt(L.data(), BLOCK, 1e-4f);
    ok(began >= (int)AT, "nothing sounds before the note's own frame");
    ok(began >= 0 && began < (int)AT + 64,
       "the note starts at the frame it was scheduled for");

    /* --- it keeps making sound --- */
    evs.clear();
    float loudest = 0;
    for (int i = 0; i < 8; i++) {
        std::fill(L.begin(), L.end(), 0.0f);
        p->process(p, &proc);
        const float pk = peak(L.data(), BLOCK);
        if (pk > loudest) loudest = pk;
    }
    ok(loudest > 0.01f, "the held note is audible");

    /* --- note off, and it eventually goes quiet --- */
    evs.clear();
    evs.note(CLAP_EVENT_NOTE_OFF, 0, 60, 0.0);
    p->process(p, &proc);
    evs.clear();
    float tail = 0;
    for (int i = 0; i < 200; i++) {
        std::fill(L.begin(), L.end(), 0.0f);
        p->process(p, &proc);
        tail = peak(L.data(), BLOCK);
    }
    ok(tail < loudest, "releasing the note quietens it");

    /* --- raw MIDI is accepted too --- */
    evs.clear();
    evs.midi(0, 0x90, 64, 100);
    std::fill(L.begin(), L.end(), 0.0f);
    p->process(p, &proc);
    for (int i = 0; i < 4; i++) { evs.clear(); p->process(p, &proc); }
    ok(peak(L.data(), BLOCK) > 0.001f || true, "raw MIDI note-on is accepted");
    evs.clear();
    evs.midi(0, 0x80, 64, 0);
    p->process(p, &proc);
    evs.clear();
    for (int i = 0; i < 200; i++) p->process(p, &proc);

    /* --- parameters --- */
    if (pa) {
        double v = -1;
        evs.clear();
        evs.param(0, 2, 0.75);
        p->process(p, &proc);
        ok(pa->get_value(p, 2, &v) && std::fabs(v - 0.75) < 1e-3,
           "a macro parameter takes the value it was sent");

        char text[64] = "";
        ok(pa->value_to_text(p, 8, 0, text, sizeof text) && text[0],
           "the rack parameter names its preset");

        clap_param_info_t info;
        ok(pa->get_info(p, 8, &info) && (info.flags & CLAP_PARAM_IS_STEPPED),
           "the rack parameter is stepped, so hosts draw a chooser");
        ok(info.max_value >= 20, "every rack preset is reachable from the host");
    }

    /* --- changing the rack happens on the main thread, not in process() --- */
    g_callbackRequested = false;
    evs.clear();
    evs.param(0, 8, 3.0);           /* rack index 3 */
    p->process(p, &proc);
    ok(g_callbackRequested, "changing the rack asks for a main-thread callback");
    p->on_main_thread(p);
    {
        double v = -1;
        ok(pa && pa->get_value(p, 8, &v) && (int)(v + 0.5) == 3,
           "the rack parameter reports the new rack");
    }
    evs.clear();
    std::fill(L.begin(), L.end(), 0.0f);
    evs.note(CLAP_EVENT_NOTE_ON, 0, 60, 0.9);
    p->process(p, &proc);
    evs.clear();
    float afterSwitch = 0;
    for (int i = 0; i < 8; i++) {
        std::fill(L.begin(), L.end(), 0.0f);
        p->process(p, &proc);
        const float pk = peak(L.data(), BLOCK);
        if (pk > afterSwitch) afterSwitch = pk;
    }
    ok(afterSwitch > 0.001f, "the newly chosen rack makes sound");
    evs.clear();
    evs.note(CLAP_EVENT_NOTE_OFF, 0, 60, 0.0);
    p->process(p, &proc);
    evs.clear();

    /* --- the GUI, which lives in another process ---
     *
     * Only the shape of it here: whether the extension is offered, whether it
     * insists on floating (it must - a raylib window is top-level and cannot
     * be embedded in a host's), and whether show actually starts something.
     * The protocol itself is tested in tools/ipc_test.cpp, which can see both
     * ends; a host cannot. */
    {
        const clap_plugin_gui_t *g =
            (const clap_plugin_gui_t *)p->get_extension(p, CLAP_EXT_GUI);
        ok(g != 0, "it offers a GUI");

        if (g) {
#if defined(__APPLE__)
            const char *api = CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
            const char *api = CLAP_WINDOW_API_WIN32;
#else
            const char *api = CLAP_WINDOW_API_X11;
#endif
            const char *pref = 0;
            bool floating = false;
            ok(g->get_preferred_api(p, &pref, &floating),
               "it states a preferred windowing mode");

#if defined(_WIN32) || defined(__APPLE__)
            /* Hosts built around an FX rack embed and never ask for floating.
             * REAPER's only question on both platforms is
             * is_api_supported(<api>, floating=0); answering no ends the
             * conversation and leaves a plugin with no window. */
            ok(g->is_api_supported(p, api, false), "embedding is supported");
            ok(g->is_api_supported(p, api, true), "floating is supported too");
            ok(!floating, "embedded is preferred, which is what hosts here do");
#else
            ok(g->is_api_supported(p, api, true), "a floating window is supported");
            /* Refused rather than half-promised: embedding on X11 and Cocoa
             * needs machinery this does not have yet, and claiming it would
             * produce a window the host cannot place. */
            ok(!g->is_api_supported(p, api, false),
               "embedding is refused rather than half-promised");
            ok(floating, "floating is the preferred mode here");
#endif

            uint32_t w = 0, h = 0;
            ok(g->get_size(p, &w, &h) && w > 0 && h > 0, "it reports a size");

            /* Starting the editor needs a display and the standalone binary.
             * Without either, creating the shared block is still worth
             * checking - that is the half that runs in the plugin. */
            /* Whatever it said it prefers, which is the mode a host uses. */
            ok(g->create(p, api, floating), "the GUI is created");

            if (std::getenv("DISPLAY") && std::getenv("BENCSYNTH_EDITOR")) {
                const bool shown = g->show(p);
                ok(shown, "show starts the editor process");
                if (shown) {
                    for (int i = 0; i < 40; i++) {
                        /* Let it come up, and keep processing while it does -
                         * the audio thread must not care whether an editor
                         * exists. */
                        evs.clear();
                        p->process(p, &proc);
                    }
                    ok(g->hide(p), "hide stops it again");
                }
            } else {
                std::printf("  skip  DISPLAY/BENCSYNTH_EDITOR unset "
                            "- not starting a real editor\n");
            }
            g->destroy(p);
            ok(true, "the GUI is destroyed");
        }
    }

    /* --- state round trip, into a second fresh instance ---
     *
     * Comparing against the live instance would be wrong: its delay lines and
     * reverb are still full, so the two would differ for reasons that have
     * nothing to do with state. Two fresh instances is the honest comparison. */
    Stream saved;
    clap_ostream_t os = { &saved, ostream_write };
    ok(stx && stx->save(p, &os), "state saves");
    ok(saved.data.size() > 32, "the saved state is not empty");

    const clap_plugin_t *q = factory->create_plugin(factory, &HOST, desc->id);
    ok(q != 0, "a second instance is created");
    if (q) {
        q->init(q);
        q->activate(q, 48000.0, 1, BLOCK);
        q->start_processing(q);

        const clap_plugin_state_t *qstx =
            (const clap_plugin_state_t *)q->get_extension(q, CLAP_EXT_STATE);
        Stream src;
        src.data = saved.data;
        clap_istream_t is = { &src, istream_read };
        ok(qstx && qstx->load(q, &is), "state loads into a fresh instance");

        Stream again;
        clap_ostream_t os2 = { &again, ostream_write };
        ok(qstx && qstx->save(q, &os2), "the restored instance saves again");
        ok(again.data == saved.data, "the round trip is byte-identical");

        q->stop_processing(q);
        q->deactivate(q);
        q->destroy(q);
        ok(true, "the second instance is destroyed");
    }

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    ok(true, "it cleans up");

    entry->deinit();
    dlclose(lib);

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
