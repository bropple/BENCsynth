/*
 * BENCsynth - the module set
 *
 * Each class is a panel. The constructor declares the knobs and jacks in the
 * order they appear on it, because the GUI lays a panel out from those lists
 * rather than from a description written twice; process() then does the work
 * with the accessors from bs_module.h.
 *
 * The loops are all the same shape: work out how many channels are arriving,
 * declare that many on the outputs, then run the per-sample code once per
 * channel. The per-channel state lives in BS_MAX_POLY-long arrays, so one
 * panel really is eight oscillators when eight notes are down and one when
 * one is.
 */

#include "bs_modules.h"
#include "bs_keys.h"

#include <cstring>

namespace bs {

/* Audio leaves a module at this amplitude for a full-scale signal. Everything
 * agrees on it so that a VCO into a VCF into a VCA arrives at the output stage
 * at a sane level with every knob at noon. */
static const float VOLT = 5.0f;
static const float GATE_HI = 10.0f;

static float gateOpen(float v) { return v >= 1.0f; }

/* ================================================================== *
 * KBD - keyboard interface
 * ================================================================== */

ModuleKbd::ModuleKbd() : keys(0)
{
    /* Wider than its knob count needs, because "V/OCT" under a jack is the
     * longest label in the module set and a truncated one is a jack nobody
     * can identify. */
    configure("KBD", "KBD", 7, 2);

    static const char *MODES[] = { "POLY", "MONO", "LEGATO" };

    addKnob("GLIDE", 0.0f, 2.0f, 0.0f, "%.2f s", PC_QUAD);
    addKnob("OCT",  -3.0f, 3.0f, 0.0f, "%+.0f", PC_LIN, 1.0f);
    addKnob("BEND",  0.0f, 12.0f, 2.0f, "%.0f st", PC_LIN, 1.0f);
    addKnob("VOICES", 1.0f, (float)BS_MAX_POLY, 4.0f, "%.0f", PC_LIN, 1.0f);
    addSwitch("MODE", MODES, 3, KM_POLY);

    addOutput("V/OCT");
    addOutput("GATE");
    addOutput("TRIG");
    addOutput("VEL");
    addOutput("MOD");
    addOutput("BEND");

    reset();
}

void ModuleKbd::reset()
{
    for (int i = 0; i < BS_MAX_POLY; i++) {
        glide[i].reset(0.0f);
        trigTimer[i] = 0.0f;
        primed[i]    = false;
    }
}

void ModuleKbd::process()
{
    if (!keys) { setAllOutputChannels(1); for (size_t i = 0; i < outs.size(); i++) outs[i].clear(); return; }

    keys->mode = pi(4);
    keys->setPolyphony((int)(pv(3) + 0.5f));

    const int ch = keys->channels();
    setAllOutputChannels(ch);

    const float glideTime = pv(0);
    const float octave    = pv(1);
    const float bendRange = pv(2);
    const float bendV     = keys->bend * bendRange / 12.0f;
    const float modV      = keys->mod * GATE_HI;

    for (int c = 0; c < ch; c++) {
        KeyVoice &kv = keys->v[c];

        /* A channel that has never sounded starts at the pitch it is about to
         * play rather than gliding up from middle C the first time it is
         * used - which is what an unprimed portamento sounds like, and it
         * sounds like a mistake. */
        const float target = (kv.note >= 0) ? noteToVolts((float)kv.note) + octave : 0.0f;
        if (kv.note >= 0 && !primed[c]) { glide[c].reset(target); primed[c] = true; }

        if (kv.retrig) trigTimer[c] = 0.001f;   /* 1 ms trigger pulse */

        const float gate = kv.gate ? GATE_HI : 0.0f;
        const float vel  = kv.vel * GATE_HI;

        for (int i = 0; i < BS_BLOCK; i++) {
            const float p = glide[c].step(target, glideTime, sr);
            outs[0].v[c][i] = p + bendV;
            outs[1].v[c][i] = gate;
            outs[2].v[c][i] = trigTimer[c] > 0.0f ? GATE_HI : 0.0f;
            outs[3].v[c][i] = vel;
            outs[4].v[c][i] = modV;
            outs[5].v[c][i] = keys->bend * VOLT;
            if (trigTimer[c] > 0.0f) trigTimer[c] -= st;
        }
    }
    /* The retrigger flags are cleared by the engine after every module has
     * run, not here - two keyboard panels in one rack would otherwise have the
     * first to run clear them before the second could see them. */
}

/* ================================================================== *
 * VCO
 * ================================================================== */

class ModuleVco : public Module {
public:
    ModuleVco()
    {
        configure("VCO", "VCO", 7, 3);
        addKnob("OCT",    -4.0f, 4.0f, 0.0f, "%+.0f", PC_LIN, 1.0f);
        addKnob("COARSE", -7.0f, 7.0f, 0.0f, "%+.0f st", PC_LIN, 1.0f);
        addKnob("FINE",  -50.0f, 50.0f, 0.0f, "%+.0f c");
        addKnob("PW",      0.05f, 0.95f, 0.5f, "%.2f");
        addKnob("FM",      0.0f, 1.0f, 0.0f, "%.2f");
        addKnob("PWM",     0.0f, 1.0f, 0.0f, "%.2f");

        addInput("V/OCT");
        addInput("FM");
        addInput("PWM");
        addInput("SYNC");

        addOutput("SAW");
        addOutput("PLS");
        addOutput("TRI");
        addOutput("SIN");
    }

    void reset() { for (int i = 0; i < BS_MAX_POLY; i++) osc[i].reset(); }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);

        const float base = pv(0) + pv(1) / 12.0f + pv(2) / 1200.0f;
        const float fmA  = pv(4);
        const float pwA  = pv(5);
        const float pw0  = pv(3);
        const bool  syncPatched = patched(3);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float pitch = base + in(0).get(c, i) + fmA * in(1).get(c, i);
                const float dt    = voltsToHz(pitch) * st;
                const float pw    = pw0 + pwA * in(2).get(c, i) / VOLT;

                if (syncPatched) osc[c].syncEdge(in(3).get(c, i));

                float saw, pls, tri, sin;
                osc[c].step(dt, pw, &saw, &pls, &tri, &sin);
                outs[0].v[c][i] = saw * VOLT;
                outs[1].v[c][i] = pls * VOLT;
                outs[2].v[c][i] = tri * VOLT;
                outs[3].v[c][i] = sin * VOLT;
            }
        }
    }

private:
    BlepOsc osc[BS_MAX_POLY];
};

/* ================================================================== *
 * LFO
 * ================================================================== */

class ModuleLfo : public Module {
public:
    ModuleLfo()
    {
        configure("LFO", "LFO", 5, 2);
        addKnob("RATE", 0.02f, 40.0f, 2.0f, "%.2f Hz", PC_EXP);
        addKnob("CV",   -2.0f, 2.0f, 0.0f, "%+.2f");
        addToggle("UNI", false);

        addInput("RATE");
        addInput("SYNC");

        addOutput("SIN");
        addOutput("TRI");
        addOutput("SQR");
        addOutput("SAW");
    }

    void reset()
    {
        for (int i = 0; i < BS_MAX_POLY; i++) { osc[i].reset(); osc[i].phase = 0.25f; }
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);

        const float rate = pv(0);
        const float cvA  = pv(1);
        const bool  uni  = pb(2);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float hz = clampf(rate * std::exp2(cvA * in(0).get(c, i)),
                                        0.005f, 400.0f);
                osc[c].syncEdge(in(1).get(c, i));

                float saw, pls, tri, sin;
                osc[c].step(hz * st, 0.5f, &saw, &pls, &tri, &sin);

                /* Unipolar is the same shape lifted so it never goes below
                 * zero, which is what a VCA wants and what a bipolar LFO
                 * silently halves the level of. */
                const float k = uni ? 0.5f : 1.0f;
                const float b = uni ? 1.0f : 0.0f;
                outs[0].v[c][i] = (sin + b) * k * VOLT;
                outs[1].v[c][i] = (tri + b) * k * VOLT;
                outs[2].v[c][i] = (pls + b) * k * VOLT;
                outs[3].v[c][i] = (saw + b) * k * VOLT;
            }
        }
    }

private:
    BlepOsc osc[BS_MAX_POLY];
};

/* ================================================================== *
 * NOISE / S&H
 * ================================================================== */

class ModuleNoise : public Module {
public:
    ModuleNoise()
    {
        configure("NOISE", "NOISE", 5, 2);
        addKnob("LEVEL", 0.0f, 2.0f, 1.0f, "%.2f");

        addInput("S&H IN");
        addInput("TRIG");

        addOutput("WHT");
        addOutput("PNK");
        addOutput("S&H");
    }

    void reset()
    {
        for (int i = 0; i < BS_MAX_POLY; i++) {
            noise[i].seed((uint32_t)(0x9e3779b9u * (uint32_t)(i + 1)));
            held[i] = 0.0f;
            last[i] = 0.0f;
        }
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);
        const float lvl = pv(0);
        const bool  ext = patched(0);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float w = noise[c].white();
                const float p = noise[c].pink();
                outs[0].v[c][i] = w * VOLT * lvl;
                outs[1].v[c][i] = p * VOLT * lvl;

                /* Unpatched, the sample-and-hold samples its own noise - the
                 * classic random-stepped-voltage patch, without the cable. */
                const float trg = in(1).get(c, i);
                if (last[c] < 1.0f && trg >= 1.0f)
                    held[c] = ext ? in(0).get(c, i) : w * VOLT;
                last[c] = trg;
                outs[2].v[c][i] = held[c];
            }
        }
    }

private:
    Noise noise[BS_MAX_POLY];
    float held[BS_MAX_POLY];
    float last[BS_MAX_POLY];
};

