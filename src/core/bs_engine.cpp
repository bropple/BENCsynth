#include "bs_engine.h"
#include "bs_modules.h"

#include <chrono>

namespace bs {

Engine::Engine() : load(0.0f), cachedRev(0), blockPos(BS_BLOCK)
{
    for (int i = 0; i < BS_BLOCK; i++) { blockL[i] = 0.0f; blockR[i] = 0.0f; }
}

void Engine::init(float sampleRate)
{
    std::lock_guard<std::mutex> g(mutex);
    patch.clear();
    keys.reset();
    patch.setSampleRate(sampleRate);
    cachedRev = 0;
    blockPos  = BS_BLOCK;
}

void Engine::setSampleRate(float sampleRate)
{
    std::lock_guard<std::mutex> g(mutex);
    patch.setSampleRate(sampleRate);
    blockPos = BS_BLOCK;
}

void Engine::refreshOutputs()
{
    outputs.clear();
    for (int i = 0; i < patch.slotCount(); i++) {
        Module *m = patch.module(i);
        if (m && m->masterL()) outputs.push_back(m);
    }
    cachedRev = patch.revision();
}

int Engine::addModule(const char *typeId, float x, float y)
{
    Module *m = createModule(typeId);
    if (!m) return -1;
    m->bindKeys(&keys);

    std::lock_guard<std::mutex> g(mutex);
    return patch.add(m, x, y);
}

void Engine::removeModule(int moduleId)
{
    std::lock_guard<std::mutex> g(mutex);
    patch.removeModule(moduleId);
}

int Engine::connect(int src, int srcPort, int dst, int dstPort)
{
    std::lock_guard<std::mutex> g(mutex);
    return patch.connect(src, srcPort, dst, dstPort);
}

void Engine::disconnect(int cableId)
{
    std::lock_guard<std::mutex> g(mutex);
    patch.disconnect(cableId);
}

void Engine::clear()
{
    std::lock_guard<std::mutex> g(mutex);
    const float sr = patch.sampleRate();
    patch.clear();
    patch.setSampleRate(sr);
    keys.allNotesOff();
}

static NoteEvent ev(int kind, int note, float value)
{
    NoteEvent e;
    e.kind  = (uint8_t)kind;
    e.note  = (uint8_t)(note < 0 ? 0 : (note > 127 ? 127 : note));
    e.value = value;
    return e;
}

void Engine::noteOn(int note, float velocity)
{
    if (note < 0 || note > 127) return;
    events.push(ev(NE_NOTE_ON, note, velocity));
}

void Engine::noteOff(int note)
{
    if (note < 0 || note > 127) return;
    events.push(ev(NE_NOTE_OFF, note, 0.0f));
}

void Engine::setBend(float b)    { events.push(ev(NE_BEND, 0, clampf(b, -1.0f, 1.0f))); }
void Engine::setMod(float m)     { events.push(ev(NE_MOD,  0, clampf(m, 0.0f, 1.0f))); }
void Engine::setSustain(bool on) { events.push(ev(NE_SUSTAIN, 0, on ? 1.0f : 0.0f)); }

void Engine::panic()
{
    std::lock_guard<std::mutex> g(mutex);
    /* Drop anything queued before silencing, so an event posted a moment ago
     * cannot arrive after the panic and restart a note. */
    NoteEvent e;
    while (events.pop(&e)) { }
    keys.allNotesOff();
    for (int i = 0; i < patch.slotCount(); i++) {
        Module *m = patch.module(i);
        if (m) m->reset();
    }
}

void Engine::render(float *interleaved, int frames)
{
    int done = 0;
    while (done < frames) {
        if (blockPos >= BS_BLOCK) {
            const std::chrono::steady_clock::time_point t0 =
                std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> g(mutex);

                /* Everything posted since the last block lands here, in order,
                 * before anything reads it. Draining in the engine rather than
                 * in the keyboard module means a rack with two keyboard panels
                 * - or none at all - behaves: the events are consumed exactly
                 * once either way. */
                NoteEvent e;
                while (events.pop(&e)) keys.apply(e);

                patch.process();
                keys.clearRetriggers();
                keys.publish();

                if (cachedRev != patch.revision()) refreshOutputs();

                for (int i = 0; i < BS_BLOCK; i++) { blockL[i] = 0.0f; blockR[i] = 0.0f; }
                for (size_t o = 0; o < outputs.size(); o++) {
                    const float *l = outputs[o]->masterL();
                    const float *r = outputs[o]->masterR();
                    for (int i = 0; i < BS_BLOCK; i++) { blockL[i] += l[i]; blockR[i] += r[i]; }
                }
            }
            const double us = std::chrono::duration<double, std::micro>(
                                  std::chrono::steady_clock::now() - t0).count();
            const double budget = 1e6 * (double)BS_BLOCK / (double)patch.sampleRate();
            load += 0.05f * ((float)(us / budget) - load);
            blockPos = 0;
        }

        int n = BS_BLOCK - blockPos;
        if (n > frames - done) n = frames - done;
        for (int i = 0; i < n; i++) {
            interleaved[(done + i) * 2 + 0] = blockL[blockPos + i];
            interleaved[(done + i) * 2 + 1] = blockR[blockPos + i];
        }
        blockPos += n;
        done     += n;
    }
}

