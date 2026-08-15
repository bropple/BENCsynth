/*
 * "SUPERNOVA" - happy hardcore, out of nothing but BENCsynth factory racks.
 *
 * Lead-heavy on purpose: the melody is the song, and everything else is there
 * to hold it up. Same method as the techno demo - one Engine per rack, one WAV
 * per layer, summed at the end - because a layer you can render alone is a
 * layer you can measure alone.
 *
 * 172 BPM, A minor, 44 bars, over the chord loop Am - F - C - G.
 */
#include "bstrack.h"

#include <algorithm>

static const double BPM   = 172.0;
static const double BEAT  = 60.0 / BPM;          /* 0.3488 s                */
static const double BAR   = BEAT * 4.0;          /* 1.3953 s                */
static const double STEP  = BEAT / 4.0;          /* a sixteenth             */
static const int    BARS  = 44;                  /* 61.4 s                  */
static const double TAIL  = 3.0;

/* A minor. The lead lives from A4 up to C6, which is where this music puts
 * its melodies and why they read as euphoric rather than as a bass part. */
enum {
    F1 = 29, G1 = 31, A1 = 33, C2 = 36,
    D3 = 50, E3 = 52, F3 = 53, G3 = 55, A3 = 57, B3 = 59,
    C4 = 60, D4 = 62, E4 = 64, F4 = 65, G4 = 67, A4 = 69, B4 = 71,
    C5 = 72, D5 = 74, E5 = 76, F5 = 77, G5 = 79, A5 = 81, B5 = 83, C6 = 84
};
#define R (-1)

struct Ev { double at, len; int note; float vel; };

/* ------------------------------------------------------------------ *
 * The sequencer - unchanged from the techno build
 * ------------------------------------------------------------------ */

static std::vector<float> play(int preset, void (*tweak)(bs::Engine &),
                               void (*autom)(bs::Engine &, double),
                               std::vector<Ev> evs, double seconds)
{
    bs::Engine eng;
    eng.init((float)RATE);
    eng.buildPreset(preset);
    eng.patch.transport.bpm     = (float)BPM;
    eng.patch.transport.playing = 1;
    if (tweak) tweak(eng);

    struct Edge { long f; int on, note; float vel; };
    std::vector<Edge> edges;
    for (size_t i = 0; i < evs.size(); i++) {
        Edge a = { (long)(evs[i].at * RATE), 1, evs[i].note, evs[i].vel };
        Edge b = { (long)((evs[i].at + evs[i].len) * RATE), 0, evs[i].note, 0.0f };
        edges.push_back(a);
        edges.push_back(b);
    }
    std::sort(edges.begin(), edges.end(),
              [](const Edge &a, const Edge &b) {
                  if (a.f != b.f) return a.f < b.f;
                  return a.on < b.on;
              });

    const long total = (long)(seconds * RATE);
    std::vector<float> buf((size_t)total * 2, 0.0f);

    long at = 0;
    size_t e = 0;
    while (at < total) {
        while (e < edges.size() && edges[e].f <= at) {
            if (edges[e].on) eng.noteOn(edges[e].note, edges[e].vel);
            else             eng.noteOff(edges[e].note);
            e++;
        }
        if (autom) autom(eng, (double)at / RATE);
        long next = (e < edges.size() && edges[e].f < total) ? edges[e].f : total;
        if (next <= at) next = at + 1;
        long n = next - at;
        if (n > 480) n = 480;
        eng.render(&buf[(size_t)at * 2], (int)n);
        at += n;
    }
    return buf;
}

static double T(int bar, double step) { return bar * BAR + step * STEP; }

/* Turn a sixteen-slot bar of note numbers into events, each note running to
 * the next one so a melody phrases instead of stuttering. */
static void addBar(std::vector<Ev> &v, int bar, const int *steps,
                   double gate, float vel, int transpose)
{
    for (int s = 0; s < 16; s++) {
        if (steps[s] < 0) continue;
        int nxt = s + 1;
        while (nxt < 16 && steps[nxt] < 0) nxt++;
        const double len = (double)(nxt - s) * gate;
        Ev e = { T(bar, s), len * STEP, steps[s] + transpose, vel };
        v.push_back(e);
    }
}

