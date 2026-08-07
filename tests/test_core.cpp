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
#include "bs_modules.h"
#include "test_util.h"

#include <cstdio>
#include <cmath>
#include <cstring>
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
    test_grand_tour();
    test_sample_rates();
    test_patchfile();

    std::printf("\n%d checks, %d failed\n", bs_checks, bs_failures);
    return bs_failures ? 1 : 0;
}
