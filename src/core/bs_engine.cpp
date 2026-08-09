#include "bs_engine.h"
#include "bs_modules.h"

#include <chrono>

namespace bs {

Engine::Engine() : load(0.0f), cachedRev(0), blockPos(BS_BLOCK), havePending(false)
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

static NoteEvent ev(int kind, int note, float value, int atFrame)
{
    NoteEvent e;
    e.kind   = (uint8_t)kind;
    e.note   = (uint8_t)(note < 0 ? 0 : (note > 127 ? 127 : note));
    e.value  = value;
    e.offset = (uint32_t)(atFrame < 0 ? 0 : atFrame);
    return e;
}

void Engine::noteOn(int note, float velocity, int atFrame)
{
    if (note < 0 || note > 127) return;
    events.push(ev(NE_NOTE_ON, note, velocity, atFrame));
}

void Engine::noteOff(int note, int atFrame)
{
    if (note < 0 || note > 127) return;
    events.push(ev(NE_NOTE_OFF, note, 0.0f, atFrame));
}

void Engine::setBend(float b, int atFrame)
{
    events.push(ev(NE_BEND, 0, clampf(b, -1.0f, 1.0f), atFrame));
}

void Engine::noteExpression(int note, int kind, float value, int atFrame)
{
    static const uint8_t KIND[3] = { NE_NOTE_BEND, NE_NOTE_PRESS, NE_NOTE_TIMBRE };
    if (kind < 0 || kind > 2) return;
    events.push(ev(KIND[kind], (uint8_t)note, value, atFrame));
}

void Engine::setMod(float m, int atFrame)
{
    events.push(ev(NE_MOD, 0, clampf(m, 0.0f, 1.0f), atFrame));
}

void Engine::setSustain(bool on, int atFrame)
{
    events.push(ev(NE_SUSTAIN, 0, on ? 1.0f : 0.0f, atFrame));
}

/* Which CC drives a given macro, or 0 for none.
 *
 * A macro's own assignment wins; otherwise it takes its place in the run of
 * eight that CC BASE starts. Reading the rack rather than a table means there
 * is nothing to keep in step and nothing extra to save. */
int Engine::macroCcFor(int macro) const
{
    if (macro < 0 || macro >= BS_MACROS) return 0;
    for (int i = 0; i < patch.slotCount(); i++) {
        const Module *m = patch.module(i);
        if (!m || m->typeId != "MACRO") continue;
        if (m->paramCount() > BS_MACROS + 1 + macro) {
            const int own = (int)(m->params[(size_t)(BS_MACROS + 1 + macro)].value + 0.5f);
            if (own > 0) return own;
        }
        if (m->paramCount() > BS_MACROS) {
            const int base = (int)(m->params[(size_t)BS_MACROS].value + 0.5f);
            if (base > 0) return base + macro;
        }
        return 0;
    }
    return 0;
}

void Engine::setMacroCc(int macro, int cc)
{
    if (macro < 0 || macro >= BS_MACROS) return;
    std::lock_guard<std::mutex> g(mutex);
    for (int i = 0; i < patch.slotCount(); i++) {
        Module *m = patch.module(i);
        if (!m || m->typeId != "MACRO") continue;
        if (m->paramCount() > BS_MACROS + 1 + macro)
            m->params[(size_t)(BS_MACROS + 1 + macro)].value = (float)cc;
        return;
    }
}

/* The macro a CC drives, or -1. */
int Engine::macroForCc(int cc) const
{
    if (cc <= 0) return -1;
    for (int i = 0; i < BS_MACROS; i++)
        if (macroCcFor(i) == cc) return i;
    return -1;
}

int Engine::macroCcBase() const
{
    /* The first MACRO panel wins. Two of them with different assignments is
     * not a thing worth defining behaviour for; it is a thing to not do. */
    for (int i = 0; i < patch.slotCount(); i++) {
        const Module *m = patch.module(i);
        if (m && m->typeId == "MACRO" && m->paramCount() > BS_MACROS)
            return (int)(m->params[(size_t)BS_MACROS].value + 0.5f);
    }
    return 0;
}

void Engine::setMacro(int index, float value01)
{
    if (index < 0 || index >= BS_MACROS) return;
    const float v = clampf(value01, 0.0f, 1.0f);
    for (int i = 0; i < patch.slotCount(); i++) {
        Module *m = patch.module(i);
        if (m && m->typeId == "MACRO" && index < m->paramCount())
            m->params[(size_t)index].value = v;
    }
}

float Engine::macroValue(int index) const
{
    if (index < 0 || index >= BS_MACROS) return 0.0f;
    for (int i = 0; i < patch.slotCount(); i++) {
        const Module *m = patch.module(i);
        if (m && m->typeId == "MACRO" && index < m->paramCount())
            return m->params[(size_t)index].value;
    }
    return 0.0f;
}

void Engine::panic()
{
    std::lock_guard<std::mutex> g(mutex);
    /* Drop anything queued before silencing, so an event posted a moment ago
     * cannot arrive after the panic and restart a note. */
    NoteEvent e;
    while (events.pop(&e)) { }
    havePending = false;
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

                /* Everything due by this point in the buffer, in order,
                 * before anything reads it. Draining in the engine rather than
                 * in the keyboard module means a rack with two keyboard panels
                 * - or none at all - behaves: the events are consumed exactly
                 * once either way.
                 *
                 * An event lands on the block that supplies its frame. The
                 * rack processes whole blocks, so that is as fine as the
                 * timing gets - 32 frames - and an event asking for frame 400
                 * of a 512-frame buffer is applied before the block that
                 * starts at 384 rather than at the top of the call. */
                for (;;) {
                    if (!havePending) {
                        if (!events.pop(&pending)) break;
                        havePending = true;
                    }
                    if (pending.offset > (uint32_t)done) break;
                    keys.apply(pending);
                    havePending = false;
                }

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

    /* An event whose offset ran past the end of this call belongs to the next
     * one, at its start. A host should not produce those - the offsets it
     * gives are inside the buffer it asked for - so this is a guard against
     * being handed something out of spec rather than a mechanism, and getting
     * it slightly early is the failure to prefer over never getting it. */
    if (havePending) pending.offset = 0;
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
const float R3 = 730.0f;
const float R4 = 1085.0f;

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
    const int out = b.put("OUT");

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
    const int pan = b.put("PAN");
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
    /* Eight voices arrive here on one cable and used to be summed into the
     * middle. PAN gives each one a place, and the reverb's dry path now keeps
     * the two sides apart instead of flattening them again. */
    b.wire(vca, 0, pan, 0);
    b.wire(pan, 0, rvb, 0);
    b.wire(pan, 1, rvb, 1);
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
    b.set(pan, 0, 0.85f);
}

/* No sustain at all on either envelope, and enough resonance that the filter
 * envelope is audible as a chirp on the front of every note. */
/* A piano, out of parts that have never met one.
 *
 * Subtractive synthesis cannot actually make an acoustic piano: a real one is
 * a struck string whose partials are stretched sharp of the harmonic series,
 * and nothing here can be inharmonic. What it can do is reproduce the three
 * gestures the ear identifies a piano BY, which turn out not to be the
 * waveform at all:
 *
 *   1. Amplitude decays continuously while the key is held. Not a plateau -
 *      almost every other patch in this rack sustains, and that alone is most
 *      of what makes them sound synthetic.
 *   2. Brightness collapses far faster than loudness. The hammer strike is
 *      the only bright part; a second later the same note is nearly a sine.
 *   3. Both of those scale with velocity and with pitch. Hard notes are
 *      brighter, high notes are shorter and thinner.
 *
 * So the work is in three envelopes and the filter, not the oscillators. Two
 * saws a hair apart stand in for the two or three strings a real note has, a
 * pulse adds the hollow midrange, and a noise burst two hundredths of a second
 * long is the hammer hitting - remove that last one and the whole thing turns
 * into an organ. */
void presetPiano(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int nse  = b.put("NOISE");
    const int vcaN = b.put("VCA");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");

    b.row(R2);
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int envN = b.put("ADSR");
    const int vca  = b.put("VCA");
    const int rvb  = b.put("RVB");
    const int out  = b.put("OUT");

    /* Pitch to both oscillators and to the filter: keyboard tracking is what
     * makes the top octave thin and short instead of a loud honk. */
    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vcf,  VCF_IN_PITCH);

    b.wire(kbd, KBD_GATE, envF, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE, envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE, envN, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG, envF, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG, envA, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG, envN, ADSR_IN_TRIG);

    /* Velocity into the filter envelope as well as the amplifier. Playing
     * harder on a piano does not just make it louder, it makes it brighter,
     * and that is the half people actually hear. */
    b.wire(kbd, KBD_VEL, envA, ADSR_IN_VEL);
    b.wire(kbd, KBD_VEL, envF, ADSR_IN_VEL);
    b.wire(kbd, KBD_VEL, envN, ADSR_IN_VEL);

    /* Two saws a few cents apart for the strings, a narrow pulse for the
     * hollow middle. */
    b.wire(vco1, VCO_SAW, mix, 0);
    b.wire(vco2, VCO_SAW, mix, 1);
    b.wire(vco2, VCO_PLS, mix, 2);

    /* The hammer. Its own envelope, because it has to be gone long before the
     * filter envelope is. */
    b.wire(nse, NOISE_WHT, vcaN, VCA_IN);
    b.wire(envN, ADSR_ENV, vcaN, VCA_IN_CV);
    b.wire(vcaN, 0, mix, 3);

    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd, KBD_VOICES, 8.0f);

    b.set(vco1, VCO_FINE, -4.0f);
    b.set(vco2, VCO_FINE,  5.0f);
    b.set(vco2, VCO_PW,   0.32f);

    b.set(nse, NOISE_LEVEL, 0.85f);
    b.set(vcaN, VCA_GAIN, 0.0f);       /* the envelope opens it, nothing else */
    b.set(vcaN, VCA_RESP, 1.0f);

    b.set(mix, MIX_1, 0.52f);
    b.set(mix, MIX_2, 0.44f);
    b.set(mix, MIX_3, 0.20f);
    b.set(mix, MIX_4, 0.30f);
    b.set(mix, MIX_MASTER, 0.80f);

    /* Low base cutoff and a lot of envelope: the note has to arrive bright and
     * be dull within a second, which is a bigger sweep than most patches want.
     * Resonance stays low - a piano has no whistle in it. */
    b.set(vcf, VCF_CUTOFF, 320.0f);
    b.set(vcf, VCF_RES,    0.14f);
    b.set(vcf, VCF_DRIVE,  1.25f);
    b.set(vcf, VCF_CV1,    0.74f);
    b.set(vcf, VCF_KTRK,   0.85f);

    /* The filter envelope is the short one. */
    b.set(envF, ADSR_A, 0.001f); b.set(envF, ADSR_D, 0.60f);
    b.set(envF, ADSR_S, 0.00f);  b.set(envF, ADSR_R, 0.22f);
    b.set(envF, ADSR_VEL, 0.75f);

    /* The amplifier envelope is the long one, and its S is *zero* on purpose.
     * A sustain level is a plateau, and a piano has none - the string decays
     * continuously from the moment it is struck. Holding at any nonzero S is
     * what makes a synth patch sound like an organ pretending. Release is
     * short, which is the damper coming down. */
    b.set(envA, ADSR_A, 0.002f); b.set(envA, ADSR_D, 5.00f);
    b.set(envA, ADSR_S, 0.00f);  b.set(envA, ADSR_R, 0.30f);
    b.set(envA, ADSR_VEL, 0.85f);

    /* Twenty milliseconds. Long enough to hear, short enough not to be a
     * texture. */
    b.set(envN, ADSR_A, 0.0005f); b.set(envN, ADSR_D, 0.022f);
    b.set(envN, ADSR_S, 0.0f);    b.set(envN, ADSR_R, 0.02f);
    b.set(envN, ADSR_VEL, 0.9f);

    b.set(vca, VCA_RESP, 1.0f);

    b.set(rvb, RVB_SIZE, 0.44f);
    b.set(rvb, RVB_DAMP, 0.55f);
    b.set(rvb, RVB_MIX,  0.17f);

    b.set(out, OUT_LEVEL, 0.82f);
}