/* ------------------------------------------------------------------ *
 * The tune
 * ------------------------------------------------------------------ */

/* Eight bars over Am - F - C - G twice. Written to climb: it opens low,
 * touches C6 at the end of the first four, peaks across bars five and six,
 * and walks back down to hand itself back to the top. */
static const int MEL[8][16] = {
 /* Am */ { A4, R, R, R,  C5, R, E5, R,  A5, R, R, R,  E5, R, R, R },
 /* F  */ { F5, R, R, R,  E5, R, R,  R,  C5, R, R, R,  A4, R, R, R },
 /* C  */ { C5, R, R, R,  E5, R, G5, R,  C6, R, R, R,  B5, R, R, R },
 /* G  */ { B5, R, R, R,  A5, R, G5, R,  D5, R, R, R,  R,  R, R, R },
 /* Am */ { A5, R, R, R,  C6, R, B5, R,  A5, R, R, R,  E5, R, R, R },
 /* F  */ { F5, R, R, R,  A5, R, R,  R,  C6, R, R, R,  A5, R, R, R },
 /* C  */ { G5, R, R, R,  E5, R, R,  R,  C5, R, R, R,  D5, R, R, R },
 /* G  */ { E5, R, R, R,  D5, R, R,  R,  B4, R, R, R,  D5, R, R, R }
};

/* Chord tones, indexed by bar % 4. */
static const int CHORD[4][3] = {
    { A4, C5, E5 },   /* Am */
    { F4, A4, C5 },   /* F  */
    { E4, G4, C5 },   /* C  */
    { D4, G4, B4 }    /* G  */
};
/* An octave above where this started. At A1 the sub oscillator sits at
 * 27.5 Hz - a 36 ms period - so a 40 ms stab was barely one cycle of it, which
 * is a thump with a spike on the front, not a bass note. Up here the sub is
 * 55 Hz and the kick keeps the bottom octave to itself. */
static const int ROOT[4] = { A1 + 12, F1 + 12, C2 + 12, G1 + 12 };

/* A four-note figure per chord for the sixteenth arpeggio under the lead. */
static const int ARP[4][4] = {
    { A3, C4, E4, A4 },
    { F3, A3, C4, F4 },
    { E3, G3, C4, E4 },
    { D3, G3, B3, D4 }
};

/* ------------------------------------------------------------------ *
 * Voicing
 * ------------------------------------------------------------------ */

/* A hardcore kick is a techno kick that has been through something. The rack
 * ends VCA -> OUT, so a FOLD goes in between: at 172 BPM the kick has a third
 * of a second to make its point and distortion is how it does it. */
static void vKick(bs::Engine &e)
{
    knob(e, 0, 1,  0.0f);      /* KBD OCT - stock -2 is subsonic            */
    knob(e, 0, 3,  2.0f);
    knob(e, 1, 1,  0.045f);    /* pitch env - snappier than the techno one  */
    knob(e, 1, 3,  0.045f);
    knob(e, 2, 4,  0.62f);     /* deeper drop                               */
    knob(e, 3, 1,  0.30f);     /* short: 172 BPM leaves no room             */
    knob(e, 3, 3,  0.30f);

    const int fold = addMod(e, "FOLD");
    wire(e, 4, 0, fold, 0);    /* VCA  -> FOLD */
    knob(e, fold, 0, 2.2f);    /* GAIN */
    /* SYM stays at zero. An asymmetric fold is a rectifier: at 0.10 this kick
     * measured a DC offset of +0.164, which is not a sound, it is a battery
     * wired across the speaker. Symmetric folding still gives plenty of
     * harmonics, and an SVF high-pass behind it catches whatever DC the fold
     * still makes on a signal that is not centred to begin with. */
    knob(e, fold, 1, 0.0f);

    /* A lowpass between the folder and the output, and it is not optional.
     * The pitch envelope has this oscillator near 4 kHz for the first few
     * milliseconds - about twelve samples a cycle - so consecutive samples
     * land either side of several fold boundaries and the folder aliases into
     * a hard click. Measured as a one-sample jump of 0.92, against 0.31 for
     * the rack with no folder at all. Behind a 2.6 kHz lowpass the same folder
     * measures 0.10, and keeps the weight it was added for: rms 0.16 where
     * the stock rack gives 0.11. */
    const int lp = addMod(e, "SVF");
    wire(e, fold, 0, lp, 0);
    knob(e, lp, 0, 2600.0f);
    knob(e, lp, 1, 0.05f);

    const int dcb = addMod(e, "SVF");
    wire(e, lp, 0, dcb, 0);    /* LP -> HP  */
    wire(e, dcb, 1, 5, 0);     /* HP -> OUT */
    knob(e, dcb, 0, 28.0f);    /* well below the 55 Hz body */
    knob(e, dcb, 1, 0.05f);
    knob(e, 5, 0, 0.62f);
}