/* ================================================================== *
 * VCF - Moog ladder
 * ================================================================== */

class ModuleVcf : public Module {
public:
    ModuleVcf()
    {
        configure("VCF", "VCF", 7, 3);
        addKnob("CUTOFF", 20.0f, 18000.0f, 1200.0f, "%.0f Hz", PC_EXP);
        addKnob("RES",     0.0f, 1.08f, 0.15f, "%.2f");
        addKnob("DRIVE",   1.0f, 8.0f, 1.0f, "%.2f");
        addKnob("CV1",    -2.0f, 2.0f, 0.0f, "%+.2f");
        addKnob("CV2",    -2.0f, 2.0f, 0.0f, "%+.2f");
        addKnob("KTRK",    0.0f, 1.0f, 0.0f, "%.2f");

        addInput("IN");
        addInput("CV1");
        addInput("CV2");
        addInput("V/OCT");
        addInput("RES");

        addOutput("LP24");
        addOutput("LP12");
    }

    void onSampleRate()
    {
        for (int i = 0; i < BS_MAX_POLY; i++) filt[i].setSampleRate(sr);
    }
    void reset() { for (int i = 0; i < BS_MAX_POLY; i++) filt[i].reset(); }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);

        const float f0    = pv(0);
        const float res0  = pv(1);
        const float drive = pv(2);
        const float a1    = pv(3);
        const float a2    = pv(4);
        const float ktrk  = pv(5);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                /* Every contribution is in octaves, which is what makes a
                 * cutoff CV from an envelope and a cutoff CV from the keyboard
                 * add up the way the panel implies. */
                const float oct = a1 * in(1).get(c, i)
                                + a2 * in(2).get(c, i)
                                + ktrk * in(3).get(c, i);
                const float fc  = clampf(f0 * std::exp2(oct), 8.0f, sr * 0.45f);
                const float res = clampf(res0 + in(4).get(c, i) * 0.1f, 0.0f, 1.15f);

                const float y = filt[c].step(in(0).get(c, i) / VOLT, fc, res, drive);
                outs[0].v[c][i] = y * VOLT;
                outs[1].v[c][i] = filt[c].tap2 * VOLT;
            }
        }
    }

private:
    Ladder filt[BS_MAX_POLY];
};

/* ================================================================== *
 * VCA
 * ================================================================== */

class ModuleVca : public Module {
public:
    ModuleVca()
    {
        configure("VCA", "VCA", 5, 2);
        /* RING is a third response rather than a separate module: an amplifier
         * whose gain is allowed to go negative is a ring modulator, and that is
         * the only difference between the two. Appended rather than inserted,
         * so a saved patch's 0 and 1 still mean what they meant. */
        static const char *RESP[] = { "LIN", "EXP", "RING" };
        addKnob("GAIN", 0.0f, 1.0f, 0.0f, "%.2f");
        addKnob("CV",   0.0f, 1.0f, 1.0f, "%.2f");
        addSwitch("RESP", RESP, 3, 1);

        addInput("IN");
        addInput("CV");

        addOutput("OUT");
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);

        const float g0   = pv(0);
        const float cvA  = pv(1);
        const int   resp = pi(2);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                if (resp == 2) {
                    /* Four quadrants: the gain follows the control voltage
                     * through zero and out the other side, so a bipolar
                     * audio-rate CV multiplies the two signals together and
                     * what comes out is their sum and difference frequencies
                     * and none of either original. Scaled so +/-5 V is +/-1. */
                    const float g = g0 + cvA * in(1).get(c, i) * 0.2f;
                    outs[0].v[c][i] = in(0).get(c, i) * clampf(g, -1.5f, 1.5f);
                    continue;
                }
                /* CV is read against the 10 V an envelope or a gate produces,
                 * so a full envelope opens the VCA fully. */
                float g = g0 + cvA * in(1).get(c, i) * 0.1f;
                g = clampf(g, 0.0f, 1.5f);
                if (resp == 1) {
                    /* Exponential, and then held to the same ceiling as the
                     * linear response. The curve is normalised so 1 maps to 1,
                     * which means the headroom above it does not map to 1.5 -
                     * it maps to 7.5, a seventeen decibel jump on a knob that
                     * looks like it does the same thing as the one next to it.
                     * No factory rack reaches it, because they all leave GAIN
                     * at zero, so this only ever bit somebody riding it. */
                    g = (std::exp(4.0f * g) - 1.0f) * (1.0f / 53.598150f);
                    if (g > 1.5f) g = 1.5f;
                }
                outs[0].v[c][i] = in(0).get(c, i) * g;
            }
        }
    }
};

/* ================================================================== *
 * ADSR
 * ================================================================== */

class ModuleAdsr : public Module {
public:
    ModuleAdsr()
    {
        configure("ADSR", "ADSR", 6, 2);
        addKnob("A", 0.001f, 8.0f, 0.01f, "%.3f s", PC_QUAD);
        addKnob("D", 0.001f, 8.0f, 0.30f, "%.3f s", PC_QUAD);
        addKnob("S", 0.0f, 1.0f, 0.70f, "%.2f");
        addKnob("R", 0.001f, 8.0f, 0.40f, "%.3f s", PC_QUAD);
        addKnob("VEL", 0.0f, 1.0f, 0.0f, "%.2f");

        addInput("GATE");
        addInput("TRIG");
        addInput("VEL");

        addOutput("ENV");
        addOutput("INV");
    }

    void onSampleRate()
    {
        for (int i = 0; i < BS_MAX_POLY; i++) env[i].setSampleRate(sr);
    }
    void reset()
    {
        for (int i = 0; i < BS_MAX_POLY; i++) { env[i].reset(); lastTrig[i] = 0.0f; }
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);

        const float a = pv(0), d = pv(1), s = pv(2), r = pv(3);
        const float velA = pv(4);
        const bool  hasVel = patched(2);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                env[c].gate(gateOpen(in(0).get(c, i)));

                const float trg = in(1).get(c, i);
                if (lastTrig[c] < 1.0f && trg >= 1.0f) env[c].retrigger();
                lastTrig[c] = trg;

                float e = env[c].step(a, d, s, r);
                if (hasVel) {
                    const float v = clampf(in(2).get(c, i) * 0.1f, 0.0f, 1.0f);
                    e *= 1.0f - velA + velA * v;
                }
                outs[0].v[c][i] = e * GATE_HI;
                outs[1].v[c][i] = (1.0f - e) * GATE_HI;
            }
        }
    }

private:
    ADSR  env[BS_MAX_POLY];
    float lastTrig[BS_MAX_POLY];
};

/* ================================================================== *
 * MIX - four in, one out
 * ================================================================== */

class ModuleMix : public Module {
public:
    ModuleMix()
    {
        configure("MIX", "MIX", 5, 2);
        addKnob("1", 0.0f, 1.0f, 0.75f, "%.2f");
        addKnob("2", 0.0f, 1.0f, 0.0f, "%.2f");
        addKnob("3", 0.0f, 1.0f, 0.0f, "%.2f");
        addKnob("4", 0.0f, 1.0f, 0.0f, "%.2f");
        addKnob("MASTER", 0.0f, 2.0f, 1.0f, "%.2f");

        addInput("IN1");
        addInput("IN2");
        addInput("IN3");
        addInput("IN4");

        addOutput("OUT");
        addOutput("INV");
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);
        const float m = pv(4);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                float y = 0.0f;
                for (int k = 0; k < 4; k++) y += in(k).get(c, i) * pv(k);
                y *= m;
                outs[0].v[c][i] =  y;
                outs[1].v[c][i] = -y;
            }
        }
    }
};

/* ================================================================== *
 * MULT - two buffered multiples
 * ================================================================== */

class ModuleMult : public Module {
public:
    ModuleMult()
    {
        configure("MULT", "MULT", 4, 1);
        addInput("A");
        addInput("B");
        addOutput("A1"); addOutput("A2"); addOutput("A3");
        addOutput("B1"); addOutput("B2"); addOutput("B3");
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);
        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float a = in(0).get(c, i), b = in(1).get(c, i);
                outs[0].v[c][i] = outs[1].v[c][i] = outs[2].v[c][i] = a;
                outs[3].v[c][i] = outs[4].v[c][i] = outs[5].v[c][i] = b;
            }
        }
    }
};

/* ================================================================== *
 * ATT - dual attenuverter with offset
 * ================================================================== */

class ModuleAtt : public Module {
public:
    ModuleAtt()
    {
        configure("ATT", "ATT", 5, 2);
        addKnob("AMT 1", -1.0f, 1.0f, 1.0f, "%+.2f");
        addKnob("OFF 1", -5.0f, 5.0f, 0.0f, "%+.2f V");
        addKnob("AMT 2", -1.0f, 1.0f, 1.0f, "%+.2f");
        addKnob("OFF 2", -5.0f, 5.0f, 0.0f, "%+.2f V");

        addInput("IN 1");
        addInput("IN 2");
        addOutput("OUT 1");
        addOutput("OUT 2");
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);
        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                outs[0].v[c][i] = in(0).get(c, i) * pv(0) + pv(1);
                outs[1].v[c][i] = in(1).get(c, i) * pv(2) + pv(3);
            }
        }
    }
};

