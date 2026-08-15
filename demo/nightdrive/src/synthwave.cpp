/*
 * "NIGHT DRIVE" - synthwave, out of nothing but BENCsynth factory racks.
 *
 * 104 BPM, D minor, 28 bars, over Dm - Bb - F - C.
 *
 * The one thing this genre cannot do without is the gated reverb snare, and
 * no rack has one, so it gets built: the SNARE rack's reverb runs into a pair
 * of VCAs opened by a second envelope that is all attack and sustain and no
 * release worth the name. The reverb blooms and then stops dead, which is the
 * sound of every drum in 1984 and is a gate on a reverb return, exactly as it
 * was done then.
 */
#include "bstrack.h"

#include <algorithm>

static const double BPM   = 104.0;
static const double BEAT  = 60.0 / BPM;          /* 0.5769 s                */
static const double BAR   = BEAT * 4.0;          /* 2.3077 s                */
static const double STEP  = BEAT / 4.0;
static const int    BARS  = 28;                  /* 64.6 s                  */
static const double TAIL  = 4.0;                 /* long verbs, long tail   */

/* D minor. */
enum {
    C2 = 36, D2 = 38, Bb1 = 34, F2 = 41,
    C3 = 48, D3 = 50, E3 = 52, F3 = 53, G3 = 55, A3 = 57, Bb2 = 46, Bb3 = 58,
    C4 = 60, D4 = 62, E4 = 64, F4 = 65, G4 = 67, A4 = 69, Bb4 = 70,
    C5 = 72, D5 = 74, E5 = 76, F5 = 77, G5 = 79, A5 = 81
};
#define R (-1)

struct Ev { double at, len; int note; float vel; };

/* ------------------------------------------------------------------ */

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
        edges.push_back({ (long)(evs[i].at * RATE), 1, evs[i].note, evs[i].vel });
        edges.push_back({ (long)((evs[i].at + evs[i].len) * RATE), 0,
                          evs[i].note, 0.0f });
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

static void addBar(std::vector<Ev> &v, int bar, const int *steps,
                   double gate, float vel)
{
    for (int s = 0; s < 16; s++) {
        if (steps[s] < 0) continue;
        int nxt = s + 1;
        while (nxt < 16 && steps[nxt] < 0) nxt++;
        v.push_back({ T(bar, s), (double)(nxt - s) * gate * STEP, steps[s], vel });
    }
}

/* ------------------------------------------------------------------ *
 * The tune
 * ------------------------------------------------------------------ */

/* Long notes and a lot of space - the melody is meant to sing over the
 * bassline rather than compete with it. */
static const int MEL[8][16] = {
 /* Dm */ { A4, R,R,R, R,R,R,R,  D5, R,R,R, R,R,R,R },
 /* Bb */ { C5, R,R,R, R,R,R,R,  D5, R,R,R, R,R,R,R },
 /* F  */ { F5, R,R,R, R,R,R,R,  E5, R,R,R, R,R,R,R },
 /* C  */ { D5, R,R,R, R,R,R,R,  C5, R,R,R, R,R,R,R },
 /* Dm */ { D5, R,R,R, F5,R,R,R, A5, R,R,R, R,R,R,R },
 /* Bb */ { G5, R,R,R, F5,R,R,R, D5, R,R,R, R,R,R,R },
 /* F  */ { C5, R,R,R, D5,R,R,R, F5, R,R,R, E5,R,R,R },
 /* C  */ { D5, R,R,R, R,R,R,R,  A4, R,R,R, R,R,R,R }
};

static const int PAD[4][4] = {
    { D3, F3, A3, D4 },      /* Dm */
    { Bb2, D3, F3, Bb3 },    /* Bb */
    { F3, A3, C4, F4 },      /* F  */
    { C3, E3, G3, C4 }       /* C  */
};
static const int ARP[4][4] = {
    { D4, F4, A4, D5 },
    { D4, F4, Bb4, D5 },
    { C4, F4, A4, C5 },
    { C4, E4, G4, C5 }
};
static const int ROOT[4] = { D2, Bb1, F2, C2 };