static int hatSvf(bs::Engine &e, float cutoff, float res)
{
    const int svf = addMod(e, "SVF");
    wire(e, 2, 0, svf, 0);
    wire(e, svf, 1, 4, 0);     /* HP -> VCA */
    knob(e, svf, 0, cutoff);
    knob(e, svf, 1, res);
    return svf;
}

static void vHatClosed(bs::Engine &e)
{
    knob(e, 2, 0, 11000.0f);
    knob(e, 2, 1, 0.10f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 7000.0f, 0.18f);
    knob(e, 3, 1, 0.018f);
    knob(e, 3, 3, 0.018f);
    knob(e, 5, 2, 0.0f);
    knob(e, 6, 0, 0.55f);
}

static void vHatOpen(bs::Engine &e)
{
    knob(e, 2, 0, 12000.0f);
    knob(e, 2, 1, 0.10f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 5000.0f, 0.22f);
    knob(e, 3, 1, 0.100f);
    knob(e, 3, 3, 0.100f);
    knob(e, 5, 2, 0.10f);
    knob(e, 6, 0, 0.48f);
}

static void vClap(bs::Engine &e)
{
    knob(e, 1, 0, 1.70f);
    knob(e, 2, 0, 2400.0f);
    knob(e, 2, 1, 0.50f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 800.0f, 0.28f);
    knob(e, 3, 1, 0.050f);
    knob(e, 3, 3, 0.050f);
    knob(e, 5, 0, 0.40f);
    knob(e, 5, 1, 0.35f);
    knob(e, 5, 2, 0.38f);
    knob(e, 6, 0, 0.95f);
}

static void vRoll(bs::Engine &e)      /* the snare that runs into the drop */
{
    knob(e, 1, 0, 1.60f);
    knob(e, 2, 0, 2000.0f);
    knob(e, 2, 1, 0.58f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 900.0f, 0.25f);
    knob(e, 3, 1, 0.040f);
    knob(e, 3, 3, 0.040f);
    knob(e, 5, 2, 0.20f);
    knob(e, 6, 0, 0.85f);
}

static void vCrash(bs::Engine &e)
{
    knob(e, 2, 0, 14000.0f);
    knob(e, 2, 1, 0.08f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 3600.0f, 0.15f);
    knob(e, 3, 1, 1.40f);
    knob(e, 3, 3, 1.40f);
    knob(e, 5, 0, 0.72f);
    knob(e, 5, 1, 0.30f);
    knob(e, 5, 2, 0.35f);
    knob(e, 6, 0, 0.40f);
}

/* Noise held for two bars with the high-pass walking up under it. */
static void vRiser(bs::Engine &e)
{
    knob(e, 1, 0, 1.20f);
    knob(e, 2, 0, 16000.0f);
    knob(e, 2, 1, 0.10f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 400.0f, 0.55f);
    knob(e, 3, 0, 0.60f);      /* slow attack                               */
    knob(e, 3, 1, 0.50f);
    knob(e, 3, 2, 0.85f);      /* sustain, so it can be held                */
    knob(e, 3, 3, 0.12f);
    knob(e, 5, 2, 0.25f);
    knob(e, 6, 0, 0.30f);
}

