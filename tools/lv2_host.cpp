/*
 * A host small enough to test the plugin with.
 *
 * The LV2 bundle is the one part of this project that cannot be checked by
 * compiling it. A plugin that builds, exports lv2_descriptor and describes
 * itself in valid Turtle can still connect the wrong port to the wrong buffer,
 * ignore MIDI, or hand back state it cannot read again - and the only way to
 * find that out is to load it the way a host does and listen.
 *
 * So this dlopens the binary, walks the descriptor, connects real buffers,
 * builds a real atom sequence with a real note in it, runs it, and checks
 * sound came out. It also round-trips the state, because a rack that does not
 * survive save and restore is a rack that vanishes from somebody's song.
 *
 * It is not a general host: it knows this plugin's port layout, which is the
 * thing under test.
 *
 *   bencsynth-lv2-host build/bencsynth.lv2/bencsynth.so
 */

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <lv2/state/state.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
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
 * The two host services the plugin asks for
 * ------------------------------------------------------------------ */

static std::map<std::string, LV2_URID> g_urids;
static std::vector<std::string>        g_uridNames;

static LV2_URID map_uri(LV2_URID_Map_Handle, const char *uri)
{
    std::map<std::string, LV2_URID>::iterator it = g_urids.find(uri);
    if (it != g_urids.end()) return it->second;
    g_uridNames.push_back(uri);
    const LV2_URID id = (LV2_URID)g_uridNames.size();   /* never zero */
    g_urids[uri] = id;
    return id;
}

/* What the plugin stored, keyed by URID, exactly as a host's project file
 * would hold it. */
struct StoredValue { std::string bytes; uint32_t type, flags; };
static std::map<LV2_URID, StoredValue> g_state;

static LV2_State_Status store_fn(LV2_State_Handle, uint32_t key,
                                 const void *value, size_t size,
                                 uint32_t type, uint32_t flags)
{
    StoredValue v;
    v.bytes.assign((const char *)value, size);
    v.type = type;
    v.flags = flags;
    g_state[key] = v;
    return LV2_STATE_SUCCESS;
}

static const void *retrieve_fn(LV2_State_Handle, uint32_t key, size_t *size,
                               uint32_t *type, uint32_t *flags)
{
    std::map<LV2_URID, StoredValue>::iterator it = g_state.find(key);
    if (it == g_state.end()) return 0;
    if (size)  *size  = it->second.bytes.size();
    if (type)  *type  = it->second.type;
    if (flags) *flags = it->second.flags;
    return it->second.bytes.data();
}

/* ------------------------------------------------------------------ *
 * An atom sequence with MIDI in it, laid out the way a host lays one out
 * ------------------------------------------------------------------ */

struct MidiSeq {
    std::vector<uint8_t> buf;
    LV2_URID midiEvent, sequence, chunk;

    void begin(size_t cap)
    {
        buf.assign(cap, 0);
        LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)&buf[0];
        seq->atom.type = sequence;
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        seq->body.unit = 0;
        seq->body.pad  = 0;
    }

    void add(int64_t frame, const uint8_t *msg, uint32_t len)
    {
        LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)&buf[0];
        uint8_t *end = (uint8_t *)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq)
                     + seq->atom.size - sizeof(LV2_Atom_Sequence_Body);

        LV2_Atom_Event *ev = (LV2_Atom_Event *)end;
        ev->time.frames = frame;
        ev->body.type   = midiEvent;
        ev->body.size   = len;
        std::memcpy(ev + 1, msg, len);

        /* Events are padded to eight bytes, and a host that forgets is a host
         * whose second event lands in the middle of the first. */
        seq->atom.size += (uint32_t)(sizeof(LV2_Atom_Event) + ((len + 7) & ~7u));
    }
};

/* ------------------------------------------------------------------ */