/* ------------------------------------------------------------------ *
 * Voicing
 * ------------------------------------------------------------------ */

/* Tight and dry. A LinnDrum kick is not a rave kick: it lands, it does not
 * linger, and at 104 BPM there is room for it to be heard rather than felt. */
static void vKick(bs::Engine &e)
{
    knob(e, 0, 1,  0.0f);
    knob(e, 0, 3,  2.0f);
    knob(e, 1, 1,  0.040f);
    knob(e, 1, 3,  0.040f);
    knob(e, 2, 4,  0.50f);
    knob(e, 3, 1,  0.26f);
    knob(e, 3, 3,  0.26f);
    knob(e, 5, 0,  0.70f);
}

/* Toms are the same rack an octave up with the pitch drop softened. */
static void vTom(bs::Engine &e)
{
    knob(e, 0, 1,  1.0f);
    knob(e, 0, 3,  4.0f);
    knob(e, 1, 1,  0.090f);
    knob(e, 1, 3,  0.090f);
    knob(e, 2, 4,  0.22f);
    knob(e, 3, 1,  0.34f);
    knob(e, 3, 3,  0.34f);
    knob(e, 5, 0,  0.55f);
}

static int hatSvf(bs::Engine &e, float cutoff, float res)
{
    const int svf = addMod(e, "SVF");
    wire(e, 2, 0, svf, 0);
    wire(e, svf, 1, 4, 0);
    knob(e, svf, 0, cutoff);
    knob(e, svf, 1, res);
    return svf;
}

static void vHat(bs::Engine &e)
{
    knob(e, 2, 0, 11000.0f);
    knob(e, 2, 1, 0.10f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 6500.0f, 0.18f);
    knob(e, 3, 1, 0.026f);
    knob(e, 3, 3, 0.026f);
    knob(e, 5, 2, 0.0f);
    knob(e, 6, 0, 0.45f);
}

/* The snare, and the gate on its reverb.
 *
 * The rack ends VCA -> RVB -> OUT, so the reverb tail runs as long as the
 * reverb wants. Putting a VCA on each side of the reverb's output and opening
 * both with an envelope that has no decay and no release turns that tail into
 * a block: full level while the note is held, nothing a few milliseconds after
 * it is let go. The note length is therefore the gate time, and 190 ms is the
 * length that sounds like the records. */
static void vSnare(bs::Engine &e)
{
    knob(e, 1, 0, 1.55f);          /* NOISE                                 */
    knob(e, 2, 0, 1900.0f);        /* VCF - body                            */
    knob(e, 2, 1, 0.42f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 200.0f, 0.25f);      /* keep it off the kick                  */
    knob(e, 3, 1, 0.085f);         /* the hit itself is short               */
    knob(e, 3, 3, 0.085f);

    knob(e, 5, 0, 0.88f);          /* RVB SIZE - a big room                 */
    knob(e, 5, 1, 0.22f);          /* DAMP - bright, which is the point     */
    knob(e, 5, 2, 0.58f);          /* MIX - wet, but the crack still lands  */

    const int ge = addMod(e, "ADSR");
    wire(e, 0, 1, ge, 0);          /* KBD GATE -> ADSR GATE                 */
    wire(e, 0, 2, ge, 1);          /* KBD TRIG -> ADSR TRIG                 */
    knob(e, ge, 0, 0.001f);        /* A                                     */
    knob(e, ge, 1, 0.001f);        /* D                                     */
    knob(e, ge, 2, 1.0f);          /* S - hold wide open                    */
    knob(e, ge, 3, 0.004f);        /* R - slam shut                         */

    const int vl = addMod(e, "VCA");
    const int vr = addMod(e, "VCA");
    wire(e, 5, 0, vl, 0);          /* RVB L -> VCA L                        */
    wire(e, 5, 1, vr, 0);          /* RVB R -> VCA R                        */
    wire(e, ge, 0, vl, 1);
    wire(e, ge, 0, vr, 1);
    wire(e, vl, 0, 6, 0);          /* -> OUT L                              */
    wire(e, vr, 0, 6, 1);          /* -> OUT R                              */
    knob(e, vl, 1, 1.0f);
    knob(e, vr, 1, 1.0f);
    knob(e, 6, 0, 1.25f);
}

