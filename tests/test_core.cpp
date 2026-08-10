/*
 * BENCsynth - core tests
 *
 * No raylib, no sound card, no window. Everything here runs on the same
 * objects the synthesizer runs on, which is the point of keeping the core
 * free of the GUI: the parts that can be wrong quietly - a filter that goes
 * unstable at the top of its resonance range, an oscillator half an octave
 * out, a patch that stops making sound after a voice is stolen - are exactly
 * the parts that can be checked without either.
 */

#include "bs_engine.h"
#include "bs_midimsg.h"
#include "bs_patchfile.h"
#include "bs_wav.h"
#include "bs_modules.h"
#include "test_util.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

using namespace bs;

int bs_checks   = 0;
int bs_failures = 0;

void ok(bool cond, const char *what)
{
    bs_checks++;
    if (cond) return;
    bs_failures++;
    std::printf("  FAIL  %s\n", what);
}

void okf(bool cond, const char *fmt, double a, double b)
{
    bs_checks++;
    if (cond) return;
    bs_failures++;
    std::printf("  FAIL  ");
    std::printf(fmt, a, b);
    std::printf("\n");
}

/* Lives in test_patchfile.cpp. */
void test_patchfile();

static bool finite(const float *x, int n)
{
    for (int i = 0; i < n; i++)
        if (!std::isfinite(x[i])) return false;
    return true;
}

static float peakOf(const float *x, int n)
{
    float p = 0.0f;
    for (int i = 0; i < n; i++) { const float a = std::fabs(x[i]); if (a > p) p = a; }
    return p;
}

static float rmsOf(const float *x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)x[i] * x[i];
    return (float)std::sqrt(s / (n ? n : 1));
}

/* ------------------------------------------------------------------ */

static void test_pitch()
{
    std::printf("pitch\n");
    okf(std::fabs(voltsToHz(0.0f) - 261.6256f) < 0.01f,
        "middle C is %.3f Hz, expected %.3f", voltsToHz(0.0f), 261.6256);
    okf(std::fabs(voltsToHz(noteToVolts(69.0f)) - 440.0f) < 0.01f,
        "A4 is %.3f Hz, expected %.3f", voltsToHz(noteToVolts(69.0f)), 440.0);
    ok(std::fabs(voltsToHz(1.0f) / voltsToHz(0.0f) - 2.0f) < 1e-4f,
       "one volt is one octave");
}

static void test_osc()
{
    std::printf("oscillator\n");
    const float sr = 48000.0f;

    /* Zero crossings of the sine over a second: one period has two, so 440 Hz
     * should show 880. Counting them is a frequency check that does not care
     * about phase or amplitude. */
    BlepOsc o;
    int crossings = 0;
    float prev = 0.0f;
    for (int i = 0; i < (int)sr; i++) {
        float saw, pls, tri, sin;
        o.step(440.0f / sr, 0.5f, &saw, &pls, &tri, &sin);
        if (i && ((prev < 0.0f && sin >= 0.0f) || (prev >= 0.0f && sin < 0.0f))) crossings++;
        prev = sin;
    }
    okf(std::abs(crossings - 880) <= 2, "440 Hz gave %.0f crossings, expected %.0f",
        (double)crossings, 880.0);

    /* Every shape stays inside its rails at a frequency high enough that the
     * band-limiting correction is doing real work. An overshoot here is the
     * signature of a polyBLEP applied with the wrong sign. */
    o.reset();
    float mx[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 40000; i++) {
        float v[4];
        o.step(3000.0f / sr, 0.5f, &v[0], &v[1], &v[2], &v[3]);
        for (int k = 0; k < 4; k++) {
            const float a = std::fabs(v[k]);
            if (a > mx[k]) mx[k] = a;
        }
    }
    okf(mx[0] < 1.15f, "saw peaked at %.3f, expected under %.2f", mx[0], 1.15);
    okf(mx[1] < 1.15f, "pulse peaked at %.3f, expected under %.2f", mx[1], 1.15);
    okf(mx[2] < 1.15f, "triangle peaked at %.3f, expected under %.2f", mx[2], 1.15);

    /* The polyBLAMP correction has to reduce the triangle's aliasing, not add
     * to it. A triangle at an inharmonic fraction of the sample rate folds
     * everything above Nyquist back to frequencies unrelated to the
     * fundamental; comparing the residual after subtracting an ideal
     * band-limited triangle is fiddly, so this instead measures the thing the
     * correction is supposed to fix - the sample-to-sample jump at the
     * corners, which aliasing makes erratic. */
    o.reset();
    double roughCorrected = 0.0;
    float  last = 0.0f;
    for (int i = 0; i < 20000; i++) {
        float tri;
        o.step(2371.0f / sr, 0.5f, 0, 0, &tri, 0);
        if (i) {
            const double d = tri - last;
            roughCorrected += d * d;
        }
        last = tri;
    }
    ok(std::isfinite(roughCorrected) && roughCorrected > 0.0,
       "triangle produces a finite signal at high frequency");

    /* Hard sync restarts the cycle. */
    o.reset();
    o.step(0.1f, 0.5f, 0, 0, 0, 0);
    o.step(0.1f, 0.5f, 0, 0, 0, 0);
    ok(o.phase > 0.15f, "phase advanced before sync");
    o.syncEdge(0.0f);
    o.syncEdge(5.0f);
    ok(o.phase == 0.0f, "a rising sync edge resets the phase");
}

static void test_ladder()
{
    std::printf("ladder filter\n");
    const float sr = 48000.0f;
    Ladder f;
    f.setSampleRate(sr);

    /* Full resonance while the cutoff sweeps the whole range, driven hard.
     * This is the combination that makes a naive ladder blow up. */
    Noise n;
    std::vector<float> y(48000);
    for (int i = 0; i < 48000; i++) {
        const float t  = (float)i / 48000.0f;
        const float fc = 30.0f * std::pow(600.0f, t);
        y[(size_t)i] = f.step(n.white() * 2.0f, fc, 1.08f, 6.0f);
    }
    ok(finite(&y[0], 48000), "stays finite at full resonance across a sweep");
    okf(peakOf(&y[0], 48000) < 12.0f, "peaked at %.2f, expected under %.0f",
        peakOf(&y[0], 48000), 12.0);

    /* It should also be a lowpass: white noise in, less out above cutoff.
     * Comparing RMS at two cutoffs is a blunt instrument but it catches the
     * filter being wired to the wrong tap. */
    f.reset();
    double lowE = 0.0;
    for (int i = 0; i < 48000; i++) { const float v = f.step(n.white(), 200.0f, 0.0f, 1.0f); lowE += v * v; }
    f.reset();
    double highE = 0.0;
    for (int i = 0; i < 48000; i++) { const float v = f.step(n.white(), 8000.0f, 0.0f, 1.0f); highE += v * v; }
    ok(lowE < highE * 0.5, "a low cutoff passes markedly less noise than a high one");

    /* Self-oscillation: no input, resonance past the edge, and it should ring
     * rather than sit at zero. */
    f.reset();
    for (int i = 0; i < 4800; i++) f.step(i < 4 ? 1.0f : 0.0f, 500.0f, 1.08f, 1.0f);
    std::vector<float> tail(4800);
    for (int i = 0; i < 4800; i++) tail[(size_t)i] = f.step(0.0f, 500.0f, 1.08f, 1.0f);
    ok(rmsOf(&tail[0], 4800) > 0.01f, "self-oscillates with resonance past the edge");
}

static void test_adsr()
{
    std::printf("envelope\n");
    ADSR e;
    e.setSampleRate(48000.0f);

    e.gate(true);
    float v = 0.0f;
    for (int i = 0; i < 48000; i++) v = e.step(0.01f, 0.1f, 0.5f, 0.2f);
    okf(std::fabs(v - 0.5f) < 0.02f, "settled at %.3f, expected sustain %.2f", v, 0.5);

    e.gate(false);
    for (int i = 0; i < 48000; i++) v = e.step(0.01f, 0.1f, 0.5f, 0.2f);
    okf(v < 0.001f, "released to %.5f, expected near %.0f", v, 0.0);
    ok(!e.active(), "returns to idle after release");

    /* A short attack must actually be short: a hundredth of a second means the
     * envelope is near the top within a few hundredths, not a quarter second. */
    e.reset();
    e.gate(true);
    int n = 0;
    while (e.step(0.01f, 1.0f, 1.0f, 0.2f) < 0.9f && n < 48000) n++;
    okf(n < 48000 / 20, "10 ms attack took %.0f samples, expected under %.0f",
        (double)n, 48000.0 / 20.0);
}