/* ================================================================== *
 * STRING - a struck or plucked string, by physical modelling
 *
 * Everything else in this rack is an oscillator with exactly harmonic
 * partials, and no amount of filtering makes one inharmonic. A real string is
 * stiff, so wave speed depends on frequency and its partials are stretched
 * sharp of the harmonic series:
 *
 *     f(n) = n * f0 * sqrt(1 + B * n^2)
 *
 * By the sixteenth partial that is tens of cents. It is the single thing that
 * separates a piano from an organ with a good envelope, and it cannot be
 * reached by subtraction at all.
 *
 * A waveguide gets it honestly. A delay line is the string; the length is the
 * pitch. In the loop:
 *
 *   - a cascade of first-order allpasses, which makes the loop delay depend on
 *     frequency. That IS the stiffness. The coefficient is negative, so high
 *     partials come round sooner and land sharp - positive would flatten them,
 *     which is a physical impossibility that sounds like a bell.
 *   - a lowpass, so high partials die first, as they do on anything real.
 *   - a gain below one, which is how long the note rings.
 *
 * Two strings per voice a few cents apart, because a piano has two or three
 * per note and their coupling is what produces the characteristic two-stage
 * decay: a fast initial fall and a long quiet aftersound underneath it.
 *
 * The exciter is a short burst of filtered noise whose brightness rises with
 * velocity - a hammer's felt compresses harder when struck harder, so playing
 * loudly changes the spectrum and not only the level. That relationship is
 * physics here rather than a filter envelope dialled in by hand.
 * ================================================================== */

class ModuleString : public Module {
public:
    ModuleString()
    {
        configure("STRING", "STRING", 6, 3);
        addKnob("OCT",    -4.0f, 4.0f, 0.0f, "%+.0f", PC_LIN, 1.0f);
        addKnob("FINE",  -50.0f, 50.0f, 0.0f, "%+.0f c");
        addKnob("DECAY",   0.0f, 1.0f, 0.60f, "%.2f");   /* about four seconds */
        addKnob("BRIGHT",  0.0f, 1.0f, 0.55f, "%.2f");
        /* The stiffness. Zero is an ideal string - a guitar harmonic. Up is a
         * thicker, shorter, more tightly wound one, which is what the bass of
         * a piano is and why those notes sound almost like bells. */
        addKnob("INHARM",  0.0f, 1.0f, 0.45f, "%.2f");
        addKnob("STRIKE",  0.02f, 0.5f, 0.13f, "%.2f");
        addKnob("SPREAD",  0.0f, 12.0f, 2.2f, "%.1f c");

        addInput("V/OCT");
        addInput("TRIG");
        addInput("IN");        /* excite it with anything at all */
        addOutput("OUT");
    }

    void onSampleRate()
    {
        /* Down to about 20 Hz, which is below the bottom of a piano. */
        const int n = (int)(sr / 20.0f) + 8;
        for (int c = 0; c < BS_MAX_POLY; c++) {
            for (int k = 0; k < 2; k++) line[c][k].alloc(n);
            combLine[c].alloc(n);      /* the strike comb reads from this */
        }
        reset();
    }

    void reset()
    {
        for (int c = 0; c < BS_MAX_POLY; c++) {
            combLine[c].clear();
            for (int k = 0; k < 2; k++) {
                line[c][k].clear();
                damp[c][k].reset();
                for (int a = 0; a < AP; a++) ap1[c][k][a] = ap2[c][k][a] = 0.0f;
            }
            last[c] = 0.0f; burst[c] = 0; burstLen[c] = 1;
            vel[c] = 0.0f; nz[c] = 12345 + c * 7919;
        }
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);

        const float oct = pv(0), fine = pv(1);
        const float decay = pv(2), bright = pv(3);
        const float inh = pv(4), strike = pv(5), spread = pv(6);

        /* Negative: high partials round the loop sooner and land sharp. */
        const float a = -0.72f * inh;

        /* Loop gain from a decay time, not from a number that felt about
         * right. Losing g per round trip with f0 trips a second puts the level
         * 60 dB down after t = -3 / (f0 * log10 g); inverting that makes the
         * knob mean seconds. The old mapping was linear in gain, which crammed
         * every musically useful value into the top two percent of its travel
         * and gave a grand a three-second decay - measured at -46 dB over
         * three seconds, which is a guitar. */
        const float t60 = 0.15f * std::pow(240.0f, clampf(decay, 0.0f, 1.0f));

        /* The coefficient, not the filter. Assigning a whole OnePole over each
         * string's would carry the template's z with it and wipe out the state
         * of a note that is still ringing. */
        OnePole shape;
        shape.setCutoff(400.0f + bright * bright * 11000.0f, sr);
        const float dampA = shape.a;

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float g = in(1).get(c, i);
                if (g > 1.0f && last[c] <= 1.0f) {
                    vel[c] = clampf(g * 0.1f, 0.05f, 1.0f);
                    /* How long the felt stays in contact, which is the most
                     * important number in a hammer.
                     *
                     * Contact time is a lowpass: a partial whose period is
                     * shorter than the contact receives almost no energy. Hard
                     * playing compresses the felt and shortens the contact,
                     * which is why loud is also bright; a long contact on a
                     * thick string is why the bottom of a piano is so much
                     * darker than the top. About 1 ms in the treble to 4 ms
                     * low down, shortened by force. */
                    const float fh = clampf(261.6256f *
                        std::exp2(in(0).get(c, i) + pv(0)), 20.0f, 4000.0f);
                    const float contact = clampf(
                        0.0035f * (1.0f - 0.55f * vel[c]) *
                        std::sqrt(261.6256f / fh), 0.0004f, 0.010f);
                    burstLen[c] = (int)(sr * contact) + 2;
                    burst[c]    = burstLen[c];
                }
                last[c] = g;

                const float f0 = clampf(261.6256f *
                    std::exp2(in(0).get(c, i) + oct + fine / 1200.0f),
                    20.0f, 4000.0f);

                /* Everything in the loop delays, not only the line, and the
                 * total is what sets the pitch. Approximating it left the
                 * string 29 cents flat with the stiffness at zero - audible,
                 * and worse at settings where it should be exact.
                 *
                 * So the phase delay of the damping filter and the allpass
                 * chain is evaluated at the note's own frequency and taken
                 * off. Both are one-pole sections, so this is a handful of
                 * trig calls per note per block rather than per sample. */
                const float w = 2.0f * 3.14159265358979f * f0 / sr;
                const float cw = std::cos(w), sw = std::sin(w);

                /* One-pole lowpass a/(1 - (1-a)z^-1): the phase is that of the
                 * denominator, negated. */
                const float b = 1.0f - dampA;
                const float lpPhase = -std::atan2(b * sw, 1.0f - b * cw);

                /* First-order allpass (a + z^-1)/(1 + a z^-1), AP of them. */
                const float apPhase = (float)AP *
                    (std::atan2(-sw, a + cw) - std::atan2(-a * sw, 1.0f + a * cw));

                const float apDelay = -(lpPhase + apPhase) / w;

                /* The second string rings half again as long. Strings in a
                 * course are coupled through the bridge and trade energy,
                 * which is what produces the piano's double decay: a quick
                 * initial fall, then a quiet aftersound that outlasts it. */
                float loopGain[2];
                for (int k = 0; k < 2; k++)
                    loopGain[k] = clampf(std::pow(10.0f,
                        -3.0f / (f0 * t60 * (k ? 2.2f : 0.45f))), 0.5f, 0.99999f);

                float sum = 0.0f;
                for (int k = 0; k < 2; k++) {
                    const float det = (k == 0 ? -0.5f : 0.5f) * spread / 1200.0f;
                    const float f   = f0 * std::exp2(det);
                    /* No fudge term here. An earlier version took a sample
                     * off for luck, which at middle C is 0.54% of the period -
                     * eleven cents sharp, constant across every partial, which
                     * is exactly what the measurement showed. */
                    float d = sr / f - apDelay;
                    if (d < 4.0f) d = 4.0f;

                    float x = line[c][k].read(d);

                    /* Stiffness. */
                    for (int n = 0; n < AP; n++) {
                        const float y = a * x + ap1[c][k][n] - a * ap2[c][k][n];
                        ap1[c][k][n] = x;
                        ap2[c][k][n] = y;
                        x = y;
                    }

                    damp[c][k].a = dampA;
                    x = damp[c][k].lp(x);
                    x *= loopGain[k];

                    float e = in(2).get(c, i) * 0.2f;
                    if (burst[c] > 0) {
                        /* A raised cosine, not noise.
                         *
                         * The first version excited the string with filtered
                         * noise, and noise is what a scraped or plucked string
                         * sounds like - measured, 62% of the energy landed
                         * BETWEEN the partials rather than on them, which the
                         * ear reads as a guitar. A hammer applies a smooth
                         * force with a beginning and an end, and the spectrum
                         * that puts into a string is the transform of that
                         * shape: partials, and no hiss between them.
                         *
                         * A little noise stays, because felt is not silent. */
                        const float t = 1.0f - (float)burst[c] / (float)burstLen[c];
                        const float w = 0.5f - 0.5f * std::cos(6.2831853f * t);
                        nz[c] = nz[c] * 1103515245u + 12345u;
                        const float r = (float)((int)(nz[c] >> 9) & 0x7fff) / 16384.0f - 1.0f;
                        e += (w + 0.035f * r * w) * vel[c] * 0.55f;
                    }

                    line[c][k].write(x + e);
                    sum += x;
                }

                /* Where it was struck. A string cannot sound a partial with a
                 * node at the hammer, and that comb is a large part of why a
                 * piano is not a sawtooth. */
                const float pd = clampf(strike * sr / f0, 1.0f, 2000.0f);
                combLine[c].write(sum * 0.5f);
                const float shaped = sum * 0.5f - combLine[c].read(pd) * 0.6f;

                outs[0].v[c][i] = clampf(shaped * 5.0f, -8.0f, 8.0f);
                if (burst[c] > 0) burst[c]--;
            }
        }
    }

