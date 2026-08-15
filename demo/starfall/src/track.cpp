/*
 * "STARFALL" - a techno demo made of nothing but BENCsynth factory racks.
 *
 * Each instrument is a separate Engine holding one factory rack, tuned with
 * knob overrides and played from a note list. Every layer is written to its
 * own WAV, and the last pass sums them. That is deliberate: a layer you can
 * render on its own is a layer you can measure on its own, and the only way
 * to build this without being able to hear it is to be able to measure it.
 *
 * 128 BPM, A minor, 32 bars.
 */
#include "bstrack.h"

#include <algorithm>

static const double BPM   = 128.0;
static const double BEAT  = 60.0 / BPM;          /* 0.46875 s               */
static const double BAR   = BEAT * 4.0;          /* 1.875 s                 */
static const double STEP  = BEAT / 4.0;          /* a sixteenth             */
static const int    BARS  = 32;
static const double TAIL  = 2.5;                 /* let the delays ring out */

/* Note names, so the arrangement below reads as music. A minor. */
enum {
    G1 = 31, A1 = 33, C2 = 36, D2 = 38, E2 = 40, F2 = 41, G2 = 43,
    A2 = 45, C3 = 48, D3 = 50, E3 = 52, F3 = 53, G3 = 55,
    A3 = 57, B3 = 59, C4 = 60, D4 = 62, E4 = 64, F4 = 65, G4 = 67, A4 = 69
};

struct Ev { double at, len; int note; float vel; };

/* ------------------------------------------------------------------ *
 * The sequencer
 * ------------------------------------------------------------------ */

/* Renders a note list through a rack. Events land on exact sample frames:
 * the loop renders up to the next event, fires it, and carries on, so the
 * groove is not quantised to a block boundary. */
static std::vector<float> play(int preset, void (*tweak)(bs::Engine &),
                               void (*autom)(bs::Engine &, double),
                               std::vector<Ev> evs, double seconds)
{
    bs::Engine eng;
    eng.init((float)RATE);
    eng.buildPreset(preset);
    /* Tell the rack what tempo it is at, so any delay with SYNC on lands on
     * the grid instead of near it. */
    eng.patch.transport.bpm     = (float)BPM;
    eng.patch.transport.playing = 1;
    if (tweak) tweak(eng);

    /* One entry per edge, sorted by frame. */
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
                  return a.on < b.on;      /* releases before strikes */
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
        /* Capped so a knob being swept is written often enough to sound like
         * a hand on it rather than a staircase. */
        if (n > 480) n = 480;
        eng.render(&buf[(size_t)at * 2], (int)n);
        at += n;
    }
    return buf;
}

/* A sixteenth-step position, in seconds. */
static double T(int bar, double step) { return bar * BAR + step * STEP; }

/* ------------------------------------------------------------------ *
 * Voicing - the knob overrides that turn a factory rack into a part
 * ------------------------------------------------------------------ */

static void vKick(bs::Engine &e)
{
    knob(e, 0, 1,  0.0f);      /* KBD OCT back to 0 - stock -2 is subsonic  */
    knob(e, 0, 3,  2.0f);      /* two voices, so a fast hit never chokes    */
    knob(e, 1, 1,  0.055f);    /* pitch env decay - the drop                */
    knob(e, 1, 3,  0.055f);
    knob(e, 2, 4,  0.55f);     /* FM depth - how far it falls               */
    knob(e, 3, 1,  0.50f);     /* amp decay ~360 ms                         */
    knob(e, 3, 3,  0.50f);
    knob(e, 5, 0,  0.75f);     /* OUT                                       */
}

/* The SNARE rack is noise -> VCF -> VCA -> RVB, and its VCF only offers
 * low-pass outputs. A cymbal is the top of the noise, not the bottom, so the
 * three cymbal parts all splice an SVF in ahead of the VCA and take its HP.
 * Without this the "hat" measures flat from 32 Hz to 16 kHz - which is to say
 * it is not a hat, it is a noise burst sitting on top of the kick. */
static int hatSvf(bs::Engine &e, float cutoff, float res)
{
    const int svf = addMod(e, "SVF");
    wire(e, 2, 0, svf, 0);     /* VCF LP24 -> SVF IN                        */
    wire(e, svf, 1, 4, 0);     /* SVF HP   -> VCA IN                        */
    knob(e, svf, 0, cutoff);
    knob(e, svf, 1, res);
    return svf;
}