/* Offbeat bass. The whole genre puts a short root on every "and", and the
 * kick owns the downbeat - so this is deliberately tiny and dry. */
static void vBass(bs::Engine &e)      /* BASS (2) */
{
    knob(e, 0, 0, 0.0f);       /* no glide - these are separate stabs       */
    knob(e, 3, 0, 0.85f);      /* MIX: main osc                             */
    knob(e, 3, 1, 0.70f);      /* sub                                       */
    knob(e, 4, 0, 330.0f);     /* VCF cutoff - up with the octave           */
    knob(e, 4, 1, 0.35f);
    knob(e, 4, 2, 2.0f);       /* less drive - it was sharpening the attack */
    /* CV1 is octaves per volt against a ten-volt envelope, so 0.34 is 3.4
     * octaves and the knob above would mean nothing. 0.16 keeps the thump. */
    knob(e, 4, 3, 0.16f);
    /* Measured before this: peak 0.41 at 4 ms, 0.19 by 8 ms, silent by 40 ms
     * against a 100 ms note. All transient and no note - which is what the
     * spike was. Slower attack, and a decay long enough that the gate is what
     * ends the sound rather than the envelope beating it to it. */
    knob(e, 5, 1, 0.120f);     /* filter env                                */
    knob(e, 6, 0, 0.005f);     /* attack - take the edge off the click      */
    knob(e, 6, 1, 0.150f);     /* amp decay                                 */
    knob(e, 6, 2, 0.22f);      /* a little sustain, so the note has a body  */
    knob(e, 6, 3, 0.035f);
    knob(e, 8, 0, 0.80f);
}

/* The lead. Three detuned saws is already the sound; what it needs is width
 * and enough filter to stay bright at the top of the melody. */
static void vLead(bs::Engine &e)      /* SUPERSAW (18) */
{
    knob(e, 1, 2, -19.0f);     /* spread the three a little wider           */
    knob(e, 3, 2,  20.0f);
    knob(e, 5, 0, 2600.0f);    /* VCF cutoff                                */
    knob(e, 5, 1, 0.22f);
    knob(e, 5, 3, 0.18f);      /* 1.8 octaves of envelope, not four         */
    knob(e, 6, 1, 0.30f);
    knob(e, 6, 2, 0.55f);
    knob(e, 7, 0, 0.004f);     /* amp: fast on, held, quick off             */
    knob(e, 7, 1, 0.40f);
    knob(e, 7, 2, 0.80f);
    knob(e, 7, 3, 0.090f);
    knob(e, 9, 5, 1.0f);       /* DLY SYNC                                  */
    knob(e, 9, 6, 4.0f);       /* 1/16                                      */
    knob(e, 9, 1, 0.24f);
    knob(e, 9, 3, 0.16f);
    knob(e, 10, 2, 0.22f);     /* reverb                                    */

    /* A chorus across the very end, for the width the genre wants. */
    const int ch = addMod(e, "CHORUS");
    wire(e, 10, 0, ch, 0);     /* RVB L -> CHORUS IN   */
    wire(e, 10, 1, ch, 1);     /* RVB R -> CHORUS IN R */
    wire(e, ch, 0, 11, 0);     /* CHORUS L -> OUT L    */
    wire(e, ch, 1, 11, 1);
    knob(e, ch, 0, 0.45f);     /* RATE  */
    knob(e, ch, 1, 0.50f);     /* DEPTH */
    knob(e, ch, 4, 0.45f);     /* MIX   */
    knob(e, 11, 0, 1.10f);     /* the lead is the song - give it headroom   */
}

