/*
 * BENCsynth as an LV2 instrument.
 *
 * LV2 rather than VST3 because LMMS cannot load VST3 at all, and rather than
 * VST2 because Steinberg withdrew that SDK in 2018 and will not license it.
 * See docs/PLUGIN.md for the whole argument, including the part where this
 * needs an LMMS from the 1.3 line rather than the 1.2.2 that is still called
 * stable.
 *
 * This is the entire plugin. It is short because the core was built to be
 * hosted: bs::Engine takes a sample rate, an event stream carrying frame
 * offsets, and produces two channels, and none of it has ever known whether a
 * window exists. What is left is translating LV2's vocabulary into that one.
 *
 * Headless, deliberately. A rack is built in the standalone, saved as a
 * .bencsynth, and loaded here - and the state extension carries the whole rack
 * as text inside the host's project file, so a song reopens with the patch it
 * was written with. An editor is the large remaining piece and it is not this
 * file's problem.
 */

#include "bs_engine.h"
#include "bs_patchfile.h"

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <lv2/state/state.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#define BS_URI       "https://github.com/bropple/BENCsynth"
#define BS_URI_RACK  BS_URI "#rack"

/* Ports, in the order bencsynth.ttl declares them. The two lists have to agree
 * and nothing checks that they do, which is the one genuinely fragile seam in
 * an LV2 plugin. */
enum {
    PORT_MIDI = 0,
    PORT_OUT_L,
    PORT_OUT_R,
    PORT_MACRO_0
    /* ... BS_MACROS of them ... */
};

struct BencSynth {
    bs::Engine engine;

    const LV2_Atom_Sequence *midi;
    float *outL;
    float *outR;
    const float *macro[bs::BS_MACROS];

    LV2_URID_Map *map;
    LV2_URID      uridMidiEvent;
    LV2_URID      uridRack;
    LV2_URID      uridAtomString;

    /* What the host was last given or handed, so save() has something to
     * return even between edits. */
    std::string state;

    /* The last macro value seen on each port, so a value is pushed into the
     * rack when it changes rather than on every run - which would fight a
     * person turning the same knob in a running editor. */
    float macroSeen[bs::BS_MACROS];
};

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

static LV2_Handle instantiate(const LV2_Descriptor *descriptor,
                              double rate, const char *bundlePath,
                              const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)bundlePath;

    LV2_URID_Map *map = 0;
    for (int i = 0; features && features[i]; i++) {
        if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
            map = (LV2_URID_Map *)features[i]->data;
    }
    /* urid:map is declared requiredFeature, so a host without it should never
     * get here - but a plugin that segfaults on a null map is a plugin that
     * takes the host down with it. */
    if (!map) return 0;

    BencSynth *self = new (std::nothrow) BencSynth();
    if (!self) return 0;

    self->map            = map;
    self->uridMidiEvent  = map->map(map->handle, LV2_MIDI__MidiEvent);
    self->uridRack       = map->map(map->handle, BS_URI_RACK);
    self->uridAtomString = map->map(map->handle, LV2_ATOM__String);

    self->midi = 0;
    self->outL = self->outR = 0;
    for (int i = 0; i < bs::BS_MACROS; i++) {
        self->macro[i] = 0;
        self->macroSeen[i] = -1.0f;      /* nothing seen yet */
    }

    self->engine.init((float)rate);
    self->engine.buildDefaultPatch();
    self->state = bs_patch_to_string(&self->engine);
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data)
{
    BencSynth *self = (BencSynth *)instance;
    switch (port) {
    case PORT_MIDI:  self->midi = (const LV2_Atom_Sequence *)data; break;
    case PORT_OUT_L: self->outL = (float *)data; break;
    case PORT_OUT_R: self->outR = (float *)data; break;
    default:
        if (port >= PORT_MACRO_0 && port < PORT_MACRO_0 + bs::BS_MACROS)
            self->macro[port - PORT_MACRO_0] = (const float *)data;
        break;
    }
}

static void activate(LV2_Handle instance)
{
    BencSynth *self = (BencSynth *)instance;
    self->engine.panic();
}

static void cleanup(LV2_Handle instance)
{
    delete (BencSynth *)instance;
}

/* ------------------------------------------------------------------ *
 * Audio
 * ------------------------------------------------------------------ */