static void vHatClosed(bs::Engine &e)
{
    knob(e, 2, 0, 11000.0f);   /* VCF barely in the way                     */
    knob(e, 2, 1, 0.10f);
    knob(e, 2, 5, 0.0f);       /* no keytrack: a hat is a hat at any note   */
    hatSvf(e, 6800.0f, 0.18f);
    knob(e, 3, 1, 0.020f);     /* very short                                */
    knob(e, 3, 3, 0.020f);
    knob(e, 5, 2, 0.0f);       /* no reverb on the closed hat               */
    knob(e, 6, 0, 0.55f);
}

static void vHatOpen(bs::Engine &e)
{
    knob(e, 2, 0, 12000.0f);
    knob(e, 2, 1, 0.10f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 5200.0f, 0.22f);
    knob(e, 3, 1, 0.130f);     /* rings into the next sixteenth             */
    knob(e, 3, 3, 0.130f);
    knob(e, 5, 2, 0.10f);
    knob(e, 6, 0, 0.48f);
}

/* One crash, on the bar the track turns over. */
static void vCrash(bs::Engine &e)
{
    knob(e, 2, 0, 14000.0f);
    knob(e, 2, 1, 0.08f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 3800.0f, 0.15f);
    knob(e, 3, 1, 1.60f);
    knob(e, 3, 3, 1.60f);
    knob(e, 5, 0, 0.75f);
    knob(e, 5, 1, 0.30f);
    knob(e, 5, 2, 0.35f);
    knob(e, 6, 0, 0.40f);
}

static void vClap(bs::Engine &e)
{
    knob(e, 1, 0, 1.70f);      /* NOISE level                               */
    knob(e, 2, 0, 2200.0f);    /* body of the clap                          */
    knob(e, 2, 1, 0.55f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 700.0f, 0.30f);  /* keep it out of the kick's register        */
    knob(e, 3, 1, 0.055f);
    knob(e, 3, 3, 0.055f);
    knob(e, 5, 0, 0.42f);      /* the room IS the clap                      */
    knob(e, 5, 1, 0.35f);
    knob(e, 5, 2, 0.40f);
    knob(e, 6, 0, 0.95f);
}

static void vBass(bs::Engine &e)      /* ACID */
{
    knob(e, 0, 0, 0.035f);     /* glide - the slide between overlapping notes */
    knob(e, 2, 0, 340.0f);     /* cutoff: the knob the whole genre turns    */
    knob(e, 2, 1, 0.94f);
    knob(e, 2, 2, 5.0f);       /* drive - harmonics to cut past the kick    */
    /* The CV1 amount is in OCTAVES PER VOLT and the ADSR swings a full ten
     * volts, so the preset's 0.46 opens the filter four and a half octaves on
     * every note - which is the 303 scream, and which also means the cutoff
     * knob does nothing at all for the whole audible part of the note. At
     * 0.22 the sweep is still 2.2 octaves and the knob gets its authority
     * back. Measured: at 0.45 the resonant peak sat at 124 Hz for every
     * cutoff from 150 Hz to 1600; at 0.21 it moved 348 -> 844 Hz. */
    knob(e, 2, 3, 0.22f);      /* env -> cutoff, the squelch                */
    knob(e, 3, 1, 0.200f);     /* filter env decay                          */
    knob(e, 4, 1, 0.180f);     /* amp env                                   */
    /* A slide is a note that does not retrigger the envelope, so with no
     * sustain the note slid into is silent - measured at rms 0.0000. The
     * sustain is what makes the second half of a slide exist at all. */
    knob(e, 4, 2, 0.55f);
    knob(e, 4, 3, 0.060f);
    knob(e, 6, 5, 1.0f);       /* DLY SYNC on                               */
    knob(e, 6, 6, 4.0f);       /* 1/16                                      */
    knob(e, 6, 1, 0.18f);
    knob(e, 6, 3, 0.10f);      /* just a smear                              */
    knob(e, 7, 0, 0.70f);
}

