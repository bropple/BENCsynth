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
                if (resp == 1) g = (std::exp(4.0f * g) - 1.0f) * (1.0f / 53.598150f);
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

        const float t0  = pv(0);
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
    for (int i = 0; i < BS_BLOCK; i++) {
        acc[0] += in(0).sum(i);
        acc[1] += in(1).sum(i);
        if (++accCount < decim) continue;
        const float inv = 1.0f / (float)accCount;
        traceA[writePos] = acc[0] * inv * g;
        traceB[writePos] = acc[1] * inv * g;
        writePos = (writePos + 1) % TRACE;
        acc[0] = acc[1] = 0.0f;
        accCount = 0;
    }
}

/* ================================================================== *
 * OUT - master output
 * ================================================================== */

ModuleOut::ModuleOut() : clipping(false), clipHold(0.0f)
{
    configure("OUT", "OUT", 6, 2);
    addKnob("LEVEL", 0.0f, 1.5f, 0.6f, "%.2f");

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

    for (int i = 0; i < BS_BLOCK; i++) {
        const float l = in(0).sum(i) * g;
        const float r = (dual ? in(1).sum(i) * g : l);

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
 * Registry
 * ================================================================== */

template <class T> static Module *makeT() { return new T(); }

static const ModuleType TYPES[] = {
    { "KBD",   "KBD",        "SOURCE", makeT<ModuleKbd>   },
    { "VCO",   "VCO",        "SOURCE", makeT<ModuleVco>   },
    { "LFO",   "LFO",        "SOURCE", makeT<ModuleLfo>   },
    { "NOISE", "NOISE / S&H","SOURCE", makeT<ModuleNoise> },
    { "VCF",   "VCF",        "SHAPE",  makeT<ModuleVcf>   },
    { "VCA",   "VCA",        "SHAPE",  makeT<ModuleVca>   },
    { "ADSR",  "ADSR",       "SHAPE",  makeT<ModuleAdsr>  },
    { "MIX",   "MIX",        "UTILITY",makeT<ModuleMix>   },
    { "MULT",  "MULT",       "UTILITY",makeT<ModuleMult>  },
    { "ATT",   "ATT",        "UTILITY",makeT<ModuleAtt>   },
    { "DLY",   "DLY",        "EFFECT", makeT<ModuleDly>   },
    { "RVB",   "RVB",        "EFFECT", makeT<ModuleRvb>   },
    { "SCOPE", "SCOPE",      "UTILITY",makeT<ModuleScope> },
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