static void vArp(bs::Engine &e)       /* SQUARE LEAD (3) */
{
    knob(e, 0, 0, 0.0f);       /* no glide on a sixteenth arp               */
    knob(e, 0, 4, 0.0f);       /* POLY, so the tail of one can ring         */
    knob(e, 0, 3, 4.0f);
    knob(e, 1, 3, 0.30f);      /* narrower pulse - more edge                */
    knob(e, 2, 0, 3000.0f);
    knob(e, 2, 1, 0.30f);
    knob(e, 2, 3, 0.12f);
    knob(e, 3, 0, 0.002f);
    knob(e, 3, 1, 0.090f);
    knob(e, 3, 2, 0.0f);       /* plucked                                   */
    knob(e, 3, 3, 0.050f);
    knob(e, 6, 5, 1.0f);       /* DLY SYNC                                  */
    knob(e, 6, 6, 4.0f);
    knob(e, 6, 1, 0.20f);
    knob(e, 6, 3, 0.18f);
}

static void vStab(bs::Engine &e)      /* HOOVER (20) */
{
    knob(e, 4, 0, 900.0f);
    knob(e, 4, 1, 0.50f);
    knob(e, 4, 3, 0.22f);      /* 2.2 octaves                               */
    knob(e, 5, 1, 0.130f);
    knob(e, 6, 1, 0.160f);
    knob(e, 6, 2, 0.0f);
    knob(e, 6, 3, 0.120f);
    knob(e, 9, 5, 1.0f);
    knob(e, 9, 6, 2.0f);       /* 1/8                                       */
    knob(e, 9, 3, 0.20f);
    knob(e, 10, 2, 0.20f);
    knob(e, 11, 0, 0.45f);
}

static void vPiano(bs::Engine &e)     /* PIANO (5) */
{
    knob(e, 6, 0, 520.0f);
    knob(e, 6, 3, 0.30f);      /* 3 octaves is plenty of hammer             */
    knob(e, 11, 2, 0.30f);     /* reverb, for the breakdown                 */
    knob(e, 12, 0, 0.70f);
}

static void vPad(bs::Engine &e)
{
    (void)e;
}

/* ------------------------------------------------------------------ *
 * Automation
 * ------------------------------------------------------------------ */

struct BP { double bar, v; };

static double curve(const BP *p, int n, double t)
{
    const double bar = t / BAR;
    if (bar <= p[0].bar) return p[0].v;
    for (int i = 1; i < n; i++) {
        if (bar <= p[i].bar) {
            const double f = (bar - p[i - 1].bar) / (p[i].bar - p[i - 1].bar);
            return p[i - 1].v + f * (p[i].v - p[i - 1].v);
        }
    }
    return p[n - 1].v;
}

/* The lead opens through the first drop and opens further through the second,
 * which is the whole shape of the arrangement in one knob. */
static void aLead(bs::Engine &e, double t)
{
    static const BP CUT[] = {
        { 0, 1500 }, { 12, 2100 }, { 19, 3200 }, { 20, 1500 },
        { 28, 2600 }, { 36, 3600 }, { 44, 4200 }
    };
    knob(e, 5, 0, (float)curve(CUT, 7, t));
}

static void aArp(bs::Engine &e, double t)
{
    static const BP CUT[] = { { 0, 1600 }, { 19, 3400 }, { 28, 2400 },
                              { 43, 4200 }, { 44, 4200 } };
    knob(e, 2, 0, (float)curve(CUT, 5, t));
}

/* The riser is the high-pass climbing, which is the cheapest convincing
 * build there is and needs no extra module. */
static void aRiser(bs::Engine &e, double t)
{
    static const BP CUT[] = { { 26, 350 }, { 27.9, 7000 }, { 44, 7000 } };
    /* The SVF is the last module added by vRiser, so it is the last slot. */
    const int svf = e.patch.slotCount() - 1;
    knob(e, svf, 0, (float)curve(CUT, 3, t));
}

static void aPad(bs::Engine &e, double t)
{
    static const BP CUT[] = { { 0, 900 }, { 20, 700 }, { 27.5, 2600 },
                              { 44, 2600 } };
    knob(e, 4, 0, (float)curve(CUT, 4, t));
}

/* ------------------------------------------------------------------ *
 * The arrangement
 *
 * intro 0-3   build 4-11   DROP 12-19   breakdown 20-25
 * build 26-27 (kick out on 27)          DROP 28-43
 * ------------------------------------------------------------------ */