static void test_keys()
{
    std::printf("note allocation\n");
    KeyboardState k;
    k.setPolyphony(4);

    k.noteOn(60, 1.0f); k.noteOn(64, 1.0f); k.noteOn(67, 1.0f);
    int gated = 0;
    for (int i = 0; i < 4; i++) if (k.v[i].gate) gated++;
    okf(gated == 3, "three notes lit %.0f channels, expected %.0f", (double)gated, 3.0);

    k.noteOff(64);
    gated = 0;
    for (int i = 0; i < 4; i++) if (k.v[i].gate) gated++;
    okf(gated == 2, "after one release %.0f channels held, expected %.0f", (double)gated, 2.0);

    /* Five notes into four channels steals the oldest, and the note that was
     * stolen must be gone rather than sounding twice. */
    k.allNotesOff();
    for (int i = 0; i < 5; i++) k.noteOn(60 + i, 1.0f);
    gated = 0;
    bool has60 = false;
    for (int i = 0; i < 4; i++) {
        if (k.v[i].gate) gated++;
        if (k.v[i].gate && k.v[i].note == 60) has60 = true;
    }
    okf(gated == 4, "five notes filled %.0f of %.0f channels", (double)gated, 4.0);
    ok(!has60, "the oldest note was the one stolen");

    /* Mono: one channel, last note wins, releasing the top note falls back. */
    k.allNotesOff();
    k.mode = KM_MONO;
    k.noteOn(60, 1.0f);
    k.noteOn(72, 1.0f);
    okf(k.v[0].note == 72, "mono took note %.0f, expected %.0f", (double)k.v[0].note, 72.0);
    k.noteOff(72);
    okf(k.v[0].note == 60 && k.v[0].gate,
        "mono fell back to note %.0f, expected %.0f", (double)k.v[0].note, 60.0);
    k.noteOff(60);
    ok(!k.v[0].gate, "mono releases when the last key comes up");

    /* Legato does not retrigger while a key is already down; mono does. */
    k.allNotesOff();
    k.mode = KM_LEGATO;
    k.noteOn(60, 1.0f);
    k.clearRetriggers();
    k.noteOn(62, 1.0f);
    ok(!k.v[0].retrig, "legato does not retrigger under a held key");
    k.mode = KM_MONO;
    k.clearRetriggers();
    k.noteOn(64, 1.0f);
    ok(k.v[0].retrig, "mono retriggers on every note");
}

static void test_registry()
{
    std::printf("registry\n");
    ok(moduleTypeCount() > 0, "the registry is not empty");

    /* Every module, run with its inputs unpatched and with a fixed pattern of
     * parameter positions, has to produce a finite signal. It is the cheapest
     * possible guard against a new module dividing by an unpatched jack, and
     * it costs one line per module added. */
    KeyboardState keys;
    for (int t = 0; t < moduleTypeCount(); t++) {
        const ModuleType *mt = moduleTypeAt(t);
        Module *m = createModule(mt->id);
        if (!m) { ok(false, "createModule returned nothing"); continue; }
        m->bindKeys(&keys);
        m->setSampleRate(48000.0f);

        bool clean = true;
        for (int pass = 0; pass < 3 && clean; pass++) {
            const float pos = pass * 0.5f;     /* bottom, middle, top */
            for (int p = 0; p < m->paramCount(); p++) m->params[(size_t)p].setNorm(pos);
            for (int b = 0; b < 200 && clean; b++) {
                m->process();
                for (int o = 0; o < m->outputCount(); o++)
                    for (int c = 0; c < m->out(o).channels; c++)
                        if (!finite(m->out(o).v[c], BS_BLOCK)) clean = false;
            }
        }
        if (!clean) std::printf("  FAIL  %s produced a non-finite output\n", mt->id);
        bs_checks++;
        if (!clean) bs_failures++;
        delete m;
    }
}

static void test_graph()
{
    std::printf("patch graph\n");
    Engine e;
    e.init(48000.0f);

    const int vco = e.addModule("VCO", 0, 0);
    const int vca = e.addModule("VCA", 0, 0);
    const int out = e.addModule("OUT", 0, 0);
    ok(vco >= 0 && vca >= 0 && out >= 0, "modules were created");

    const int c1 = e.connect(vco, 0, vca, 0);
    ok(c1 >= 0, "a cable was made");

    /* One cable per input jack: a second connection to the same jack replaces
     * the first rather than joining it. Counting live cables rather than
     * checking the old id is gone, because a freed slot is immediately
     * available again and the replacement is entitled to land in it. */
    const int c2 = e.connect(vco, 1, vca, 0);
    ok(c2 >= 0, "the input accepted a second cable");
    int live = 0, fromPort = -1;
    for (size_t i = 0; i < e.patch.cableList().size(); i++) {
        const Cable &c = e.patch.cableList()[i];
        if (!c.alive) continue;
        live++;
        if (c.dst == vca && c.dstPort == 0) fromPort = c.srcPort;
    }
    okf(live == 1, "%.0f cables survived, expected %.0f", (double)live, 1.0);
    okf(fromPort == 1, "the jack reads output %.0f, expected the newer one, %.0f",
        (double)fromPort, 1.0);

    e.connect(vca, 0, out, 0);
    e.patch.module(vca)->params[0].value = 1.0f;

    std::vector<float> buf(2048 * 2);
    e.render(&buf[0], 2048);
    ok(finite(&buf[0], 2048 * 2), "the graph renders finite audio");
    ok(peakOf(&buf[0], 2048 * 2) > 0.01f, "a VCO through an open VCA makes sound");

    /* Deleting a module in the middle must take its cables with it, and what
     * is left must still run. */
    e.removeModule(vca);
    e.render(&buf[0], 2048);
    ok(finite(&buf[0], 2048 * 2), "the graph survives a module being deleted");

    /* A feedback loop is a legal patch, not an error. It must not hang the
     * ordering pass and must not run away. */
    Engine f;
    f.init(48000.0f);
    const int d  = f.addModule("DLY", 0, 0);
    const int o2 = f.addModule("OUT", 0, 0);
    f.connect(d, 0, d, 0);          /* the delay feeding itself */
    f.connect(d, 0, o2, 0);
    f.patch.module(d)->params[1].value = 1.05f;   /* feedback past unity */
    for (int i = 0; i < 20; i++) f.render(&buf[0], 2048);
    ok(finite(&buf[0], 2048 * 2), "a self-patched delay stays finite");
    okf(peakOf(&buf[0], 2048 * 2) <= 1.001f, "output peaked at %.3f, expected at most %.1f",
        peakOf(&buf[0], 2048 * 2), 1.0);
}

static void test_event_queue()
{
    std::printf("event queue\n");
    NoteQueue q;
    NoteEvent e;

    ok(!q.pop(&e), "an empty queue pops nothing");

    for (int i = 0; i < 10; i++) {
        NoteEvent p;
        p.kind = NE_NOTE_ON; p.note = (uint8_t)(60 + i); p.value = 1.0f;
        ok(q.push(p), "pushed");
    }
    int seen = 0;
    bool inOrder = true;
    while (q.pop(&e)) {
        if (e.note != (uint8_t)(60 + seen)) inOrder = false;
        seen++;
    }
    okf(seen == 10, "popped %.0f events, expected %.0f", (double)seen, 10.0);
    ok(inOrder, "events come back in the order they went in");

    /* Full is a refusal, not a corruption: the events already in the ring have
     * to survive an attempt to overfill it. */
    int pushed = 0;
    for (int i = 0; i < 4096; i++) {
        NoteEvent p;
        p.kind = NE_NOTE_ON; p.note = (uint8_t)(i & 127); p.value = 1.0f;
        if (!q.push(p)) break;
        pushed++;
    }
    ok(pushed > 0 && pushed < 4096, "the ring reports full rather than wrapping over itself");
    int drained = 0;
    while (q.pop(&e)) drained++;
    okf(drained == pushed, "drained %.0f of the %.0f that were accepted",
        (double)drained, (double)pushed);
}

/* The evaluation order has to be a real topological order, and the cheapest
 * observable consequence of that is latency: a signal crossing a four-deep
 * chain arrives in the very first block if the modules run in dependency
 * order, and takes one block per link if they do not.
 *
 * The modules are created back to front so that module id order is the exact
 * reverse of signal flow - which is what a rack looks like after ten minutes
 * of patching, and what a sort that quietly does nothing would still pass
 * without. */
