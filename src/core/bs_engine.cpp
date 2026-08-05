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

void Engine::setMod(float m, int atFrame)
{
    events.push(ev(NE_MOD, 0, clampf(m, 0.0f, 1.0f), atFrame));
}

void Engine::setSustain(bool on, int atFrame)
{
    events.push(ev(NE_SUSTAIN, 0, on ? 1.0f : 0.0f, atFrame));
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
    b.wire(rvb, 0, out, 0);
    b.wire(rvb, 1, out, 1);

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
    b.set(out,  OUT_LEVEL, 0.55f);
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
      "SAW PAD\n\nEight voices, two saws pulled a few cents apart, and enough reverb to lose the edges. The slow attack is on both envelopes.\n\nTRY: hold a chord and turn VCF CV2 - that is the LFO on the cutoff, and it is what keeps a long note from standing still." }, presetPad },
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

    { { "GRAND TOUR",   "every module in the set, patched into one instrument", 0,
      "GRAND TOUR\n\nEvery module type in the rack, working at once.\n\nThe signal path is ordinary: three oscillators through a mixer into the ladder, two envelopes, a delay and a reverb. What is not ordinary is everything driving it.\n\nThe keyboard never reaches the oscillators. It reaches an ARP, and the arpeggiator's clock is what plays the rack. That same clock is sent through a MULT to both envelopes AND to the noise module's sample-and-hold, so every arpeggio step also grabs a new random voltage - which an ATT scales down to something musical and sends to the filter's second CV input.\n\nA second LFO bends the delay's TIME, which is why the echoes wow like tape.\n\nTRY: hold a chord and change the ARP's MODE and OCT. Then turn the ATT's AMT 1 to zero and hear the filter stop jumping." },
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
