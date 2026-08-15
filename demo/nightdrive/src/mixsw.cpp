/*
 * Reads the layer WAVs the renderer wrote and sums them.
 *
 * Separate from the renderer because rendering is seventeen seconds and
 * balancing is a dozen attempts. It also reports what each layer contributes
 * where it actually plays - a layer that is silent for half the track has a
 * whole-file rms that says nothing about how loud it sounds.
 */
#include "bstrack.h"

#include <cstdlib>

static const double BPM  = 104.0;
static const double BAR  = 60.0 / BPM * 4.0;
static const int    BARS = 28;

struct Mix { const char *name; float gain, pan, duck; };

/* The balance, and how hard each layer gets out of the kick's way.
 *
 * The kick and the bass both peak at 63 Hz, which is the oldest problem in
 * the genre and has exactly one idiomatic answer: duck everything else on
 * every kick. It buys the low end back and it is also, audibly, the pump
 * that makes the track sound like the thing it is imitating. */
static Mix MIX[] = {
    { "kick",  0.80f,  0.00f, 0.00f },
    { "bass",  1.50f,  0.00f, 0.30f },
    { "pad",   1.45f,  0.00f, 0.22f },
    { "lead",  4.40f,  0.05f, 0.10f },
    { "arp",   3.20f,  0.26f, 0.20f },
    { "snare", 2.80f,  0.00f, 0.00f },
    { "hat",   2.15f, -0.24f, 0.15f },
    { "tom",   0.55f, -0.10f, 0.00f },
    { "crash", 1.25f,  0.08f, 0.15f }
};
static const int N = (int)(sizeof MIX / sizeof MIX[0]);

/* One trim in front of the limiter, so the balance above can be reasoned
 * about in its own terms and loudness stays a separate decision. */
static const float TRIM = 0.70f;

/* rms of the frames that are not silence, which is the level a listener
 * hears rather than the level averaged over the rests. */
static double activeRms(const std::vector<float> &b, double floorFrac)
{
    const Stats s = measure(b);
    const double thr = s.peak * floorFrac;
    double sq = 0.0; size_t n = 0;
    for (size_t i = 0; i < b.size(); i++)
        if (std::fabs(b[i]) > thr) { sq += (double)b[i] * b[i]; n++; }
    return n ? std::sqrt(sq / (double)n) : 0.0;
}