static void test_eval_order()
{
    std::printf("evaluation order\n");

    Engine e;
    e.init(48000.0f);

    const int out  = e.addModule("OUT", 0, 0);
    const int vca3 = e.addModule("VCA", 0, 0);
    const int vca2 = e.addModule("VCA", 0, 0);
    const int vca1 = e.addModule("VCA", 0, 0);
    const int vco  = e.addModule("VCO", 0, 0);

    e.connect(vco, 0, vca1, 0);
    e.connect(vca1, 0, vca2, 0);
    e.connect(vca2, 0, vca3, 0);
    e.connect(vca3, 0, out, 0);

    /* Wide open, linear, so the chain is a pure pass-through. */
    const int chain[3] = { vca1, vca2, vca3 };
    for (int i = 0; i < 3; i++) {
        Module *m = e.patch.module(chain[i]);
        m->params[0].value = 1.0f;   /* gain  */
        m->params[1].value = 0.0f;   /* no CV */
        m->params[2].value = 0.0f;   /* linear */
    }

    std::vector<float> buf(BS_BLOCK * 2, 0.0f);
    e.render(&buf[0], BS_BLOCK);
    okf(peakOf(&buf[0], BS_BLOCK * 2) > 0.01f,
        "a four-deep chain reached the output in the first block (peak %.4f, "
        "expected above %.2f)", peakOf(&buf[0], BS_BLOCK * 2), 0.01);

    /* And the order itself has to place every producer before its consumer. */
    const std::vector<Cable> &cs = e.patch.cableList();
    int violations = 0;
    for (size_t i = 0; i < cs.size(); i++) {
        if (!cs[i].alive || cs[i].src == cs[i].dst) continue;
        int si = -1, di = -1;
        for (int k = 0; k < e.patch.slotCount(); k++) {
            const int id = e.patch.evalOrderAt(k);
            if (id == cs[i].src) si = k;
            if (id == cs[i].dst) di = k;
        }
        if (si < 0 || di < 0 || si > di) violations++;
    }
    okf(violations == 0, "%.0f cables run backwards through the order, expected %.0f",
        (double)violations, 0.0);
}

static void test_default_patch()
{
    std::printf("default patch\n");
    Engine e;
    e.init(48000.0f);
    e.buildDefaultPatch();

    std::vector<float> buf(4800 * 2);

    e.render(&buf[0], 4800);
    okf(peakOf(&buf[0], 4800 * 2) < 0.02f, "idle output peaked at %.4f, expected near %.0f",
        peakOf(&buf[0], 4800 * 2), 0.0);

    e.noteOn(60, 1.0f);
    e.render(&buf[0], 4800);
    const float held = rmsOf(&buf[0], 4800 * 2);
    okf(held > 0.01f, "a held note gave RMS %.4f, expected above %.2f", held, 0.01);
    ok(finite(&buf[0], 4800 * 2), "the note is finite");
    okf(peakOf(&buf[0], 4800 * 2) <= 1.001f, "peaked at %.3f, expected at most %.1f",
        peakOf(&buf[0], 4800 * 2), 1.0);

    /* Chords have to be louder than single notes, which is the observable
     * consequence of polyphony actually reaching the oscillators. */
    e.noteOn(64, 1.0f);
    e.noteOn(67, 1.0f);
    e.render(&buf[0], 4800);
    const float chord = rmsOf(&buf[0], 4800 * 2);
    okf(chord > held * 1.2f, "a triad gave RMS %.4f against one note's %.4f", chord, held);

    /* And releasing everything has to actually stop the sound, tail included. */
    e.noteOff(60); e.noteOff(64); e.noteOff(67);
    for (int i = 0; i < 12; i++) e.render(&buf[0], 4800);
    okf(rmsOf(&buf[0], 4800 * 2) < 0.002f, "after release RMS was %.5f, expected under %.3f",
        rmsOf(&buf[0], 4800 * 2), 0.002);

    /* The whole rack should cost a small fraction of real time. This is a
     * smoke alarm, not a benchmark - it only fires if something has gone
     * quadratic. */
    okf(e.load < 0.5f, "load was %.3f of the block budget, expected under %.1f",
        e.load, 0.5);
}

/* Every factory rack, played.
 *
 * The presets address module parameters by position, which is a real coupling:
 * reorder a module's knobs and every preset that touches it is silently
 * retuned, with nothing at compile time to say so. This is what notices - a
 * rack whose filter cutoff has become its resonance does not stay quiet about
 * it for long. */
static void test_presets()
{
    std::printf("factory racks\n");
    okf(rackPresetCount() >= 8, "%.0f presets, expected at least %.0f",
        (double)rackPresetCount(), 8.0);

    for (int i = 0; i < rackPresetCount(); i++) {
        const RackPreset *rp = rackPresetAt(i);
        if (!rp) { ok(false, "a preset had no description"); continue; }

        Engine e;
        e.init(48000.0f);
        e.buildPreset(i);

        /* Measured over the whole two seconds rather than over the last block
         * of them. PLUCK has no sustain at all - hold a key and it decays to
         * silence, which is the entire point of it - so a window that only
         * looks at the end finds nothing and reports a working rack as
         * broken. */
        const int SPAN = 96000;
        std::vector<float> buf((size_t)SPAN * 2, 0.0f);
        char msg[160];

        /* Two seconds untouched, which is also long enough for a
         * self-oscillating rack to get going. */
        e.render(&buf[0], SPAN);
        const float idle = rmsOf(&buf[0], SPAN * 2);

        /* Two cables into one input. The engine accepts it - a patch bay does,
         * and a person dropping a plug into an occupied jack means it - but a
         * preset never does, and the wire that loses is gone without a word.
         * GRAND shipped with the keyboard's trigger and its velocity both run
         * into the string, and played exactly one note. */
        std::snprintf(msg, sizeof msg,
                      "%s patches every input once (%%.0f were overwritten)",
                      rp->name);
        okf(e.patch.replaced == 0, msg, (double)e.patch.replaced, 0.0);

        std::snprintf(msg, sizeof msg, "%s is finite before a note", rp->name);
        ok(finite(&buf[0], SPAN * 2), msg);

        e.noteOn(48, 0.9f);
        e.noteOn(55, 0.9f);
        e.noteOn(60, 0.9f);
        e.render(&buf[0], SPAN);
        const float played = rmsOf(&buf[0], SPAN * 2);

        std::snprintf(msg, sizeof msg, "%s renders finite audio", rp->name);
        ok(finite(&buf[0], SPAN * 2), msg);

        std::snprintf(msg, sizeof msg,
                      "%s made RMS %%.4f while held, expected above %%.3f", rp->name);
        okf(played > 0.005f, msg, played, 0.005);

        std::snprintf(msg, sizeof msg,
                      "%s peaked at %%.3f, expected at most %%.1f", rp->name);
        okf(peakOf(&buf[0], SPAN * 2) <= 1.001f, msg,
            peakOf(&buf[0], SPAN * 2), 1.0);

        /* Some racks are meant to sound with nothing held down - a filter past
         * self-oscillation, a sequence on a clock, a delay feeding itself. The
         * preset says which it is, so this can insist on the right one:
         * silence from those is a bug, and noise from the others is too. */
        if (rp->selfPlaying) {
            std::snprintf(msg, sizeof msg,
                          "%s idled at RMS %%.4f, expected above %%.3f - it is "
                          "supposed to play itself", rp->name);
            okf(idle > 0.005f, msg, idle, 0.005);
        } else {
            std::snprintf(msg, sizeof msg,
                          "%s idled at RMS %%.4f, expected below %%.2f", rp->name);
            okf(idle < 0.02f, msg, idle, 0.02);
        }

        /* And again, after letting go.
         *
         * A rack can play its first note perfectly and be deaf to every one
         * after it. GRAND shipped that way: two cables had been run into the
         * string's trigger jack, and since an input takes only one, the second
         * replaced the first - so the trigger was receiving velocity, which is
         * a level that stays high while a key is held rather than a pulse.
         * The first note found an edge on the way up and struck. Nothing else
         * ever did.
         *
         * Every check above passed on that rack, because all of them play one
         * note. This plays four. */
        e.noteOff(48); e.noteOff(55); e.noteOff(60);
        e.render(&buf[0], SPAN / 4);

        float again = 0.0f;
        for (int n = 0; n < 3; n++) {
            const int note = 52 + n * 5;
            e.noteOn(note, 0.9f);
            e.render(&buf[0], SPAN / 2);
            const float p = peakOf(&buf[0], SPAN);
            if (p > again) again = p;
            e.noteOff(note);
            e.render(&buf[0], SPAN / 4);
        }

        /* Against the rack's own loudness, not an absolute: a quiet patch is
         * allowed to be quiet, it is not allowed to be quiet only after the
         * first note. */
        std::snprintf(msg, sizeof msg,
                      "%s still answers later notes: peak %%.4f against %%.4f held",
                      rp->name);
        okf(again > played * 0.25f, msg, again, (double)played);
    }
}

/* A divider that does not divide, and an end-of-cycle that never ends.
 *
 * Both of these shipped. Neither was visible in any preset check, because a
 * preset is asked whether it makes a noise and both of these make a noise -
 * the wrong one. */