/* A piano the way a piano actually works.
 *
 * PIANO next door does it by subtraction and gets remarkably close on
 * gesture - decay while held, brightness dying faster than loudness, both
 * scaling with velocity. What it cannot do at all is inharmonicity, because
 * every oscillator in this rack has exactly harmonic partials and no filter
 * introduces stretch. That is the difference between "convincing synthesizer
 * piano" and "piano".
 *
 * This one is a modelled string: a delay line whose length is the pitch, with
 * a chain of allpasses in the loop making the delay depend on frequency. The
 * partials come out sharp of the harmonic series by exactly the amount the
 * stiffness knob asks for - measured at 1.2e-4, which is a real piano's
 * midrange - and the hammer is a noise burst that gets brighter when struck
 * harder, because felt stiffens under load.
 *
 * The rest is a room: a little chorus for the two-strings-per-note beating
 * that the model's own detuning starts, and a reverb for the soundboard.
 */

/* A bowed string. The same waveguide as GRAND, driven continuously instead of
 * struck - which is the whole demonstration: nothing about the string changes,
 * only what is putting energy into it. */
void presetBowed(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int str  = b.put("STRING");
    const int svf  = b.put("SVF");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 560.0f;
    const int lfo = b.put("LFO");
    /* The vibrato has to be summed with the pitch, not plugged into the same
     * jack. An input takes one cable and a second replaces the first - which
     * is right at a patch bay and never what a preset means. The rack's own
     * check caught this one the moment it was written. */
    const int mix = b.put("MIX");
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, mix, 0);
    b.wire(lfo, 0,         mix, 1);
    b.wire(mix, 0,         str, 0);
    /* The gate, not the trigger. A bow is not an event - the string is driven
     * for as long as the key is down, and how far down the gate sits is how
     * hard the hair presses. */
    b.wire(kbd, KBD_GATE,  str, 1);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);
    b.wire(kbd, KBD_VEL,   envA, ADSR_IN_VEL);

    b.wire(str, 0, svf, 0);
    b.wire(svf, 0, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd, KBD_VOICES, 4.0f);

    b.set(str, 2, 0.55f);        /* DECAY - short; the bow supplies the rest  */
    b.set(str, 3, 0.42f);        /* BRIGHT                                     */
    b.set(str, 4, 0.06f);        /* INHARM - gut and steel, not piano wire     */
    b.set(str, 5, 0.10f);        /* STRIKE - where the bow sits                */
    b.set(str, 6, 1.2f);         /* SPREAD                                     */
    b.set(str, 7, 1.0f);         /* EXCITE = BOW                               */
    b.set(str, 8, 0.75f);        /* FORCE                                      */

    b.set(svf, 0, 3400.0f);
    b.set(svf, 1, 0.10f);

    /* Slow in, slow out. A bow does not begin or end abruptly. */
    b.set(envA, 0, 0.18f);
    b.set(envA, 1, 0.20f);
    b.set(envA, 2, 0.90f);
    b.set(envA, 3, 0.35f);

    /* Vibrato: a left hand, not an effect. Unity on the pitch and a hair of
     * the oscillator - a few cents is all a player uses. */
    b.set(mix, MIX_1, 1.0f);
    b.set(mix, MIX_2, 0.004f);
    b.set(mix, MIX_MASTER, 1.0f);
    b.set(lfo, 0, 5.2f);         /* vibrato speed */

    b.set(rvb, 0, 0.72f);
    b.set(rvb, 2, 0.28f);
}

void presetGrand(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int str  = b.put("STRING");
    const int svf  = b.put("SVF");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");
    /* The hammer's force. STRING reads the height of its trigger as velocity,
     * and the keyboard's TRIG is a fixed 10 V pulse - so the two are combined
     * here rather than both being patched into one jack, which is a thing a
     * patch bay cannot do and which quietly cost this rack every note after
     * the first. */
    const int hit  = b.put("VCA");

    b.row(R2);
    b.x = 560.0f;
    const int cho = b.put("CHORUS");
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, str, 0);
    b.wire(kbd, KBD_TRIG,  hit, VCA_IN);
    b.wire(kbd, KBD_VEL,   hit, VCA_IN_CV);
    b.wire(hit, 0,         str, 1);      /* a pulse as tall as you played */
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);
    b.wire(kbd, KBD_VEL,   envA, ADSR_IN_VEL);
    b.wire(kbd, KBD_PITCH, svf, 3);      /* the top of a piano is not the bottom */

    b.wire(str, 0, svf, 0);
    b.wire(svf, 0, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, cho, 0);
    b.wire(cho, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd, KBD_VOICES, 8.0f);

    /* A floor under the gain, so the softest possible key still clears the
     * string's trigger threshold instead of being inaudible. */
    b.set(hit, VCA_GAIN, 0.15f);
    b.set(hit, VCA_CV,   1.0f);
    b.set(hit, VCA_RESP, 0.0f);

    b.set(str, 2, 0.72f);        /* DECAY  - about seven seconds             */
    b.set(str, 3, 0.60f);        /* BRIGHT                                    */
    b.set(str, 4, 0.50f);        /* INHARM - measured at B = 1.2e-4           */
    b.set(str, 5, 0.13f);        /* STRIKE - an eighth along, as a piano is   */
    b.set(str, 6, 2.6f);         /* SPREAD - the strings per note, in cents   */

    /* The damper, not the tone: the string already decides its own colour, so
     * this only stops the very top from being glassy, and tracks the keyboard
     * so the treble is not filtered into silence. */
    b.set(svf, 0, 5200.0f);
    b.set(svf, 1, 0.05f);
    b.set(svf, 4, 0.55f);

    /* Only the release matters. The string decays on its own - the amplifier
     * envelope is a damper, and its job is to stop the note when the key comes
     * up rather than to shape it while it is down. */
    b.set(envA, ADSR_A, 0.0005f); b.set(envA, ADSR_D, 8.0f);
    b.set(envA, ADSR_S, 1.0f);    b.set(envA, ADSR_R, 0.22f);
    b.set(envA, ADSR_VEL, 0.25f);
    b.set(vca, VCA_RESP, 0.0f);

    /* Both kept well back. The string now produces its own beating from the
     * two courses and its own decay, and a wet room on top of that fills the
     * gaps between the partials and flattens the attack - measured, the patch
     * fell 1 dB in its first half second where the bare string falls 11, which
     * is a pad rather than a piano. */
    b.set(cho, 0, 0.30f); b.set(cho, 1, 0.18f); b.set(cho, 4, 0.10f);
    b.set(rvb, RVB_SIZE, 0.44f); b.set(rvb, RVB_DAMP, 0.66f);
    b.set(rvb, RVB_MIX, 0.12f);
    b.set(out, OUT_LEVEL, 0.80f);
}

/* The other tradition, in one rack. No filter anywhere. */
void presetWestCoast(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco  = b.put("VCO");
    const int fold = b.put("FOLD");
    const int func = b.put("FUNC");
    const int mix  = b.put("MIX");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 560.0f;
    const int lfo = b.put("LFO");
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco, VCO_IN_PITCH);
    b.wire(kbd, KBD_TRIG,  func, 0);
    b.wire(vco, VCO_SIN,   fold, 0);

    /* Both the envelope and the LFO move the folding, so they are summed
     * first. An input takes one cable; patching them both at the jack would
     * keep only the second, which is how this rack lost its tremolo. */
    b.wire(func, 0, mix, 0);
    b.wire(lfo, LFO_TRI, mix, 1);
    /* The envelope drives the folding, not a filter: louder is not brighter
     * here, it is MORE FOLDED, which is a different kind of movement. */
    b.wire(mix, 0, fold, 1);

    b.wire(fold, 0, vca, VCA_IN);
    b.wire(func, 0, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd, KBD_VOICES, 4.0f);
    b.set(fold, 0, 1.4f); b.set(fold, 2, 5.0f);
    b.set(mix, MIX_1, 0.75f); b.set(mix, MIX_2, 0.45f); b.set(mix, MIX_MASTER, 1.0f);
    b.set(func, 0, 0.004f); b.set(func, 1, 1.1f); b.set(func, 2, 0.25f);
    b.set(lfo, LFO_RATE, 0.25f); b.set(lfo, LFO_UNI, 1.0f);
    b.set(vca, VCA_RESP, 1.0f);
    b.set(rvb, RVB_SIZE, 0.66f); b.set(rvb, RVB_MIX, 0.30f);
    b.set(out, OUT_LEVEL, 0.55f);
}

/* Nothing held down, and it plays for as long as you leave it. */
void presetOracle(Builder &b)
{
    b.row(R1);
    const int clk   = b.put("CLK");
    const int seq   = b.put("SEQ");
    const int quant = b.put("QUANT");
    const int slew  = b.put("SLEW");
    const int vco   = b.put("VCO");
    const int vcf   = b.put("SVF");

    b.row(R2);
    const int func  = b.put("FUNC");
    const int logic = b.put("LOGIC");
    const int vca   = b.put("VCA");
    const int dly   = b.put("DLY");
    const int rvb   = b.put("RVB");
    const int out   = b.put("OUT");

    b.wire(clk, 0, seq, 0);
    b.wire(clk, 1, logic, 0);
    b.wire(clk, 2, logic, 1);
    b.wire(logic, 2, func, 0);          /* XOR: an accent pattern */

    b.wire(seq, 0, quant, 0);
    b.wire(quant, 0, slew, 0);
    b.wire(slew, 0, vco, VCO_IN_PITCH);
    b.wire(seq, 1, vca, VCA_IN_CV);

    b.wire(vco, VCO_SAW, vcf, 0);
    b.wire(func, 0, vcf, 1);
    b.wire(vcf, 0, vca, VCA_IN);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(clk, 0, 96.0f);
    b.set(seq, 0, -1.0f);  b.set(seq, 1, 0.33f); b.set(seq, 2, 0.0f);
    b.set(seq, 3, 0.58f);  b.set(seq, 4, 0.25f); b.set(seq, 5, -0.42f);
    b.set(seq, 6, 0.75f);  b.set(seq, 7, 0.08f);
    b.set(seq, 9, 0.02f);
    b.set(quant, 0, 4.0f); b.set(quant, 1, 9.0f);
    b.set(slew, 0, 0.02f); b.set(slew, 1, 0.02f);
    b.set(vcf, 0, 500.0f); b.set(vcf, 1, 0.42f); b.set(vcf, 2, 2.2f);
    b.set(func, 0, 0.002f); b.set(func, 1, 0.30f);
    b.set(vca, VCA_RESP, 1.0f);
    b.set(dly, DLY_TIME, 0.375f); b.set(dly, DLY_FBK, 0.45f);
    b.set(dly, DLY_MIX, 0.35f);
    b.set(rvb, RVB_SIZE, 0.78f); b.set(rvb, RVB_MIX, 0.34f);
    b.set(out, OUT_LEVEL, 0.5f);
}