/* ==================================================================== *
 * The factory racks
 *
 * Each one is written against a tiny placement helper rather than a page of
 * coordinates. The helper is the whole reason these stay readable: a preset is
 * a list of the modules it uses, the cables between them and the handful of
 * knobs that are not at their defaults, and anything more than that is
 * bookkeeping that would drown the musical decisions.
 *
 * Parameter indices are positional, matching the order each module's
 * constructor declares its knobs in. That is a real coupling - reordering a
 * module's knobs silently retunes every preset that touches it - and it is
 * accepted because the alternative is looking parameters up by name on the
 * audio thread's data structures, for a gain nothing else in the program
 * needs. The test suite plays every preset, which is what catches it.
 * ==================================================================== */

namespace {

struct Builder {
    Engine *e;
    float   x, y;

    void row(float atY) { x = 20.0f; y = atY; }
    void gap(float w)   { x += w; }

    int put(const char *type)
    {
        const int id = e->addModule(type, x, y);
        Module *m = e->patch.module(id);
        if (m) x += (float)panelWidth(*m) + 14.0f;
        return id;
    }

    void set(int id, int param, float v)
    {
        Module *m = e->patch.module(id);
        if (m && param >= 0 && param < m->paramCount())
            m->params[(size_t)param].value = v;
    }

    void wire(int s, int sp, int d, int dp) { e->connect(s, sp, d, dp); }
};

/* Module parameter and port indices, named. The numbers are positional and
 * unavoidable; writing them out once here is the difference between a preset
 * that can be read and one that can only be run. */
enum { KBD_GLIDE, KBD_OCT, KBD_BEND, KBD_VOICES, KBD_MODE };
enum { KBD_PITCH = 0, KBD_GATE, KBD_TRIG, KBD_VEL, KBD_MOD, KBD_WHEEL };

enum { VCO_OCT, VCO_COARSE, VCO_FINE, VCO_PW, VCO_FM, VCO_PWM };
enum { VCO_IN_PITCH = 0, VCO_IN_FM, VCO_IN_PWM, VCO_IN_SYNC };
enum { VCO_SAW = 0, VCO_PLS, VCO_TRI, VCO_SIN };

enum { LFO_RATE, LFO_CV, LFO_UNI };
enum { LFO_SIN = 0, LFO_TRI, LFO_SQR, LFO_SAW };

enum { NOISE_LEVEL };
enum { NOISE_WHT = 0, NOISE_PNK, NOISE_SH };

enum { VCF_CUTOFF, VCF_RES, VCF_DRIVE, VCF_CV1, VCF_CV2, VCF_KTRK };
enum { VCF_IN = 0, VCF_IN_CV1, VCF_IN_CV2, VCF_IN_PITCH, VCF_IN_RES };
enum { VCF_LP24 = 0, VCF_LP12 };

enum { VCA_GAIN, VCA_CV, VCA_RESP };
enum { VCA_IN = 0, VCA_IN_CV };

enum { ADSR_A, ADSR_D, ADSR_S, ADSR_R, ADSR_VEL };
enum { ADSR_IN_GATE = 0, ADSR_IN_TRIG, ADSR_IN_VEL };
enum { ADSR_ENV = 0, ADSR_INV };

enum { MIX_1, MIX_2, MIX_3, MIX_4, MIX_MASTER };
enum { ATT_AMT1, ATT_OFF1, ATT_AMT2, ATT_OFF2 };
enum { DLY_TIME, DLY_FBK, DLY_TONE, DLY_MIX, DLY_CV };
enum { RVB_SIZE, RVB_DAMP, RVB_MIX };
enum { OUT_LEVEL };

const float R1 = 20.0f;
const float R2 = 375.0f;

/* ---------------------------------------------------------------- */

void presetClassic(Builder &b)
{
    b.row(R1);
    const int kbd   = b.put("KBD");
    const int vco1  = b.put("VCO");
    const int vco2  = b.put("VCO");
    const int noise = b.put("NOISE");
    const int mix   = b.put("MIX");
    const int vcf   = b.put("VCF");
    const int envF  = b.put("ADSR");
    const int envA  = b.put("ADSR");
    const int vca   = b.put("VCA");

    /* The LFO sits under the oscillators it modulates and the tail of the
     * chain under the VCA it comes out of, so the long cables in the default
     * rack are short ones. A patch laid out by signal flow is also a patch you
     * can read, and the first thing anyone does here is try to work out what
     * is plugged into what. */
    b.row(R2);
    const int lfo = b.put("LFO");
    b.x = 640.0f;
    const int scope = b.put("SCOPE");
    const int dly   = b.put("DLY");
    const int rvb   = b.put("RVB");
    const int out   = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envF, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envF, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);
    b.wire(kbd, KBD_VEL,   envA, ADSR_IN_VEL);