static int kickOn(int b)  { return (b >= 4 && b < 20) || b == 26 || b >= 28; }
static int hatOn(int b)   { return (b >= 6 && b < 20) || b >= 28; }
static int openOn(int b)  { return (b >= 8 && b < 20) || b >= 30; }
static int clapOn(int b)  { return (b >= 8 && b < 20) || b >= 28; }
static int bassOn(int b)  { return (b >= 8 && b < 20) || b >= 28; }
static int arpOn(int b)   { return (b >= 10 && b < 20) || b >= 30; }
static int leadOn(int b)  { return (b >= 12 && b < 20) || b >= 28; }
static int stabOn(int b)  { return (b >= 16 && b < 20) || b >= 36; }
static int pianoOn(int b) { return b < 12 || (b >= 20 && b < 28); }
static int padOn(int b)   { return b < 4 || (b >= 20 && b < 28); }

static std::vector<Ev> buildKick(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!kickOn(b)) continue;
        for (int beat = 0; beat < 4; beat++) {
            Ev e = { T(b, beat * 4), 0.07, A1, 1.0f };
            v.push_back(e);
        }
    }
    return v;
}

static std::vector<Ev> buildHatClosed(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!hatOn(b)) continue;
        for (int s = 0; s < 16; s++) {
            float vel = 0.30f;
            if (s % 4 == 0) vel = 0.40f;
            if (s % 4 == 3) vel = 0.52f;
            if (s % 4 == 2) vel = 0.22f;      /* the open hat lives here */
            Ev e = { T(b, s), 0.018, 78, vel };
            v.push_back(e);
        }
    }
    return v;
}

static std::vector<Ev> buildHatOpen(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!openOn(b)) continue;
        for (int s = 2; s < 16; s += 4) {
            Ev e = { T(b, s), 0.045, 84, 0.60f };
            v.push_back(e);
        }
    }
    return v;
}

static void clapAt(std::vector<Ev> &v, double t, float vel)
{
    static const double OFF[4] = { 0.0, 0.0080, 0.0160, 0.0250 };
    static const float  AMP[4] = { 0.75f, 0.85f, 0.70f, 1.0f };
    for (int i = 0; i < 4; i++) {
        Ev e = { t + OFF[i], 0.030, 55 + i, vel * AMP[i] };
        v.push_back(e);
    }
}

static std::vector<Ev> buildClap(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!clapOn(b)) continue;
        for (int s = 4; s < 16; s += 8) clapAt(v, T(b, s), 1.0f);
    }
    return v;
}

/* Sixteenths that halve into thirty-seconds across the last beat. */
static std::vector<Ev> buildRoll(void)
{
    std::vector<Ev> v;
    static const int AT[3] = { 11, 19, 27 };
    for (int i = 0; i < 3; i++) {
        const int b = AT[i];
        for (int s = 8; s < 12; s++) {
            Ev e = { T(b, s), 0.030, 60, 0.55f + 0.04f * (s - 8) };
            v.push_back(e);
        }
        for (int s = 0; s < 8; s++) {
            Ev e = { T(b, 12 + s * 0.5), 0.022, 60, 0.72f + 0.035f * s };
            v.push_back(e);
        }
    }
    return v;
}

static std::vector<Ev> buildCrash(void)
{
    std::vector<Ev> v;
    static const int AT[5] = { 4, 12, 20, 28, 36 };
    for (int i = 0; i < 5; i++) {
        Ev e = { T(AT[i], 0), 0.25, 72, 0.9f };
        v.push_back(e);
    }
    return v;
}

static std::vector<Ev> buildRiser(void)
{
    std::vector<Ev> v;
    Ev e = { T(26, 0), BAR * 1.95, 66, 0.85f };
    v.push_back(e);
    return v;
}

static std::vector<Ev> buildBass(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!bassOn(b)) continue;
        const int root = ROOT[b % 4];
        for (int s = 2; s < 16; s += 4) {
            Ev e = { T(b, s), STEP * 1.15, root, 0.95f };
            v.push_back(e);
        }
    }
    return v;
}