private:
    /* Eight rather than four: each section bends the delay curve a little, and
     * a piano's stretch is not a little. */
    static const int AP = 8;
    Delay   line[BS_MAX_POLY][2];
    Delay   combLine[BS_MAX_POLY];
    OnePole damp[BS_MAX_POLY][2], hammer[BS_MAX_POLY];
    float   ap1[BS_MAX_POLY][2][AP], ap2[BS_MAX_POLY][2][AP];
    float   last[BS_MAX_POLY], vel[BS_MAX_POLY];
    int     burst[BS_MAX_POLY], burstLen[BS_MAX_POLY];
    unsigned nz[BS_MAX_POLY];
};

/* ================================================================== *
 * QUANT - pitch quantizer
 *
 * A sample and hold, an LFO or a sequencer produces whatever voltage it
 * produces, and sent to a pitch input that is a note between the notes. This
 * rounds to the nearest degree of a scale, which is the difference between a
 * generative patch that is music and one that is a fault condition.
 * ================================================================== */

class ModuleQuant : public Module {
public:
    ModuleQuant()
    {
        configure("QUANT", "QUANT", 5, 2);
        static const char *const SCALES[] = {
            "CHROM", "MAJOR", "MINOR", "PENT+", "PENT-", "DORIAN", "WHOLE", "BLUES"
        };
        addSwitch("SCALE", SCALES, 8, 1);
        static const char *const ROOTS[] = {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
        };
        addSwitch("ROOT", ROOTS, 12, 0);
        addKnob("OCT", -4.0f, 4.0f, 0.0f, "%+.0f", PC_LIN, 1.0f);

        addInput("IN");
        addOutput("OUT");
        addOutput("TRIG");     /* fires when the chosen note changes */
    }

    void reset()
    {
        for (int c = 0; c < BS_MAX_POLY; c++) { held[c] = -999; trig[c] = 0.0f; }
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);

        static const unsigned MASK[8] = {
            0xFFFu,   /* chromatic          */
            0xAB5u,   /* major       101010110101 */
            0x5ADu,   /* natural minor            */
            0x295u,   /* major pentatonic         */
            0x4A9u,   /* minor pentatonic         */
            0x6ADu,   /* dorian                   */
            0x555u,   /* whole tone               */
            0x4E9u    /* blues                    */
        };
        const unsigned mask = MASK[(int)(pv(0) + 0.5f) & 7];
        const int root = (int)(pv(1) + 0.5f);
        const float oct = pv(2);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float v = in(0).get(c, i);
                const float semis = v * 12.0f - (float)root;
                int n = (int)std::floor(semis + 0.5f);

                /* Search outward: the nearest degree, not the one below. A
                 * quantizer that always rounds down drags a rising line flat. */
                int best = n;
                for (int d = 0; d < 12; d++) {
                    const int up = n + d, dn = n - d;
                    if (mask & (1u << (((up % 12) + 12) % 12))) { best = up; break; }
                    if (mask & (1u << (((dn % 12) + 12) % 12))) { best = dn; break; }
                }

                if (best != held[c]) { held[c] = best; trig[c] = sr * 0.002f; }
                if (trig[c] > 0.0f) trig[c] -= 1.0f;

                outs[0].v[c][i] = ((float)best + (float)root) / 12.0f + oct;
                outs[1].v[c][i] = trig[c] > 0.0f ? 10.0f : 0.0f;
            }
        }
    }

private:
    int   held[BS_MAX_POLY];
    float trig[BS_MAX_POLY];
};

/* ================================================================== *
 * CLK - clock, with divisions
 *
 * Everything that repeats needs one, and an LFO's square output is a clock
 * with no way to say "half that". The divisions come out together so a patch
 * can have a bass line on the fours and a hat on the ones without a second
 * module.
 * ================================================================== */

class ModuleClk : public Module {
public:
    ModuleClk()
    {
        configure("CLK", "CLK", 5, 2);
        addKnob("BPM", 20.0f, 300.0f, 120.0f, "%.0f", PC_LIN, 1.0f);
        addKnob("PW", 0.05f, 0.9f, 0.5f, "%.2f");
        addToggle("RUN", 1.0f);
        /* With a host, the knob is ignored and the tempo comes from the
         * session - which is the difference between a rack that grooves with
         * the track and one that walks away from it inside eight bars. */
        addToggle("SYNC", 1.0f);
        static const char *const DIVS[] = { "1/1","1/2","1/4","1/8","1/16","1/4T","1/8T" };
        addSwitch("DIV", DIVS, 7, 2);

        addInput("EXT");       /* an external clock takes over when patched */
        addInput("RESET");
        addOutput("X1");
        addOutput("/2");
        addOutput("/4");
        addOutput("/8");
    }

    void reset() { ph = 0.0f; count = 0; lastExt = 0.0f; lastRst = 0.0f; }

    void process()
    {
        setAllOutputChannels(1);
        const float pw = pv(1);
        const int run = pv(2) >= 0.5f;

        /* The knob when there is no host, the host when there is one and SYNC
         * is on. Beats per bar-division: 1/4 is one beat, 1/8 is half of one,
         * and the triplets are two thirds of their straight neighbour. */
        static const float BEATS[7] = { 4.0f, 2.0f, 1.0f, 0.5f, 0.25f,
                                        2.0f / 3.0f, 1.0f / 3.0f };
        const int sync = pv(3) >= 0.5f;
        const float beats = BEATS[(int)(pv(4) + 0.5f) % 7];
        const float tempo = sync ? hostBpm(pv(0)) : pv(0);
        const float bpm = tempo / beats;

        for (int i = 0; i < BS_BLOCK; i++) {
            const float rst = in(1).get(0, i);
            if (rst > 1.0f && lastRst <= 1.0f) { ph = 0.0f; count = 0; }
            lastRst = rst;

            int tick = 0;
            const float ext = in(0).get(0, i);
            if (ext > 1.0f && lastExt <= 1.0f) tick = 1;
            const int external = (ext != 0.0f || lastExt != 0.0f);
            lastExt = ext;

            if (run && !external) {
                ph += bpm / 60.0f / sr;
                if (ph >= 1.0f) { ph -= 1.0f; tick = 1; }
            }
            if (tick) count++;

            /* Every output is the master pulse, gated by whether this tick is
             * one the division lands on. Shifting before masking was the bug:
             * (count >> 1) & 3 is zero for counts 0 and 1 and then not again
             * until 8, so /4 fired twice and rested six - measured gaps of
             * 1,7,1,7 rather than 4,4,4,4. /8 was worse: four hits and then
             * twenty-eight beats off. Every rhythm built on these jacks was
             * wrong, including the ones the preset notes teach. */
            const int high1 = external ? (ext > 1.0f) : (run && ph < pw);
            outs[0].v[0][i] = high1 ? 10.0f : 0.0f;
            outs[1].v[0][i] = ((count & 1u) == 0) && high1 ? 10.0f : 0.0f;
            outs[2].v[0][i] = ((count & 3u) == 0) && high1 ? 10.0f : 0.0f;
            outs[3].v[0][i] = ((count & 7u) == 0) && high1 ? 10.0f : 0.0f;
        }
    }

private:
    float ph, lastExt, lastRst;
    unsigned count;
};

/* ================================================================== *
 * SEQ - eight step sequencer
 * ================================================================== */

class ModuleSeq : public Module {
public:
    ModuleSeq()
    {
        configure("SEQ", "SEQ", 8, 4);
        for (int i = 0; i < 8; i++) {
            static const char *const N[8] = { "1","2","3","4","5","6","7","8" };
            addKnob(N[i], -2.0f, 2.0f, 0.0f, "%+.2f V");
        }
        addKnob("LEN", 1.0f, 8.0f, 8.0f, "%.0f", PC_LIN, 1.0f);
        addKnob("GLIDE", 0.0f, 0.5f, 0.0f, "%.3f s", PC_EXP);

        addInput("CLOCK");
        addInput("RESET");
        addOutput("CV");
        addOutput("GATE");
        addOutput("EOC");
    }

    void reset() { step = 0; last = 0.0f; lastRst = 0.0f; eoc = 0.0f; cv = 0.0f; }

    void process()
    {
        setAllOutputChannels(1);
        const int len = (int)(pv(8) + 0.5f);
        const float glide = pv(9);

        for (int i = 0; i < BS_BLOCK; i++) {
            const float r = in(1).get(0, i);
            if (r > 1.0f && lastRst <= 1.0f) step = 0;
            lastRst = r;

            const float c = in(0).get(0, i);
            if (c > 1.0f && last <= 1.0f) {
                step++;
                if (step >= (len < 1 ? 1 : len)) { step = 0; eoc = sr * 0.002f; }
            }
            last = c;

            if (eoc > 0.0f) eoc -= 1.0f;

            const float target = pv(step & 7);
            if (glide <= 0.0002f) cv = target;
            else cv += (target - cv) * (1.0f - std::exp(-1.0f / (glide * sr + 1.0f)));

            outs[0].v[0][i] = cv;
            /* The gate follows the clock rather than being a blip, so a note
             * lasts as long as the step does - which is what makes a sequence
             * play legato when the clock's pulse width is wide. */
            outs[1].v[0][i] = (c > 1.0f) ? 10.0f : 0.0f;
            outs[2].v[0][i] = eoc > 0.0f ? 10.0f : 0.0f;
        }
    }

private:
    int   step;
    float last, lastRst, eoc, cv;
};

/* ================================================================== *
 * LOGIC - gates about gates
 *
 * Two gate inputs and the four answers. What it is for is rhythm: two clock
 * divisions ANDed together fire on their common beat, XOR gives everything
 * except it, and a pattern appears that neither input had.
 * ================================================================== */

