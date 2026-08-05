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

void Engine::noteOn(int note, float velocity)  { keys.noteOn(note, velocity); }
void Engine::noteOff(int note)                 { keys.noteOff(note); }
void Engine::setBend(float b)                  { keys.bend = clampf(b, -1.0f, 1.0f); }
void Engine::setMod(float m)                   { keys.mod  = clampf(m, 0.0f, 1.0f); }
void Engine::setSustain(bool on)               { keys.setSustain(on); }

void Engine::panic()
{
    std::lock_guard<std::mutex> g(mutex);
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
                patch.process();
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

/* ------------------------------------------------------------------ *
 * The patch the window opens on
 * ------------------------------------------------------------------ */

static void setP(Module *m, int idx, float v)
{
    if (m && idx >= 0 && idx < m->paramCount()) m->params[(size_t)idx].value = v;
}

void Engine::buildDefaultPatch()
{
    clear();

    const float GAP = 14.0f;
    const float R1  = 20.0f;
    const float R2  = 375.0f;

    float x = 20.0f;
    struct Placer {
        Engine *e; float *x; float y; float gap;
        int put(const char *t)
        {
            const int id = e->addModule(t, *x, y);
            Module *m = e->patch.module(id);
            if (m) *x += (float)panelWidth(*m) + gap;
            return id;
        }
    };

    Placer row = { this, &x, R1, GAP };
    const int kbd   = row.put("KBD");
    const int vco1  = row.put("VCO");
    const int vco2  = row.put("VCO");
    const int noise = row.put("NOISE");
    const int mix   = row.put("MIX");
    const int vcf   = row.put("VCF");
    const int envF  = row.put("ADSR");
    const int envA  = row.put("ADSR");
    const int vca   = row.put("VCA");

    /* The LFO sits under the oscillators it modulates and the tail of the
     * chain sits under the VCA it comes out of, so the long cables in the
     * default rack are short ones. A patch laid out by signal flow is also a
     * patch you can read, and the first thing anyone does with this window is
     * try to work out what is plugged into what. */
    x = 20.0f;
    Placer left = { this, &x, R2, GAP };
    const int lfo = left.put("LFO");

    x = 640.0f;
    Placer row2 = { this, &x, R2, GAP };
    const int scope = row2.put("SCOPE");
    const int dly   = row2.put("DLY");
    const int rvb   = row2.put("RVB");
    const int out   = row2.put("OUT");

    /* Two oscillators through a mixer into the ladder, one envelope on the
     * cutoff and one on the amplifier: the patch every subtractive synth is a
     * hard-wired version of. */
    connect(kbd, 0, vco1, 0);
    connect(kbd, 0, vco2, 0);
    connect(kbd, 1, envF, 0);
    connect(kbd, 1, envA, 0);
    connect(kbd, 2, envF, 1);
    connect(kbd, 2, envA, 1);
    connect(kbd, 3, envA, 2);

    connect(vco1, 0, mix, 0);      /* saw   */
    connect(vco2, 1, mix, 1);      /* pulse */
    connect(noise, 0, mix, 2);
    connect(lfo, 1, vco2, 2);      /* triangle into pulse width */

    connect(mix, 0, vcf, 0);
    connect(envF, 0, vcf, 1);
    connect(kbd, 0, vcf, 3);       /* keyboard tracking */

    connect(vcf, 0, vca, 0);
    connect(envA, 0, vca, 1);

    connect(vca, 0, dly, 0);
    connect(dly, 0, rvb, 0);
    connect(rvb, 0, out, 0);
    connect(rvb, 1, out, 1);

    connect(vca, 0, scope, 0);
    connect(lfo, 1, scope, 1);

    Module *m;
    if ((m = patch.module(kbd)))   { setP(m, 3, 6.0f); }
    if ((m = patch.module(vco2)))  { setP(m, 2, 7.0f); setP(m, 5, 0.35f); }
    if ((m = patch.module(noise))) { setP(m, 0, 1.0f); }
    if ((m = patch.module(mix)))   { setP(m, 0, 0.70f); setP(m, 1, 0.55f);
                                     setP(m, 2, 0.05f); setP(m, 4, 1.0f); }
    if ((m = patch.module(vcf)))   { setP(m, 0, 420.0f); setP(m, 1, 0.36f);
                                     setP(m, 2, 1.6f);   setP(m, 3, 0.28f);
                                     setP(m, 5, 0.35f); }
    if ((m = patch.module(envF)))  { setP(m, 0, 0.004f); setP(m, 1, 0.35f);
                                     setP(m, 2, 0.22f);  setP(m, 3, 0.40f); }
    if ((m = patch.module(envA)))  { setP(m, 0, 0.006f); setP(m, 1, 0.60f);
                                     setP(m, 2, 0.75f);  setP(m, 3, 0.35f);
                                     setP(m, 4, 0.55f); }
    if ((m = patch.module(vca)))   { setP(m, 0, 0.0f);  setP(m, 1, 1.0f);
                                     setP(m, 2, 1.0f); }
    if ((m = patch.module(lfo)))   { setP(m, 0, 0.35f); }
    if ((m = patch.module(dly)))   { setP(m, 0, 0.28f); setP(m, 1, 0.30f);
                                     setP(m, 3, 0.20f); }
    if ((m = patch.module(rvb)))   { setP(m, 0, 0.55f); setP(m, 1, 0.40f);
                                     setP(m, 2, 0.22f); }
    if ((m = patch.module(out)))   { setP(m, 0, 0.60f); }
}

} /* namespace bs */