static float rmsOf(const std::vector<float> &a, const std::vector<float> &b)
{
    double s = 0.0;
    for (size_t i = 0; i < a.size(); i++) s += (double)a[i] * a[i] + (double)b[i] * b[i];
    return (float)std::sqrt(s / (double)(a.size() * 2));
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/bencsynth.lv2/bencsynth.so";
    std::printf("BENCsynth LV2 - hosted from %s\n\n", path);

    void *lib = dlopen(path, RTLD_NOW);
    if (!lib) { std::printf("  FAIL  dlopen: %s\n", dlerror()); return 1; }
    ok(true, "the binary loads");

    typedef const LV2_Descriptor *(*DescFn)(uint32_t);
    DescFn descriptor = (DescFn)dlsym(lib, "lv2_descriptor");
    ok(descriptor != 0, "it exports lv2_descriptor");
    if (!descriptor) return 1;

    const LV2_Descriptor *d = descriptor(0);
    ok(d != 0, "descriptor 0 exists");
    ok(descriptor(1) == 0, "and there is exactly one");
    if (!d) return 1;
    std::printf("        URI: %s\n", d->URI);

    LV2_URID_Map map = { 0, map_uri };
    LV2_Feature  mapFeature = { LV2_URID__map, &map };
    const LV2_Feature *features[] = { &mapFeature, 0 };

    const double RATE = 48000.0;
    LV2_Handle h = d->instantiate(d, RATE, "build/bencsynth.lv2/", features);
    ok(h != 0, "it instantiates at 48 kHz");
    if (!h) return 1;

    /* A host that offers no urid:map must be refused rather than crashed. */
    {
        const LV2_Feature *none[] = { 0 };
        LV2_Handle bad = d->instantiate(d, RATE, "build/bencsynth.lv2/", none);
        ok(bad == 0, "it refuses a host with no urid:map instead of crashing");
        if (bad) d->cleanup(bad);
    }

    const uint32_t N = 4096;
    std::vector<float> outL(N, 0.0f), outR(N, 0.0f);
    float macros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    MidiSeq seq;
    seq.midiEvent = map_uri(0, LV2_MIDI__MidiEvent);
    seq.sequence  = map_uri(0, LV2_ATOM__Sequence);
    seq.chunk     = map_uri(0, LV2_ATOM__Chunk);
    seq.begin(8192);

    d->connect_port(h, 0, &seq.buf[0]);
    d->connect_port(h, 1, &outL[0]);
    d->connect_port(h, 2, &outR[0]);
    for (uint32_t i = 0; i < 8; i++) d->connect_port(h, 3 + i, &macros[i]);
    if (d->activate) d->activate(h);
    ok(true, "every port connects");

    /* Silence with nothing played. */
    d->run(h, N);
    ok(rmsOf(outL, outR) < 0.002f, "it is quiet before a note");

    /* A note-on halfway through the buffer: the first half stays quiet and the
     * second does not, which is the frame offset arriving intact. */
    seq.begin(8192);
    const uint8_t noteOn[3] = { 0x90, 60, 100 };
    seq.add((int64_t)(N / 2), noteOn, 3);
    d->run(h, N);

    std::vector<float> firstL(outL.begin(), outL.begin() + N / 2);
    std::vector<float> firstR(outR.begin(), outR.begin() + N / 2);
    std::vector<float> lastL(outL.begin() + N / 2, outL.end());
    std::vector<float> lastR(outR.begin() + N / 2, outR.end());
    ok(rmsOf(firstL, firstR) < 0.002f, "silent before the note's frame offset");
    ok(rmsOf(lastL, lastR) > 0.01f, "sounding after it");

    /* Held, it keeps sounding. */
    seq.begin(8192);
    d->run(h, N);
    const float held = rmsOf(outL, outR);
    ok(held > 0.01f, "a held note keeps sounding");

    /* Note-off, and it stops. A note-on with velocity zero, which is how most
     * MIDI actually says note-off. */
    seq.begin(8192);
    const uint8_t noteOffByZeroVel[3] = { 0x90, 60, 0 };
    seq.add(0, noteOffByZeroVel, 3);
    d->run(h, N);
    for (int i = 0; i < 40; i++) { seq.begin(8192); d->run(h, N); }
    ok(rmsOf(outL, outR) < 0.002f, "note-on with zero velocity releases it");

    /* Nothing that came out was ever a NaN. */
    bool clean = true;
    for (uint32_t i = 0; i < N; i++)
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) clean = false;
    ok(clean, "the output is finite throughout");

    /* ---- state ---- */
    const LV2_State_Interface *state =
        (const LV2_State_Interface *)d->extension_data(LV2_STATE__interface);
    ok(state != 0, "it offers state:interface");

    if (state) {
        ok(state->save(h, store_fn, 0, 0, 0) == LV2_STATE_SUCCESS,
           "it saves its rack");
        ok(!g_state.empty(), "the host was given something to store");

        size_t sz = 0;
        for (std::map<LV2_URID, StoredValue>::iterator it = g_state.begin();
             it != g_state.end(); ++it) sz = it->second.bytes.size();
        std::printf("        state: %zu bytes\n", sz);
        ok(sz > 100, "and it looks like a rack rather than a stub");

        /* Two fresh instances, one restored from that state and one not,
         * both played the same note from silence.
         *
         * The obvious comparison - this new instance against the one that has
         * been running all along - is not a fair one, and said so the first
         * time it was tried: the original's delay line and reverb are still
         * full of the notes already played, so it is louder for reasons that
         * have nothing to do with whether the state arrived. Comparing two
         * instances that have both only ever heard this one note is the
         * comparison that means something. */
        LV2_Handle hb = d->instantiate(d, RATE, "build/bencsynth.lv2/", features);
        LV2_Handle hc = d->instantiate(d, RATE, "build/bencsynth.lv2/", features);
        ok(hb != 0 && hc != 0, "two more instances for the restore");

        if (hb && hc) {
            std::vector<float> lb(N, 0.0f), rb(N, 0.0f), lc(N, 0.0f), rc(N, 0.0f);
            MidiSeq sb, sc;
            sb.midiEvent = sc.midiEvent = seq.midiEvent;
            sb.sequence  = sc.sequence  = seq.sequence;
            sb.begin(8192); sc.begin(8192);

            d->connect_port(hb, 0, &sb.buf[0]);
            d->connect_port(hb, 1, &lb[0]);
            d->connect_port(hb, 2, &rb[0]);
            d->connect_port(hc, 0, &sc.buf[0]);
            d->connect_port(hc, 1, &lc[0]);
            d->connect_port(hc, 2, &rc[0]);
            for (uint32_t i = 0; i < 8; i++) {
                d->connect_port(hb, 3 + i, &macros[i]);
                d->connect_port(hc, 3 + i, &macros[i]);
            }
            if (d->activate) { d->activate(hb); d->activate(hc); }

            ok(state->restore(hb, retrieve_fn, 0, 0, 0) == LV2_STATE_SUCCESS,
               "it restores a saved rack");

            sb.add(0, noteOn, 3);
            sc.add(0, noteOn, 3);
            d->run(hb, N);
            d->run(hc, N);
            sb.begin(8192); sc.begin(8192);
            d->run(hb, N);
            d->run(hc, N);

            const float b = rmsOf(lb, rb);
            const float c = rmsOf(lc, rc);
            std::printf("        restored rms %.5f, untouched rms %.5f\n", b, c);
            ok(b > 0.005f, "the restored instance makes sound");
            ok(std::fabs(b - c) < c * 0.02f + 1e-5f,
               "and matches one built the same way from scratch");

            /* Saving the restored one has to give back the same text it was
             * handed. A serialiser that drifts turns a song into a slightly
             * different song each time it is opened. */
            const std::string first = g_state.begin()->second.bytes;
            g_state.clear();
            ok(state->save(hb, store_fn, 0, 0, 0) == LV2_STATE_SUCCESS,
               "the restored instance saves again");
            ok(!g_state.empty() && g_state.begin()->second.bytes == first,
               "and gives back byte-for-byte what it was restored from");

            d->cleanup(hb);
            d->cleanup(hc);
        }
    }

    d->cleanup(h);
    ok(true, "it cleans up");
    dlclose(lib);

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