class ModuleLogic : public Module {
public:
    ModuleLogic()
    {
        configure("LOGIC", "LOGIC", 4, 2);
        addKnob("THRESH", 0.1f, 9.0f, 1.0f, "%.1f V");
        addInput("A");
        addInput("B");
        addOutput("AND");
        addOutput("OR");
        addOutput("XOR");
        addOutput("NOT A");
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);
        const float th = pv(0);
        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const int a = in(0).get(c, i) > th;
                const int b = in(1).get(c, i) > th;
                outs[0].v[c][i] = (a && b) ? 10.0f : 0.0f;
                outs[1].v[c][i] = (a || b) ? 10.0f : 0.0f;
                outs[2].v[c][i] = (a != b) ? 10.0f : 0.0f;
                outs[3].v[c][i] = a ? 0.0f : 10.0f;
            }
        }
    }
};

/* ================================================================== *
 * SWITCH - sequential switch
 *
 * One output, four inputs, and a clock that moves between them. Routing as a
 * rhythmic act: four oscillators become one line that changes timbre on the
 * beat, or one source is scattered across four destinations by patching it
 * the other way round.
 * ================================================================== */

class ModuleSwitch : public Module {
public:
    ModuleSwitch()
    {
        configure("SWITCH", "SWITCH", 5, 2);
        addKnob("STEPS", 2.0f, 4.0f, 4.0f, "%.0f", PC_LIN, 1.0f);
        addInput("CLOCK");
        addInput("RESET");
        addInput("IN 1");
        addInput("IN 2");
        addInput("IN 3");
        addInput("IN 4");
        addOutput("OUT");
        addOutput("STEP");
    }

    void reset() { pos = 0; last = 0.0f; lastRst = 0.0f; }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);
        const int steps = (int)(pv(0) + 0.5f);

        for (int i = 0; i < BS_BLOCK; i++) {
            const float r = in(1).get(0, i);
            if (r > 1.0f && lastRst <= 1.0f) pos = 0;
            lastRst = r;

            const float c = in(0).get(0, i);
            if (c > 1.0f && last <= 1.0f) pos = (pos + 1) % (steps < 2 ? 2 : steps);
            last = c;

            for (int ci = 0; ci < ch; ci++)
                outs[0].v[ci][i] = in(2 + pos).get(ci, i);
            outs[1].v[0][i] = (float)pos * 2.5f;
        }
    }

private:
    int   pos;
    float last, lastRst;
};

/* ================================================================== *
 * FOLD - wavefolder
 *
 * The other tradition. Subtractive synthesis starts with a rich wave and
 * takes parts away; this starts with a sine and makes it complicated by
 * folding it back on itself every time it exceeds a limit. Each fold adds
 * harmonics, and the count changes with level - so the timbre moves with the
 * envelope in a way a filter sweep cannot imitate.
 * ================================================================== */

class ModuleFold : public Module {
public:
    ModuleFold()
    {
        configure("FOLD", "FOLD", 4, 2);
        addKnob("GAIN", 1.0f, 12.0f, 2.0f, "%.2f");
        addKnob("SYM", -1.0f, 1.0f, 0.0f, "%+.2f");
        addKnob("CV",   0.0f, 8.0f, 0.0f, "%.2f");
        addInput("IN");
        addInput("CV");
        addOutput("OUT");
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);
        const float g0 = pv(0), sym = pv(1), cv = pv(2);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float g = clampf(g0 + cv * in(1).get(c, i) * 0.1f, 0.0f, 16.0f);
                float x = in(0).get(c, i) * 0.2f * g + sym;

                /* Six passes is enough for any gain this module offers, and
                 * bounded so a runaway input cannot spin here. */
                for (int n = 0; n < 6; n++) {
                    if (x > 1.0f)       x =  2.0f - x;
                    else if (x < -1.0f) x = -2.0f - x;
                    else break;
                }
                /* Rounded off at the peaks: the triangle a pure fold produces
                 * has corners, and corners alias. */
                outs[0].v[c][i] = std::sin(1.5707963f * x) * 5.0f;
            }
        }
    }
};

/* ================================================================== *
 * CRUSH - bit depth and sample rate reduction
 * ================================================================== */

class ModuleCrush : public Module {
public:
    ModuleCrush()
    {
        configure("CRUSH", "CRUSH", 4, 2);
        addKnob("BITS", 1.0f, 16.0f, 8.0f, "%.1f");
        addKnob("RATE", 200.0f, 24000.0f, 24000.0f, "%.0f Hz", PC_EXP);
        addKnob("MIX",  0.0f, 1.0f, 1.0f, "%.2f");
        addInput("IN");
        addOutput("OUT");
    }

    void reset()
    {
        for (int c = 0; c < BS_MAX_POLY; c++) { hold[c] = 0.0f; phase[c] = 0.0f; }
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);
        const float steps = std::pow(2.0f, clampf(pv(0), 1.0f, 16.0f)) * 0.5f;
        const float inc = clampf(pv(1), 1.0f, sr) / sr;
        const float mix = pv(2);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float x = in(0).get(c, i);
                phase[c] += inc;
                if (phase[c] >= 1.0f) {
                    phase[c] -= 1.0f;
                    hold[c] = std::floor(x * 0.2f * steps + 0.5f) / steps * 5.0f;
                }
                outs[0].v[c][i] = lerpf(x, hold[c], mix);
            }
        }
    }

private:
    float hold[BS_MAX_POLY], phase[BS_MAX_POLY];
};

/* ================================================================== *
 * CHORUS - the modulated delay
 *
 * The third of the three time effects and the one that was missing. A delay
 * short enough to be heard as thickness rather than as an echo, with its
 * length moving - so the copy is always slightly out of tune with the
 * original, which is what a room full of nearly identical instruments sounds
 * like. Mono in, stereo out, with the two sides moving in opposition.
 * ================================================================== */

class ModuleChorus : public Module {
public:
    ModuleChorus()
    {
        configure("CHORUS", "CHORUS", 5, 2);
        addKnob("RATE",  0.02f, 8.0f, 0.6f, "%.2f Hz", PC_EXP);
        addKnob("DEPTH", 0.0f, 1.0f, 0.45f, "%.2f");
        addKnob("DELAY", 0.002f, 0.040f, 0.012f, "%.3f s");
        addKnob("FBK",  -0.9f, 0.9f, 0.0f, "%+.2f");
        addKnob("MIX",   0.0f, 1.0f, 0.5f, "%.2f");
        addInput("IN");
        addOutput("L");
        addOutput("R");
    }

    void onSampleRate() { for (int k = 0; k < 2; k++) line[k].alloc((int)(sr * 0.09f) + 8); }
    void reset() { for (int k = 0; k < 2; k++) line[k].clear(); ph = 0.0f; }

    void process()
    {
        setAllOutputChannels(1);
        const float rate = pv(0), depth = pv(1), base = pv(2);
        const float fbk = pv(3), mix = pv(4);

        for (int i = 0; i < BS_BLOCK; i++) {
            const float x = in(0).sum(i);
            ph += rate / sr;
            if (ph >= 1.0f) ph -= 1.0f;

            float wet[2];
            for (int k = 0; k < 2; k++) {
                /* Half a cycle apart, so the two sides drift against each
                 * other and the result is wide rather than merely doubled. */
                const float lfo = std::sin(6.2831853f * (ph + (k ? 0.5f : 0.0f)));
                const float d = (base * (1.0f + depth * 0.7f * lfo)) * sr;
                wet[k] = line[k].read(d);
                line[k].write(x * 0.2f + wet[k] * fbk);
                outs[k].v[0][i] = lerpf(x, wet[k] * 5.0f, mix);
            }
        }
    }

private:
    Delay line[2];
    float ph;
};

/* ================================================================== *
 * SLEW - portamento for anything
 *
 * FUNC in SLEW mode does this too; this is the four-unit version for when the
 * rack is full and all that is wanted is to round the corners off a stepped
 * voltage.
 * ================================================================== */

class ModuleSlew : public Module {
public:
    ModuleSlew()
    {
        configure("SLEW", "SLEW", 4, 2);
        addKnob("RISE", 0.0f, 4.0f, 0.05f, "%.3f s", PC_EXP);
        addKnob("FALL", 0.0f, 4.0f, 0.05f, "%.3f s", PC_EXP);
        addInput("IN");
        addOutput("OUT");
    }

    void reset() { for (int c = 0; c < BS_MAX_POLY; c++) v[c] = 0.0f; }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);
        const float rise = pv(0), fall = pv(1);
        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float t = in(0).get(c, i);
                const float time = (t > v[c]) ? rise : fall;
                if (time <= 0.0002f) v[c] = t;
                else v[c] += (t - v[c]) * (1.0f - std::exp(-1.0f / (time * sr + 1.0f)));
                outs[0].v[c][i] = v[c];
            }
        }
    }

private:
    float v[BS_MAX_POLY];
};

/* ================================================================== *
 * SVF - state variable filter
 *
 * The ladder is a lowpass and only a lowpass, which leaves out most of what a
 * filter is for: a highpass is how a lead stops being muddy and how a pad
 * stops fighting the bass, and a bandpass is most of a wah or a formant.
 *
 * Topology-preserving transform, same family as the ladder, so it stays stable
 * when the cutoff is swept hard. All four responses come out of the same two
 * integrators - they are not four filters, they are four places to listen.
 * ================================================================== */

class ModuleSvf : public Module {
public:
    ModuleSvf()
    {
        configure("SVF", "SVF", 6, 3);
        addKnob("CUTOFF", 20.0f, 18000.0f, 1200.0f, "%.0f Hz", PC_EXP);
        addKnob("RES",    0.0f, 1.0f, 0.2f, "%.2f");
        addKnob("CV1",   -4.0f, 4.0f, 0.0f, "%+.2f");
        addKnob("CV2",   -4.0f, 4.0f, 0.0f, "%+.2f");
        addKnob("KTRK",   0.0f, 1.0f, 0.0f, "%.2f");

        addInput("IN");
        addInput("CV1");
        addInput("CV2");
        addInput("V/OCT");
        addOutput("LP");
        addOutput("HP");
        addOutput("BP");
        addOutput("NOTCH");
    }