static void run(LV2_Handle instance, uint32_t nframes)
{
    BencSynth *self = (BencSynth *)instance;
    if (!self->outL || !self->outR) return;

    /* Macros first, and only the ones that moved. A host writes every control
     * port every run whether or not the value changed, so pushing all eight
     * unconditionally would overwrite a knob the moment anyone touched it in
     * an editor. */
    for (int i = 0; i < bs::BS_MACROS; i++) {
        if (!self->macro[i]) continue;
        const float v = *self->macro[i];
        if (v == self->macroSeen[i]) continue;
        self->macroSeen[i] = v;
        self->engine.setMacro(i, v);
    }

    /* MIDI and audio, interleaved chunk by chunk.
     *
     * The obvious arrangement - push every event, then render the buffer - is
     * wrong here, and quietly. LV2 hands over a whole buffer, the engine's
     * event offsets are relative to one render() call, and this has to render
     * in chunks because de-interleaving needs somewhere to put the result and
     * run() must not allocate. Push everything up front and a note asked for
     * at frame 2048 is compared against a chunk that only reaches 256, is
     * never due, and falls out of the end to fire at the start of the next
     * chunk instead. It sounds almost right, which is the problem.
     *
     * So the events are fed in as the buffer is walked, each one rebased onto
     * the chunk that contains it. */
    float tmp[512];
    const uint32_t CHUNK = (uint32_t)(sizeof tmp / (2 * sizeof tmp[0]));

    const LV2_Atom_Event *ev = 0;
    if (self->midi) {
        ev = lv2_atom_sequence_begin(&self->midi->body);
        if (lv2_atom_sequence_is_end(&self->midi->body, self->midi->atom.size, ev))
            ev = 0;
    }

    uint32_t done = 0;
    while (done < nframes) {
        uint32_t n = nframes - done;
        if (n > CHUNK) n = CHUNK;

        while (ev && (uint32_t)ev->time.frames < done + n) {
            const uint8_t *m = (const uint8_t *)(ev + 1);
            if (ev->body.type == self->uridMidiEvent && ev->body.size >= 2) {
                /* Rebased onto this chunk. An event stamped before the point
                 * the buffer has already reached is late rather than invalid -
                 * it lands at the start of the chunk. */
                const int64_t rel = ev->time.frames - (int64_t)done;
                const int at = rel < 0 ? 0 : (int)rel;
                const int status = m[0] & 0xf0;

                switch (status) {
                case LV2_MIDI_MSG_NOTE_ON:
                    /* A note-on with zero velocity is a note-off, and has been
                     * since running-status MIDI made that cheaper to send. */
                    if (ev->body.size >= 3 && m[2] > 0)
                        self->engine.noteOn(m[1], (float)m[2] / 127.0f, at);
                    else
                        self->engine.noteOff(m[1], at);
                    break;
                case LV2_MIDI_MSG_NOTE_OFF:
                    self->engine.noteOff(m[1], at);
                    break;
                case LV2_MIDI_MSG_BENDER:
                    if (ev->body.size >= 3) {
                        const int raw = ((int)m[2] << 7) | (int)m[1];  /* 0..16383 */
                        self->engine.setBend((float)(raw - 8192) / 8192.0f, at);
                    }
                    break;
                case LV2_MIDI_MSG_CONTROLLER:
                    if (ev->body.size < 3) break;
                    if (m[1] == LV2_MIDI_CTL_MSB_MODWHEEL)
                        self->engine.setMod((float)m[2] / 127.0f, at);
                    else if (m[1] == LV2_MIDI_CTL_SUSTAIN)
                        self->engine.setSustain(m[2] >= 64, at);
                    else if (m[1] == LV2_MIDI_CTL_ALL_NOTES_OFF ||
                             m[1] == LV2_MIDI_CTL_ALL_SOUNDS_OFF)
                        self->engine.panic();
                    break;
                default:
                    break;
                }
            }
            ev = lv2_atom_sequence_next(ev);
            if (lv2_atom_sequence_is_end(&self->midi->body, self->midi->atom.size, ev))
                ev = 0;
        }

        self->engine.render(tmp, (int)n);
        for (uint32_t i = 0; i < n; i++) {
            self->outL[done + i] = tmp[i * 2 + 0];
            self->outR[done + i] = tmp[i * 2 + 1];
        }
        done += n;
    }
}

/* ------------------------------------------------------------------ *
 * State - the whole rack, as text, inside the host's project
 * ------------------------------------------------------------------ */

static LV2_State_Status save(LV2_Handle instance,
                             LV2_State_Store_Function store,
                             LV2_State_Handle handle,
                             uint32_t flags,
                             const LV2_Feature *const *features)
{
    (void)flags;
    (void)features;
    BencSynth *self = (BencSynth *)instance;

    self->state = bs_patch_to_string(&self->engine);
    return store(handle, self->uridRack,
                 self->state.c_str(), self->state.size() + 1,
                 self->uridAtomString,
                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

static LV2_State_Status restore(LV2_Handle instance,
                                LV2_State_Retrieve_Function retrieve,
                                LV2_State_Handle handle,
                                uint32_t flags,
                                const LV2_Feature *const *features)
{
    (void)flags;
    (void)features;
    BencSynth *self = (BencSynth *)instance;

    size_t   size = 0;
    uint32_t type = 0;
    uint32_t vflags = 0;
    const void *v = retrieve(handle, self->uridRack, &size, &type, &vflags);
    if (!v || size == 0) return LV2_STATE_ERR_NO_PROPERTY;

    /* The stored value is NUL-terminated text, but a host is entitled to hand
     * back exactly the bytes it was given and nothing says the terminator has
     * to be inside them. Copying with an explicit length is the only way to be
     * sure of where it ends. */
    const std::string text((const char *)v, size);
    char status[192] = "";
    if (!bs_patch_from_string(&self->engine, text.c_str(), status,
                              (int)sizeof status))
        return LV2_STATE_ERR_BAD_TYPE;

    self->state = bs_patch_to_string(&self->engine);
    /* Whatever the macro ports are currently holding belongs to the rack that
     * just arrived, so let the next run() push them in again. */
    for (int i = 0; i < bs::BS_MACROS; i++) self->macroSeen[i] = -1.0f;
    return LV2_STATE_SUCCESS;
}

static const void *extension_data(const char *uri)
{
    static const LV2_State_Interface iface = { save, restore };
    if (std::strcmp(uri, LV2_STATE__interface) == 0) return &iface;
    return 0;
}

/* ------------------------------------------------------------------ */

static const LV2_Descriptor DESCRIPTOR = {
    BS_URI,
    instantiate,
    connect_port,
    activate,
    run,
    0,               /* deactivate - nothing to wind down */
    cleanup,
    extension_data
};

extern "C" LV2_SYMBOL_EXPORT
const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &DESCRIPTOR : 0;
}