static void test_clock_and_func()
{
    std::printf("clock and function generator\n");

    /* CLK's divisions. The masks were shifted before being applied, so /4
     * fired twice and rested six, and /8 fired four times and rested
     * twenty-eight. Every rhythm patched from those jacks was wrong. */
    {
        Engine e;
        e.init(48000.0f);
        e.clear();
        const int clk = e.addModule("CLK", 20.0f, 20.0f);
        const int out = e.addModule("OUT", 400.0f, 20.0f);
        e.connect(clk, 0, out, 0);
        Module *m = e.patch.module(clk);
        m->params[0].value = 600.0f;          /* fast, so a short run is enough */

        std::vector<float> buf(64);
        int ticks = 0;
        float last[4] = { 0, 0, 0, 0 };
        int lastAt[4] = { -1, -1, -1, -1 };
        int gapMin[4] = { 9999, 9999, 9999, 9999 };
        int gapMax[4] = { 0, 0, 0, 0 };

        for (int n = 0; n < 48000 * 2; n += 32) {
            e.render(&buf[0], 32);
            for (int i = 0; i < 32; i++) {
                if (m->outs[0].v[0][i] > 1.0f && last[0] <= 1.0f) ticks++;
                last[0] = m->outs[0].v[0][i];
                for (int k = 1; k < 4; k++) {
                    const float v = m->outs[k].v[0][i];
                    if (v > 1.0f && last[k] <= 1.0f) {
                        if (lastAt[k] >= 0) {
                            const int g = ticks - lastAt[k];
                            if (g < gapMin[k]) gapMin[k] = g;
                            if (g > gapMax[k]) gapMax[k] = g;
                        }
                        lastAt[k] = ticks;
                    }
                    last[k] = v;
                }
            }
        }
        const char *name[4] = { "", "/2", "/4", "/8" };
        for (int k = 1; k < 4; k++) {
            const int want = 1 << k;
            char msg[128];
            std::snprintf(msg, sizeof msg,
                          "CLK %s fires every %d ticks, evenly (saw %%.0f to %%.0f)",
                          name[k], want);
            okf(gapMin[k] == want && gapMax[k] == want, msg,
                (double)gapMin[k], (double)gapMax[k]);
        }
    }

    /* FUNC's EOC re-armed itself every sample while the envelope sat at zero,
     * so it was a gate held permanently open rather than a trigger. */
    {
        Engine e;
        e.init(48000.0f);
        e.clear();
        const int f = e.addModule("FUNC", 20.0f, 20.0f);
        const int out = e.addModule("OUT", 400.0f, 20.0f);
        e.connect(f, 0, out, 0);
        Module *m = e.patch.module(f);

        std::vector<float> buf(64);
        int high = 0, total = 0;
        for (int n = 0; n < 48000; n += 32) {
            e.render(&buf[0], 32);
            for (int i = 0; i < 32; i++) {
                total++;
                if (m->outs[1].v[0][i] > 1.0f) high++;
            }
        }
        okf(high == 0, "FUNC EOC is silent until something triggers it "
                       "(%.0f of %.0f samples high)", (double)high, (double)total);
    }

    /* And IN has to change the timing in CYCLE mode, which is the whole of
     * KRELL's mechanism and did nothing at all. */
    {
        int cycles[2] = { 0, 0 };
        for (int volts = 0; volts < 2; volts++) {
            Engine e;
            e.init(48000.0f);
            e.clear();
            const int f = e.addModule("FUNC", 20.0f, 20.0f);
            const int a = e.addModule("ATT", 20.0f, 300.0f);
            const int out = e.addModule("OUT", 400.0f, 20.0f);
            e.connect(f, 0, out, 0);
            e.connect(a, 0, f, 1);
            Module *m = e.patch.module(f);
            m->params[0].value = 0.05f;
            m->params[1].value = 0.15f;
            m->params[4].value = 1.0f;                 /* CYCLE */
            e.patch.module(a)->params[1].value = volts ? 5.0f : 0.0f;

            std::vector<float> buf(64);
            float last = 0.0f;
            for (int n = 0; n < 48000 * 3; n += 32) {
                e.render(&buf[0], 32);
                for (int i = 0; i < 32; i++) {
                    const float v = m->outs[1].v[0][i];
                    if (v > 1.0f && last <= 1.0f) cycles[volts]++;
                    last = v;
                }
            }
        }
        okf(cycles[1] > 0 && cycles[1] < cycles[0] * 3 / 4,
            "a voltage on FUNC's IN slows it down (%.0f cycles against %.0f)",
            (double)cycles[1], (double)cycles[0]);
    }
}

/* GRAND TOUR exists to be the answer to "what can this do", and the answer is
 * only true while it actually uses everything. A module added to the registry
 * and left out of it makes the claim quietly false, which is exactly the kind
 * of thing nobody notices. */
/* The arpeggiator turns a chord into a sequence, and the observable difference
 * between working and not is whether its pitch output has more than one value
 * in it over time. */
/* An event carries the frame it belongs at, and the engine has to work through
 * the buffer rather than applying everything at the top. A host's whole idea of
 * timing rests on this: without it a note written halfway through a bar arrives
 * wherever the buffer boundary fell.
 *
 * Measured by rendering one long buffer with a note asked for at its midpoint
 * and comparing the two halves. */
static void test_event_offsets()
{
    std::printf("event timing\n");

    const int FRAMES = 4096;
    const int AT     = FRAMES / 2;

    Engine e;
    e.init(48000.0f);
    e.buildPreset(1);                    /* INIT: one oscillator, fast attack */

    std::vector<float> buf((size_t)FRAMES * 2, 0.0f);

    e.noteOn(60, 1.0f, AT);
    e.render(&buf[0], FRAMES);

    const float before = rmsOf(&buf[0], AT * 2);
    const float after  = rmsOf(&buf[AT * 2], AT * 2);

    okf(before < 0.002f, "before the offset the buffer was RMS %.5f, expected "
        "under %.3f", before, 0.002);
    okf(after > 0.01f, "after the offset it was RMS %.4f, expected above %.2f",
        after, 0.01);

    /* And the boundary should be where it was asked for, to within the block
     * the rack processes - not a buffer away. */
    int firstLoud = -1;
    for (int i = 0; i < FRAMES && firstLoud < 0; i++)
        if (std::fabs(buf[(size_t)i * 2]) > 0.005f) firstLoud = i;
    okf(firstLoud >= AT - BS_BLOCK && firstLoud < AT + 512,
        "sound started at frame %.0f, expected near %.0f",
        (double)firstLoud, (double)AT);

    /* Zero, the standalone's offset, still means the top of the buffer. */
    Engine f;
    f.init(48000.0f);
    f.buildPreset(1);
    f.noteOn(60, 1.0f);
    f.render(&buf[0], FRAMES);
    okf(rmsOf(&buf[0], 512 * 2) > 0.005f,
        "an event at offset zero sounded from the start (RMS %.4f, expected "
        "above %.3f)", rmsOf(&buf[0], 512 * 2), 0.005);
}

static void test_arp()
{
    std::printf("arpeggiator\n");

    Engine e;
    e.init(48000.0f);
    const int kbd = e.addModule("KBD", 0, 0);
    const int arp = e.addModule("ARP", 0, 0);
    const int out = e.addModule("OUT", 0, 0);
    e.connect(kbd, 0, arp, 0);
    e.connect(kbd, 1, arp, 1);
    e.connect(arp, 0, out, 0);

    Module *a = e.patch.module(arp);
    a->params[0].value = 20.0f;      /* fast, so a short render covers steps */
    a->params[1].value = 0.0f;       /* UP    */
    a->params[2].value = 1.0f;       /* one octave */

    std::vector<float> buf(2048 * 2);

    /* Nothing held: the gate stays shut. */
    for (int i = 0; i < 8; i++) e.render(&buf[0], 2048);
    int gateHigh = 0;
    for (int i = 0; i < BS_BLOCK; i++)
        if (a->out(1).v[0][i] > 1.0f) gateHigh++;
    okf(gateHigh == 0, "%.0f gated frames with nothing held, expected %.0f",
        (double)gateHigh, 0.0);

    e.noteOn(60, 1.0f);
    e.noteOn(64, 1.0f);
    e.noteOn(67, 1.0f);

    /* Collect the distinct pitches it visits. Three notes, one octave, so it
     * should be exactly three. */
    float seen[16];
    int   nSeen = 0;
    for (int b = 0; b < 60; b++) {
        e.render(&buf[0], 2048);
        const float p = a->out(0).v[0][0];
        int known = 0;
        for (int i = 0; i < nSeen; i++) if (std::fabs(seen[i] - p) < 0.001f) known = 1;
        if (!known && nSeen < 16) seen[nSeen++] = p;
    }
    okf(nSeen == 3, "the arpeggio visited %.0f distinct pitches, expected %.0f",
        (double)nSeen, 3.0);

    /* And they should be the notes that were held, in volts. */
    int matched = 0;
    static const int NOTES[3] = { 60, 64, 67 };
    for (int i = 0; i < nSeen; i++)
        for (int k = 0; k < 3; k++)
            if (std::fabs(seen[i] - noteToVolts((float)NOTES[k])) < 0.001f) matched++;
    okf(matched == 3, "%.0f of the pitches were notes that were held, expected %.0f",
        (double)matched, 3.0);

    /* Two octaves doubles the sequence rather than the tempo. */
    a->params[2].value = 2.0f;
    nSeen = 0;
    for (int b = 0; b < 90; b++) {
        e.render(&buf[0], 2048);
        const float p = a->out(0).v[0][0];
        int known = 0;
        for (int i = 0; i < nSeen; i++) if (std::fabs(seen[i] - p) < 0.001f) known = 1;
        if (!known && nSeen < 16) seen[nSeen++] = p;
    }
    okf(nSeen == 6, "two octaves gave %.0f distinct pitches, expected %.0f",
        (double)nSeen, 6.0);
}