/* The same string, wound tight. */
void presetBell(Builder &b)
{
    b.row(R1);
    const int kbd = b.put("KBD");
    const int str = b.put("STRING");
    const int hit = b.put("VCA");
    const int crs = b.put("CRUSH");
    const int cho = b.put("CHORUS");

    b.row(R2);
    b.x = 460.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, str, 0);
    b.wire(kbd, KBD_TRIG,  hit, VCA_IN);
    b.wire(kbd, KBD_VEL,   hit, VCA_IN_CV);
    b.wire(hit, 0,         str, 1);
    b.wire(str, 0, crs, 0);
    b.wire(crs, 0, cho, 0);
    b.wire(cho, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd, KBD_VOICES, 6.0f);
    b.set(hit, VCA_GAIN, 0.15f);
    b.set(hit, VCA_CV,   1.0f);
    b.set(hit, VCA_RESP, 0.0f);
    /* Stiffness at the top of its range: the partials go so far sharp that
     * they stop being a harmonic series at all, which is what a struck metal
     * bar is. */
    /* Twelve seconds, not thirty. A bell should outlast the phrase, but a
     * decay long enough that six of them overlap is a rack that only gets
     * louder - it measured at 0.72 RMS against a comfortable 0.1. */
    b.set(str, 2, 0.80f);
    b.set(str, 3, 0.80f);
    b.set(str, 4, 1.00f);
    b.set(str, 5, 0.28f);
    b.set(str, 6, 6.0f);
    b.set(crs, 0, 11.0f); b.set(crs, 1, 16000.0f); b.set(crs, 2, 0.30f);
    b.set(cho, 0, 0.18f); b.set(cho, 1, 0.35f); b.set(cho, 4, 0.35f);
    b.set(rvb, RVB_SIZE, 0.86f); b.set(rvb, RVB_DAMP, 0.35f);
    b.set(rvb, RVB_MIX, 0.34f);
    b.set(out, OUT_LEVEL, 0.32f);
}

/* The 303, near enough. A sequence, a glide, and a filter with too much
 * resonance - the three things that made a bass machine into a genre. */
void presetAcidSeq(Builder &b)
{
    b.row(R1);
    const int clk  = b.put("CLK");
    const int seq  = b.put("SEQ");
    const int quan = b.put("QUANT");
    const int vco  = b.put("VCO");
    const int vcf  = b.put("VCF");

    b.row(R2);
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");
    const int dly  = b.put("DLY");
    const int out  = b.put("OUT");

    b.wire(clk, 0, seq, 0);
    b.wire(seq, 0, quan, 0);
    b.wire(quan, 0, vco, VCO_IN_PITCH);
    b.wire(seq, 1, envF, ADSR_IN_TRIG);
    b.wire(seq, 1, envA, ADSR_IN_GATE);
    b.wire(clk, 0, envA, ADSR_IN_TRIG);

    b.wire(vco, VCO_SAW, vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, out, 0);
    b.wire(dly, 0, out, 1);

    b.set(clk, 0, 130.0f); b.set(clk, 1, 0.42f);
    b.set(seq, 0, -1.0f);  b.set(seq, 1, -1.0f);  b.set(seq, 2, -0.75f);
    b.set(seq, 3, -1.0f);  b.set(seq, 4, -0.42f); b.set(seq, 5, -1.0f);
    b.set(seq, 6, -0.58f); b.set(seq, 7, -0.25f);
    /* The glide is the sound. Turn it off and it is a bass line; leave it on
     * and consecutive notes slide into each other, which is the whole thing. */
    b.set(seq, 9, 0.055f);
    b.set(quan, 0, 4.0f); b.set(quan, 1, 9.0f);
    b.set(vco, VCO_OCT, -1.0f);
    b.set(vcf, VCF_CUTOFF, 180.0f); b.set(vcf, VCF_RES, 0.88f);
    b.set(vcf, VCF_DRIVE, 2.2f);    b.set(vcf, VCF_CV1, 0.55f);
    b.set(envF, ADSR_A, 0.001f); b.set(envF, ADSR_D, 0.22f);
    b.set(envF, ADSR_S, 0.0f);   b.set(envF, ADSR_R, 0.10f);
    b.set(envA, ADSR_A, 0.002f); b.set(envA, ADSR_D, 0.30f);
    b.set(envA, ADSR_S, 0.0f);   b.set(envA, ADSR_R, 0.08f);
    b.set(vca, VCA_RESP, 1.0f);
    b.set(dly, DLY_TIME, 0.231f); b.set(dly, DLY_FBK, 0.38f);
    b.set(dly, DLY_MIX, 0.24f);
    b.set(out, OUT_LEVEL, 0.55f);
}

/* The patch that plays itself and never repeats.
 *
 * A cycling function generator clocks a sample and hold; the voltage it grabs
 * sets how long the NEXT cycle takes. So the rhythm is generated by the same
 * thing the rhythm is driving, and the loop never settles. This idea is older
 * than most modular systems and still the best argument for one.
 */
void presetKrell(Builder &b)
{
    b.row(R1);
    const int func = b.put("FUNC");
    const int nse  = b.put("NOISE");
    const int quan = b.put("QUANT");
    const int vco  = b.put("VCO");
    const int svf  = b.put("SVF");

    b.row(R2);
    const int env  = b.put("FUNC");
    const int vca  = b.put("VCA");
    const int cho  = b.put("CHORUS");
    const int rvb  = b.put("RVB");
    const int out  = b.put("OUT");

    /* The clock is its own end-of-cycle. */
    b.wire(func, 1, nse, 0);          /* EOC samples the noise            */
    b.wire(func, 1, env, 0);          /* and starts the note              */
    b.wire(nse, NOISE_SH, quan, 0);
    b.wire(quan, 0, vco, VCO_IN_PITCH);
    b.wire(nse, NOISE_SH, func, 1);   /* and decides the next interval    */

    b.wire(vco, VCO_TRI, svf, 0);
    b.wire(env, 0, svf, 1);
    b.wire(svf, 0, vca, VCA_IN);
    b.wire(env, 0, vca, VCA_IN_CV);
    b.wire(vca, 0, cho, 0);
    b.wire(cho, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(func, 0, 0.9f); b.set(func, 1, 1.4f); b.set(func, 4, 1.0f); /* CYCLE */
    b.set(nse, NOISE_LEVEL, 1.0f);
    b.set(quan, 0, 3.0f); b.set(quan, 1, 2.0f); b.set(quan, 2, 1.0f);
    b.set(vco, VCO_OCT, 0.0f);
    b.set(svf, 0, 700.0f); b.set(svf, 1, 0.30f); b.set(svf, 2, 2.6f);
    b.set(env, 0, 0.6f); b.set(env, 1, 2.2f); b.set(env, 2, 0.35f);
    b.set(vca, VCA_RESP, 1.0f);
    b.set(cho, 1, 0.5f); b.set(cho, 4, 0.4f);
    b.set(rvb, RVB_SIZE, 0.85f); b.set(rvb, RVB_MIX, 0.42f);
    b.set(out, OUT_LEVEL, 0.5f);
}

/* Two bandpasses in parallel are a mouth. */
void presetVowel(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco  = b.put("VCO");
    const int f1   = b.put("SVF");
    const int f2   = b.put("SVF");
    const int mix  = b.put("MIX");

    b.row(R2);
    const int lfo  = b.put("LFO");
    const int env  = b.put("ADSR");
    const int vca  = b.put("VCA");
    const int rvb  = b.put("RVB");
    const int out  = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  env, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  env, ADSR_IN_TRIG);

    b.wire(vco, VCO_SAW, f1, 0);
    b.wire(vco, VCO_SAW, f2, 0);
    /* The LFO slides the first formant while the second stays put, which is
     * the difference between a vowel and a wah. */
    b.wire(lfo, LFO_SIN, f1, 1);

    b.wire(f1, 2, mix, 0);            /* both bandpass outputs */
    b.wire(f2, 2, mix, 1);
    b.wire(mix, 0, vca, VCA_IN);
    b.wire(env, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd, KBD_VOICES, 4.0f);
    /* Roughly "ah": a first formant near 700 Hz and a second near 1100. */
    b.set(f1, 0, 700.0f);  b.set(f1, 1, 0.82f); b.set(f1, 2, 0.9f);
    b.set(f2, 0, 1150.0f); b.set(f2, 1, 0.86f);
    b.set(mix, MIX_1, 0.7f); b.set(mix, MIX_2, 0.55f); b.set(mix, MIX_MASTER, 1.0f);
    b.set(lfo, LFO_RATE, 0.35f);
    b.set(env, ADSR_A, 0.06f); b.set(env, ADSR_D, 0.30f);
    b.set(env, ADSR_S, 0.75f); b.set(env, ADSR_R, 0.35f);
    b.set(vca, VCA_RESP, 1.0f);
    b.set(rvb, RVB_SIZE, 0.68f); b.set(rvb, RVB_MIX, 0.28f);
    b.set(out, OUT_LEVEL, 0.6f);
}

