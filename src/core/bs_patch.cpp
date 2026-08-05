#include "bs_patch.h"

namespace bs {

const Signal &zeroSignal()
{
    static const Signal z;
    return z;
}

Patch::Patch() : sr(48000.0f), rev(1), orderDirty(true) {}

Patch::~Patch() { clear(); }

void Patch::clear()
{
    for (size_t i = 0; i < mods.size(); i++) delete mods[i];
    mods.clear();
    cables.clear();
    order.clear();
    orderDirty = true;
    rev++;
}

int Patch::add(Module *m, float x, float y)
{
    if (!m) return -1;

    /* Reuse a hole left by a deleted module, so ids stay dense and a long
     * session of adding and removing does not grow the slot array without
     * bound. Ids are only unique among live modules, which is all a cable
     * needs them to be. */
    int slot = -1;
    for (size_t i = 0; i < mods.size(); i++) if (!mods[i]) { slot = (int)i; break; }
    if (slot < 0) { slot = (int)mods.size(); mods.push_back(0); }

    mods[(size_t)slot] = m;
    m->id = slot;
    m->x  = x;
    m->y  = y;
    m->setSampleRate(sr);

    orderDirty = true;
    rev++;
    return slot;
}

void Patch::removeModule(int moduleId)
{
    Module *m = module(moduleId);
    if (!m) return;

    for (size_t i = 0; i < cables.size(); i++) {
        Cable &c = cables[i];
        if (!c.alive) continue;
        if (c.src == moduleId || c.dst == moduleId) {
            /* Unpoint the far end before the buffer it names goes away. */
            if (c.dst != moduleId) {
                Module *d = module(c.dst);
                if (d) d->ins[(size_t)c.dstPort] = 0;
            }
            c.alive = false;
        }
    }

    mods[(size_t)moduleId] = 0;
    delete m;

    orderDirty = true;
    rev++;
}

int Patch::cableAtInput(int dst, int dstPort) const
{
    for (size_t i = 0; i < cables.size(); i++) {
        const Cable &c = cables[i];
        if (c.alive && c.dst == dst && c.dstPort == dstPort) return c.id;
    }
    return -1;
}

int Patch::connect(int src, int srcPort, int dst, int dstPort)
{
    Module *s = module(src);
    Module *d = module(dst);
    if (!s || !d) return -1;
    if (srcPort < 0 || srcPort >= s->outputCount()) return -1;
    if (dstPort < 0 || dstPort >= d->inputCount())  return -1;

    const int existing = cableAtInput(dst, dstPort);
    if (existing >= 0) disconnect(existing);

    int slot = -1;
    for (size_t i = 0; i < cables.size(); i++) if (!cables[i].alive) { slot = (int)i; break; }
    if (slot < 0) { slot = (int)cables.size(); cables.push_back(Cable()); }

    Cable &c = cables[(size_t)slot];
    c.id      = slot;
    c.src     = src;
    c.srcPort = srcPort;
    c.dst     = dst;
    c.dstPort = dstPort;
    c.alive   = true;
    /* Colour by cable id so a rack repatched the same way twice looks the
     * same, rather than by connection order. */
    c.color   = slot;

    rebind(dst, dstPort);
    orderDirty = true;
    rev++;
    return slot;
}

void Patch::disconnect(int cableId)
{
    Cable *c = cable(cableId);
    if (!c) return;
    const int dst = c->dst, dstPort = c->dstPort;
    c->alive = false;
    rebind(dst, dstPort);
    orderDirty = true;
    rev++;
}

void Patch::rebind(int dst, int dstPort)
{
    Module *d = module(dst);
    if (!d || dstPort < 0 || dstPort >= d->inputCount()) return;

    const int id = cableAtInput(dst, dstPort);
    if (id < 0) { d->ins[(size_t)dstPort] = 0; return; }

    const Cable &c = cables[(size_t)id];
    Module *s = module(c.src);
    d->ins[(size_t)dstPort] = s ? &s->outs[(size_t)c.srcPort] : 0;
}

void Patch::setSampleRate(float rate)
{
    sr = rate;
    for (size_t i = 0; i < mods.size(); i++)
        if (mods[i]) mods[i]->setSampleRate(rate);
}

/* Kahn's algorithm over the module graph, an edge running from the module
 * that produces a signal to the one that reads it.
 *
 * Feedback patches are legal and common - a delay fed back into its own input,
 * a filter self-modulating - so a cycle cannot be an error. What is left over
 * when the sort runs dry is appended in id order, which means one of the
 * cables in each cycle is read a block late. That is a fixed delay of
 * BS_BLOCK samples, under a millisecond, and it is the same compromise a
 * hardware patch makes with the propagation time of the cable itself. */
void Patch::rebuildOrder()
{
    const int n = (int)mods.size();
    order.clear();
    order.reserve((size_t)n);

    std::vector<int> indeg((size_t)n, 0);
    for (size_t i = 0; i < cables.size(); i++) {
        const Cable &c = cables[i];
        if (!c.alive) continue;
        if (c.src == c.dst) continue;          /* self-patch is a pure cycle */
        if (!module(c.src) || !module(c.dst)) continue;
        indeg[(size_t)c.dst]++;
    }

    std::vector<int> queue;
    for (int i = 0; i < n; i++)
        if (mods[(size_t)i] && indeg[(size_t)i] == 0) queue.push_back(i);

    std::vector<char> done((size_t)n, 0);
    size_t head = 0;
    while (head < queue.size()) {
        const int m = queue[head++];
        order.push_back(m);
        done[(size_t)m] = 1;
        for (size_t i = 0; i < cables.size(); i++) {
            const Cable &c = cables[i];
            if (!c.alive || c.src != m || c.src == c.dst) continue;
            if (!module(c.dst) || done[(size_t)c.dst]) continue;
            if (--indeg[(size_t)c.dst] == 0) queue.push_back(c.dst);
        }
    }

    for (int i = 0; i < n; i++)
        if (mods[(size_t)i] && !done[(size_t)i]) order.push_back(i);

    orderDirty = false;
}

void Patch::process()
{
    if (orderDirty) rebuildOrder();
    for (size_t i = 0; i < order.size(); i++) {
        Module *m = mods[(size_t)order[i]];
        if (m) m->process();
    }
}

} /* namespace bs */