    b.wire(vco1,  VCO_SAW, mix, MIX_1);
    b.wire(vco2,  VCO_PLS, mix, MIX_2);
    b.wire(noise, NOISE_WHT, mix, MIX_3);
    b.wire(lfo,   LFO_TRI, vco2, VCO_IN_PWM);

    b.wire(mix,  0, vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(kbd,  KBD_PITCH, vcf, VCF_IN_PITCH);

    b.wire(vcf,  VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);

    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);
    b.wire(vca, 0, scope, 0);
    b.wire(lfo, LFO_TRI, scope, 1);

    b.set(kbd,   KBD_VOICES, 6.0f);
    b.set(vco2,  VCO_FINE, 7.0f);   b.set(vco2, VCO_PWM, 0.35f);
    b.set(mix,   MIX_1, 0.70f);     b.set(mix, MIX_2, 0.55f);
    b.set(mix,   MIX_3, 0.05f);
    b.set(vcf,   VCF_CUTOFF, 420.0f); b.set(vcf, VCF_RES, 0.36f);
    b.set(vcf,   VCF_DRIVE, 1.6f);    b.set(vcf, VCF_CV1, 0.28f);
    b.set(vcf,   VCF_KTRK, 0.35f);
    b.set(envF,  ADSR_A, 0.004f); b.set(envF, ADSR_D, 0.35f);
    b.set(envF,  ADSR_S, 0.22f);  b.set(envF, ADSR_R, 0.40f);
    b.set(envA,  ADSR_A, 0.006f); b.set(envA, ADSR_D, 0.60f);
    b.set(envA,  ADSR_S, 0.75f);  b.set(envA, ADSR_R, 0.35f);
    b.set(envA,  ADSR_VEL, 0.55f);
    b.set(vca,   VCA_RESP, 1.0f);
    b.set(lfo,   LFO_RATE, 0.35f);
    b.set(dly,   DLY_TIME, 0.28f); b.set(dly, DLY_FBK, 0.30f);
    b.set(dly,   DLY_MIX, 0.20f);
    b.set(rvb,   RVB_SIZE, 0.55f); b.set(rvb, RVB_DAMP, 0.40f);
    b.set(rvb,   RVB_MIX, 0.22f);
    b.set(out,   OUT_LEVEL, 0.60f);
}

/* The smallest thing that makes a note: one oscillator, one envelope, one
 * amplifier. Nothing to unpick before you start building. */
void presetInit(Builder &b)
{
    b.row(R1);
    const int kbd = b.put("KBD");
    const int vco = b.put("VCO");
    const int env = b.put("ADSR");
    const int vca = b.put("VCA");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  env, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  env, ADSR_IN_TRIG);
    b.wire(vco, VCO_SAW,   vca, VCA_IN);
    b.wire(env, ADSR_ENV,  vca, VCA_IN_CV);
    b.wire(vca, 0, out, 0);

    b.set(kbd, KBD_VOICES, 6.0f);
    b.set(env, ADSR_A, 0.005f); b.set(env, ADSR_D, 0.30f);
    b.set(env, ADSR_S, 0.70f);  b.set(env, ADSR_R, 0.30f);
    b.set(vca, VCA_RESP, 1.0f);
    b.set(out, OUT_LEVEL, 0.55f);
}

/* Legato mono, a sine an octave down under the saw, and a filter envelope
 * short enough to be a thump rather than a sweep. */
void presetBass(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int scope = b.put("SCOPE");
    const int out   = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envF, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envF, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);

    b.wire(vco1, VCO_SAW, mix, MIX_1);
    b.wire(vco2, VCO_SIN, mix, MIX_2);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, out, 0);
    b.wire(vca, 0, scope, 0);

    b.set(kbd,  KBD_MODE, (float)KM_LEGATO);
    b.set(kbd,  KBD_VOICES, 1.0f);
    b.set(kbd,  KBD_GLIDE, 0.02f);
    b.set(vco2, VCO_OCT, -1.0f);
    b.set(mix,  MIX_1, 0.80f); b.set(mix, MIX_2, 0.55f);
    b.set(vcf,  VCF_CUTOFF, 130.0f); b.set(vcf, VCF_RES, 0.45f);
    b.set(vcf,  VCF_DRIVE, 2.4f);    b.set(vcf, VCF_CV1, 0.34f);
    b.set(vcf,  VCF_KTRK, 0.30f);
    b.set(envF, ADSR_A, 0.001f); b.set(envF, ADSR_D, 0.18f);
    b.set(envF, ADSR_S, 0.05f);  b.set(envF, ADSR_R, 0.15f);
    b.set(envA, ADSR_A, 0.001f); b.set(envA, ADSR_D, 0.35f);
    b.set(envA, ADSR_S, 0.55f);  b.set(envA, ADSR_R, 0.12f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(out,  OUT_LEVEL, 0.65f);
}