/* Four sources, one output, and a clock deciding which is which. */
void presetSwitchboard(Builder &b)
{
    b.row(R1);
    const int clk  = b.put("CLK");
    const int logi = b.put("LOGIC");
    const int sw   = b.put("SWITCH");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");

    b.row(R2);
    const int nse  = b.put("NOISE");
    const int str  = b.put("STRING");
    const int crs  = b.put("CRUSH");
    const int dly  = b.put("DLY");
    const int out  = b.put("OUT");

    b.wire(clk, 0, sw, 0);
    b.wire(clk, 1, logi, 0);
    b.wire(clk, 3, logi, 1);
    b.wire(logi, 0, str, 1);          /* AND of /2 and /8: a sparse strike */

    b.wire(vco1, VCO_SAW, sw, 2);
    b.wire(vco2, VCO_PLS, sw, 3);
    b.wire(nse, NOISE_PNK, sw, 4);
    b.wire(str, 0, sw, 5);

    b.wire(sw, 0, crs, 0);
    b.wire(crs, 0, dly, 0);
    b.wire(dly, 0, out, 0);
    b.wire(dly, 0, out, 1);

    b.set(clk, 0, 112.0f);
    b.set(vco1, VCO_OCT, -1.0f);
    b.set(vco2, VCO_OCT, 0.0f); b.set(vco2, VCO_FINE, 7.0f); b.set(vco2, VCO_PW, 0.25f);
    b.set(nse, NOISE_LEVEL, 0.55f);
    b.set(str, 2, 0.45f); b.set(str, 3, 0.7f); b.set(str, 4, 0.65f);
    b.set(crs, 0, 7.0f); b.set(crs, 1, 9000.0f); b.set(crs, 2, 0.6f);
    b.set(dly, DLY_TIME, 0.268f); b.set(dly, DLY_FBK, 0.44f);
    b.set(dly, DLY_MIX, 0.32f);
    b.set(out, OUT_LEVEL, 0.42f);
}

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
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(noise, NOISE_PNK, vcf, VCF_IN);
    b.wire(lfo1,  LFO_SIN, att, 0);
    b.wire(lfo2,  LFO_TRI, att, 1);
    b.wire(att, 0, vcf, VCF_IN_CV1);
    b.wire(att, 1, vcf, VCF_IN_CV2);
    b.wire(vcf, VCF_LP24, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

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
    const int dly = b.put("DLY");
    const int out = b.put("OUT");

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
    const int out = b.put("OUT");

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

/* ---- racks that each show one trick ------------------------------- *
 *
 * Every one below exists because it does something none of the others do -
 * an oscillator used as a modulator, a filter inside a delay's feedback path,
 * an amplifier with its gain allowed to go negative. They are instruments, but
 * they are also the shortest answer to "what else is this thing for".
 * ------------------------------------------------------------------- */

/* Three oscillators a major third and a fifth apart, so one key is a chord.
 * Retune COARSE on the upper two to +3 and +7 and the whole rack turns minor,
 * which is the cheapest music theory lesson available. */
void presetChord(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int vco3 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int env  = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco3, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  env, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  env, ADSR_IN_TRIG);
    b.wire(vco1, VCO_SAW, mix, MIX_1);
    b.wire(vco2, VCO_SAW, mix, MIX_2);
    b.wire(vco3, VCO_SAW, mix, MIX_3);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(env, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 4.0f);
    b.set(vco2, VCO_COARSE, 4.0f);      /* major third */
    b.set(vco3, VCO_COARSE, 7.0f);      /* fifth       */
    b.set(mix,  MIX_1, 0.50f); b.set(mix, MIX_2, 0.42f); b.set(mix, MIX_3, 0.42f);
    b.set(vcf,  VCF_CUTOFF, 1300.0f); b.set(vcf, VCF_RES, 0.18f);
    b.set(vcf,  VCF_KTRK, 0.50f);
    b.set(env,  ADSR_A, 0.02f); b.set(env, ADSR_D, 0.60f);
    b.set(env,  ADSR_S, 0.60f); b.set(env, ADSR_R, 0.50f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(rvb,  RVB_SIZE, 0.65f); b.set(rvb, RVB_MIX, 0.30f);
    b.set(out,  OUT_LEVEL, 0.50f);
}

/* Four oscillators an octave apart, mixed like drawbars. The mixer levels are
 * the registration - pull one out and the timbre changes without a filter
 * being involved at all, which is additive synthesis wearing a modular's
 * clothes. */
void presetOctaves(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int vco3 = b.put("VCO");
    const int vco4 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int env  = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    const int oscs[4] = { vco1, vco2, vco3, vco4 };
    for (int i = 0; i < 4; i++) {
        b.wire(kbd, KBD_PITCH, oscs[i], VCO_IN_PITCH);
        b.wire(oscs[i], VCO_TRI, mix, MIX_1 + i);
    }
    b.wire(kbd, KBD_GATE, env, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG, env, ADSR_IN_TRIG);
    b.wire(mix, 0, vca, VCA_IN);
    b.wire(env, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 6.0f);
    b.set(vco1, VCO_OCT, -1.0f);
    b.set(vco3, VCO_OCT,  1.0f);
    b.set(vco4, VCO_OCT,  2.0f);
    b.set(mix,  MIX_1, 0.70f); b.set(mix, MIX_2, 0.55f);
    b.set(mix,  MIX_3, 0.35f); b.set(mix, MIX_4, 0.20f);
    b.set(env,  ADSR_A, 0.006f); b.set(env, ADSR_D, 0.30f);
    b.set(env,  ADSR_S, 0.85f);  b.set(env, ADSR_R, 0.18f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(rvb,  RVB_SIZE, 0.55f); b.set(rvb, RVB_MIX, 0.22f);
    b.set(out,  OUT_LEVEL, 0.40f);
}

/* One oscillator restarting another. The slave's own pitch is swept by an
 * envelope, but it can never finish a cycle before the master resets it, so
 * what changes is the shape of each period rather than the note - a hard,
 * vocal tearing sound that no filter can make. COARSE on the second VCO is the
 * knob to turn. */
void presetHardSync(Builder &b)
{
    b.row(R1);
    const int kbd    = b.put("KBD");
    const int master = b.put("VCO");
    const int slave  = b.put("VCO");
    const int envS   = b.put("ADSR");
    const int vcf    = b.put("VCF");
    const int envA   = b.put("ADSR");
    const int vca    = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int dly = b.put("DLY");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, master, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, slave,  VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envS, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envS, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);

    b.wire(master, VCO_SAW, slave, VCO_IN_SYNC);
    b.wire(envS,   ADSR_ENV, slave, VCO_IN_FM);
    b.wire(slave,  VCO_SAW, vcf, VCF_IN);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, out, 0);

    b.set(kbd,   KBD_VOICES, 4.0f);
    b.set(slave, VCO_COARSE, 7.0f);
    b.set(slave, VCO_FM, 0.13f);        /* about 1.3 octaves of sweep */
    b.set(envS,  ADSR_A, 0.002f); b.set(envS, ADSR_D, 0.70f);
    b.set(envS,  ADSR_S, 0.10f);  b.set(envS, ADSR_R, 0.40f);
    b.set(vcf,   VCF_CUTOFF, 3200.0f); b.set(vcf, VCF_RES, 0.12f);
    b.set(envA,  ADSR_A, 0.003f); b.set(envA, ADSR_D, 0.50f);
    b.set(envA,  ADSR_S, 0.70f);  b.set(envA, ADSR_R, 0.25f);
    b.set(vca,   VCA_RESP, 1.0f);
    b.set(dly,   DLY_TIME, 0.28f); b.set(dly, DLY_FBK, 0.28f);
    b.set(dly,   DLY_MIX, 0.20f);
    b.set(out,   OUT_LEVEL, 0.50f);
}

/* An amplifier whose gain is allowed to go negative, driven at audio rate by a
 * second oscillator. What comes out is the sum and difference of the two
 * frequencies and neither of the originals, which is why it never sounds like
 * a note - detune the modulator and it goes from bell to gong to scrap metal. */
void presetRingMod(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int ring = b.put("VCA");
    const int vcf  = b.put("VCF");
    const int env  = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  env, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  env, ADSR_IN_TRIG);

    b.wire(vco1, VCO_SIN, ring, VCA_IN);
    b.wire(vco2, VCO_SIN, ring, VCA_IN_CV);
    b.wire(ring, 0, vcf, VCF_IN);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(env, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 4.0f);
    b.set(vco2, VCO_COARSE, 5.0f);
    b.set(vco2, VCO_FINE, 11.0f);
    b.set(ring, VCA_RESP, 2.0f);        /* four quadrants */
    b.set(ring, VCA_CV, 1.0f);
    b.set(vcf,  VCF_CUTOFF, 4000.0f); b.set(vcf, VCF_RES, 0.10f);
    b.set(env,  ADSR_A, 0.002f); b.set(env, ADSR_D, 1.20f);
    b.set(env,  ADSR_S, 0.15f);  b.set(env, ADSR_R, 0.80f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(rvb,  RVB_SIZE, 0.80f); b.set(rvb, RVB_MIX, 0.35f);
    b.set(out,  OUT_LEVEL, 0.60f);
}

/* One sine modulating another's pitch, with an envelope on how much. Deep at
 * the start and clean by the end is what makes a struck sound: the clang is
 * the modulation, and it decays faster than the note does. The modulation is
 * exponential rather than through-zero, so the ratios are not the tidy
 * integers an FM synth uses - which is why it lands closer to a gamelan than
 * to an electric piano. */
void presetFmBell(Builder &b)
{
    b.row(R1);
    const int kbd   = b.put("KBD");
    const int mod   = b.put("VCO");
    const int depth = b.put("VCA");
    const int envM  = b.put("ADSR");
    const int car   = b.put("VCO");
    const int envA  = b.put("ADSR");
    const int vca   = b.put("VCA");

    b.row(R2);
    b.x = 640.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, mod, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, car, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envM, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envM, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);

    b.wire(mod, VCO_SIN, depth, VCA_IN);
    b.wire(envM, ADSR_ENV, depth, VCA_IN_CV);
    b.wire(depth, 0, car, VCO_IN_FM);
    b.wire(car, VCO_SIN, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 6.0f);
    b.set(mod,  VCO_COARSE, 7.0f);
    b.set(mod,  VCO_OCT, 1.0f);
    b.set(depth, VCA_RESP, 0.0f);
    b.set(depth, VCA_CV, 1.0f);
    b.set(car,  VCO_FM, 0.20f);
    b.set(envM, ADSR_A, 0.001f); b.set(envM, ADSR_D, 0.35f);
    b.set(envM, ADSR_S, 0.00f);  b.set(envM, ADSR_R, 0.30f);
    b.set(envA, ADSR_A, 0.001f); b.set(envA, ADSR_D, 1.60f);
    b.set(envA, ADSR_S, 0.00f);  b.set(envA, ADSR_R, 1.20f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(rvb,  RVB_SIZE, 0.85f); b.set(rvb, RVB_MIX, 0.38f);
    b.set(out,  OUT_LEVEL, 0.60f);
}

/* Plays itself. A square LFO clocks the sample-and-hold, which grabs a new
 * random voltage on every tick; an attenuverter squeezes that into about an
 * octave and it becomes the pitch. The same clock opens the envelope, so every
 * step is a note. Nothing is connected to the keyboard at all - turn the LFO's
 * RATE and the tempo changes, turn the attenuverter's AMT and the tune gets
 * wider or narrower. */