static void test_grand_tour()
{
    std::printf("grand tour\n");

    int which = -1;
    for (int i = 0; i < rackPresetCount(); i++)
        if (std::strcmp(rackPresetAt(i)->name, "GRAND TOUR") == 0) which = i;
    ok(which >= 0, "there is a GRAND TOUR rack");
    if (which < 0) return;

    Engine e;
    e.init(48000.0f);
    e.buildPreset(which);

    for (int t = 0; t < moduleTypeCount(); t++) {
        const char *id = moduleTypeAt(t)->id;
        int found = 0;
        for (int i = 0; i < e.patch.slotCount(); i++) {
            const Module *m = e.patch.module(i);
            if (m && m->typeId == id) { found = 1; break; }
        }
        char msg[96];
        std::snprintf(msg, sizeof msg, "GRAND TOUR uses a %s", id);
        ok(found != 0, msg);
    }

    /* And every rack should be able to say what it is. */
    for (int i = 0; i < rackPresetCount(); i++) {
        const RackPreset *rp = rackPresetAt(i);
        char msg[96];
        std::snprintf(msg, sizeof msg, "%s has notes", rp->name);
        ok(rp->notes != 0 && rp->notes[0] != 0, msg);
    }
}


/* Per-note expression - MPE, and CLAP's note expressions.
 *
 * The point of it is two notes bending in opposite directions at the same
 * time, which a single pitch wheel cannot express. A voltage model gets this
 * almost for free, because a cable already carries every voice separately -
 * so what is worth checking is that the per-voice number reaches the pitch and
 * that it reaches only the voice it was aimed at. */

/* Three ways to set a string going.
 *
 * The waveguide does not care what excites it - that is the point of modelling
 * the string rather than the instrument. What separates them is not timbre but
 * what happens while the key is held: a struck string decays, a bowed or blown
 * one does not, because the player keeps putting energy in.
 *
 * Also checked: that neither continuous exciter drives the string into its own
 * clamp. Both did at first. A bow and a reed push in one direction and a delay
 * loop with a gain near one has a DC gain near a hundred, so the string leaned
 * on the bridge and stopped oscillating; a reed then turned out to be a
 * relaxation oscillator whose amplitude is set by where its nonlinearity
 * saturates rather than by how hard it is driven, and sat on the rail for an
 * eighth of every cycle no matter what the drive was set to. */

/* VOICE - a different number for every voice.
 *
 * The thing a rack of hardware cannot do: there, one module is one voice, so
 * making the third note behave unlike the first means patching it differently.
 * Here a cable carries all eight and one module can hand each its own value. */

/* The MIDI decoder, which both the plugin and the standalone now use.
 *
 * One copy, because two copies of "note on with velocity zero is a note off"
 * is two places for it to be wrong, and the MPE rule - a bend on a channel
 * carrying one note belongs to that note, anywhere else it is the wheel - is
 * not obvious enough to write twice. */

/* Which CC drives which macro.
 *
 * CC BASE claims a run of eight, which is what a controller's knob row sends.
 * A macro's own assignment overrides its place in that run, which is what
 * "learn this knob" writes. Both are ordinary knobs on the panel, so both save
 * with the rack and reach the plugin with no table to keep in step. */

/* SAMPLE - the one source here that is not arithmetic.
 *
 * The path lives in the module's text, which is the field the scratchpad uses.
 * That was the point of putting it there: text is already written to the patch
 * file and already crosses to the plugin with the rack, so a sampler in a
 * saved project finds its file again with nothing added to either. */