/* A pulse whose width the LFO keeps moving, which is what stops a square wave
 * from sounding like a test tone, plus glide and an echo. */
void presetSquareLead(Builder &b)
{
    b.row(R1);
    const int kbd = b.put("KBD");
    const int vco = b.put("VCO");
    const int vcf = b.put("VCF");
    const int env = b.put("ADSR");
    const int vca = b.put("VCA");

    b.row(R2);
    const int lfo = b.put("LFO");
    b.x = 640.0f;
    const int dly = b.put("DLY");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  env, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  env, ADSR_IN_TRIG);
    b.wire(lfo, LFO_TRI,   vco, VCO_IN_PWM);
    b.wire(vco, VCO_PLS,   vcf, VCF_IN);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(env, ADSR_ENV,  vcf, VCF_IN_CV1);
    b.wire(vcf, VCF_LP24,  vca, VCA_IN);
    b.wire(env, ADSR_ENV,  vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, out, 0);

    b.set(kbd, KBD_MODE, (float)KM_LEGATO);
    b.set(kbd, KBD_VOICES, 1.0f);
    b.set(kbd, KBD_GLIDE, 0.06f);
    b.set(vco, VCO_PWM, 0.45f);
    b.set(lfo, LFO_RATE, 0.9f);
    b.set(vcf, VCF_CUTOFF, 2200.0f); b.set(vcf, VCF_RES, 0.28f);
    b.set(vcf, VCF_CV1, 0.16f);      b.set(vcf, VCF_KTRK, 0.5f);
    b.set(env, ADSR_A, 0.010f); b.set(env, ADSR_D, 0.30f);
    b.set(env, ADSR_S, 0.80f);  b.set(env, ADSR_R, 0.25f);
    b.set(vca, VCA_RESP, 1.0f);
    b.set(dly, DLY_TIME, 0.33f); b.set(dly, DLY_FBK, 0.38f);
    b.set(dly, DLY_MIX, 0.28f);
    b.set(out, OUT_LEVEL, 0.55f);
}

/* Eight voices, two saws pulled a few cents apart so they beat against each
 * other, and enough reverb to lose the edges. */
void presetPad(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    const int lfo = b.put("LFO");
    b.x = 640.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envF, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_VEL,   envA, ADSR_IN_VEL);

    b.wire(vco1, VCO_SAW, mix, MIX_1);
    b.wire(vco2, VCO_SAW, mix, MIX_2);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(lfo,  LFO_SIN,  vcf, VCF_IN_CV2);
    b.wire(kbd,  KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf,  VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 8.0f);
    b.set(vco1, VCO_FINE, -6.0f);
    b.set(vco2, VCO_FINE,  7.0f);
    b.set(mix,  MIX_1, 0.60f); b.set(mix, MIX_2, 0.60f);
    b.set(vcf,  VCF_CUTOFF, 700.0f); b.set(vcf, VCF_RES, 0.20f);
    b.set(vcf,  VCF_CV1, 0.18f);     b.set(vcf, VCF_CV2, 0.35f);
    b.set(vcf,  VCF_KTRK, 0.40f);
    b.set(lfo,  LFO_RATE, 0.18f);
    b.set(envF, ADSR_A, 0.80f); b.set(envF, ADSR_D, 1.50f);
    b.set(envF, ADSR_S, 0.50f); b.set(envF, ADSR_R, 1.20f);
    b.set(envA, ADSR_A, 0.90f); b.set(envA, ADSR_D, 1.50f);
    b.set(envA, ADSR_S, 0.80f); b.set(envA, ADSR_R, 1.60f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(rvb,  RVB_SIZE, 0.85f); b.set(rvb, RVB_DAMP, 0.35f);
    b.set(rvb,  RVB_MIX, 0.45f);
    b.set(out,  OUT_LEVEL, 0.50f);
}

/* No sustain at all on either envelope, and enough resonance that the filter
 * envelope is audible as a chirp on the front of every note. */