    void reset()
    {
        for (int c = 0; c < BS_MAX_POLY; c++) { s1[c] = 0.0f; s2[c] = 0.0f; }
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);

        const float base = pv(0);
        /* Damping, not resonance: the two are reciprocal, and at k = 0 this
         * self-oscillates. Stopping a little short of zero keeps it musical
         * without needing the saturator the ladder has. */
        const float k = 2.0f - 1.98f * clampf(pv(1), 0.0f, 1.0f);
        const float a1 = pv(2), a2 = pv(3), ktrk = pv(4);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float mod = a1 * in(1).get(c, i) + a2 * in(2).get(c, i)
                                + ktrk * in(3).get(c, i);
                const float fc = clampf(base * std::exp2(mod), 10.0f, sr * 0.45f);
                const float g  = std::tan(3.14159265358979f * fc / sr);

                const float x  = in(0).get(c, i);
                const float hp = (x - (k + g) * s1[c] - s2[c]) / (1.0f + g * (g + k));
                const float bp = g * hp + s1[c];
                s1[c] = g * hp + bp;
                const float lp = g * bp + s2[c];
                s2[c] = g * bp + lp;

                outs[0].v[c][i] = lp;
                outs[1].v[c][i] = hp;
                outs[2].v[c][i] = bp;
                outs[3].v[c][i] = hp + lp;      /* notch */
            }
        }
    }

private:
    float s1[BS_MAX_POLY], s2[BS_MAX_POLY];
};

/* ================================================================== *
 * FUNC - function generator
 *
 * The most useful module in a modular, and the hardest to describe, because
 * what it is depends on what you patch. Rise and fall with a shape control:
 * triggered it is an attack-decay envelope; cycling it is an LFO of any shape
 * from ramp to triangle to inverse ramp; fed a stepped voltage with CYCLE off
 * and a long rise it is a slew limiter; fed audio it is an envelope follower.
 *
 * EOC fires at the end of each fall, which is what chains two of them into a
 * sequence, or makes one clock another.
 * ================================================================== */

class ModuleFunc : public Module {
public:
    ModuleFunc()
    {
        configure("FUNC", "FUNC", 5, 2);
        addKnob("RISE", 0.0005f, 10.0f, 0.02f, "%.3f s", PC_EXP);
        addKnob("FALL", 0.0005f, 10.0f, 0.40f, "%.3f s", PC_EXP);
        /* Below a half it is exponential, above it logarithmic, and at a half
         * it is a straight line. One knob because the two are the same
         * gesture: how the curve leans. */
        addKnob("SHAPE", 0.0f, 1.0f, 0.5f, "%.2f");
        addKnob("LEVEL", 0.0f, 10.0f, 10.0f, "%.1f V");
        /* Stated rather than inferred. An unpatched jack reads as silence on
         * purpose here, so process() never has to ask whether a cable exists -
         * which means "follow IN when TRIG is empty" would be a hidden branch
         * on something the rest of the rack deliberately does not look at. */
        static const char *const MODES[] = { "ENV", "CYCLE", "SLEW" };
        addSwitch("MODE", MODES, 3, 0);

        addInput("TRIG");
        addInput("IN");        /* what SLEW follows */
        addOutput("OUT");
        addOutput("EOC");
        addOutput("INV");
    }

    void reset()
    {
        for (int c = 0; c < BS_MAX_POLY; c++) {
            v[c] = 0.0f; rising[c] = 0; last[c] = 0.0f; eoc[c] = 0.0f;
        }
    }

    void process()
    {
        const int ch = polyIn();
        setAllOutputChannels(ch);

        const float riseK = pv(0), fallK = pv(1);
        const float shape = clampf(pv(2), 0.0f, 1.0f);
        const float level = pv(3);
        const int mode  = (int)(pv(4) + 0.5f);   /* 0 env, 1 cycle, 2 slew */
        const int cycle = (mode == 1);

        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < BS_BLOCK; i++) {
                const float g = in(0).get(c, i);
                const int edge = (g > 1.0f && last[c] <= 1.0f);
                last[c] = g;
                if (edge) rising[c] = 1;
                if (cycle && v[c] <= 0.0f && !rising[c]) rising[c] = 1;

                if (eoc[c] > 0.0f) eoc[c] -= 1.0f;

                /* IN sets the timing unless the mode is SLEW, where it is the
                 * signal being followed instead. A couple of volts roughly
                 * doubles both times, which is what lets a function generator
                 * clocked by its own end-of-cycle wander rather than tick.
                 *
                 * The jack did nothing at all in CYCLE mode before, so KRELL -
                 * whose entire premise is a sampled voltage choosing the next
                 * interval - ran metronomic while its panel explained a
                 * mechanism that was not connected to anything. */
                float rise = riseK, fall = fallK;
                if (mode != 2) {
                    const float cv = std::exp2(clampf(in(1).get(c, i) * 0.2f,
                                                      -4.0f, 4.0f));
                    rise = clampf(riseK * cv, 0.0002f, 30.0f);
                    fall = clampf(fallK * cv, 0.0002f, 30.0f);
                }

                /* Shape leans the curve by moving the target past the
                 * destination: chasing 1.2 and stopping at 1.0 is a straight
                 * line, chasing exactly 1.0 is the usual exponential crawl. */
                const float lean = 1.0f + (1.0f - shape) * 3.0f;
                if (rising[c]) {
                    const float k = 1.0f - std::exp(-1.0f / (rise * sr + 1.0f));
                    v[c] += (lean - v[c]) * k;
                    if (v[c] >= 1.0f) { v[c] = 1.0f; rising[c] = 0; }
                } else {
                    const float k = 1.0f - std::exp(-1.0f / (fall * sr + 1.0f));
                    const float was = v[c];
                    v[c] += ((1.0f - lean) - v[c]) * k;
                    if (v[c] <= 0.0f) {
                        v[c] = 0.0f;
                        /* Only on the sample it CROSSES zero. Testing the
                         * value rather than the crossing re-armed the trigger
                         * every sample at rest, so EOC sat at 10 V forever
                         * between notes and anything gate-driven downstream
                         * was held permanently open. */
                        if (was > 0.0f) eoc[c] = sr * 0.002f;
                    }
                }

                /* SLEW chases the input at the same two rates: a stepped
                 * voltage becomes a glide, and audio becomes an envelope
                 * follower. Same module, same two knobs. */
                float outv = v[c];
                if (mode == 2) {
                    const float target = clampf(in(1).get(c, i) * 0.1f, -1.0f, 1.0f);
                    const float k = 1.0f - std::exp(-1.0f /
                        ((target > v[c] ? rise : fall) * sr + 1.0f));
                    v[c] += (target - v[c]) * k;
                    outv = v[c];
                }

                /* SLEW may legitimately go negative - it is following a
                 * bipolar voltage - so only the generated shapes are clamped
                 * to the positive half. */
                if (mode != 2) outv = clampf(outv, 0.0f, 1.0f);
                outs[0].v[c][i] = outv * level;
                outs[1].v[c][i] = eoc[c] > 0.0f ? 10.0f : 0.0f;
                outs[2].v[c][i] = (1.0f - clampf(outv, 0.0f, 1.0f)) * level;
                (void)0;
            }
        }
    }

private:
    float v[BS_MAX_POLY], last[BS_MAX_POLY], eoc[BS_MAX_POLY];
    int   rising[BS_MAX_POLY];
};

/* ================================================================== *
 * DLY - echo
 *
 * Monophonic by construction: it sums the channels arriving at its input.
 * Eight independent two-second delay lines would be four megabytes and would
 * not sound like anything a person wants, because a polyphonic echo is just a
 * mono echo of the same notes.
 * ================================================================== */

class ModuleDly : public Module {
public:
    ModuleDly()
    {
        configure("DLY", "DLY", 5, 2);
        addKnob("TIME", 0.005f, 2.0f, 0.32f, "%.3f s", PC_EXP);
        addKnob("FBK",  0.0f, 1.05f, 0.35f, "%.2f");
        addKnob("TONE", 300.0f, 12000.0f, 3500.0f, "%.0f Hz", PC_EXP);
        addKnob("MIX",  0.0f, 1.0f, 0.3f, "%.2f");
        addKnob("CV",  -2.0f, 2.0f, 0.0f, "%+.2f");
        addToggle("SYNC", 0.0f);
        static const char *const DIVS[] = { "1/2","1/4","1/8","1/8.","1/16","1/4T","1/8T" };
        addSwitch("DIV", DIVS, 7, 2);

        addInput("IN");
        addInput("TIME");
        addOutput("OUT");
        addOutput("WET");
    }

    void onSampleRate()
    {
        line.alloc((int)(sr * 2.2f) + 8);
        smooth.reset(0.0f);
    }
    void reset() { line.clear(); tone.reset(); dc.setSampleRate(sr); }