static std::vector<Ev> buildLead(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!leadOn(b)) continue;
        addBar(v, b, MEL[b % 8], 0.94, 0.95f, 0);
    }
    return v;
}

static std::vector<Ev> buildArp(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!arpOn(b)) continue;
        const int *a = ARP[b % 4];
        for (int s = 0; s < 16; s++) {
            Ev e = { T(b, s), STEP * 0.55, a[s % 4], s % 4 == 0 ? 0.80f : 0.62f };
            v.push_back(e);
        }
    }
    return v;
}

static std::vector<Ev> buildStab(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!stabOn(b)) continue;
        const int *c = CHORD[b % 4];
        for (int s = 0; s < 16; s += 8)
            for (int k = 0; k < 3; k++) {
                Ev e = { T(b, s), STEP * 2.2, c[k] - 12, 0.85f };
                v.push_back(e);
            }
    }
    return v;
}

/* The piano carries the tune where the lead is not playing: it states it in
 * the intro and takes it back for the breakdown. */
static std::vector<Ev> buildPiano(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!pianoOn(b)) continue;
        const int *c = CHORD[b % 4];
        for (int k = 0; k < 3; k++) {
            Ev e = { T(b, 0), BAR * 0.92, c[k] - 12, 0.55f };
            v.push_back(e);
        }
        /* Melody in the intro and through the breakdown. */
        if (b < 4)             addBar(v, b, MEL[b % 8], 0.90, 0.80f, 0);
        else if (b >= 20 && b < 28) addBar(v, b, MEL[(b - 20) % 8], 0.90, 0.85f, 0);
    }
    return v;
}

static std::vector<Ev> buildPad(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!padOn(b)) continue;
        const int *c = CHORD[b % 4];
        for (int k = 0; k < 3; k++) {
            Ev e = { T(b, 0), BAR * 0.98, c[k] - 12, 0.65f };
            v.push_back(e);
        }
    }
    return v;
}

/* ------------------------------------------------------------------ */

struct Layer {
    const char *name;
    int         preset;
    void      (*tweak)(bs::Engine &);
    void      (*autom)(bs::Engine &, double);
    std::vector<Ev> (*build)(void);
};

static Layer LAYERS[] = {
    { "kick",  33, vKick,      0,      buildKick      },
    { "bass",   2, vBass,      0,      buildBass      },
    { "lead",  18, vLead,      aLead,  buildLead      },
    { "arp",    3, vArp,       aArp,   buildArp       },
    { "stab",  20, vStab,      0,      buildStab      },
    { "piano",  5, vPiano,     0,      buildPiano     },
    { "pad",    4, vPad,       aPad,   buildPad       },
    { "clap",  34, vClap,      0,      buildClap      },
    { "hatcl", 34, vHatClosed, 0,      buildHatClosed },
    { "hatop", 34, vHatOpen,   0,      buildHatOpen   },
    { "roll",  34, vRoll,      0,      buildRoll      },
    { "crash", 34, vCrash,     0,      buildCrash     },
    { "riser", 34, vRiser,     aRiser, buildRiser     }
};
static const int NLAYERS = (int)(sizeof LAYERS / sizeof LAYERS[0]);

int main(int argc, char **argv)
{
    const std::string dir = (argc > 1) ? argv[1] : ".";
    const double seconds = BARS * BAR + TAIL;

    for (int i = 0; i < NLAYERS; i++) {
        std::vector<Ev> evs = LAYERS[i].build();
        std::vector<float> b = play(LAYERS[i].preset, LAYERS[i].tweak,
                                    LAYERS[i].autom, evs, seconds);
        const Stats s = measure(b);
        std::printf("%-6s %-11s %4zu notes  peak %.3f  rms %.4f  crest %5.2f\n",
                    LAYERS[i].name, bs::rackPresetAt(LAYERS[i].preset)->name,
                    evs.size(), s.peak, s.rms, s.crest);
        writeWav(dir + "/hc-" + LAYERS[i].name + ".wav", b);
    }
    std::printf("\n%d layers, %.1f s each, in %s\n", NLAYERS, seconds, dir.c_str());
    return 0;
}