static void vCrash(bs::Engine &e)
{
    knob(e, 2, 0, 14000.0f);
    knob(e, 2, 1, 0.08f);
    knob(e, 2, 5, 0.0f);
    hatSvf(e, 3400.0f, 0.15f);
    knob(e, 3, 1, 1.90f);
    knob(e, 3, 3, 1.90f);
    knob(e, 5, 0, 0.80f);
    knob(e, 5, 2, 0.40f);
    knob(e, 6, 0, 0.38f);
}

/* Straight eighths, and the whole track rides on it. */
static void vBass(bs::Engine &e)      /* BASS (2) */
{
    knob(e, 0, 0, 0.0f);
    knob(e, 3, 0, 0.85f);
    knob(e, 3, 1, 0.55f);
    knob(e, 4, 0, 380.0f);
    knob(e, 4, 1, 0.30f);
    knob(e, 4, 2, 2.2f);
    knob(e, 4, 3, 0.14f);      /* 1.4 octaves of envelope, not three        */
    knob(e, 5, 1, 0.130f);
    knob(e, 6, 0, 0.004f);
    knob(e, 6, 1, 0.180f);
    knob(e, 6, 2, 0.30f);
    knob(e, 6, 3, 0.045f);
    knob(e, 8, 0, 0.75f);
}

/* The pad is the weather. Slow in, slow out, and wide. */
static void vPad(bs::Engine &e)       /* SAW PAD (4) */
{
    knob(e, 5, 0, 0.55f);      /* filter env attack                         */
    knob(e, 6, 0, 0.70f);      /* amp attack - it swells                    */
    knob(e, 6, 3, 1.60f);      /* and takes its time leaving                */
    knob(e, 10, 2, 0.38f);     /* RVB mix                                   */

    const int ch = addMod(e, "CHORUS");
    wire(e, 10, 0, ch, 0);
    wire(e, 10, 1, ch, 1);
    wire(e, ch, 0, 11, 0);
    wire(e, ch, 1, 11, 1);
    knob(e, ch, 0, 0.28f);     /* RATE  - slow                              */
    knob(e, ch, 1, 0.70f);     /* DEPTH - deep, this is the 80s             */
    knob(e, ch, 4, 0.55f);     /* MIX                                       */
    knob(e, 11, 0, 0.50f);
}

/* Lead: glide, echo, and a room. The rack has the first two already. */
static void vLead(bs::Engine &e)      /* SQUARE LEAD (3) */
{
    knob(e, 0, 0, 0.055f);     /* GLIDE - the portamento is the style       */
    knob(e, 1, 3, 0.42f);      /* pulse width                               */
    knob(e, 1, 5, 0.30f);      /* a little PWM movement                     */
    knob(e, 2, 0, 2400.0f);
    knob(e, 2, 1, 0.26f);
    knob(e, 2, 3, 0.14f);
    knob(e, 3, 0, 0.020f);
    knob(e, 3, 1, 0.50f);
    knob(e, 3, 2, 0.75f);
    knob(e, 3, 3, 0.30f);
    knob(e, 6, 5, 1.0f);       /* DLY SYNC                                  */
    knob(e, 6, 6, 3.0f);       /* dotted eighth                             */
    knob(e, 6, 1, 0.40f);
    knob(e, 6, 3, 0.30f);

    /* A pulse wave whose width is being modulated has a DC term that moves
     * with the modulation, which is subsonic rumble rather than sound: the
     * lead measured -5 dB at 32 Hz against its own 500 Hz peak. The lowest
     * note it ever plays is A4, so a high-pass at 130 Hz costs nothing and
     * buys back the headroom. */
    const int hp = addMod(e, "SVF");
    wire(e, 4, 0, hp, 0);      /* VCA -> HP  */
    wire(e, hp, 1, 6, 0);      /* HP  -> DLY */
    knob(e, hp, 0, 130.0f);
    knob(e, hp, 1, 0.05f);

    /* The rack has no reverb and this genre is mostly reverb. */
    const int ch = addMod(e, "CHORUS");
    const int rv = addMod(e, "RVB");
    wire(e, 6, 0, ch, 0);      /* DLY OUT   -> CHORUS IN   */
    wire(e, 6, 2, ch, 1);      /* DLY OUT R -> CHORUS IN R */
    wire(e, ch, 0, rv, 0);
    wire(e, ch, 1, rv, 1);
    wire(e, rv, 0, 7, 0);
    wire(e, rv, 1, 7, 1);
    knob(e, ch, 0, 0.35f);
    knob(e, ch, 1, 0.55f);
    knob(e, ch, 4, 0.40f);
    knob(e, rv, 0, 0.72f);     /* SIZE */
    knob(e, rv, 1, 0.30f);     /* DAMP */
    knob(e, rv, 2, 0.34f);     /* MIX  */
    knob(e, 7, 0, 0.60f);
}