void presetPluck(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco  = b.put("VCO");
    const int vcf  = b.put("VCF");
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int dly = b.put("DLY");
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envF, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envF, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);
    b.wire(kbd, KBD_VEL,   envA, ADSR_IN_VEL);
    b.wire(vco, VCO_SAW,   vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24,  vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 6.0f);
    b.set(vcf,  VCF_CUTOFF, 240.0f); b.set(vcf, VCF_RES, 0.62f);
    b.set(vcf,  VCF_CV1, 0.42f);     b.set(vcf, VCF_KTRK, 0.50f);
    b.set(envF, ADSR_A, 0.001f); b.set(envF, ADSR_D, 0.16f);
    b.set(envF, ADSR_S, 0.00f);  b.set(envF, ADSR_R, 0.12f);
    b.set(envA, ADSR_A, 0.001f); b.set(envA, ADSR_D, 0.38f);
    b.set(envA, ADSR_S, 0.00f);  b.set(envA, ADSR_R, 0.22f);
    b.set(envA, ADSR_VEL, 0.7f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(dly,  DLY_TIME, 0.26f); b.set(dly, DLY_FBK, 0.32f);
    b.set(dly,  DLY_MIX, 0.25f);
    b.set(rvb,  RVB_SIZE, 0.60f); b.set(rvb, RVB_MIX, 0.28f);
    /* Louder than the rest, and deliberately: an envelope with no sustain
     * spends most of its life at zero, so the same output level that suits a
     * held note leaves a plucked one sounding half-finished. */
    b.set(out,  OUT_LEVEL, 0.85f);
}

/* The one rack that makes a sound with nothing held down. The filter is past
 * the edge of self-oscillation, so it is the oscillator; a slow LFO walks its
 * cutoff and the noise floor gives it something to catch on. Turn RES down and
 * it stops - which is the clearest demonstration of what resonance is that
 * the module set can offer. */
void presetDrone(Builder &b)
{
    b.row(R1);
    const int noise = b.put("NOISE");
    const int lfo1  = b.put("LFO");
    const int lfo2  = b.put("LFO");
    const int att   = b.put("ATT");
    const int vcf   = b.put("VCF");

    b.row(R2);
    b.x = 340.0f;
    const int scope = b.put("SCOPE");
    const int rvb   = b.put("RVB");
    const int out   = b.put("OUT");

    b.wire(noise, NOISE_PNK, vcf, VCF_IN);
    b.wire(lfo1,  LFO_SIN, att, 0);
    b.wire(lfo2,  LFO_TRI, att, 1);
    b.wire(att, 0, vcf, VCF_IN_CV1);
    b.wire(att, 1, vcf, VCF_IN_CV2);
    b.wire(vcf, VCF_LP24, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);
    b.wire(vcf, VCF_LP24, scope, 0);
    b.wire(att, 0, scope, 1);

    b.set(noise, NOISE_LEVEL, 0.35f);
    b.set(lfo1,  LFO_RATE, 0.05f);
    b.set(lfo2,  LFO_RATE, 0.13f);
    b.set(att,   ATT_AMT1, 0.55f);
    b.set(att,   ATT_AMT2, 0.22f);
    b.set(vcf,   VCF_CUTOFF, 110.0f);
    b.set(vcf,   VCF_RES, 1.06f);      /* past the edge: it rings on its own */
    b.set(vcf,   VCF_DRIVE, 1.4f);
    b.set(vcf,   VCF_CV1, 1.0f);
    b.set(vcf,   VCF_CV2, 1.0f);
    b.set(rvb,   RVB_SIZE, 0.90f); b.set(rvb, RVB_DAMP, 0.25f);
    b.set(rvb,   RVB_MIX, 0.55f);
    b.set(out,   OUT_LEVEL, 0.45f);
}

/* Noise through a resonant filter and nothing else - no oscillator anywhere.
 * The keyboard opens the amplifier and tracks the cutoff, so it is playable
 * even though there is no pitch in it. */
void presetWind(Builder &b)
{
    b.row(R1);
    const int kbd   = b.put("KBD");
    const int noise = b.put("NOISE");
    const int vcf   = b.put("VCF");
    const int env   = b.put("ADSR");
    const int vca   = b.put("VCA");

    b.row(R2);
    const int lfo = b.put("LFO");
    b.x = 640.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd,   KBD_GATE, env, ADSR_IN_GATE);
    b.wire(kbd,   KBD_TRIG, env, ADSR_IN_TRIG);
    b.wire(kbd,   KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(noise, NOISE_WHT, vcf, VCF_IN);
    b.wire(lfo,   LFO_SIN, vcf, VCF_IN_CV1);
    b.wire(env,   ADSR_ENV, vcf, VCF_IN_CV2);
    b.wire(vcf,   VCF_LP24, vca, VCA_IN);
    b.wire(env,   ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,   KBD_VOICES, 4.0f);
    b.set(noise, NOISE_LEVEL, 1.0f);
    b.set(vcf,   VCF_CUTOFF, 600.0f); b.set(vcf, VCF_RES, 0.80f);
    b.set(vcf,   VCF_CV1, 0.30f);     b.set(vcf, VCF_CV2, 0.20f);
    b.set(vcf,   VCF_KTRK, 1.0f);
    b.set(lfo,   LFO_RATE, 0.28f);
    b.set(env,   ADSR_A, 0.45f); b.set(env, ADSR_D, 0.90f);
    b.set(env,   ADSR_S, 0.60f); b.set(env, ADSR_R, 1.10f);
    b.set(vca,   VCA_RESP, 1.0f);
    b.set(rvb,   RVB_SIZE, 0.80f); b.set(rvb, RVB_DAMP, 0.30f);
    b.set(rvb,   RVB_MIX, 0.40f);
    b.set(out,   OUT_LEVEL, 0.55f);
}