int main(int argc, char **argv)
{
    const std::string dir = (argc > 1) ? argv[1] : ".";

    std::vector<std::vector<float> > lay((size_t)N);
    size_t frames = 0;
    for (int i = 0; i < N; i++) {
        if (!readWav(dir + "/sw-" + MIX[i].name + ".wav", lay[(size_t)i])) {
            std::fprintf(stderr, "missing layer %s\n", MIX[i].name);
            return 1;
        }
        if (lay[(size_t)i].size() / 2 > frames) frames = lay[(size_t)i].size() / 2;
    }

    std::printf("layer     gain    pan   raw-rms  active-rms  post-gain\n");
    for (int i = 0; i < N; i++) {
        const Stats s = measure(lay[(size_t)i]);
        const double a = activeRms(lay[(size_t)i], 0.02);
        std::printf("%-7s %6.2f %6.2f   %.4f      %.4f     %.4f\n",
                    MIX[i].name, MIX[i].gain, MIX[i].pan, s.rms, a,
                    a * MIX[i].gain);
    }

    /* The ducking envelope, taken off the kick layer the way a sidechain
     * compressor takes it off the kick bus: instant attack, and a release
     * just short of a sixteenth so the level is back before the next hit. */
    std::vector<float> duck(frames, 0.0f);
    {
        const std::vector<float> &k = lay[0];
        const double rel = 1.0 - std::exp(-1.0 / (0.150 * RATE));
        double e = 0.0, mx = 0.0;
        for (size_t i = 0; i < frames; i++) {
            const double v = (2 * i + 1 < k.size())
                ? std::fabs(0.5 * ((double)k[2 * i] + (double)k[2 * i + 1])) : 0.0;
            if (v > e) e = v;                 /* attack: immediate */
            else       e += rel * (v - e);
            duck[i] = (float)e;
            if (e > mx) mx = e;
        }
        if (mx > 0.0)
            for (size_t i = 0; i < frames; i++) duck[i] = (float)(duck[i] / mx);
    }

    std::vector<float> mix(frames * 2, 0.0f);
    for (int i = 0; i < N; i++) {
        const double th = (MIX[i].pan * 0.5 + 0.5) * 1.5707963;
        const float gl = (float)(std::cos(th) * 1.41421356) * MIX[i].gain;
        const float gr = (float)(std::sin(th) * 1.41421356) * MIX[i].gain;
        const float dp = MIX[i].duck;
        const std::vector<float> &b = lay[(size_t)i];
        for (size_t f = 0; f < frames; f++) {
            const size_t k = f * 2;
            if (k + 1 >= b.size()) break;
            const float g = 1.0f - dp * duck[f];
            mix[k]     += b[k]     * gl * g;
            mix[k + 1] += b[k + 1] * gr * g;
        }
    }
    for (size_t i = 0; i < mix.size(); i++) mix[i] *= TRIM;

    /* One-pole DC blocker per side, as a net rather than a fix: the kick's
     * wavefolder was the culprit and is corrected at the source, but nothing
     * downstream should ever be able to ship a constant to a speaker. */
    for (int c = 0; c < 2; c++) {
        double x1 = 0.0, y1 = 0.0;
        for (size_t f = 0; f < frames; f++) {
            const double x = mix[f * 2 + c];
            const double y = x - x1 + 0.9995 * y1;
            x1 = x; y1 = y;
            mix[f * 2 + c] = (float)y;
        }
    }

    const Stats pre = measure(mix);
    std::printf("\nsum      peak %.3f  rms %.4f  crest %5.2f\n",
                pre.peak, pre.rms, pre.crest);

    /* Soft knee to round the transients, then a straight normalise to just
     * under full scale. Driving tanh hard enough to reach 0 dBFS on its own
     * would flatten the kick, which is the one thing in here that has to keep
     * its shape. */
    for (size_t i = 0; i < mix.size(); i++) mix[i] = std::tanh(mix[i] * 1.18f);
    {
        double pk = 0.0;
        for (size_t i = 0; i < mix.size(); i++)
            if (std::fabs(mix[i]) > pk) pk = std::fabs(mix[i]);
        if (pk > 0.0) {
            const float g = (float)(0.95 / pk);
            for (size_t i = 0; i < mix.size(); i++) mix[i] *= g;
        }
    }

    const Stats post = measure(mix);
    std::printf("master   peak %.3f  rms %.4f  crest %5.2f  dc %+.5f\n",
                post.peak, post.rms, post.crest, post.dc);

    /* A bar-by-bar level map, so the arrangement can be seen rather than
     * assumed: the sections have to actually differ. */
    std::printf("\nper-bar rms (each column one bar)\n");
    for (int b = 0; b < BARS; b++) {
        const size_t a = (size_t)(b * BAR * RATE) * 2;
        size_t z = (size_t)((b + 1) * BAR * RATE) * 2;
        if (z > mix.size()) z = mix.size();
        if (a >= z) break;
        const double r = measure(mix, a, z).rms;
        const int bars = (int)(r * 220.0);
        std::printf("%3d |%.*s%*s| %.4f\n", b, bars > 60 ? 60 : bars,
                    "############################################################",
                    bars > 60 ? 0 : 60 - bars, "", r);
    }

    writeWav(dir + "/nightdrive.wav", mix);
    std::printf("\nwritten: %s/nightdrive.wav\n", dir.c_str());
    return 0;
}