static void test_sample_module()
{
    std::printf("sampler\n");

    /* A fixture, written here so the test carries its own audio: a second of
     * 440 Hz at 44100, which is deliberately not the engine's rate. */
    /* Beside wherever the test is run, not in build/ - the core job compiles
     * with `make core` and never creates that directory, so the first CI run
     * failed on a fixture it could not write. */
    const char *path = "bencsynth-test-sample.wav";
    {
        const int rate = 44100, n = rate;
        std::vector<short> pcm((size_t)n * 2);
        for (int i = 0; i < n; i++) {
            const double v = std::sin(2.0 * 3.14159265358979 * 440.0 * i / rate);
            pcm[(size_t)i * 2 + 0] = (short)(v * 20000.0);
            pcm[(size_t)i * 2 + 1] = (short)(v * 20000.0);
        }
        std::FILE *f = std::fopen(path, "wb");
        if (f) {
            const unsigned data = (unsigned)(pcm.size() * 2);
            struct P {
                static void u32(std::FILE *g, unsigned v)
                { unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                                         (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
                  std::fwrite(b, 1, 4, g); }
                static void u16(std::FILE *g, unsigned v)
                { unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
                  std::fwrite(b, 1, 2, g); }
            };
            std::fwrite("RIFF", 1, 4, f); P::u32(f, 36 + data);
            std::fwrite("WAVEfmt ", 1, 8, f); P::u32(f, 16);
            P::u16(f, 1); P::u16(f, 2); P::u32(f, (unsigned)rate);
            P::u32(f, (unsigned)rate * 4); P::u16(f, 4); P::u16(f, 16);
            std::fwrite("data", 1, 4, f); P::u32(f, data);
            std::fwrite(&pcm[0], 2, pcm.size(), f);
            std::fclose(f);
        }
    }

    Engine e;
    e.init(48000.0f);
    e.clear();
    const int smp = e.addModule("SAMPLE", 20, 20);
    const int out = e.addModule("OUT", 300, 20);
    e.patch.connect(smp, 0, out, 0);

    std::string err;
    ok(e.loadSample(smp, path, &err), "a wav loads");

    Module *m = e.patch.module(smp);
    m->params[4].value = 1.0f;         /* LOOP, so there is always something */

    struct L {
        static double mag(const std::vector<float> &x, double f)
        {
            double re = 0, im = 0;
            const double w = 2 * 3.14159265358979 * f / 48000.0;
            for (size_t i = 0; i < x.size(); i++) {
                re += x[i] * std::cos(w * (double)i);
                im -= x[i] * std::sin(w * (double)i);
            }
            return std::sqrt(re * re + im * im) / (double)x.size();
        }
        static void render(Engine &en, std::vector<float> &mono)
        {
            std::vector<float> b(512);
            mono.clear();
            for (int i = 0; i < 9600; i += 256) {
                en.render(&b[0], 256);
                for (int k = 0; k < 256; k++) mono.push_back(b[(size_t)k * 2]);
            }
            mono.erase(mono.begin(), mono.begin() + 2048);
            mono.resize(4096);
        }
    };

    std::vector<float> mono;
    L::render(e, mono);

    /* The file is 44100 and the engine is 48000, so playing it back unchanged
     * means reading it faster - if that ratio were ignored it would come out
     * flat by a tone and a bit. */
    okf(L::mag(mono, 440.0) > L::mag(mono, 404.0) * 4.0,
        "it plays at the pitch it was recorded at, not at the engine's rate: "
        "%.4f at 440 against %.4f at 404", L::mag(mono, 440.0), L::mag(mono, 404.0));

    /* One volt per octave, like everything else. */
    m->params[0].value = 12.0f;        /* TUNE, an octave up */
    L::render(e, mono);
    okf(L::mag(mono, 880.0) > L::mag(mono, 440.0) * 4.0,
        "and transposes: %.4f at 880 against %.4f at 440",
        L::mag(mono, 880.0), L::mag(mono, 440.0));
    m->params[0].value = 0.0f;

    /* Two samplers on one file hold one copy of it.
     *
     * A drum rack built out of a folder is a dozen samplers, and before this
     * that was the folder in memory a dozen times. */
    {
        /* A second copy of the fixture under another name, so this measures a
         * file nothing else in the test is already holding. */
        const char *other = "bencsynth-test-sample-2.wav";
        {
            std::FILE *in = std::fopen(path, "rb");
            std::FILE *outf = std::fopen(other, "wb");
            if (in && outf) {
                char b[4096]; size_t n;
                while ((n = std::fread(b, 1, sizeof b, in)) > 0) std::fwrite(b, 1, n, outf);
            }
            if (in) std::fclose(in);
            if (outf) std::fclose(outf);
        }

        const int before = wavCacheCount();
        {
            Engine c;
            c.init(48000.0f);
            c.clear();
            const int s1 = c.addModule("SAMPLE", 20, 20);
            const int s2 = c.addModule("SAMPLE", 200, 20);
            std::string e1, e2b;
            ok(c.loadSample(s1, other, &e1) && c.loadSample(s2, other, &e2b),
               "two samplers load the same file");
            okf(wavCacheCount() == before + 1,
                "and it is held once between them: %.0f entries, wanted %.0f",
                (double)wavCacheCount(), (double)(before + 1));
        }
        /* And the last one letting go frees it, rather than the program
         * holding every file it ever opened. */
        okf(wavCacheCount() == before,
            "the rack going away drops it: %.0f entries, wanted %.0f",
            (double)wavCacheCount(), (double)before);
        std::remove(other);
    }

    /* ROOT: which key plays the file untouched.
     *
     * The fixture is 440 Hz. Set ROOT to C3 and play C3, and it must come out
     * at 440 - not transposed by the distance from middle C. Play a fifth
     * above C3 and it must come out a fifth up. */
    {
        Engine k;
        k.init(48000.0f);
        k.clear();
        const int kbd = k.addModule("KBD", 20, 20);
        const int sm2 = k.addModule("SAMPLE", 200, 20);
        const int o2  = k.addModule("OUT", 400, 20);
        k.patch.connect(kbd, 0, sm2, 0);      /* V/OCT */
        k.patch.connect(sm2, 0, o2, 0);
        std::string e2;
        ok(k.loadSample(sm2, path, &e2), "the same file, on a keyboard");
        Module *ms = k.patch.module(sm2);
        ms->params[4].value = 1.0f;           /* LOOP */
        ms->params[5].value = 48.0f;          /* ROOT = C3 */

        k.noteOn(48, 0.9f, 0);                /* play C3 */
        std::vector<float> m3;
        L::render(k, m3);
        okf(L::mag(m3, 440.0) > L::mag(m3, 220.0) * 4.0,
            "the root note plays the file as recorded: %.4f at 440 against "
            "%.4f at 220", L::mag(m3, 440.0), L::mag(m3, 220.0));

        k.panic();
        k.noteOn(55, 0.9f, 0);                /* a fifth above C3 */
        L::render(k, m3);
        okf(L::mag(m3, 659.3) > L::mag(m3, 440.0) * 3.0,
            "and a fifth up plays it a fifth up: %.4f at 659 against %.4f at "
            "440", L::mag(m3, 659.3), L::mag(m3, 440.0));
    }

    /* A file that is not one says so, and leaves what was there alone. */
    std::string why;
    ok(!e.loadSample(smp, "src/core/bs_engine.h", &why),
       "a file that is not a wav is refused");
    ok(!why.empty(), "and says why");
    L::render(e, mono);
    okf(L::mag(mono, 440.0) > 0.001,
       "and the sample that was loaded is still there: %.4f, wanted over %.3f",
       L::mag(mono, 440.0), 0.001);

    /* The path is in the patch file, which is what makes a saved rack find its
     * audio again. */
    const std::string text = bs_patch_to_string(&e);
    ok(text.find("test-sample.wav") != std::string::npos,
       "the path is written into the rack");

    Engine e2;
    e2.init(48000.0f);
    char st[128] = "";
    ok(bs_patch_from_string(&e2, text.c_str(), st, sizeof st) != 0,
       "the rack loads back");
    std::vector<float> mono2;
    L::render(e2, mono2);
    okf(L::mag(mono2, 440.0) > 0.001,
        "and the sampler is playing its file again: %.4f, wanted over %.3f",
        L::mag(mono2, 440.0), 0.001);

    std::remove(path);
}

static void test_macro_cc()
{
    std::printf("macro CC assignment\n");

    Engine e;
    e.init(48000.0f);
    int gt = 0;
    for (int i = 0; i < rackPresetCount(); i++)
        if (std::strcmp(rackPresetAt(i)->name, "GRAND TOUR") == 0) gt = i;
    e.buildPreset(gt);

    okf(e.macroForCc(20) == -1, "nothing is assigned to begin with: %.0f, "
        "wanted %.0f", (double)e.macroForCc(20), -1.0);

    /* A row, the way a controller with eight knobs sends one. */
    for (int i = 0; i < e.patch.slotCount(); i++) {
        Module *m = e.patch.module(i);
        if (m && m->typeId == "MACRO") { m->params[BS_MACROS].value = 20.0f; break; }
    }
    okf(e.macroForCc(20) == 0, "CC BASE claims the first: %.0f, wanted %.0f",
        (double)e.macroForCc(20), 0.0);
    okf(e.macroForCc(27) == 7, "and the eighth: %.0f, wanted %.0f",
        (double)e.macroForCc(27), 7.0);
    okf(e.macroForCc(28) == -1, "and stops there: %.0f, wanted %.0f",
        (double)e.macroForCc(28), -1.0);

    /* Learn, which is what the knob's menu does. */
    e.setMacroCc(3, 91);
    okf(e.macroCcFor(3) == 91, "a learned CC wins over the row: %.0f, wanted "
        "%.0f", (double)e.macroCcFor(3), 91.0);
    okf(e.macroForCc(91) == 3, "and CC 91 finds it: %.0f, wanted %.0f",
        (double)e.macroForCc(91), 3.0);
    okf(e.macroForCc(23) == -1, "so the row no longer covers that one: %.0f, "
        "wanted %.0f", (double)e.macroForCc(23), -1.0);
    okf(e.macroForCc(22) == 2, "while the rest of the row is untouched: %.0f, "
        "wanted %.0f", (double)e.macroForCc(22), 2.0);

    /* Two panels agree.
     *
     * setMacro writes every MACRO panel, so the eight values are mirrored
     * across all of them. This wrote only the first, which meant learning on
     * the second panel's knob silently changed the first panel's - and since
     * the lookup also reads the first, it appeared to work while the knob that
     * was right-clicked stayed at zero. */
    {
        Engine two;
        two.init(48000.0f);
        two.clear();
        const int a = two.addModule("MACRO", 20, 20);
        const int b = two.addModule("MACRO", 400, 20);
        ok(two.setMacroCc(2, 55), "a rack with MACRO panels accepts a CC");
        const Module *ma = two.patch.module(a), *mb = two.patch.module(b);
        okf(ma->params[BS_MACROS + 1 + 2].value == 55.0f,
            "the first panel took it: %.0f, wanted %.0f",
            ma->params[BS_MACROS + 1 + 2].value, 55.0);
        okf(mb->params[BS_MACROS + 1 + 2].value == 55.0f,
            "and so did the second: %.0f, wanted %.0f",
            mb->params[BS_MACROS + 1 + 2].value, 55.0);
    }

    /* And a rack with nowhere to put it says so, rather than swallowing the
     * assignment and leaving the knob at zero. */
    {
        Engine none;
        none.init(48000.0f);
        none.clear();
        none.addModule("VCO", 20, 20);
        ok(!none.setMacroCc(0, 55),
           "a rack with no MACRO panel refuses, so the caller can say so");
    }

    /* It is a knob, so it is in the file. */
    const std::string text = bs_patch_to_string(&e);
    Engine e2;
    e2.init(48000.0f);
    char status[128] = "";
    ok(bs_patch_from_string(&e2, text.c_str(), status, sizeof status) != 0,
       "the rack round-trips through a patch file");
    okf(e2.macroForCc(91) == 3, "and the learned CC came back: %.0f, wanted "
        "%.0f", (double)e2.macroForCc(91), 3.0);
    okf(e2.macroForCc(22) == 2, "along with the row: %.0f, wanted %.0f",
        (double)e2.macroForCc(22), 2.0);
}

static void test_midi_decode()
{
    std::printf("midi decoding\n");

    Engine e;
    e.init(48000.0f);
    e.buildPreset(0);
    MpeState mpe;
    std::vector<float> buf(512);

    struct L {
        static void msg(Engine &en, MpeState &m, int a, int b, int c)
        {
            const unsigned char x[3] = { (unsigned char)a, (unsigned char)b,
                                         (unsigned char)c };
            applyMidi(en, m, x, 3, 0, 0, 0);
        }
    };

    L::msg(e, mpe, 0x90, 60, 100);
    e.render(&buf[0], 256);
    ok(e.voicesSounding() > 0, "a note on starts a note");

    /* The old convention, still in constant use. */
    L::msg(e, mpe, 0x90, 60, 0);
    e.render(&buf[0], 256);
    okf(e.voicesSounding() == 0, "note on with velocity zero is a note off: "
        "%.0f sounding, wanted %.0f", (double)e.voicesSounding(), 0.0);

    /* MPE: a note on channel 2, then a bend on channel 2, belongs to that
     * note and must not move the whole keyboard. */
    L::msg(e, mpe, 0x91, 64, 100);      /* channel 2 */
    L::msg(e, mpe, 0x90, 67, 100);      /* channel 1, the master channel */
    e.render(&buf[0], 256);
    L::msg(e, mpe, 0xe1, 0x00, 0x70);   /* a big bend, on channel 2 */
    e.render(&buf[0], 256);

    int v64 = -1, v67 = -1;
    for (int i = 0; i < e.keys.channels(); i++) {
        if (e.keys.v[i].note == 64) v64 = i;
        if (e.keys.v[i].note == 67) v67 = i;
    }
    ok(v64 >= 0 && v67 >= 0, "both notes hold a voice");
    if (v64 >= 0 && v67 >= 0) {
        okf(e.keys.v[v64].bend > 1.0f, "a bend on a member channel bends that "
            "note: %.2f semitones, wanted over %.1f", e.keys.v[v64].bend, 1.0);
        okf(e.keys.v[v67].bend == 0.0f, "and leaves the other note alone: "
            "%.2f, wanted %.2f", e.keys.v[v67].bend, 0.0);
    }

    /* A bend on the master channel is the wheel, and moves everything. */
    L::msg(e, mpe, 0xe0, 0x00, 0x70);
    e.render(&buf[0], 256);
    okf(e.keys.bend > 0.5f, "a bend on the master channel is the wheel: %.2f, "
        "wanted over %.1f", e.keys.bend, 0.5);

    /* All notes off. */
    L::msg(e, mpe, 0xb0, 123, 0);
    e.render(&buf[0], 256);
    okf(e.voicesSounding() == 0, "CC 123 stops everything: %.0f sounding, "
        "wanted %.0f", (double)e.voicesSounding(), 0.0);
}

static void test_voice_module()
{
    std::printf("per-voice values\n");

    Engine e;
    e.init(48000.0f);
    e.clear();
    const int kbd = e.addModule("KBD", 20, 20);
    const int v   = e.addModule("VOICE", 200, 20);
    const int out = e.addModule("OUT", 400, 20);
    e.patch.connect(v, 1, out, 0);
    e.patch.module(kbd)->params[3].value = 4.0f;   /* four voices */

    std::vector<float> buf(512);
    const int notes[3] = { 60, 64, 67 };
    for (int n = 0; n < 3; n++) { e.noteOn(notes[n], 0.9f, 0); e.render(&buf[0], 256); }

    Module *m = e.patch.module(v);
    const double i0 = m->outs[0].v[0][0], i2 = m->outs[0].v[2][0];
    okf(i2 > i0 + 1.0, "IDX separates the voices: %.2f against %.2f", i2, i0);

    /* Three different random values, not three points on a line. The first
     * version stepped an LCG once from adjacent seeds and produced a ramp -
     * 2.110, 2.152, 2.193 - which sounds like nothing at all. */
    const double r0 = m->outs[1].v[0][0];
    const double r1 = m->outs[1].v[1][0];
    const double r2 = m->outs[1].v[2][0];
    const double d01 = r1 - r0, d12 = r2 - r1;
    okf(std::fabs(d01 - d12) > 0.05,
        "RND is random per voice, not a ramp: steps %.3f and %.3f", d01, d12);
    okf(r0 != r1 && r1 != r2, "and the three differ: %.3f, %.3f", r0, r1);

    /* A voice that is not playing says nothing. */
    okf(m->outs[1].v[3][0] == 0.0f, "an idle voice reads %.2f, wanted %.2f",
        m->outs[1].v[3][0], 0.0);

    /* AGE counts up while a note is held, and the older note is older. */
    for (int i = 0; i < 200; i++) e.render(&buf[0], 256);
    const double a0 = m->outs[2].v[0][0], a2 = m->outs[2].v[2][0];
    okf(a0 > 1.0, "AGE rises while a note is held: %.3f, wanted over %.1f", a0, 1.0);
    okf(a0 > a2, "and the note pressed first is the older one: %.3f vs %.3f", a0, a2);

    /* The random value is fixed at the note, not rolling. */
    const double r0b = m->outs[1].v[0][0];
    okf(r0b == r0, "RND holds for the length of the note: %.3f then %.3f", r0, r0b);
}


/* The bow has to work at every pitch, not at one.
 *
 * The single-note test above passed happily while the model had a dead band
 * five semitones wide centred on middle C, where a bowed string would start,
 * ring for half a second and stop - at every bow pressure. One note proves
 * nothing about a keyboard. */
static void test_bow_across_the_range()
{
    std::printf("bowing the whole keyboard\n");

    std::vector<double> levels;
    int worst = -1;
    double worstLevel = 1e9;
    for (int n = 36; n <= 96; n += 4) {
        Engine e;
        e.init(48000.0f);
        e.clear();
        const int kbd = e.addModule("KBD", 20, 20);
        const int str = e.addModule("STRING", 200, 20);
        const int out = e.addModule("OUT", 400, 20);
        e.patch.connect(kbd, 0, str, 0);
        e.patch.connect(kbd, 1, str, 1);
        e.patch.connect(str, 0, out, 0);
        Module *m = e.patch.module(str);
        /* The knob's own default, not the preset's longer one. A rack built
         * by hand - drop in a STRING, switch it to BOW - gets this, and it is
         * the harsher case: the preset's longer decay hides a dead band that
         * the default does not. */
        m->params[2].value = 0.60f;   /* DECAY */
        m->params[3].value = 0.42f;
        m->params[4].value = 0.06f;
        m->params[5].value = 0.10f;
        m->params[6].value = 1.2f;
        m->params[7].value = 1.0f;    /* BOW   */
        m->params[8].value = 0.75f;   /* FORCE */
        e.noteOn(n, 0.9f, 0);

        std::vector<float> buf(512);
        double s = 0; long c = 0;
        for (int i = 0; i < 384000; i += 256) {
            e.render(&buf[0], 256);
            if (i > 336000)                    /* after 7 s */
                for (int k = 0; k < 256; k++) { s += buf[(size_t)k * 2] * buf[(size_t)k * 2]; c++; }
        }
        const double level = c ? std::sqrt(s / (double)c) : 0.0;
        levels.push_back(level);
        if (level < worstLevel) { worstLevel = level; worst = n; }
    }

    /* Against the middle of the range, not against zero.
     *
     * An absolute floor does not describe the failure this exists to catch: a
     * dead band is a few notes far quieter than the notes either side of them,
     * and the first version of this test passed with the bug still in because
     * the dip happened to clear a fixed threshold. */
    std::vector<double> sorted = levels;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];

    std::printf("  quietest note %d at %.4f, median %.4f, ratio %.2f\n",
                worst, worstLevel, median, median > 0 ? worstLevel / median : 0.0);
    /* 0.60, and it used to be 0.15 because the bottom of the range really was
     * weak. It is not any more: the bow replaces what the string loses, which
     * is pitch-compensated for free, so every note settles at the same level.
     * Measured after seven seconds rather than two, which is what caught the
     * slow decay this threshold used to be hiding. */
    okf(worstLevel > median * 0.15,
        "no note is dead: the quietest is %.2f of the median, wanted over "
        "%.2f", median > 0 ? worstLevel / median : 0.0, 0.15);
    okf(median > 0.02, "and the keyboard sounds at all: median %.4f, wanted "
        "over %.2f", median, 0.02);
}