/* ---- dance-floor racks -------------------------------------------- *
 *
 * The six below are recognisable sounds rather than categories, which is the
 * point: "supersaw" and "acid" are things you can hum, and a rack you can hum
 * is a rack you can then take apart to find out why it does that.
 * ------------------------------------------------------------------- */

/* Three saws pulled apart by a few cents each. The beating between them is the
 * entire sound - detune them to zero and it collapses into one thin oscillator,
 * which is the fastest way to hear what the FINE knobs are for. */
void presetSupersaw(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int vco3 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int dly = b.put("DLY");
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco3, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envF, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envF, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);

    b.wire(vco1, VCO_SAW, mix, MIX_1);
    b.wire(vco2, VCO_SAW, mix, MIX_2);
    b.wire(vco3, VCO_SAW, mix, MIX_3);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 8.0f);
    b.set(vco1, VCO_FINE, -16.0f);
    b.set(vco3, VCO_FINE,  17.0f);
    b.set(mix,  MIX_1, 0.45f); b.set(mix, MIX_2, 0.45f); b.set(mix, MIX_3, 0.45f);
    b.set(vcf,  VCF_CUTOFF, 1400.0f); b.set(vcf, VCF_RES, 0.18f);
    b.set(vcf,  VCF_CV1, 0.22f);      b.set(vcf, VCF_KTRK, 0.50f);
    b.set(envF, ADSR_A, 0.010f); b.set(envF, ADSR_D, 0.80f);
    b.set(envF, ADSR_S, 0.50f);  b.set(envF, ADSR_R, 0.50f);
    b.set(envA, ADSR_A, 0.008f); b.set(envA, ADSR_D, 1.00f);
    b.set(envA, ADSR_S, 0.85f);  b.set(envA, ADSR_R, 0.60f);
    b.set(vca,  VCA_RESP, 1.0f);
    /* Three sixteenths at 128 bpm, which is where a trance delay lives. */
    b.set(dly,  DLY_TIME, 0.352f); b.set(dly, DLY_FBK, 0.40f);
    b.set(dly,  DLY_MIX, 0.28f);
    b.set(rvb,  RVB_SIZE, 0.70f);  b.set(rvb, RVB_MIX, 0.28f);
    b.set(out,  OUT_LEVEL, 0.45f);
}

/* A 303 in the parts that matter: one oscillator, glide between overlapping
 * notes, and a filter with the resonance high enough that its envelope is the
 * melody. Play it legato and it slides; play it detached and it does not. */
void presetAcid(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco  = b.put("VCO");
    const int vcf  = b.put("VCF");
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int scope = b.put("SCOPE");
    const int dly   = b.put("DLY");
    const int out   = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envF, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envF, ADSR_IN_TRIG);
    b.wire(vco, VCO_SAW,   vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24,  vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, out, 0);
    b.wire(vcf, VCF_LP24, scope, 0);
    b.wire(envF, ADSR_ENV, scope, 1);

    b.set(kbd,  KBD_MODE, (float)KM_LEGATO);
    b.set(kbd,  KBD_VOICES, 1.0f);
    b.set(kbd,  KBD_GLIDE, 0.045f);
    b.set(vcf,  VCF_CUTOFF, 190.0f); b.set(vcf, VCF_RES, 0.84f);
    b.set(vcf,  VCF_DRIVE, 3.0f);    b.set(vcf, VCF_CV1, 0.46f);
    b.set(vcf,  VCF_KTRK, 0.30f);
    b.set(envF, ADSR_A, 0.001f); b.set(envF, ADSR_D, 0.24f);
    b.set(envF, ADSR_S, 0.00f);  b.set(envF, ADSR_R, 0.20f);
    /* The amplifier is nearly a gate on a 303 - the filter does the shaping. */
    b.set(envA, ADSR_A, 0.001f); b.set(envA, ADSR_D, 0.50f);
    b.set(envA, ADSR_S, 0.90f);  b.set(envA, ADSR_R, 0.08f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(dly,  DLY_TIME, 0.26f); b.set(dly, DLY_FBK, 0.34f);
    b.set(dly,  DLY_MIX, 0.22f);
    b.set(out,  OUT_LEVEL, 0.55f);
}