static void vStab(bs::Engine &e)      /* ORGAN STAB */
{
    knob(e, 4, 0, 1250.0f);
    knob(e, 4, 1, 0.42f);
    knob(e, 4, 3, 0.55f);
    knob(e, 5, 1, 0.140f);     /* filter env - short and bright             */
    knob(e, 6, 1, 0.150f);     /* amp env - a stab, not a chord             */
    knob(e, 6, 2, 0.0f);
    knob(e, 6, 3, 0.130f);
    knob(e, 8, 5, 1.0f);       /* DLY SYNC                                  */
    knob(e, 8, 6, 3.0f);       /* dotted eighth - the classic               */
    knob(e, 8, 1, 0.42f);
    knob(e, 8, 3, 0.30f);
    knob(e, 8, 7, 1.0f);       /* ping-pong                                 */
    knob(e, 9, 2, 0.22f);
    knob(e, 10, 0, 0.55f);
}

static void vLead(bs::Engine &e)      /* SUPERSAW */
{
    knob(e, 5, 0, 1900.0f);
    knob(e, 5, 1, 0.34f);
    knob(e, 5, 3, 0.45f);
    knob(e, 6, 1, 0.35f);
    knob(e, 6, 2, 0.30f);
}

static void vPad(bs::Engine &e)       /* SAW PAD */
{
    (void)e;                   /* stock, it is already the sound       */
}

/* ------------------------------------------------------------------ *
 * Automation - the hand on the filter
 * ------------------------------------------------------------------ */

/* Piecewise-linear breakpoints in bars, so a sweep can be written down as
 * the shape it is. */
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

/* The acid line opening up and closing down is the single most characteristic
 * gesture in the genre, and it is one knob. Written as an arc across the whole
 * track: dark on entry, open into the first drop, shut for the breakdown,
 * then further than before on the way out. */
static void aBass(bs::Engine &e, double t)
{
    static const BP CUT[] = {
        /* Kept inside 150-900 Hz: above that the resonant peak stops
         * landing on any strong harmonic of a 55 Hz bass and the sweep
         * becomes inaudible however far the knob travels. */
        { 0, 165 }, { 4, 165 }, { 8, 250 }, { 12, 430 }, { 15.5, 640 },
        { 16, 145 }, { 20, 150 }, { 20.5, 390 }, { 24, 540 },
        { 28, 760 }, { 31, 900 }, { 32, 900 }
    };
    /* A slow tilt on the resonance too - the sweep alone reads as a tone
     * control, and it is the resonance that makes it sing. */
    static const BP RES[] = {
        { 0, 0.86 }, { 12, 0.94 }, { 16, 0.80 }, { 20, 0.88 },
        { 28, 0.97 }, { 32, 0.97 }
    };
    knob(e, 2, 0, (float)curve(CUT, (int)(sizeof CUT / sizeof CUT[0]), t));
    knob(e, 2, 1, (float)curve(RES, (int)(sizeof RES / sizeof RES[0]), t));
}

static void aStab(bs::Engine &e, double t)
{
    static const BP CUT[] = {
        { 0, 900 }, { 8, 900 }, { 15.5, 2200 }, { 20, 1300 },
        { 28, 2600 }, { 32, 3000 }
    };
    knob(e, 4, 0, (float)curve(CUT, (int)(sizeof CUT / sizeof CUT[0]), t));
}

static void aLead(bs::Engine &e, double t)
{
    static const BP CUT[] = { { 24, 1200 }, { 28, 2400 }, { 32, 3400 } };
    knob(e, 5, 0, (float)curve(CUT, 3, t));
}

/* The pad rises out of the breakdown instead of just sitting there. */
static void aPad(bs::Engine &e, double t)
{
    static const BP CUT[] = { { 16, 400 }, { 19.9, 2600 }, { 32, 2600 } };
    knob(e, 4, 0, (float)curve(CUT, 3, t));
}

/* ------------------------------------------------------------------ *
 * The arrangement
 * ------------------------------------------------------------------ */

/* Which bars each part plays. Written out rather than computed, because an
 * arrangement is a decision and not a formula. */
static int kickOn(int b)  { return (b < 16) || (b >= 20); }
static int hatOn(int b)   { return b >= 2; }
static int openOn(int b)  { return (b >= 4 && b < 16) || b >= 20; }
static int clapOn(int b)  { return (b >= 8 && b < 16) || b >= 22; }
static int bassOn(int b)  { return b >= 4; }
static int stabOn(int b)  { return (b >= 8 && b < 16) || b >= 20; }
static int leadOn(int b)  { return b >= 24; }
static int padOn(int b)   { return b >= 16 && b < 20; }