    void process()
    {
        setAllOutputChannels(1);

        /* A delay in a session wants to be a note value, not a number of
         * milliseconds that was right at one tempo. With SYNC on and a host
         * present the knob is ignored and the time comes from the transport. */
        static const float BEATS[7] = { 2.0f, 1.0f, 0.5f, 0.75f, 0.25f,
                                        2.0f / 3.0f, 1.0f / 3.0f };
        float t0 = pv(0);
        if (pv(5) >= 0.5f) {
            const float bpm = hostBpm(0.0f);
            if (bpm > 1.0f)
                t0 = clampf(60.0f / bpm * BEATS[(int)(pv(6) + 0.5f) % 7],
                            0.005f, 2.0f);
        }
        const float fb  = pv(1);
        const float mix = pv(3);
        const float cvA = pv(4);
        tone.setCutoff(pv(2), sr);

        for (int i = 0; i < BS_BLOCK; i++) {
            const float x  = in(0).sum(i);
            const float tt = clampf(t0 * std::exp2(cvA * in(1).get(0, i)), 0.001f, 2.1f);
            /* Slewing the delay time is what turns a knob twist from a click
             * into the tape-speed slide people reach for it to make. */
            const float d  = smooth.step(tt, 0.05f, sr) * sr;

            const float wet = line.read(d);
            line.write(dc.step(softClip((x + wet * fb) * 0.2f) * 5.0f));

            const float w = tone.lp(wet);
            outs[0].v[0][i] = lerpf(x, w, mix);
            outs[1].v[0][i] = w;
        }
    }

private:
    Delay   line;
    OnePole tone;
    DCBlock dc;
    Slew    smooth;
};

/* ================================================================== *
 * RVB - reverb
 * ================================================================== */

class ModuleRvb : public Module {
public:
    ModuleRvb()
    {
        configure("RVB", "RVB", 5, 2);
        addKnob("SIZE", 0.0f, 1.0f, 0.6f, "%.2f");
        addKnob("DAMP", 0.0f, 0.95f, 0.4f, "%.2f");
        addKnob("MIX",  0.0f, 1.0f, 0.25f, "%.2f");

        addInput("IN");
        addOutput("L");
        addOutput("R");
    }

    void onSampleRate() { verb.setSampleRate(sr); }
    void reset() { verb.clear(); }

    void process()
    {
        setAllOutputChannels(1);
        const float size = pv(0), damp = pv(1), mix = pv(2);

        for (int i = 0; i < BS_BLOCK; i++) {
            const float x = in(0).sum(i);
            float l, r;
            verb.step(x * 0.2f, size, damp, &l, &r);
            outs[0].v[0][i] = lerpf(x, l * 5.0f, mix);
            outs[1].v[0][i] = lerpf(x, r * 5.0f, mix);
        }
    }

private:
    Reverb verb;
};

/* ================================================================== *
 * SCOPE
 * ================================================================== */

ModuleScope::ModuleScope() : traceLen(TRACE), writePos(0), accCount(0), decim(8)
{
    configure("SCOPE", "SCOPE", 8, 2);
    addKnob("TIME", 1.0f, 256.0f, 8.0f, "%.0f x", PC_EXP, 1.0f);
    addKnob("GAIN", 0.1f, 8.0f, 1.0f, "%.2f");
    /* Free-running is right for envelopes and slow modulation, where there is
     * no edge to lock to. It is wrong for anything pitched, which crawls. */
    addToggle("TRIG", 1.0f);

    addInput("A");
    addInput("B");
    addOutput("A");
    addOutput("B");

    reset();
}

void ModuleScope::reset()
{
    for (int i = 0; i < TRACE; i++) { traceA[i] = 0.0f; traceB[i] = 0.0f; }
    writePos = 0;
    acc[0] = acc[1] = 0.0f;
    accCount = 0;
    armed = 1;
    sweep = 0;
    prev = 0.0f;
}

void ModuleScope::process()
{
    const int ch = polyIn();
    setAllOutputChannels(ch);

    decim = (int)(pv(0) + 0.5f);
    if (decim < 1) decim = 1;
    const float g = pv(1) * 0.2f;

    for (int c = 0; c < ch; c++)
        for (int i = 0; i < BS_BLOCK; i++) {
            outs[0].v[c][i] = in(0).get(c, i);
            outs[1].v[c][i] = in(1).get(c, i);
        }

    /* The trace shows the sum of the channels, which for a polyphonic patch is
     * what the output stage is going to receive anyway. Averaging over the
     * decimation window rather than picking one sample keeps a fast waveform
     * from aliasing into a slow lie when the time knob is turned up. */
    const int trig = pv(2) >= 0.5f;

    for (int i = 0; i < BS_BLOCK; i++) {
        const float a = in(0).sum(i);

        /* Wait at the start of the buffer for channel A to cross zero going
         * up, then write one sweep and wait again. The picture then redraws in
         * the same place every time instead of sliding, which is the whole
         * difference between a scope you can read a waveform off and one that
         * just proves something is happening. */
        if (trig && armed) {
            const int rising = (prev <= 0.0f && a > 0.0f);
            prev = a;
            if (!rising) continue;
            armed = 0;
            sweep = 0;
            writePos = 0;
            acc[0] = acc[1] = 0.0f;
            accCount = 0;
        }
        prev = a;

        acc[0] += a;
        acc[1] += in(1).sum(i);
        if (++accCount < decim) continue;
        const float inv = 1.0f / (float)accCount;
        traceA[writePos] = acc[0] * inv * g;
        traceB[writePos] = acc[1] * inv * g;
        writePos = (writePos + 1) % TRACE;
        acc[0] = acc[1] = 0.0f;
        accCount = 0;

        if (trig && ++sweep >= TRACE) armed = 1;
    }

    /* Turned off mid-sweep, the trace must not stay frozen where it stopped. */
    if (!trig) { armed = 0; sweep = 0; }
}

/* ================================================================== *
 * OUT - master output
 * ================================================================== */

ModuleOut::ModuleOut() : clipping(false), clipHold(0.0f)
{
    configure("OUT", "OUT", 6, 2);
    addKnob("LEVEL", 0.0f, 1.5f, 0.6f, "%.2f");
    /* Everything upstream is per-voice already; this is the only place that
     * knows how many voices there are AND has two channels to put them in.
     * Zero by default, so every existing rack sounds exactly as it did.
     *
     * It only does anything if the voices are still separate when they get
     * here. DLY, RVB and CHORUS are mono by construction - they sum their
     * input - so a rack with any of them in the chain arrives as one channel
     * and this knob has nothing to spread. Making those effects stereo through
     * is the fix, and is a bigger change than this knob. */
    addKnob("SPREAD", 0.0f, 1.0f, 0.0f, "%.2f");

    addInput("L");
    addInput("R");

    reset();
}

void ModuleOut::reset()
{
    peak[0] = peak[1] = rms[0] = rms[1] = 0.0f;
    sq[0] = sq[1] = 0.0f;
    clipping = false;
    clipHold = 0.0f;
    std::memset(bufL, 0, sizeof bufL);
    std::memset(bufR, 0, sizeof bufR);
}

void ModuleOut::process()
{
    const float g = pv(0) * (1.0f / VOLT);
    /* R unpatched follows L, so a mono patch does not come out of one
     * speaker - the far more likely intent than a hard-panned voice. */
    const bool  dual = patched(1);

    /* Voices across the field rather than stacked in the middle.
     *
     * A polyphonic patch arrives here as eight channels on one cable and gets
     * summed, so an eight-voice pad is eight things in exactly the same place.
     * Panning them by voice number costs a multiply and turns the same patch
     * into something with width. Constant power, and scaled so a centred voice
     * is unity - which is what makes SPREAD at zero identical to the sum it
     * replaces. */
    const float spread = pv(1);
    const bs::Signal &A = in(0);
    const bs::Signal &B = dual ? in(1) : in(0);
    const int nch = A.channels;

    for (int i = 0; i < BS_BLOCK; i++) {
        float l = 0.0f, r = 0.0f;
        for (int c = 0; c < nch; c++) {
            float pos = (nch > 1) ? ((float)c / (float)(nch - 1) * 2.0f - 1.0f)
                                  : 0.0f;
            pos = clampf(pos * spread, -1.0f, 1.0f);
            const float gl = std::sqrt(0.5f * (1.0f - pos)) * 1.41421356f;
            const float gr = std::sqrt(0.5f * (1.0f + pos)) * 1.41421356f;
            l += A.v[c][i] * gl;
            r += B.v[c < B.channels ? c : 0][i] * gr;
        }
        l *= g;
        r *= g;

        if (std::fabs(l) > 1.0f || std::fabs(r) > 1.0f) clipHold = 0.25f;

        bufL[i] = softClip(l);
        bufR[i] = softClip(r);

        for (int s = 0; s < 2; s++) {
            const float v = std::fabs(s ? r : l);
            /* Fast attack, slow release: a meter that falls as quickly as it
             * rises reads as flicker rather than as level. */
            if (v > peak[s]) peak[s] = v;
            else             peak[s] += (v - peak[s]) * 0.0008f;
            const float x = (s ? r : l);
            sq[s] += (x * x - sq[s]) * 0.002f;
        }
    }
    rms[0] = std::sqrt(sq[0]);
    rms[1] = std::sqrt(sq[1]);

    if (clipHold > 0.0f) clipHold -= (float)BS_BLOCK * st;
    clipping = clipHold > 0.0f;
}

/* ================================================================== *
 * TEXT - a scratchpad
 * ================================================================== */

ModuleText::ModuleText()
{
    /* Wide and tall, and no jacks at all. It is the one module whose whole
     * output is read rather than heard. */
    configure("TEXT", "NOTES", 13, 1);
}

/* ================================================================== *
 * MACRO - eight controls something outside the rack can reach
 *
 * A knob and a jack, eight times. On its own that is a manual CV source, which
 * is worth having: somewhere to park a value and cable it to three places at
 * once.
 *
 * Its real job is the plugin one. A host wants a flat, fixed list of
 * automatable parameters declared before the plugin is instantiated, and a
 * modular rack has no such thing - its knobs come and go with its modules, and
 * "the cutoff" is not a stable identity when there may be no filter or four.
 * Sixteen macros that exist whatever the rack contains give the host something
 * it can name, and leave the question of what they *do* where it belongs: on
 * the panel, decided by a cable.
 * ================================================================== */