/* The rave stab: a saw and a pulse detuned much further apart than is
 * comfortable, the pulse width swept underneath, and enough resonance to make
 * it snarl. The detune is the joke - a third of a semitone, where a tuner
 * would tell you both oscillators are wrong. */
void presetHoover(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    const int lfo = b.put("LFO");
    b.x = 640.0f;
    const int dly = b.put("DLY");
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envF, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envF, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);
    b.wire(lfo, LFO_TRI,   vco2, VCO_IN_PWM);

    b.wire(vco1, VCO_SAW, mix, MIX_1);
    b.wire(vco2, VCO_PLS, mix, MIX_2);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 4.0f);
    b.set(vco1, VCO_FINE, -32.0f);
    b.set(vco2, VCO_FINE,  30.0f);
    b.set(vco2, VCO_PWM, 0.55f);
    b.set(mix,  MIX_1, 0.55f); b.set(mix, MIX_2, 0.55f);
    b.set(lfo,  LFO_RATE, 3.2f);
    b.set(vcf,  VCF_CUTOFF, 520.0f); b.set(vcf, VCF_RES, 0.55f);
    b.set(vcf,  VCF_DRIVE, 1.8f);    b.set(vcf, VCF_CV1, 0.34f);
    b.set(vcf,  VCF_KTRK, 0.40f);
    b.set(envF, ADSR_A, 0.004f); b.set(envF, ADSR_D, 0.55f);
    b.set(envF, ADSR_S, 0.30f);  b.set(envF, ADSR_R, 0.35f);
    b.set(envA, ADSR_A, 0.004f); b.set(envA, ADSR_D, 0.70f);
    b.set(envA, ADSR_S, 0.80f);  b.set(envA, ADSR_R, 0.30f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(dly,  DLY_TIME, 0.30f); b.set(dly, DLY_FBK, 0.32f);
    b.set(dly,  DLY_MIX, 0.22f);
    b.set(rvb,  RVB_SIZE, 0.65f); b.set(rvb, RVB_MIX, 0.25f);
    b.set(out,  OUT_LEVEL, 0.45f);
}

/* Two saws a hair apart and nothing else. The interference between them moves
 * slowly enough to hear as a sweep rather than as a chord, which is the whole
 * trick - and it only works in mono, because eight voices of it is mud. */
void presetReese(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int scope = b.put("SCOPE");
    const int out   = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);
    b.wire(vco1, VCO_SAW, mix, MIX_1);
    b.wire(vco2, VCO_SAW, mix, MIX_2);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, out, 0);
    b.wire(vca, 0, scope, 0);

    b.set(kbd,  KBD_MODE, (float)KM_LEGATO);
    b.set(kbd,  KBD_VOICES, 1.0f);
    b.set(vco1, VCO_FINE, -19.0f);
    b.set(vco2, VCO_FINE,  20.0f);
    b.set(mix,  MIX_1, 0.60f); b.set(mix, MIX_2, 0.60f);
    b.set(vcf,  VCF_CUTOFF, 320.0f); b.set(vcf, VCF_RES, 0.28f);
    b.set(vcf,  VCF_DRIVE, 2.0f);    b.set(vcf, VCF_KTRK, 0.30f);
    b.set(envA, ADSR_A, 0.006f); b.set(envA, ADSR_D, 0.50f);
    b.set(envA, ADSR_S, 0.90f);  b.set(envA, ADSR_R, 0.18f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(out,  OUT_LEVEL, 0.60f);
}

/* A pad behind a second amplifier that a square LFO opens and shuts sixteen
 * times a bar. Two VCAs in series, and the second one is the whole idea: hold
 * a chord and the rhythm is the LFO's, not yours. Turn its RATE knob and the
 * tempo changes. */