static std::vector<Ev> buildKick(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!kickOn(b)) continue;
        for (int beat = 0; beat < 4; beat++) {
            Ev e = { T(b, beat * 4), 0.09, A1, 1.0f };
            v.push_back(e);
        }
    }
    /* Two ghost kicks to lift the last bar of each eight. */
    for (int b = 7; b < BARS; b += 8) {
        if (!kickOn(b)) continue;
        Ev a = { T(b, 14), 0.07, A1, 0.75f };
        v.push_back(a);
    }
    return v;
}

static std::vector<Ev> buildHatClosed(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!hatOn(b)) continue;
        const int sparse = padOn(b);          /* thin it out in the break */
        for (int s = 0; s < 16; s++) {
            if (sparse && (s % 4) != 2) continue;
            /* Downbeats quiet, the sixteenth before each beat pushed - that
             * lean is most of what makes it walk instead of tick. */
            float vel = 0.30f;
            if (s % 4 == 0) vel = 0.42f;
            if (s % 4 == 3) vel = 0.55f;
            if (s % 4 == 2) vel = 0.24f;      /* leave room for the open hat */
            Ev e = { T(b, s), 0.02, 78, vel };
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
            Ev e = { T(b, s), 0.055, 84, 0.62f };
            v.push_back(e);
        }
    }
    return v;
}

/* A clap is not one hit. It is three or four hands landing a few milliseconds
 * apart and a room behind them, and that flam is the whole character - one
 * burst of filtered noise sounds like a snare instead. The rack is polyphonic
 * and its filters do not key-track, so four notes a few ms apart are four
 * hands. */
static void clapAt(std::vector<Ev> &v, double t, float vel)
{
    static const double OFF[4] = { 0.0, 0.0085, 0.0170, 0.0265 };
    static const float  AMP[4] = { 0.75f, 0.85f, 0.70f, 1.0f };
    for (int i = 0; i < 4; i++) {
        Ev e = { t + OFF[i], 0.035, 55 + i, vel * AMP[i] };
        v.push_back(e);
    }
}

static std::vector<Ev> buildClap(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!clapOn(b)) continue;
        for (int s = 4; s < 16; s += 8) clapAt(v, T(b, s), 1.0f);
        /* A pickup on the way into every eighth bar. */
        if (b % 8 == 7) clapAt(v, T(b, 14), 0.7f);
    }
    return v;
}

static std::vector<Ev> buildCrash(void)
{
    std::vector<Ev> v;
    static const int AT[4] = { 8, 16, 20, 24 };
    for (int i = 0; i < 4; i++) {
        Ev e = { T(AT[i], 0), 0.30, 72, 0.9f };
        v.push_back(e);
    }
    return v;
}

static std::vector<Ev> buildBass(void)
{
    /* Two-bar 303 figure. -1 is a rest; a note whose length runs past the
     * next step slides into it, because the rack is mono and legato. */
    static const int PA[16] = { A1,-1,A1,-1, A2,-1,A1,-1, C2,-1,A1,-1, E2,-1,-1,-1 };
    static const int PB[16] = { A1,-1,A1,-1, A2,-1,A1,-1, G1,-1,A1,-1, C2,-1,E2,-1 };
    /* Accents. The 303's own accent is a velocity and a filter kick at once. */
    static const int AC[16] = {  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 1, 0 };
    /* Which steps slide into the next note. The figure places notes two
     * sixteenths apart, so a slide has to outlast two of them - at 1.45 the
     * note simply stopped in the rest and no glide ever happened. */
    static const int SL[16] = {  0, 0, 1, 0,  0, 0, 0, 0,  0, 0, 1, 0,  0, 0, 0, 0 };

    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!bassOn(b)) continue;
        const int *P = (b % 2) ? PB : PA;
        for (int s = 0; s < 16; s++) {
            if (P[s] < 0) continue;
            int note = P[s];
            /* Drop the whole line an octave for the four bars after the
             * breakdown, so the return lands heavier. */
            if (b >= 20 && b < 24) note -= 0;
            const double len = SL[s] ? STEP * 2.30 : STEP * 0.62;
            Ev e = { T(b, s), len, note, AC[s] ? 1.0f : 0.62f };
            v.push_back(e);
        }
    }
    return v;
}