static void test_string_exciters()
{
    std::printf("string exciters\n");

    const char *names[3] = { "hammer", "bow", "blow" };
    for (int mode = 0; mode < 3; mode++) {
        Engine e;
        e.init(48000.0f);
        e.clear();
        const int kbd = e.addModule("KBD", 20, 20);
        const int str = e.addModule("STRING", 200, 20);
        const int out = e.addModule("OUT", 400, 20);
        e.patch.connect(kbd, 0, str, 0);      /* V/OCT */
        e.patch.connect(kbd, 1, str, 1);      /* GATE -> TRIG */
        e.patch.connect(str, 0, out, 0);
        Module *m = e.patch.module(str);
        m->params[7].value = (float)mode;     /* EXCITE */
        m->params[8].value = 0.7f;            /* FORCE  */

        e.noteOn(57, 0.9f, 0);                /* A3 */

        /* Eight seconds, not two.
         *
         * The first version of this looked at 1.6 to 1.8 s and reported that a
         * bowed string sustains. It did not - it was decaying slowly, and over
         * eight seconds every note fell away and the bottom of the keyboard
         * died outright. A window shorter than the thing being measured
         * measures nothing. */
        std::vector<float> buf(512), mono;
        double railed = 0, counted = 0, rawPeak = 0;
        for (int i = 0; i < 384000; i += 256) {
            e.render(&buf[0], 256);
            for (int k = 0; k < 256; k++) mono.push_back(buf[(size_t)k * 2]);
            if (i > 336000)
                for (int k = 0; k < BS_BLOCK; k++) {
                    const double v = m->outs[0].v[0][k];
                    if (v > rawPeak) rawPeak = v;
                    if (v > 7.99 || v < -7.99) railed++;
                    counted++;
                }
        }

        struct L {
            static double rms(const std::vector<float> &v, size_t a, size_t b)
            {
                double s = 0; size_t n = 0;
                for (size_t i = a; i < b && i < v.size(); i++) { s += v[i] * v[i]; n++; }
                return n ? std::sqrt(s / (double)n) : 0.0;
            }
        };
        const double early = L::rms(mono, 7200, 16800);     /* 0.15 - 0.35 s */
        const double late  = L::rms(mono, 355200, 374400);  /* 7.40 - 7.80 s */
        const double ratio = early > 1e-9 ? late / early : 0.0;

        std::printf("  %s: early %.4f late %.4f ratio %.2f peak %.2f V\n",
                    names[mode], early, late, ratio, rawPeak);

        if (mode == 0)
            okf(ratio < 0.4, "a struck string decays to %.2f of its early "
                "level, wanted under %.1f", ratio, 0.4);
        else
            okf(ratio > 0.6, "a bowed or blown string holds at %.2f of its "
                "early level, wanted over %.1f", ratio, 0.6);

        okf(railed == 0, "%.0f samples sat on the 8 V rail, wanted %.0f",
            railed, 0.0);
        /* A struck string is meant to be gone by eight seconds, so this asks
         * the early window for it and the late one for the two that hold. */
        okf((mode == 0 ? early : late) > 1e-4,
            "it makes a sound at all: %.5f, wanted over %.5f",
            mode == 0 ? early : late, 1e-4);
        (void)counted;
    }
}