class ModuleMacro : public Module {
public:
    ModuleMacro()
    {
        configure("MACRO", "MACRO", 7, 2);
        static const char *NAMES[BS_MACROS] = {
            "1", "2", "3", "4", "5", "6", "7", "8"
        };
        for (int i = 0; i < BS_MACROS; i++)
            addKnob(NAMES[i], 0.0f, 1.0f, 0.0f, "%.2f");
        for (int i = 0; i < BS_MACROS; i++)
            addOutput(NAMES[i]);
    }

    void process()
    {
        setAllOutputChannels(1);
        for (int k = 0; k < BS_MACROS; k++) {
            /* Nought to ten volts, the same range an envelope covers, so a
             * macro straight into a VCA's CV opens it fully at the top of its
             * travel and an attenuverter is only needed when something other
             * than that is wanted. */
            const float v = pv(k) * 10.0f;
            for (int i = 0; i < BS_BLOCK; i++) outs[(size_t)k].v[0][i] = v;
        }
    }
};

/* ================================================================== *
 * ARP - arpeggiator
 *
 * Reads a polyphonic pitch-and-gate pair and plays the held notes one at a
 * time, in order, on a clock. That is the natural shape for it here: the
 * keyboard module already hands out one channel per held key, so an
 * arpeggiator is a module that collapses those channels back into one and
 * decides which of them is sounding.
 * ================================================================== */

class ModuleArp : public Module {
public:
    ModuleArp() : phase(0.0f), step(-1), heldCount(0), curPitch(0.0f),
                  gateTimer(0.0f), trigTimer(0.0f), lastClock(0.0f),
                  rng(0x2545F491u)
    {
        configure("ARP", "ARP", 6, 2);
        static const char *MODES[] = { "UP", "DOWN", "UPDN", "RAND", "PLAYED" };
        addKnob("RATE", 0.5f, 24.0f, 8.0f, "%.2f Hz", PC_EXP);
        addSwitch("MODE", MODES, 5, 0);
        addKnob("OCT", 1.0f, 4.0f, 1.0f, "%.0f", PC_LIN, 1.0f);
        addKnob("GATE", 0.05f, 0.95f, 0.50f, "%.2f");

        addInput("V/OCT");
        addInput("GATE");
        addInput("CLOCK");

        addOutput("V/OCT");
        addOutput("GATE");
        addOutput("TRIG");
    }

    void reset()
    {
        phase = 0.0f; step = -1; heldCount = 0;
        curPitch = 0.0f; gateTimer = 0.0f; trigTimer = 0.0f; lastClock = 0.0f;
    }

    void process()
    {
        setAllOutputChannels(1);

        /* The held notes, taken from the channels whose gate is open, and
         * sorted - an arpeggio is about pitch order, not about which voice the
         * allocator happened to put a key in. */
        heldCount = 0;
        const int ch = in(1).channels;
        for (int c = 0; c < ch && heldCount < BS_MAX_POLY; c++) {
            if (in(1).get(c, 0) < 1.0f) continue;
            held[heldCount++] = in(0).get(c, 0);
        }
        if (pi(1) != MODE_PLAYED) {
            for (int i = 1; i < heldCount; i++) {
                const float k = held[i];
                int j = i - 1;
                while (j >= 0 && held[j] > k) { held[j + 1] = held[j]; j--; }
                held[j + 1] = k;
            }
        }

        const int octs = pi(2);
        const int len  = heldCount * (octs < 1 ? 1 : octs);
        if (len == 0) {
            /* Nothing down: rest, and start from the top next time rather than
             * from wherever the last chord happened to stop. */
            step = -1;
            phase = 0.0f;
            gateTimer = 0.0f;
            for (int i = 0; i < BS_BLOCK; i++) {
                outs[0].v[0][i] = curPitch;
                outs[1].v[0][i] = 0.0f;
                outs[2].v[0][i] = 0.0f;
            }
            return;
        }

        const float rate = pv(0);
        const float period = 1.0f / rate;
        const float gateLen = period * pv(3);
        const bool  external = patched(2);

        for (int i = 0; i < BS_BLOCK; i++) {
            bool tick = false;
            if (external) {
                const float c = in(2).get(0, i);
                if (lastClock < 1.0f && c >= 1.0f) tick = true;
                lastClock = c;
            } else {
                phase += rate * st;
                if (phase >= 1.0f) { phase -= 1.0f; tick = true; }
            }

            if (tick) {
                step = advance(step, len);
                curPitch = pitchAt(step, len, octs);
                gateTimer = gateLen;
                trigTimer = 0.001f;
            }

            outs[0].v[0][i] = curPitch;
            outs[1].v[0][i] = gateTimer > 0.0f ? 10.0f : 0.0f;
            outs[2].v[0][i] = trigTimer > 0.0f ? 10.0f : 0.0f;
            if (gateTimer > 0.0f) gateTimer -= st;
            if (trigTimer > 0.0f) trigTimer -= st;
        }
    }

private:
    enum { MODE_UP = 0, MODE_DOWN, MODE_UPDOWN, MODE_RANDOM, MODE_PLAYED };

    int advance(int s, int len)
    {
        const int mode = pi(1);
        if (mode == MODE_RANDOM) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            return (int)(rng % (unsigned)len);
        }
        if (mode == MODE_UPDOWN && len > 1) {
            /* Up and back without repeating the turning points, which is what
             * makes it a shape rather than a stutter at each end. */
            const int span = len * 2 - 2;
            return (s + 1) % span;
        }
        return (s + 1) % len;
    }

    float pitchAt(int s, int len, int octs) const
    {
        const int mode = pi(1);
        int idx = s;
        if (mode == MODE_UPDOWN && len > 1) {
            const int span = len * 2 - 2;
            idx = s % span;
            if (idx >= len) idx = span - idx;
        }
        if (idx < 0) idx = 0;
        if (idx >= len) idx = len - 1;

        const int oct = idx / (heldCount ? heldCount : 1);
        int note = heldCount ? (idx % heldCount) : 0;
        if (mode == MODE_DOWN) note = heldCount - 1 - note;
        (void)octs;
        return held[note] + (float)oct;
    }

    float phase;
    int   step;
    float held[BS_MAX_POLY];
    int   heldCount;
    float curPitch;
    float gateTimer, trigTimer, lastClock;
    unsigned rng;
};

/* ================================================================== *
 * Registry
 * ================================================================== */

template <class T> static Module *makeT() { return new T(); }

static const ModuleType TYPES[] = {
    { "KBD",   "KBD",        "SOURCE", makeT<ModuleKbd>   },
    { "VCO",   "VCO",        "SOURCE", makeT<ModuleVco>   },
    { "LFO",   "LFO",        "SOURCE", makeT<ModuleLfo>   },
    { "NOISE", "NOISE / S&H","SOURCE", makeT<ModuleNoise> },
    { "STRING","STRING",     "SOURCE", makeT<ModuleString> },
    { "VCF",   "VCF",        "SHAPE",  makeT<ModuleVcf>   },
    { "SVF",   "SVF",        "SHAPE",  makeT<ModuleSvf>   },
    { "VCA",   "VCA",        "SHAPE",  makeT<ModuleVca>   },
    { "ADSR",  "ADSR",       "SHAPE",  makeT<ModuleAdsr>  },
    { "FUNC",  "FUNC",       "SHAPE",  makeT<ModuleFunc>  },
    { "MIX",   "MIX",        "UTILITY",makeT<ModuleMix>   },
    { "MULT",  "MULT",       "UTILITY",makeT<ModuleMult>  },
    { "ATT",   "ATT",        "UTILITY",makeT<ModuleAtt>   },
    { "DLY",   "DLY",        "EFFECT", makeT<ModuleDly>   },
    { "RVB",   "RVB",        "EFFECT", makeT<ModuleRvb>   },
    { "ARP",   "ARP",        "SOURCE", makeT<ModuleArp>   },
    { "MACRO", "MACRO",      "UTILITY",makeT<ModuleMacro> },
    { "SCOPE", "SCOPE",      "UTILITY",makeT<ModuleScope> },
    { "TEXT",  "TEXT / NOTES","UTILITY",makeT<ModuleText> },
    { "FOLD",  "FOLD",       "SHAPE",  makeT<ModuleFold>  },
    { "CRUSH", "CRUSH",      "SHAPE",  makeT<ModuleCrush> },
    { "SLEW",  "SLEW",       "UTILITY",makeT<ModuleSlew>  },
    { "QUANT", "QUANT",      "UTILITY",makeT<ModuleQuant> },
    { "CLK",   "CLK",        "SOURCE", makeT<ModuleClk>   },
    { "SEQ",   "SEQ",        "SOURCE", makeT<ModuleSeq>   },
    { "LOGIC", "LOGIC",      "UTILITY",makeT<ModuleLogic> },
    { "SWITCH","SWITCH",     "UTILITY",makeT<ModuleSwitch> },
    { "CHORUS","CHORUS",     "EFFECT", makeT<ModuleChorus> },
    { "OUT",   "OUT",        "OUTPUT", makeT<ModuleOut>   }
};

int moduleTypeCount() { return (int)(sizeof TYPES / sizeof TYPES[0]); }

const ModuleType *moduleTypeAt(int i)
{
    return (i >= 0 && i < moduleTypeCount()) ? &TYPES[i] : 0;
}

Module *createModule(const char *typeId)
{
    if (!typeId) return 0;
    for (int i = 0; i < moduleTypeCount(); i++)
        if (std::strcmp(TYPES[i].id, typeId) == 0) return TYPES[i].make();
    return 0;
}

} /* namespace bs */