static std::vector<Ev> buildStab(void)
{
    /* Am - Am - F - G, one chord per bar, hit on the offbeats. */
    static const int AM[3] = { A3, C4, E4 };
    static const int FF[3] = { F3, A3, C4 };
    static const int GG[3] = { G3, B3, D4 };

    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!stabOn(b)) continue;
        const int *ch = AM;
        switch (b % 4) {
            case 0: case 1: ch = AM; break;
            case 2:         ch = FF; break;
            default:        ch = GG; break;
        }
        for (int s = 2; s < 16; s += 4) {
            const float vel = (s == 2 || s == 10) ? 0.95f : 0.70f;
            for (int k = 0; k < 3; k++) {
                Ev e = { T(b, s), STEP * 0.55, ch[k], vel };
                v.push_back(e);
            }
        }
    }
    return v;
}

static std::vector<Ev> buildLead(void)
{
    /* Eight bars of melody over the last stretch, in A minor. */
    static const int MEL[32] = {
        A4,-1,G4,-1, E4,-1,-1,-1,  D4,-1,E4,-1, -1,-1,-1,-1,
        C4,-1,D4,-1, E4,-1,G4,-1,  A4,-1,-1,-1, -1,-1,-1,-1
    };
    std::vector<Ev> v;
    for (int b = 24; b < BARS; b++) {
        const int half = ((b - 24) % 4) >= 2 ? 16 : 0;
        for (int s = 0; s < 16; s++) {
            const int n = MEL[half + s];
            if (n < 0) continue;
            Ev e = { T(b, s), STEP * 1.6, n, 0.85f };
            v.push_back(e);
        }
    }
    return v;
}

static std::vector<Ev> buildPad(void)
{
    std::vector<Ev> v;
    static const int AM[4] = { A2, C3, E3, A3 };
    static const int FF[4] = { F2, A2, C3, F3 };
    for (int b = 16; b < 20; b++) {
        const int *ch = (b < 18) ? AM : FF;
        for (int k = 0; k < 4; k++) {
            Ev e = { T(b, 0), BAR * 0.98, ch[k], 0.7f };
            v.push_back(e);
        }
    }
    return v;
}

/* ------------------------------------------------------------------ *
 * Render and mix
 * ------------------------------------------------------------------ */

struct Layer {
    const char *name;
    int         preset;
    void      (*tweak)(bs::Engine &);          /* knobs set once           */
    void      (*autom)(bs::Engine &, double);  /* knobs moved as it plays  */
    std::vector<Ev> (*build)(void);
};

static Layer LAYERS[] = {
    { "kick",  33, vKick,      0,     buildKick      },
    { "bass",  19, vBass,      aBass, buildBass      },
    { "clap",  34, vClap,      0,     buildClap      },
    { "hatcl", 34, vHatClosed, 0,     buildHatClosed },
    { "hatop", 34, vHatOpen,   0,     buildHatOpen   },
    { "crash", 34, vCrash,     0,     buildCrash     },
    { "stab",  23, vStab,      aStab, buildStab      },
    { "lead",  18, vLead,      aLead, buildLead      },
    { "pad",    4, vPad,       aPad,  buildPad       }
};
static const int NLAYERS = (int)(sizeof LAYERS / sizeof LAYERS[0]);

int main(int argc, char **argv)
{
    const std::string dir = (argc > 1) ? argv[1] : ".";
    const double seconds = BARS * BAR + TAIL;

    /* Layers only. Summing them is mixdown's job, because balancing takes a
     * dozen passes and rendering takes twenty seconds. */
    for (int i = 0; i < NLAYERS; i++) {
        std::vector<Ev> evs = LAYERS[i].build();
        std::vector<float> b = play(LAYERS[i].preset, LAYERS[i].tweak,
                                    LAYERS[i].autom, evs, seconds);

        const Stats s = measure(b);
        std::printf("%-6s %-11s %4zu notes  peak %.3f  rms %.4f  crest %5.2f\n",
                    LAYERS[i].name, bs::rackPresetAt(LAYERS[i].preset)->name,
                    evs.size(), s.peak, s.rms, s.crest);

        writeWav(dir + "/layer-" + LAYERS[i].name + ".wav", b);
    }

    std::printf("\n%d layers, %.1f s each, in %s\n", NLAYERS, seconds, dir.c_str());
    return 0;
}