void presetSampleHold(Builder &b)
{
    b.row(R1);
    const int lfo   = b.put("LFO");
    const int noise = b.put("NOISE");
    const int att   = b.put("ATT");
    const int vco   = b.put("VCO");
    const int vcf   = b.put("VCF");
    const int env   = b.put("ADSR");
    const int vca   = b.put("VCA");

    b.row(R2);
    b.x = 480.0f;
    const int dly = b.put("DLY");
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(lfo, LFO_SQR, noise, 1);            /* the clock */
    b.wire(lfo, LFO_SQR, env, ADSR_IN_GATE);
    b.wire(noise, NOISE_SH, att, 0);
    b.wire(att, 0, vco, VCO_IN_PITCH);
    b.wire(vco, VCO_SAW, vcf, VCF_IN);
    b.wire(env, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(env, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(lfo,  LFO_RATE, 5.5f);
    b.set(lfo,  LFO_UNI, 1.0f);
    b.set(noise, NOISE_LEVEL, 1.0f);
    /* The sample-and-hold swings the full +/-5 V, which as a pitch would be
     * ten octaves. A fifth of that is a tune. */
    b.set(att,  ATT_AMT1, 0.20f);
    b.set(att,  ATT_OFF1, -0.4f);
    b.set(vcf,  VCF_CUTOFF, 500.0f); b.set(vcf, VCF_RES, 0.55f);
    b.set(vcf,  VCF_CV1, 0.30f);
    b.set(env,  ADSR_A, 0.002f); b.set(env, ADSR_D, 0.14f);
    b.set(env,  ADSR_S, 0.00f);  b.set(env, ADSR_R, 0.10f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(dly,  DLY_TIME, 0.27f); b.set(dly, DLY_FBK, 0.42f);
    b.set(dly,  DLY_MIX, 0.32f);
    b.set(rvb,  RVB_SIZE, 0.70f); b.set(rvb, RVB_MIX, 0.30f);
    b.set(out,  OUT_LEVEL, 0.85f);
}

/* A mixer used as a summer for control voltages rather than audio: the
 * keyboard's pitch and an LFO both arrive at it, and their sum drives the
 * oscillator. An input jack takes one cable, so adding a wobble to a pitch is
 * not a matter of plugging in twice - it is a matter of adding the two
 * voltages first. The delay's feedback does the rest. */
void presetDubSiren(Builder &b)
{
    b.row(R1);
    const int kbd = b.put("KBD");
    const int lfo = b.put("LFO");
    const int mix = b.put("MIX");
    const int vco = b.put("VCO");
    const int vcf = b.put("VCF");
    const int env = b.put("ADSR");
    const int vca = b.put("VCA");

    b.row(R2);
    b.x = 480.0f;
    const int dly = b.put("DLY");
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, mix, MIX_1);
    b.wire(lfo, LFO_SIN,   mix, MIX_2);
    b.wire(mix, 0, vco, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE, env, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG, env, ADSR_IN_TRIG);
    b.wire(vco, VCO_PLS, vcf, VCF_IN);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(env, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, dly, 0);
    b.wire(dly, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd, KBD_MODE, (float)KM_MONO);
    b.set(kbd, KBD_VOICES, 1.0f);
    /* Unity on the pitch, a sliver on the wobble - anything more and it stops
     * being a siren and starts being a mistake. */
    b.set(mix, MIX_1, 1.00f);
    b.set(mix, MIX_2, 0.06f);
    b.set(mix, MIX_MASTER, 1.0f);
    b.set(lfo, LFO_RATE, 5.0f);
    b.set(vco, VCO_PW, 0.35f);
    b.set(vcf, VCF_CUTOFF, 2600.0f); b.set(vcf, VCF_RES, 0.30f);
    b.set(vcf, VCF_KTRK, 0.60f);
    b.set(env, ADSR_A, 0.02f); b.set(env, ADSR_D, 0.40f);
    b.set(env, ADSR_S, 0.85f); b.set(env, ADSR_R, 0.30f);
    b.set(vca, VCA_RESP, 1.0f);
    b.set(dly, DLY_TIME, 0.42f); b.set(dly, DLY_FBK, 0.72f);
    b.set(dly, DLY_TONE, 2200.0f); b.set(dly, DLY_MIX, 0.42f);
    b.set(rvb, RVB_SIZE, 0.70f);  b.set(rvb, RVB_MIX, 0.25f);
    b.set(out, OUT_LEVEL, 0.50f);
}

/* The filter's cutoff on an LFO, fast and deep, with the resonance high enough
 * that the sweep whistles. It is the same modulation TRANCE GATE puts on an
 * amplifier, moved one module to the left - and that is the whole difference
 * between a rhythm and a voice. */
void presetWobble(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int env  = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    const int lfo = b.put("LFO");
    b.x = 640.0f;
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  env, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  env, ADSR_IN_TRIG);
    b.wire(vco1, VCO_SAW, mix, MIX_1);
    b.wire(vco2, VCO_PLS, mix, MIX_2);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(lfo, LFO_TRI, vcf, VCF_IN_CV1);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(env, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, out, 0);

    b.set(kbd,  KBD_MODE, (float)KM_LEGATO);
    b.set(kbd,  KBD_VOICES, 1.0f);
    b.set(vco2, VCO_OCT, -1.0f);
    b.set(mix,  MIX_1, 0.55f); b.set(mix, MIX_2, 0.60f);
    b.set(lfo,  LFO_RATE, 4.0f);
    b.set(vcf,  VCF_CUTOFF, 260.0f); b.set(vcf, VCF_RES, 0.72f);
    b.set(vcf,  VCF_DRIVE, 2.6f);    b.set(vcf, VCF_CV1, 0.42f);
    b.set(env,  ADSR_A, 0.005f); b.set(env, ADSR_D, 0.40f);
    b.set(env,  ADSR_S, 0.95f);  b.set(env, ADSR_R, 0.20f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(out,  OUT_LEVEL, 0.55f);
}

/* Two pulse waves whose widths are moved by two LFOs at slightly different
 * speeds, so the two never line up and the pair drifts in and out of phase
 * forever. That drift is the entire string-machine sound, and it comes from
 * nothing but the two rates being close rather than equal. */
void presetPwmStrings(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int vco1 = b.put("VCO");
    const int vco2 = b.put("VCO");
    const int mix  = b.put("MIX");
    const int vcf  = b.put("VCF");
    const int env  = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    const int lfo1 = b.put("LFO");
    const int lfo2 = b.put("LFO");
    b.x = 640.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco1, VCO_IN_PITCH);
    b.wire(kbd, KBD_PITCH, vco2, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  env, ADSR_IN_GATE);
    b.wire(lfo1, LFO_TRI, vco1, VCO_IN_PWM);
    b.wire(lfo2, LFO_TRI, vco2, VCO_IN_PWM);
    b.wire(vco1, VCO_PLS, mix, MIX_1);
    b.wire(vco2, VCO_PLS, mix, MIX_2);
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(env, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,  KBD_VOICES, 8.0f);
    b.set(vco2, VCO_FINE, 5.0f);
    b.set(vco1, VCO_PWM, 0.42f);
    b.set(vco2, VCO_PWM, 0.42f);
    /* Close, but not equal. Equal would lock them together and the drift -
     * which is the sound - would never happen. */
    b.set(lfo1, LFO_RATE, 0.31f);
    b.set(lfo2, LFO_RATE, 0.47f);
    b.set(mix,  MIX_1, 0.50f); b.set(mix, MIX_2, 0.50f);
    b.set(vcf,  VCF_CUTOFF, 1800.0f); b.set(vcf, VCF_RES, 0.15f);
    b.set(vcf,  VCF_KTRK, 0.45f);
    b.set(env,  ADSR_A, 0.35f); b.set(env, ADSR_D, 1.00f);
    b.set(env,  ADSR_S, 0.80f); b.set(env, ADSR_R, 0.90f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(rvb,  RVB_SIZE, 0.80f); b.set(rvb, RVB_MIX, 0.35f);
    b.set(out,  OUT_LEVEL, 0.32f);
}

/* A sine with an envelope on its pitch and no filter anywhere. The pitch
 * envelope is fifty milliseconds long and drops three octaves; the amplitude
 * envelope outlives it. That is a kick drum, and it is also the shortest
 * demonstration that an envelope patched somewhere unusual stops being an
 * envelope and becomes a gesture. */
void presetKick(Builder &b)
{
    b.row(R1);
    const int kbd  = b.put("KBD");
    const int envP = b.put("ADSR");
    const int vco  = b.put("VCO");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");

    b.row(R2);
    b.x = 480.0f;
    const int out = b.put("OUT");

    b.wire(kbd, KBD_PITCH, vco, VCO_IN_PITCH);
    b.wire(kbd, KBD_GATE,  envP, ADSR_IN_GATE);
    b.wire(kbd, KBD_GATE,  envA, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG,  envP, ADSR_IN_TRIG);
    b.wire(kbd, KBD_TRIG,  envA, ADSR_IN_TRIG);
    b.wire(envP, ADSR_ENV, vco, VCO_IN_FM);
    b.wire(vco, VCO_SIN, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, out, 0);

    b.set(kbd,  KBD_VOICES, 3.0f);
    b.set(kbd,  KBD_OCT, -2.0f);
    b.set(vco,  VCO_FM, 0.30f);         /* three octaves at the peak */
    b.set(envP, ADSR_A, 0.001f); b.set(envP, ADSR_D, 0.05f);
    b.set(envP, ADSR_S, 0.00f);  b.set(envP, ADSR_R, 0.05f);
    b.set(envA, ADSR_A, 0.001f); b.set(envA, ADSR_D, 0.40f);
    b.set(envA, ADSR_S, 0.00f);  b.set(envA, ADSR_R, 0.30f);
    b.set(vca,  VCA_RESP, 0.0f);
    /* Low, because a kick is nearly all transient: at a level that suits its
     * average it hits the output stage's soft clip on every strike, and a
     * soft-clipped kick loses the very thing it is for. */
    b.set(out,  OUT_LEVEL, 0.55f);
}

/* Noise, a filter with the resonance most of the way up, and an envelope short
 * enough to be a hit. Cutoff is the pitch of it - drop it and the snare
 * becomes a tom, raise it and it becomes a hat, and neither involves an
 * oscillator. */
void presetSnare(Builder &b)
{
    b.row(R1);
    const int kbd   = b.put("KBD");
    const int noise = b.put("NOISE");
    const int vcf   = b.put("VCF");
    const int env   = b.put("ADSR");
    const int vca   = b.put("VCA");

    b.row(R2);
    b.x = 480.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(kbd, KBD_GATE, env, ADSR_IN_GATE);
    b.wire(kbd, KBD_TRIG, env, ADSR_IN_TRIG);
    b.wire(kbd, KBD_PITCH, vcf, VCF_IN_PITCH);
    b.wire(noise, NOISE_WHT, vcf, VCF_IN);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(env, ADSR_ENV, vca, VCA_IN_CV);
    b.wire(vca, 0, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(kbd,   KBD_VOICES, 4.0f);
    b.set(noise, NOISE_LEVEL, 1.4f);
    b.set(vcf,   VCF_CUTOFF, 1700.0f); b.set(vcf, VCF_RES, 0.88f);
    b.set(vcf,   VCF_KTRK, 1.0f);
    b.set(env,   ADSR_A, 0.001f); b.set(env, ADSR_D, 0.13f);
    b.set(env,   ADSR_S, 0.00f);  b.set(env, ADSR_R, 0.09f);
    b.set(vca,   VCA_RESP, 0.0f);
    b.set(rvb,   RVB_SIZE, 0.55f); b.set(rvb, RVB_MIX, 0.30f);
    b.set(out,   OUT_LEVEL, 0.85f);
}

/* A resonant filter inside a delay's feedback path, which is a loop the patch
 * has no way to resolve in one pass - so one cable in it is read a block late,
 * and that block of delay is what keeps it from being an infinite regress. A
 * whisper of noise gets it started and the loop does the rest. Plays itself;
 * the MIX level on the returning signal is the difference between a room and a
 * scream. */
void presetHowl(Builder &b)
{
    b.row(R1);
    const int noise = b.put("NOISE");
    const int mix   = b.put("MIX");
    const int vcf   = b.put("VCF");
    const int lfo   = b.put("LFO");
    const int dly   = b.put("DLY");

    b.row(R2);
    b.x = 340.0f;
    const int rvb = b.put("RVB");
    const int out = b.put("OUT");

    b.wire(noise, NOISE_PNK, mix, MIX_1);
    b.wire(dly, 1, mix, MIX_2);          /* the wet return, back round */
    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(lfo, LFO_SIN, vcf, VCF_IN_CV1);
    b.wire(vcf, VCF_LP24, dly, 0);
    b.wire(vcf, VCF_LP24, rvb, 0);
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

    b.set(noise, NOISE_LEVEL, 0.30f);
    b.set(mix,   MIX_1, 0.50f);
    /* Unity on the return, so the loop neither dies away nor runs off: what
     * stops it climbing is the saturation in the filter and in the delay's
     * write, which is also what gives the tone its edge. Below about 0.9 here
     * it decays into silence and the rack does nothing at all. */
    b.set(mix,   MIX_2, 1.00f);
    b.set(mix,   MIX_MASTER, 1.30f);
    b.set(lfo,   LFO_RATE, 0.09f);
    b.set(vcf,   VCF_CUTOFF, 240.0f); b.set(vcf, VCF_RES, 0.80f);
    b.set(vcf,   VCF_DRIVE, 1.5f);    b.set(vcf, VCF_CV1, 0.85f);
    b.set(dly,   DLY_TIME, 0.55f); b.set(dly, DLY_FBK, 0.45f);
    b.set(dly,   DLY_TONE, 1800.0f); b.set(dly, DLY_MIX, 1.0f);
    b.set(rvb,   RVB_SIZE, 0.88f); b.set(rvb, RVB_DAMP, 0.30f);
    b.set(rvb,   RVB_MIX, 0.45f);
    b.set(out,   OUT_LEVEL, 0.55f);
}

/* ---- the whole set, at once --------------------------------------- *
 *
 * Every module type in the rack, patched into one instrument. It is here as an
 * answer to "what can this thing do", and as the one rack where the modules
 * that are easy to overlook - the multiple, the attenuverter, the
 * sample-and-hold sitting inside the noise module - are doing visible work.
 *
 * The signal path is ordinary enough: three oscillators through a mixer into
 * the ladder, two envelopes, a delay and a reverb. What is not ordinary is
 * everything driving it. The keyboard does not reach the oscillators at all;
 * it reaches an arpeggiator, and the arpeggiator's clock is what plays the
 * rack. That same clock samples a random voltage for the filter, and a second
 * LFO bends the delay's time so the echoes wow like tape.
 * ------------------------------------------------------------------- */
void presetGrandTour(Builder &b)
{
    b.row(R1);
    const int kbd   = b.put("KBD");
    const int arp   = b.put("ARP");
    const int mult  = b.put("MULT");
    const int vco1  = b.put("VCO");
    const int vco2  = b.put("VCO");
    const int vco3  = b.put("VCO");
    const int noise = b.put("NOISE");
    const int mix   = b.put("MIX");
    const int vcf   = b.put("VCF");

    b.row(R2);
    const int lfo1  = b.put("LFO");
    const int lfo2  = b.put("LFO");
    const int att   = b.put("ATT");
    const int macro = b.put("MACRO");
    const int envF = b.put("ADSR");
    const int envA = b.put("ADSR");
    const int vca  = b.put("VCA");
    const int dly  = b.put("DLY");
    const int rvb  = b.put("RVB");
    const int out  = b.put("OUT");

    /* The keyboard plays the arpeggiator, and the arpeggiator plays the rack. */
    b.wire(kbd, KBD_PITCH, arp, 0);
    b.wire(kbd, KBD_GATE,  arp, 1);

    b.wire(arp, 0, vco1, VCO_IN_PITCH);
    b.wire(arp, 0, vco2, VCO_IN_PITCH);
    b.wire(arp, 0, vco3, VCO_IN_PITCH);
    b.wire(arp, 0, vcf,  VCF_IN_PITCH);

    /* One gate has to reach two envelopes, and one trigger has to reach two
     * envelopes and the sample-and-hold's clock. An output fans out on its own,
     * but sending it through a multiple is what makes that visible on the
     * panel instead of implied by five cables leaving one jack. */
    b.wire(arp, 1, mult, 0);
    b.wire(arp, 2, mult, 1);
    b.wire(mult, 0, envF, ADSR_IN_GATE);
    b.wire(mult, 1, envA, ADSR_IN_GATE);
    b.wire(mult, 3, envF, ADSR_IN_TRIG);
    b.wire(mult, 4, envA, ADSR_IN_TRIG);
    b.wire(mult, 5, noise, 1);           /* every arp step samples a new voltage */

    b.wire(lfo1, LFO_TRI, vco2, VCO_IN_PWM);
    b.wire(noise, NOISE_SH, att, 0);
    b.wire(att, 0, vcf, VCF_IN_CV2);
    /* One macro onto the filter's resonance. Nothing in the rack needs it -
     * it is here because a macro is the one control something outside the rack
     * can reach, and this is what that looks like once it is cabled. */
    b.wire(macro, 0, vcf, VCF_IN_RES);

    b.wire(vco1, VCO_SAW, mix, MIX_1);
    b.wire(vco2, VCO_PLS, mix, MIX_2);
    b.wire(vco3, VCO_SIN, mix, MIX_3);
    b.wire(noise, NOISE_WHT, mix, MIX_4);

    b.wire(mix, 0, vcf, VCF_IN);
    b.wire(envF, ADSR_ENV, vcf, VCF_IN_CV1);
    b.wire(vcf, VCF_LP24, vca, VCA_IN);
    b.wire(envA, ADSR_ENV, vca, VCA_IN_CV);

    b.wire(vca, 0, dly, 0);
    b.wire(lfo2, LFO_SIN, dly, 1);       /* tape wow on the echoes */
    b.wire(dly, 0, rvb, 0);
    /* The reverb reaches the output through the mixers at the bottom of the
     * rack, where the second half joins it - wiring it straight to OUT as well
     * would simply be overwritten when those are patched. */

    b.set(kbd,  KBD_VOICES, 6.0f);
    b.set(arp,  0, 9.0f);                /* rate  */
    b.set(arp,  1, 2.0f);                /* up and down */
    b.set(arp,  2, 2.0f);                /* two octaves */
    b.set(arp,  3, 0.45f);               /* gate length */
    b.set(vco2, VCO_FINE, 8.0f);
    b.set(vco2, VCO_PWM, 0.40f);
    b.set(vco3, VCO_OCT, -1.0f);
    b.set(noise, NOISE_LEVEL, 1.0f);
    b.set(mix,  MIX_1, 0.50f); b.set(mix, MIX_2, 0.40f);
    b.set(mix,  MIX_3, 0.45f); b.set(mix, MIX_4, 0.04f);
    b.set(lfo1, LFO_RATE, 0.7f);
    b.set(lfo2, LFO_RATE, 0.13f);
    b.set(att,   ATT_AMT1, 0.35f);
    b.set(att,   ATT_OFF1, 0.0f);
    b.set(macro, 0, 0.25f);
    b.set(vcf,  VCF_CUTOFF, 340.0f); b.set(vcf, VCF_RES, 0.52f);
    b.set(vcf,  VCF_DRIVE, 1.7f);
    b.set(vcf,  VCF_CV1, 0.30f);     b.set(vcf, VCF_CV2, 0.55f);
    b.set(vcf,  VCF_KTRK, 0.35f);
    b.set(envF, ADSR_A, 0.002f); b.set(envF, ADSR_D, 0.22f);
    b.set(envF, ADSR_S, 0.10f);  b.set(envF, ADSR_R, 0.18f);
    b.set(envA, ADSR_A, 0.003f); b.set(envA, ADSR_D, 0.30f);
    b.set(envA, ADSR_S, 0.30f);  b.set(envA, ADSR_R, 0.25f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(dly,  DLY_TIME, 0.28f); b.set(dly, DLY_FBK, 0.42f);
    b.set(dly,  DLY_CV, 0.06f);   b.set(dly, DLY_MIX, 0.30f);
    b.set(rvb,  RVB_SIZE, 0.72f); b.set(rvb, RVB_MIX, 0.30f);

    /* ---- and the second half of the rack ----
     *
     * A sequenced voice that shares nothing with the one above except the
     * output: its own clock, its own pitch source, a struck string instead of
     * an oscillator, and the whole shaping chain the rack has that subtraction
     * does not - folding, crushing and a modulated delay. */
    b.row(R3);
    const int clk    = b.put("CLK");
    const int seq    = b.put("SEQ");
    const int quant  = b.put("QUANT");
    const int slew   = b.put("SLEW");
    const int str    = b.put("STRING");
    const int svf    = b.put("SVF");
    /* A different number for every voice - the thing a rack of hardware
     * cannot do, because there one module is one voice. */
    const int voice  = b.put("VOICE");

    b.row(R4);
    const int func   = b.put("FUNC");
    const int logic  = b.put("LOGIC");
    const int sw     = b.put("SWITCH");
    const int fold   = b.put("FOLD");
    const int crush  = b.put("CRUSH");
    const int chorus = b.put("CHORUS");
    const int pan    = b.put("PAN");
    const int mixL   = b.put("MIX");
    const int mixR   = b.put("MIX");

    /* Every voice gets its own small detune, so a chord is eight strings
     * rather than one string played eight times. */
    b.wire(voice, 1, str, 2);          /* RND into the string's own input */
    b.set(voice, 0, 0.35f);            /* a little of it */

    /* Clock, and two divisions of it disagreeing with each other. */
    b.wire(clk, 0, seq, 0);
    b.wire(clk, 1, logic, 0);
    b.wire(clk, 2, logic, 1);
    b.wire(logic, 2, sw, 0);           /* XOR: a pattern neither input has */

    /* Pitch: stepped, rounded to a scale, then the corners taken off. */
    b.wire(seq, 0, quant, 0);
    b.wire(quant, 0, slew, 0);
    b.wire(slew, 0, str, 0);
    b.wire(seq, 1, str, 1);            /* the gate strikes it */

    b.wire(str, 0, svf, 0);
    b.wire(func, 0, svf, 1);           /* cycling, so it is an LFO here */

    /* Four things to be, one at a time, on a rhythm from the logic. */
    b.wire(svf, 0, sw, 2);
    b.wire(str, 0, sw, 3);
    b.wire(noise, NOISE_PNK, sw, 4);
    b.wire(vco3, VCO_SIN, sw, 5);

    b.wire(sw, 0, fold, 0);
    b.wire(fold, 0, crush, 0);
    b.wire(crush, 0, pan, 0);
    b.wire(pan, 0, chorus, 0);

    /* Both halves meet here rather than fighting over the output's two jacks. */
    b.wire(rvb, 0, mixL, 0);
    b.wire(chorus, 0, mixL, 1);
    b.wire(rvb, 1, mixR, 0);
    b.wire(chorus, 1, mixR, 1);
    b.wire(mixL, 0, out, 0);
    b.wire(mixR, 0, out, 1);

    /* The clock starts stopped. GRAND TOUR is declared as a rack that makes no
     * sound until you play it, and a sequencer running under a held chord
     * would quietly break that promise - so this half is an invitation rather
     * than an ambush. The note tells you where the switch is. */
    b.set(clk, 0, 104.0f);
    b.set(clk, 2, 0.0f);                /* RUN off */
    b.set(seq, 0, -1.00f); b.set(seq, 1, 0.25f);  b.set(seq, 2, 0.42f);
    b.set(seq, 3, 0.00f);  b.set(seq, 4, -0.58f); b.set(seq, 5, 0.25f);
    b.set(seq, 6, 0.67f);  b.set(seq, 7, 0.08f);
    b.set(seq, 8, 8.0f);   b.set(seq, 9, 0.01f);
    b.set(quant, 0, 4.0f);              /* minor pentatonic */
    b.set(quant, 1, 9.0f);              /* rooted on A      */
    b.set(slew, 0, 0.008f); b.set(slew, 1, 0.008f);
    b.set(str, 2, 0.90f);               /* DECAY  */
    b.set(str, 3, 0.62f);               /* BRIGHT */
    b.set(str, 4, 0.50f);               /* INHARM - about a real piano's */
    b.set(svf, 0, 900.0f); b.set(svf, 1, 0.35f); b.set(svf, 2, 1.4f);
    b.set(func, 0, 0.9f); b.set(func, 1, 1.6f); b.set(func, 4, 1.0f);  /* CYCLE */
    b.set(fold, 0, 1.7f);
    b.set(crush, 0, 9.0f); b.set(crush, 1, 12000.0f); b.set(crush, 2, 0.45f);
    b.set(chorus, 1, 0.55f); b.set(chorus, 4, 0.45f);
    b.set(pan, 0, 0.8f);
    b.set(mixL, MIX_1, 0.85f); b.set(mixL, MIX_2, 0.42f);
    b.set(mixR, MIX_1, 0.85f); b.set(mixR, MIX_2, 0.42f);
    b.set(out,  OUT_LEVEL, 0.55f);
}

/* A bright monophonic lead of a very particular 1984 vintage: a saw and a
 * pulse a few cents apart, a filter open enough to be piercing with just
 * enough resonance to whistle, and envelopes fast enough that a staccato line
 * stays staccato. The slapback echo is doing as much period work as the
 * oscillators are.
 *
 * The original was a Jupiter-8, which is two oscillators and a filter - so
 * this is the same arrangement rather than an impression of one. */
void presetAxel(Builder &b)
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

    /* Mono and retriggering rather than legato: the line is played detached,
     * and every note wants its own attack. */
    b.set(kbd,  KBD_MODE, (float)KM_MONO);
    b.set(kbd,  KBD_VOICES, 1.0f);
    b.set(vco1, VCO_FINE, -5.0f);
    b.set(vco2, VCO_FINE,  6.0f);
    b.set(vco2, VCO_PW, 0.32f);
    b.set(mix,  MIX_1, 0.60f); b.set(mix, MIX_2, 0.55f);
    b.set(vcf,  VCF_CUTOFF, 950.0f); b.set(vcf, VCF_RES, 0.42f);
    b.set(vcf,  VCF_DRIVE, 1.5f);    b.set(vcf, VCF_CV1, 0.30f);
    b.set(vcf,  VCF_KTRK, 0.50f);
    b.set(envF, ADSR_A, 0.001f); b.set(envF, ADSR_D, 0.30f);
    b.set(envF, ADSR_S, 0.30f);  b.set(envF, ADSR_R, 0.20f);
    b.set(envA, ADSR_A, 0.002f); b.set(envA, ADSR_D, 0.35f);
    b.set(envA, ADSR_S, 0.80f);  b.set(envA, ADSR_R, 0.12f);
    b.set(vca,  VCA_RESP, 1.0f);
    b.set(dly,  DLY_TIME, 0.30f); b.set(dly, DLY_FBK, 0.28f);
    b.set(dly,  DLY_MIX, 0.20f);
    b.set(rvb,  RVB_SIZE, 0.50f); b.set(rvb, RVB_MIX, 0.18f);
    /* Loud, because it is a lead and a lead that sits politely in the mix is
     * not doing its job. */
    b.set(out,  OUT_LEVEL, 0.85f);
}

typedef void (*PresetFn)(Builder &);

struct PresetEntry { RackPreset info; PresetFn build; };

const PresetEntry PRESETS[] = {
    { { "CLASSIC",      "two oscillators, ladder, delay and reverb", 0,
      "CLASSIC\n\nTwo oscillators through a mixer into the ladder filter, one envelope on the cutoff and one on the amplifier. Every hard-wired subtractive synth is a version of this patch.\n\nTRY: drop VCF CUTOFF and raise RES. Move the second VCO's FINE away from +7 and listen to the beating speed change." },  presetClassic },
    { { "INIT",         "one oscillator, one envelope - a place to start", 0,
      "INIT\n\nThe smallest thing that makes a note: one oscillator, one envelope, one amplifier. Nothing to unpick before you start building.\n\nTRY: right-click the rack to add a VCF, patch the VCO into it and it into the VCA, and you have built CLASSIC's front half yourself." }, presetInit },
    { { "BASS",         "legato mono, sub oscillator, short filter thump", 0,
      "BASS\n\nLegato mono with a sine an octave below the saw, and a filter envelope short enough to be a thump rather than a sweep.\n\nTRY: play two overlapping notes - it slides, because MODE is LEGATO and GLIDE is up. Raise the filter envelope's D to hear the thump become a sweep." }, presetBass },
    { { "SQUARE LEAD",  "pulse width on an LFO, glide and echo", 0,
      "SQUARE LEAD\n\nA pulse whose width the LFO keeps moving, which is what stops a square wave sounding like a test tone.\n\nTRY: set the VCO's PWM to zero. The movement stops dead and the sound goes flat - that one knob is most of what you were hearing." },      presetSquareLead },
    { { "SAW PAD",      "eight voices, two detuned saws, long reverb", 0,
      "SAW PAD\n\nEight voices, two saws pulled a few cents apart, and enough reverb to lose the edges. The slow attack is on both envelopes.\n\nTRY: hold a chord and turn VCF CV2 - that is the LFO on the cutoff, and it is what keeps a long note from standing still. The PAN before the reverb gives each of the eight voices its own place rather than stacking them in the middle. On a pad this wet the reverb is already doing most of the widening, so the difference is small here - PAN earns its keep on a dry patch, where the whole chord otherwise arrives from one point." }, presetPad },
    { { "PIANO",        "struck string - decays while held, brightness first", 0,
      "PIANO\n\nSubtractive synthesis cannot make a real piano: a struck string's partials are stretched sharp of the harmonic series and nothing here can be inharmonic. What it can copy is the three things the ear actually identifies a piano by, none of which are the waveform.\n\nOne: it decays while you hold it. Two: the brightness dies far faster than the loudness, so a note a second old is nearly a sine. Three: both scale with how hard you hit it and how high you play.\n\nSo the sound is in three envelopes, not the oscillators. Two saws a few cents apart are the strings, the pulse is the hollow midrange, and the 22 ms noise burst is the hammer.\n\nTRY: pull the NOISE mixer channel (IN4) to zero. The hammer disappears and it turns into an organ - that one twentieth of a second is most of the instrument. Then put it back and raise the amplifier envelope's S: it stops being a piano the moment it stops decaying." }, presetPiano },
    { { "BOWED",        "the same string, bowed instead of struck", 0,
      "BOWED\n\nThe same waveguide as GRAND, driven continuously instead of struck. Nothing about the string changed - only what is putting energy into it, which is the point of modelling a string rather than an instrument.\n\nA bow works by friction. The hair sticks to the string and drags it along, the string's tension pulls it back, the hair loses grip and slips, then catches again - hundreds of times a second, at the string's own frequency. That alternation is what sustains the note. It is also why the useful range of bow pressure is narrow: too little and the hair never grips, too much and it never slips, and both of those are silence rather than a quieter note.\n\nThe gate goes to TRIG rather than the trigger pulse. A hammer is an event and a bow is not - the string is driven for as long as the key is held, and how far the gate sits is how hard the hair presses.\n\nTRY: hold a note and turn FORCE down slowly. It does not fade - it stops, and the last thing you hear before it does is the scrape of the hair failing to catch. Then set EXCITE to HAMMER without changing anything else and play the same note: identical string, and now it is a harpsichord.\n\nAlso: DECAY is much shorter here than on GRAND. A struck string has to ring for seven seconds on its own; a bowed one only has to hold its shape between one slip and the next, and a long decay just makes it woolly." }, presetBowed },
    { { "GRAND",        "a modelled string - stretched partials, felt hammer", 0,
      "GRAND\n\nA piano the way a piano works, rather than an imitation of one.\n\nPIANO next door is subtractive, and gets remarkably close on gesture: it decays while held, its brightness dies faster than its loudness, and both scale with velocity. What it cannot do is INHARMONICITY. A real string is stiff, so wave speed depends on frequency and the partials are stretched sharp of the harmonic series - f(n) = n f0 sqrt(1 + B n^2). Every oscillator in this rack is exactly harmonic and no filter introduces stretch. That is the whole difference.\n\nThis is a modelled string: a delay line whose length is the pitch, with allpasses in the loop making the delay depend on frequency. That is the stiffness. The hammer is a smooth force pulse a few milliseconds long, and its length is the point: a partial whose period is shorter than the contact gets almost no energy, so contact time is a lowpass. Hard playing compresses the felt and shortens it, which is why loud is also bright. An earlier version used a noise burst instead and the whole thing sounded like a guitar - 62 percent of the energy was landing between the partials rather than on them.\n\nTRY: turn INHARM to 0 and play. It becomes a guitar harmonic - the partials are where the maths says they should be and it sounds wrong for a piano. Put it back to 0.5, which measures at B = 1.2e-4, about a real piano midrange. Then take it to 1.0 for a bell.\n\nAlso: DECAY is how long the note rings in seconds rather than an arbitrary number - the two strings per note are given different decay times, which is what produces a piano's double decay, a quick fall then a quiet aftersound underneath. STRIKE is where along the string the hammer lands, and a string cannot sound a partial with a node there. SPREAD is how far apart the two strings are, in cents." }, presetGrand },
    { { "WEST COAST",   "a sine, folded - no filter anywhere", 0,
      "WEST COAST\n\nThe other tradition. Subtractive synthesis starts with a rich wave and removes parts of it; this starts with a SINE - the poorest wave there is - and makes it complicated by folding it back on itself every time it passes a limit.\n\nThere is no filter in this rack at all. The envelope drives the FOLD's depth instead of a cutoff, so playing harder does not open a filter, it folds more times. The harmonics that appear are a different set each fold, which is why the movement does not sound like a sweep.\n\nTRY: turn FOLD's GAIN down to 1.00. It collapses back to the sine it always was. Then raise the LFO rate and hear the tremolo the VCA is doing underneath." }, presetWestCoast },
    { { "ORACLE",       "sequenced, quantised, and plays itself", 1,
      "ORACLE\n\nNothing is held down and nothing needs to be. A CLK drives a SEQ, the SEQ's voltages are rounded by a QUANT to A minor pentatonic - which is what stops a sequence of arbitrary voltages being a sequence of arbitrary notes - and a SLEW takes the corners off so the pitch glides rather than jumps.\n\nThe rhythm is made rather than programmed: two divisions of the same clock go into a LOGIC, and the XOR of /2 and /4 fires on every beat where exactly one of them is high. That is a pattern neither input contains.\n\nTRY: change QUANT's SCALE. The same eight voltages become a different piece of music. Then turn the CLK's RUN off and on to hear where it restarts." }, presetOracle },
    { { "BELL",         "the same string, wound until it rings", 0,
      "BELL\n\nGRAND and this are the same module. The only real difference is INHARM at 1.00 instead of 0.50.\n\nAt a piano's stiffness the partials are stretched a few cents and the ear still hears a pitch with overtones. Push it further and the stretch is so large that the partials stop forming a harmonic series at all - and a sound with inharmonic partials that ring for a long time is what a struck metal bar is. It stops being a string and becomes a bell without changing a single cable.\n\nTRY: sweep INHARM slowly from 0 to 1 while playing. Somewhere around 0.5 it is a piano and around 0.8 it stops being one." }, presetBell },
    { { "ACID SEQ",     "the same idea, sequenced and playing itself", 1,
      "ACID SEQ\n\nACID next door is this played from the keyboard. This one plays itself, and three things made the idea into a genre: a sequence, a glide between its notes, and a filter with more resonance than is sensible.\n\nThe SEQ's GLIDE knob is the one that matters. At zero this is a bass line. Turned up, consecutive steps slide into one another and it becomes the thing everyone recognises - the slide is not an ornament, it is the instrument.\n\nTRY: turn VCF RES down to 0.3 and it is suddenly a polite bass. Put it back and lower CUTOFF instead - the resonant peak sweeping through the harmonics IS the melody, more than the notes are." }, presetAcidSeq },
    { { "KRELL",        "clocks itself, and never repeats", 1,
      "KRELL\n\nA patch that plays itself, from an idea older than most modular systems.\n\nA FUNC in CYCLE mode is an LFO. Its end-of-cycle pulse triggers the note AND clocks a sample-and-hold - and the voltage that S&H grabs is patched back into the FUNC's own rise time. So how long the next cycle takes is decided by a random voltage taken at the end of the last one. The rhythm generates itself and cannot settle into a loop.\n\nThe same voltage sets the pitch, through a QUANT so it lands on notes.\n\nTRY: unpatch the cable from NOISE's S&H to the FUNC's IN. The rhythm goes metronomic instantly, and you can hear exactly how much that one cable was doing." }, presetKrell },
    { { "VOWEL",        "two bandpasses in parallel are a mouth", 0,
      "VOWEL\n\nA vowel is two resonant peaks at particular frequencies. Put a saw through two bandpass filters at once, tuned to around 700 Hz and 1150 Hz, and the result is roughly an 'ah'.\n\nThis is what the SVF is for and the ladder cannot do: the ladder is a lowpass and only a lowpass, and no amount of it makes a peak in the middle of the spectrum with the rest passing through underneath.\n\nAn LFO slides the first formant while the second stays where it is. Both moving together is a wah; only one moving is a mouth changing shape.\n\nTRY: set SVF 2's CUTOFF to 2400 and it becomes 'ee'. Around 1000 with the first at 400 is 'oo'." }, presetVowel },
    { { "SWITCHBOARD",  "four sources, one output, a clock choosing", 1,
      "SWITCHBOARD\n\nRouting as a rhythmic act. A SWITCH has four inputs and one output, and the clock decides which one is connected - so a saw, a pulse, pink noise and a struck string take turns being the instrument, one beat each.\n\nThe string is not struck every time. Its trigger comes from the AND of two clock divisions, /2 and /8, which is only true on the beat they share - so it lands once every eight rather than on every step, and the pattern has a shape nobody programmed.\n\nTRY: change SWITCH's STEPS from 4 to 3 and the cycle stops lining up with the bar. Then turn CRUSH's MIX down to hear how much of the grit is coming from there." }, presetSwitchboard },
    { { "PLUCK",        "no sustain, resonant filter chirp", 0,
      "PLUCK\n\nNo sustain at all on either envelope, so a held key still decays to nothing. The filter envelope is short and the resonance is high, which puts a chirp on the front of every note.\n\nTRY: raise the amplifier envelope's S and it stops being a pluck immediately." },          presetPluck },
    { { "DRONE",        "the filter is the oscillator - no keys needed", 1,
      "DRONE\n\nNo keyboard anywhere. The filter's RES is past the point of self-oscillation, so the filter IS the oscillator; two slow LFOs walk its cutoff and pink noise gives it something to catch on.\n\nTRY: turn VCF RES down. The sound stops - that is what resonance is." }, presetDrone },
    { { "WIND",         "noise through a resonant filter, no oscillator", 0,
      "WIND\n\nNoise through a resonant filter and no oscillator anywhere. The keyboard opens the amplifier and tracks the cutoff, so it is playable even though there is no pitch in it.\n\nTRY: raise VCF RES further and it starts to whistle a note out of the noise." }, presetWind },
    { { "SUPERSAW",     "three detuned saws - the trance lead", 0,
      "SUPERSAW\n\nThree saws a few cents apart. The beating between them is the entire sound.\n\nTRY: set all three FINE knobs to 0. It collapses into one thin oscillator, and you can hear exactly what the detuning was buying." },       presetSupersaw },
    { { "ACID",         "303: one saw, glide, and a screaming filter", 0,
      "ACID\n\nA 303 in the parts that matter: one saw, glide between overlapping notes, and resonance high enough that the filter envelope is the melody.\n\nTRY: play detached and then legato - the glide only happens on the second. Sweep VCF CUTOFF while playing." }, presetAcid },
    { { "HOOVER",       "the rave stab - detuned way past comfortable", 0,
      "HOOVER\n\nA saw and a pulse detuned a third of a semitone apart - far enough that a tuner would call both of them wrong - with the pulse width swept underneath.\n\nTRY: pull the two FINE knobs back toward zero and watch the rave drain out of it." }, presetHoover },
    { { "REESE",        "two saws a hair apart, mono, low and moving", 0,
      "REESE\n\nTwo saws a hair apart, mono and low. The interference between them moves slowly enough to hear as a sweep rather than as a chord.\n\nTRY: set KBD VOICES to 8 and MODE to POLY. It turns to mud - this one only works in mono, and that is the point." }, presetReese },
    { { "TRANCE GATE",  "a pad chopped into sixteenths by a square LFO", 0,
      "TRANCE GATE\n\nA pad, and then a SECOND amplifier that a square LFO opens and shuts. Hold a chord and the rhythm is the LFO's, not yours.\n\nTRY: turn the LFO's RATE. That is the tempo. Turn the gate VCA's RESP to EXP and the edges soften." }, presetTranceGate },
    { { "ORGAN STAB",   "house chord stab, pulse with a saw on top", 0,
      "ORGAN STAB\n\nA pulse with a saw an octave above it, and envelopes short enough that a held chord still sounds hit rather than pressed.\n\nTRY: raise both envelopes' S. It stops being a stab and becomes an organ, which is the same rack with two knobs moved." },   presetOrganStab },

    { { "CHORD",        "three oscillators a third and a fifth apart", 0,
      "CHORD\n\nThree oscillators, the second a major third up and the third a fifth up, so one key plays a triad.\n\nTRY: set the second VCO's COARSE to +3 instead of +4. The whole rack turns minor. That is the cheapest music theory lesson here." },  presetChord },
    { { "OCTAVES",      "four oscillators an octave apart, mixed like drawbars", 0,
      "OCTAVES\n\nFour oscillators an octave apart, mixed like organ drawbars. The mixer levels are the registration.\n\nTRY: pull levels 3 and 4 to zero and it goes dark; push them up and it gets reedy. No filter is involved - this is additive synthesis in a modular's clothes." }, presetOctaves },
    { { "HARD SYNC",    "one oscillator restarting another, swept", 0,
      "HARD SYNC\n\nOne oscillator restarting another. The slave's own pitch is swept by an envelope, but it can never finish a cycle before the master resets it, so what changes is the shape of each period rather than the note.\n\nTRY: turn the second VCO's COARSE. No filter can make this sound." },     presetHardSync },
    { { "RING MOD",     "gain allowed to go negative - sum and difference", 0,
      "RING MOD\n\nA VCA with its RESP switch on RING - four quadrants, so the gain follows the control voltage through zero and out the other side. Two oscillators multiplied together give their sum and difference and neither original.\n\nTRY: turn the modulator's COARSE. Bell, gong, scrap metal." }, presetRingMod },
    { { "FM BELL",      "one sine bending another's pitch, on an envelope", 0,
      "FM BELL\n\nOne sine bending another's pitch, with an envelope on how much. Deep at the start and clean by the end is what makes a struck sound.\n\nNote the modulation is exponential rather than through-zero, so the ratios are not tidy integers - which is why this lands nearer a gamelan than an electric piano." }, presetFmBell },
    { { "SAMPLE HOLD",  "random voltages on a clock - plays itself", 1,
      "SAMPLE HOLD\n\nPlays itself. A square LFO clocks the sample-and-hold, which grabs a new random voltage every tick; the attenuverter squeezes that into about an octave and it becomes the pitch. The same clock opens the envelope.\n\nTRY: the LFO's RATE is the tempo, the ATT's AMT is how wide the tune is." },    presetSampleHold },
    { { "DUB SIREN",    "a mixer summing pitch and wobble, into a long echo", 0,
      "DUB SIREN\n\nAn input jack takes one cable, so adding a wobble to a pitch is not a matter of plugging in twice - the keyboard's pitch and the LFO are summed in a MIX first, and the sum drives the oscillator.\n\nTRY: raise MIX level 2 past 0.1 and it stops being a siren and starts being a mistake." }, presetDubSiren },
    { { "WOBBLE",       "the cutoff on a fast LFO, resonance up", 0,
      "WOBBLE\n\nThe cutoff on a fast LFO, deep, with the resonance high enough that the sweep whistles.\n\nThis is the same modulation TRANCE GATE puts on an amplifier, moved one module to the left - and that is the whole difference between a rhythm and a voice." },       presetWobble },
    { { "PWM STRINGS",  "two pulses on two LFOs that never line up", 0,
      "PWM STRINGS\n\nTwo pulse waves whose widths are moved by two LFOs at CLOSE BUT UNEQUAL rates, so the pair never lines up and drifts in and out of phase forever.\n\nTRY: set both LFO rates to the same number. The drift stops and so does the string machine." },    presetPwmStrings },
    { { "KICK",         "an envelope on the pitch and no filter at all", 0,
      "KICK\n\nAn envelope on the pitch and no filter anywhere. Fifty milliseconds, three octaves down, and an amplitude envelope that outlives it.\n\nTRY: raise the pitch envelope's D toward 0.5 and the kick becomes a falling whistle. An envelope patched somewhere unusual stops being an envelope and becomes a gesture." }, presetKick },
    { { "SNARE",        "noise, a resonant filter, and a very short envelope", 0,
      "SNARE\n\nNoise, a filter with the resonance most of the way up, and an envelope short enough to be a hit. There is no oscillator.\n\nTRY: VCF CUTOFF is the pitch of it. Drop it and the snare becomes a tom; raise it and it becomes a hat." }, presetSnare },
    { { "HOWL",         "a filter inside a delay's feedback - plays itself", 1,
      "HOWL\n\nA resonant filter inside a delay's feedback path - a loop the patch cannot resolve in one pass, so one cable in it is read a block late, and that block of delay is what keeps it from being an infinite regress.\n\nTRY: MIX level 2 is the loop gain. Below about 0.9 it dies away; above it, it climbs until the filter saturates." }, presetHowl },

    { { "AXEL",         "a bright mono lead of a very particular 1984 vintage", 0,
      "AXEL\n\nA saw and a pulse five cents apart, a filter open enough to be piercing with just enough resonance to whistle, and envelopes fast enough that a staccato line stays staccato.\n\nMono and retriggering rather than legato - the line is played detached and every note wants its own attack. The slapback echo is doing as much period work as the oscillators are.\n\nThe original was a Jupiter-8, which is two oscillators and a filter, so this is the same arrangement rather than an impression of one.\n\nTRY: turn DLY MIX down and most of the decade goes with it. Then put VCF RES up to 0.7 for the version that hurts." },
      presetAxel },

    { { "GRAND TOUR",   "every module in the set, patched into one instrument", 0,
      "GRAND TOUR\n\nEvery module type in the rack, working at once.\n\nThe signal path is ordinary: three oscillators through a mixer into the ladder, two envelopes, a delay and a reverb. What is not ordinary is everything driving it.\n\nThe keyboard never reaches the oscillators. It reaches an ARP, and the arpeggiator's clock is what plays the rack. That same clock is sent through a MULT to both envelopes AND to the noise module's sample-and-hold, so every arpeggio step also grabs a new random voltage - which an ATT scales down to something musical and sends to the filter's second CV input.\n\nA second LFO bends the delay's TIME, which is why the echoes wow like tape.\n\nTRY: hold a chord and change the ARP's MODE and OCT. Then turn the ATT's AMT 1 to zero and hear the filter stop jumping.\n\nTHE OTHER HALF: the bottom two rows are a second instrument that shares nothing with this one but the output. Turn the CLK's RUN on. A sequence steps, a QUANT rounds it to A minor pentatonic, a SLEW rounds the corners off that, and it strikes a STRING - a real one, modelled, with partials stretched sharp the way a piano's are. From there a SWITCH picks between four sources on a rhythm two clock divisions make by disagreeing through a LOGIC, and what comes out is folded, crushed and run through a CHORUS.\n\nTRY: with the clock running, turn the STRING's INHARM from 0 to 1. At the bottom it is a guitar harmonic; halfway it is a piano; at the top it is a bell. That one knob is the difference, and no filter can imitate it." },
      presetGrandTour }
};

} /* anonymous namespace */

int rackPresetCount() { return (int)(sizeof PRESETS / sizeof PRESETS[0]); }

const RackPreset *rackPresetAt(int i)
{
    return (i >= 0 && i < rackPresetCount()) ? &PRESETS[i].info : 0;
}

/* Every rack gets a scope across whatever reaches its output stage.
 *
 * Added here rather than in each preset so it cannot be forgotten, and so a
 * new rack gets one for free. It reads the cables into the output module and
 * taps the same sources, which means it shows exactly the two channels that
 * are being sent to the sound card - not a guess at where the interesting
 * signal is. A rack whose right channel is unpatched has both traces on the
 * left, which is what the output stage does with it anyway.
 *
 * It goes to the right of the output module, so it is the last thing in the
 * chain on screen as well as in the signal. */
static int addOutputScope(Engine *e)
{
    Module *out = 0;
    for (int i = 0; i < e->patch.slotCount(); i++) {
        Module *m = e->patch.module(i);
        if (m && m->typeId == "OUT") { out = m; break; }
    }
    if (!out) return -1;

    int srcL = -1, portL = -1, srcR = -1, portR = -1;
    const std::vector<Cable> &cs = e->patch.cableList();
    for (size_t i = 0; i < cs.size(); i++) {
        if (!cs[i].alive || cs[i].dst != out->id) continue;
        if (cs[i].dstPort == 0) { srcL = cs[i].src; portL = cs[i].srcPort; }
        if (cs[i].dstPort == 1) { srcR = cs[i].src; portR = cs[i].srcPort; }
    }
    if (srcL < 0) return -1;                    /* nothing reaches the output */
    if (srcR < 0) { srcR = srcL; portR = portL; }

    const int sc = e->addModule("SCOPE",
                                out->x + (float)panelWidth(*out) + 14.0f, out->y);
    if (sc < 0) return -1;
    e->connect(srcL, portL, sc, 0);
    e->connect(srcR, portR, sc, 1);
    return sc;
}

/* The rack's own explanation, in a panel at the end of the row. Placed after
 * the scope so the two things you read rather than hear sit together. */
static void addNotes(Engine *e, const char *notes, int afterId)
{
    if (!notes || !*notes) return;

    float x = 20.0f, y = 20.0f;
    const Module *after = e->patch.module(afterId);
    if (after) { x = after->x + (float)panelWidth(*after) + 14.0f; y = after->y; }

    const int t = e->addModule("TEXT", x, y);
    Module *m = e->patch.module(t);
    std::string *buf = m ? m->textBuffer() : 0;
    if (buf) *buf = notes;
}

void Engine::buildPreset(int index)
{
    if (index < 0 || index >= rackPresetCount()) return;
    clear();
    Builder b = { this, 20.0f, 20.0f };
    PRESETS[index].build(b);
    addNotes(this, PRESETS[index].info.notes, addOutputScope(this));
}

void Engine::buildDefaultPatch() { buildPreset(0); }

} /* namespace bs */