static void vArp(bs::Engine &e)       /* PLUCK (15) */
{
    knob(e, 2, 0, 1500.0f);
    knob(e, 2, 1, 0.55f);
    knob(e, 3, 1, 0.100f);
    knob(e, 4, 1, 0.240f);
    knob(e, 4, 2, 0.0f);
    knob(e, 6, 5, 1.0f);       /* DLY SYNC */
    knob(e, 6, 6, 4.0f);       /* 1/16     */
    knob(e, 6, 1, 0.26f);
    knob(e, 6, 3, 0.22f);
    knob(e, 7, 2, 0.26f);      /* RVB mix  */
    knob(e, 8, 0, 1.05f);
}

/* ------------------------------------------------------------------ *
 * Automation
 * ------------------------------------------------------------------ */

struct BP { double bar, v; };

static double curve(const BP *p, int n, double t)
{
    const double bar = t / BAR;
    if (bar <= p[0].bar) return p[0].v;
    for (int i = 1; i < n; i++)
        if (bar <= p[i].bar) {
            const double f = (bar - p[i - 1].bar) / (p[i].bar - p[i - 1].bar);
            return p[i - 1].v + f * (p[i].v - p[i - 1].v);
        }
    return p[n - 1].v;
}

/* The pad opens as the track does, shuts for the breakdown, opens furthest at
 * the end. It is the only thing playing in every bar, so it carries the shape. */
static void aPad(bs::Engine &e, double t)
{
    static const BP CUT[] = {
        { 0, 700 }, { 4, 1100 }, { 12, 1900 }, { 19, 2400 },
        { 20, 1000 }, { 24, 2600 }, { 28, 3000 }
    };
    knob(e, 4, 0, (float)curve(CUT, 7, t));
}

static void aArp(bs::Engine &e, double t)
{
    static const BP CUT[] = { { 0, 1100 }, { 12, 1900 }, { 20, 1200 },
                              { 27, 2800 }, { 28, 2800 } };
    knob(e, 2, 0, (float)curve(CUT, 5, t));
}

static void aBass(bs::Engine &e, double t)
{
    static const BP CUT[] = { { 4, 300 }, { 12, 430 }, { 19, 560 },
                              { 24, 480 }, { 28, 620 } };
    knob(e, 4, 0, (float)curve(CUT, 5, t));
}

/* ------------------------------------------------------------------ *
 * Arrangement
 *
 * intro 0-3   verse 4-11   chorus 12-19   breakdown 20-23   out 24-27
 * ------------------------------------------------------------------ */

static int padOn(int b)   { (void)b; return 1; }
static int arpOn(int b)   { return b < 4 || (b >= 8 && b < 20) || b >= 24; }
static int bassOn(int b)  { return (b >= 4 && b < 20) || b >= 24; }
static int kickOn(int b)  { return (b >= 4 && b < 20) || b >= 24; }
static int snareOn(int b) { return (b >= 4 && b < 20) || b >= 24; }
static int hatOn(int b)   { return (b >= 6 && b < 20) || b >= 24; }
static int leadOn(int b)  { return b >= 12; }