void presetTranceGate(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    const int lfo  = b.put("LFO");
    const int gate = b.put("VCA");
    b.x = 640.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(vco1, VCO_SAW, mix, MIX_1);
    b.wire(vco2, VCO_SAW, mix, MIX_2);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);

    /* The gate. Square wave into a linear VCA with its own gain at zero, so
     * the LFO is the only thing that opens it. */
    b.wire(vca, 0, gate, VCA_IN);
    b.wire(lfo, LFO_SQR, gate, VCA_IN_CV);
    b.wire(gate, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 8.0f);
    b.set(vco1, VCO_FINE, -8.0f);
    b.set(vco2, VCO_FINE,  9.0f);
    b.set(mix,  MIX_1, 0.55f); b.set(mix, MIX_2, 0.55f);
    b.set(vcf,  VCF_CUTOFF, 1100.0f); b.set(vcf, VCF_RES, 0.22f);
    b.set(vcf,  VCF_KTRK, 0.45f);
    b.set(envA, ADSR_A, 0.30f); b.set(envA, ADSR_D, 1.00f);
    b.set(envA, ADSR_S, 0.85f); b.set(envA, ADSR_R, 0.70f);
    b.set(vca,  VCA_RESP, 1.0f);
    /* Sixteenths at 120 bpm. */
    b.set(lfo,  LFO_RATE, 8.0f);
    b.set(lfo,  LFO_UNI, 1.0f);
    b.set(gate, VCA_GAIN, 0.0f);
    b.set(gate, VCA_CV, 1.0f);
    b.set(gate, VCA_RESP, 0.0f);      /* linear, so the edges stay hard */
    b.set(rvb,  RVB_SIZE, 0.75f); b.set(rvb, RVB_DAMP, 0.35f);
    b.set(rvb,  RVB_MIX, 0.35f);
    /* Louder than the rest: a unipolar LFO only ever half-opens the gate, and
     * the gate is shut half the time on top of that. */
    b.set(out,  OUT_LEVEL, 1.10f);
}

/* The house chord stab: a pulse with a saw an octave above it, and envelopes
 * short enough that a held chord still sounds like it was hit rather than
 * pressed. */
void presetOrganStab(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int dly = b.put("DLY");
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envF, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envF, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);
    b.wire(kbd, KBD_VEL,   envA, ADSR_IN_VEL);
    b.wire(vco1, VCO_PLS, mix, MIX_1);
    b.wire(vco2, VCO_SAW, mix, MIX_2);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 8.0f);
    b.set(vco2, VCO_OCT, 1.0f);
    b.set(mix,  MIX_1, 0.70f); b.set(mix, MIX_2, 0.30f);
    b.set(vcf,  VCF_CUTOFF, 900.0f); b.set(vcf, VCF_RES, 0.30f);
    b.set(vcf,  VCF_CV1, 0.30f);     b.set(vcf, VCF_KTRK, 0.40f);
    b.set(envF, ADSR_A, 0.001f); b.set(envF, ADSR_D, 0.25f);
    b.set(envF, ADSR_S, 0.10f);  b.set(envF, ADSR_R, 0.20f);
    b.set(envA, ADSR_A, 0.002f); b.set(envA, ADSR_D, 0.35f);
    b.set(envA, ADSR_S, 0.25f);  b.set(envA, ADSR_R, 0.25f);
    b.set(envA, ADSR_VEL, 0.5f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(dly,  DLY_TIME, 0.375f); b.set(dly, DLY_FBK, 0.30f);
    b.set(dly,  DLY_MIX, 0.24f);
    b.set(rvb,  RVB_SIZE, 0.60f);  b.set(rvb, RVB_MIX, 0.28f);
    b.set(out,  OUT_LEVEL, 0.60f);
}

typedef void (*PresetFn)(Builder &);

struct PresetEntry { RackPreset info; PresetFn build; };

const PresetEntry PRESETS[] = {
    { { "CLASSIC",      "two oscillators, ladder, delay and reverb" },  presetClassic },
    { { "INIT",         "one oscillator, one envelope - a place to start" }, presetInit },
    { { "BASS",         "legato mono, sub oscillator, short filter thump" }, presetBass },
    { { "SQUARE LEAD",  "pulse width on an LFO, glide and echo" },      presetSquareLead },
    { { "SAW PAD",      "eight voices, two detuned saws, long reverb" }, presetPad },
    { { "PLUCK",        "no sustain, resonant filter chirp" },          presetPluck },
    { { "DRONE",        "the filter is the oscillator - no keys needed" }, presetDrone },
    { { "WIND",         "noise through a resonant filter, no oscillator" }, presetWind },
    { { "SUPERSAW",     "three detuned saws - the trance lead" },       presetSupersaw },
    { { "ACID",         "303: one saw, glide, and a screaming filter" }, presetAcid },
    { { "HOOVER",       "the rave stab - detuned way past comfortable" }, presetHoover },
    { { "REESE",        "two saws a hair apart, mono, low and moving" }, presetReese },
    { { "TRANCE GATE",  "a pad chopped into sixteenths by a square LFO" }, presetTranceGate },
    { { "ORGAN STAB",   "house chord stab, pulse with a saw on top" },   presetOrganStab }
};

} /* anonymous namespace */

int rackPresetCount() { return (int)(sizeof PRESETS / sizeof PRESETS[0]); }

const RackPreset *rackPresetAt(int i)
{
    return (i >= 0 && i < rackPresetCount()) ? &PRESETS[i].info : 0;
}

void Engine::buildPreset(int index)
{
    if (index < 0 || index >= rackPresetCount()) return;
    clear();
    Builder b = { this, 20.0f, 20.0f };
    PRESETS[index].build(b);
}

void Engine::buildDefaultPatch() { buildPreset(0); }

} /* namespace bs */