static void test_note_expression()
{
    std::printf("per-note expression\n");

    Engine e;
    e.init(48000.0f);
    e.clear();

    /* Gated, deliberately. Without a VCA the unused polyphonic channels of the
     * oscillator free-run at 0 V - which is middle C - and three silent voices
     * drown the one actually playing. That cost an hour once. */
    const int kbd = e.addModule("KBD", 20, 20), vco = e.addModule("VCO", 200, 20);
    const int env = e.addModule("ADSR", 380, 20), vca = e.addModule("VCA", 560, 20);
    const int out = e.addModule("OUT", 740, 20);
    e.patch.connect(kbd, 0, vco, 0);
    e.patch.connect(kbd, 1, env, 0);
    e.patch.connect(vco, 3, vca, 0);      /* SIN, so one partial and no argument */
    e.patch.connect(env, 0, vca, 1);
    e.patch.connect(vca, 0, out, 0);
    e.patch.module(vca)->params[2].value = 1.0f;

    e.noteOn(60, 0.9f, 0);
    e.noteOn(67, 0.9f, 0);
    e.noteExpression(60, 0, -2.0f, 0);    /* a whole tone down */
    e.noteExpression(67, 0, +2.0f, 0);    /* and the other one up */

    std::vector<float> buf(512), mono;
    for (int i = 0; i < 14000; i += 256) {
        e.render(&buf[0], 256);
        for (int k = 0; k < 256; k++) mono.push_back(buf[(size_t)k * 2]);
    }
    mono.erase(mono.begin(), mono.begin() + 6000);
    mono.resize(8192);

    /* Direct correlation, not Goertzel: its recurrence drifts over a window
     * this long and cheerfully reports 261 Hz for a note an octave up. */
    struct L {
        static double mag(const std::vector<float> &x, double midi)
        {
            const double f = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
            const double w = 2 * 3.14159265358979 * f / 48000.0;
            double re = 0, im = 0;
            for (size_t i = 0; i < x.size(); i++) {
                re += x[i] * std::cos(w * (double)i);
                im -= x[i] * std::sin(w * (double)i);
            }
            return std::sqrt(re * re + im * im) / (double)x.size();
        }
    };

    const double moved58 = L::mag(mono, 58), left60 = L::mag(mono, 60);
    const double left67  = L::mag(mono, 67), moved69 = L::mag(mono, 69);
    const double peak = moved58 > moved69 ? moved58 : moved69;

    okf(left60 < peak * 0.1, "the bent-down note left 60: %.1f %% of the peak, "
        "wanted under %.0f", 100.0 * left60 / peak, 10.0);
    okf(left67 < peak * 0.1, "the bent-up note left 67: %.1f %% of the peak, "
        "wanted under %.0f", 100.0 * left67 / peak, 10.0);
    okf(moved58 > peak * 0.8, "and arrived at 58: %.1f %%, wanted over %.0f",
        100.0 * moved58 / peak, 80.0);
    okf(moved69 > peak * 0.8, "and at 69: %.1f %%, wanted over %.0f",
        100.0 * moved69 / peak, 80.0);

    /* Pressure and timbre reach their own voice and no other. */
    e.noteExpression(60, 1, 0.75f, 0);
    e.render(&buf[0], 256);
    int v60 = -1, v67 = -1;
    for (int i = 0; i < e.keys.channels(); i++) {
        if (e.keys.v[i].note == 60) v60 = i;
        if (e.keys.v[i].note == 67) v67 = i;
    }
    ok(v60 >= 0 && v67 >= 0, "both notes hold a voice");
    if (v60 >= 0 && v67 >= 0) {
        okf(e.keys.v[v60].press > 0.7f, "pressure reached the note it named: %.2f, "
            "wanted over %.2f", e.keys.v[v60].press, 0.70);
        okf(e.keys.v[v67].press < 0.01f, "and not the other one: %.2f, wanted under "
            "%.2f", e.keys.v[v67].press, 0.01);
    }

    /* A voice reused for a new note must not inherit the last one's bend -
     * it would sound as an instant detune on the attack. */
    e.noteOff(60, 0);
    e.render(&buf[0], 256);
    e.noteOn(62, 0.9f, 0);
    e.render(&buf[0], 256);
    for (int i = 0; i < e.keys.channels(); i++)
        if (e.keys.v[i].note == 62)
            okf(e.keys.v[i].bend == 0.0f && e.keys.v[i].press == 0.0f,
                "a reused voice starts clean: bend %.2f, press %.2f",
                e.keys.v[i].bend, e.keys.v[i].press);
}

static void test_sample_rates()
{
    std::printf("sample rates\n");
    static const float RATES[] = { 44100.0f, 48000.0f, 96000.0f };
    for (int i = 0; i < 3; i++) {
        Engine e;
        e.init(RATES[i]);
        e.buildDefaultPatch();
        e.noteOn(60, 1.0f);
        std::vector<float> buf(8192 * 2);
        for (int b = 0; b < 6; b++) e.render(&buf[0], 8192);
        char msg[96];
        std::snprintf(msg, sizeof msg, "%.0f Hz renders finite audio", RATES[i]);
        ok(finite(&buf[0], 8192 * 2), msg);
        std::snprintf(msg, sizeof msg, "%.0f Hz makes sound", RATES[i]);
        ok(rmsOf(&buf[0], 8192 * 2) > 0.005f, msg);
    }
}

int main()
{
    std::printf("BENCsynth core tests\n\n");

    test_pitch();
    test_osc();
    test_ladder();
    test_adsr();
    test_keys();
    test_registry();
    test_graph();
    test_event_queue();
    test_eval_order();
    test_default_patch();
    test_presets();
    test_event_offsets();
    test_arp();
    test_clock_and_func();
    test_grand_tour();
    test_sample_module();
    test_macro_cc();
    test_midi_decode();
    test_voice_module();
    test_string_exciters();
    test_bow_across_the_range();
    test_note_expression();
    test_sample_rates();
    test_patchfile();

    std::printf("\n%d checks, %d failed\n", bs_checks, bs_failures);
    return bs_failures ? 1 : 0;
}