static std::vector<Ev> buildKick(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!kickOn(b)) continue;
        v.push_back({ T(b, 0),  0.08, D2 - 5, 1.0f });   /* beat 1 */
        v.push_back({ T(b, 8),  0.08, D2 - 5, 0.95f });  /* beat 3 */
        /* The pickup eighth before beat 3, every other bar - the little push
         * that keeps a half-time pattern from sitting still. */
        if (b % 2 == 1) v.push_back({ T(b, 6), 0.07, D2 - 5, 0.7f });
    }
    return v;
}

static std::vector<Ev> buildSnare(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!snareOn(b)) continue;
        /* 190 ms is the gate time, and the gate time is the sound. */
        v.push_back({ T(b, 4),  0.190, 57, 1.0f });
        v.push_back({ T(b, 12), 0.190, 57, 1.0f });
    }
    return v;
}

static std::vector<Ev> buildHat(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!hatOn(b)) continue;
        for (int s = 2; s < 16; s += 4)
            v.push_back({ T(b, s), 0.024, 80, 0.45f });
        for (int s = 0; s < 16; s += 4)
            v.push_back({ T(b, s), 0.024, 80, 0.22f });
    }
    return v;
}

static std::vector<Ev> buildTom(void)
{
    std::vector<Ev> v;
    static const int AT[3] = { 11, 19, 23 };
    static const int N[4]  = { 52, 50, 45, 43 };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++)
            v.push_back({ T(AT[i], 8 + k * 2), 0.16, N[k], 0.80f + 0.05f * k });
    return v;
}

static std::vector<Ev> buildCrash(void)
{
    std::vector<Ev> v;
    static const int AT[4] = { 4, 12, 20, 24 };
    for (int i = 0; i < 4; i++)
        v.push_back({ T(AT[i], 0), 0.40, 72, 0.85f });
    return v;
}

static std::vector<Ev> buildBass(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!bassOn(b)) continue;
        const int r = ROOT[b % 4];
        for (int s = 0; s < 16; s += 2) {
            /* Straight eighths on the root, with the last one an octave up so
             * the bar hands itself to the next. */
            const int n = (s == 14) ? r + 12 : r;
            v.push_back({ T(b, s), STEP * 1.55, n, s % 4 == 0 ? 0.95f : 0.78f });
        }
    }
    return v;
}

static std::vector<Ev> buildPad(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!padOn(b)) continue;
        for (int k = 0; k < 4; k++)
            v.push_back({ T(b, 0), BAR * 0.99, PAD[b % 4][k], 0.62f });
    }
    return v;
}

static std::vector<Ev> buildArp(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!arpOn(b)) continue;
        for (int s = 0; s < 16; s++)
            v.push_back({ T(b, s), STEP * 0.70, ARP[b % 4][s % 4],
                          s % 4 == 0 ? 0.80f : 0.60f });
    }
    return v;
}

static std::vector<Ev> buildLead(void)
{
    std::vector<Ev> v;
    for (int b = 0; b < BARS; b++) {
        if (!leadOn(b)) continue;
        /* Legato, so the glide between notes actually happens. */
        addBar(v, b, MEL[(b - 12) % 8], 1.04, 0.90f);
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
    { "kick",  33, vKick,  0,     buildKick  },
    { "bass",   2, vBass,  aBass, buildBass  },
    { "pad",    4, vPad,   aPad,  buildPad   },
    { "lead",   3, vLead,  0,     buildLead  },
    { "arp",   15, vArp,   aArp,  buildArp   },
    { "snare", 34, vSnare, 0,     buildSnare },
    { "hat",   34, vHat,   0,     buildHat   },
    { "tom",   33, vTom,   0,     buildTom   },
    { "crash", 34, vCrash, 0,     buildCrash }
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
        std::printf("%-6s %-11s %4zu notes  peak %.3f  rms %.4f  crest %5.2f  "
                    "dc %+.5f\n",
                    LAYERS[i].name, bs::rackPresetAt(LAYERS[i].preset)->name,
                    evs.size(), s.peak, s.rms, s.crest, s.dc);
        writeWav(dir + "/sw-" + LAYERS[i].name + ".wav", b);
    }
    std::printf("\n%d layers, %.1f s each, in %s\n", NLAYERS, seconds, dir.c_str());
    return 0;
}
