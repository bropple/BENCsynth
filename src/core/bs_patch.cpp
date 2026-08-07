#include "bs_patch.h"

namespace bs {

const Signal &zeroSignal()
{
    static const Signal z;
    return z;
}

Patch::Patch() : replaced(0), viewX(0.0f), viewY(0.0f), viewZoom(0.0f),
                 sr(48000.0f), rev(1), orderDirty(true)
{
    for (int i = 0; i < BS_EXPOSED; i++) { exposed[i].module = -1; exposed[i].param = -1; }
}

int Patch::exposedSlotOf(int module, int param) const
{
    for (int i = 0; i < BS_EXPOSED; i++)
        if (exposed[i].module == module && exposed[i].param == param) return i;
    return -1;
}

int Patch::expose(int module, int param)
{
    const int already = exposedSlotOf(module, param);
    if (already >= 0) return already;
    for (int i = 0; i < BS_EXPOSED; i++)
        if (exposed[i].module < 0) {
            exposed[i].module = module;
            exposed[i].param  = param;
            rev++;
            return i;
        }
    return -1;
}

void Patch::unexpose(int slot)
{
    if (slot < 0 || slot >= BS_EXPOSED) return;
    exposed[slot].module = -1;
    exposed[slot].param  = -1;
    rev++;
}

void Patch::unexposeModule(int module)
{
    for (int i = 0; i < BS_EXPOSED; i++)
        if (exposed[i].module == module) unexpose(i);
}

Patch::~Patch() { clear(); }

void Patch::clear()
{
    for (size_t i = 0; i < mods.size(); i++) delete mods[i];
    mods.clear();
    cables.clear();
    replaced = 0;
    viewX = viewY = 0.0f;
    viewZoom = 0.0f;
    for (int i = 0; i < BS_EXPOSED; i++) { exposed[i].module = -1; exposed[i].param = -1; }
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

    rebuildOrder();
    rev++;
    return slot;
}

void Patch::removeModule(int moduleId)
{
    Module *m = module(moduleId);
    if (!m) return;

    /* Its exposed knobs go with it. A slot left pointing at a dead id would
     * silently attach itself to whatever module is created into that slot
     * next, and the host would go on automating a parameter that has quietly
     * become a different one. */
    unexposeModule(moduleId);

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

    rebuildOrder();
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

    /* An input takes one cable, so a second replaces the first - which is
     * right at a patch bay and is what a person expects when they drop a plug
     * into an occupied jack. It is never what a preset means, though: a rack
     * built in code with two wires into one input silently loses one of them.
     * GRAND shipped that way and played only its first note. Counted here so a
     * test can insist that no preset does it. */
    const int existing = cableAtInput(dst, dstPort);
    if (existing >= 0) { disconnect(existing); replaced++; }

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
    rebuildOrder();
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
    rebuildOrder();
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

    /* Edges are collected into a compressed adjacency list first: `edgeStart`
     * indexes into `edgeTo`, so the successors of module m are the slice
     * [edgeStart[m], edgeStart[m+1]).
     *
     * The obvious version - rescanning every cable for each module dequeued -
     * is O(V*E), which is free at fifteen modules and quadratic at three
     * hundred. Building the index costs two passes over the cables and makes
     * the sort O(V+E), which is what it should have been. */
    std::vector<int> indeg((size_t)n, 0);
    std::vector<int> outdeg((size_t)n, 0);

    int edges = 0;
    for (size_t i = 0; i < cables.size(); i++) {
        const Cable &c = cables[i];
        if (!c.alive) continue;
        if (c.src == c.dst) continue;          /* self-patch is a pure cycle */
        if (!module(c.src) || !module(c.dst)) continue;
        indeg[(size_t)c.dst]++;
        outdeg[(size_t)c.src]++;
        edges++;
    }

    edgeStart.assign((size_t)n + 1, 0);
    for (int i = 0; i < n; i++) edgeStart[(size_t)i + 1] = edgeStart[(size_t)i] + outdeg[(size_t)i];

    edgeTo.assign((size_t)edges, 0);
    std::vector<int> fill(edgeStart.begin(), edgeStart.end() - 1);
    for (size_t i = 0; i < cables.size(); i++) {
        const Cable &c = cables[i];
        if (!c.alive || c.src == c.dst) continue;
        if (!module(c.src) || !module(c.dst)) continue;
        edgeTo[(size_t)fill[(size_t)c.src]++] = c.dst;
    }

    /* Kahn's algorithm. `order` doubles as the queue - a node is appended when
     * its last dependency is satisfied, and read back in the same order. */
    for (int i = 0; i < n; i++)
        if (mods[(size_t)i] && indeg[(size_t)i] == 0) order.push_back(i);

    std::vector<char> done((size_t)n, 0);
    size_t head = 0;
    while (head < order.size()) {
        const int m = order[head++];
        done[(size_t)m] = 1;
        for (int e = edgeStart[(size_t)m]; e < edgeStart[(size_t)m + 1]; e++) {
            const int d = edgeTo[(size_t)e];
            if (done[(size_t)d]) continue;
            if (--indeg[(size_t)d] == 0) order.push_back(d);
        }
    }

    /* Whatever the sort could not place is in a cycle. Appended in id order,
     * which makes one cable per cycle read the previous block's buffer. */
    for (int i = 0; i < n; i++)
        if (mods[(size_t)i] && !done[(size_t)i]) order.push_back(i);

    orderDirty = false;
}

void Patch::process()
{
    /* Every mutator rebuilds the order as it goes, so this never fires. It is
     * kept because the alternative - rebuilding lazily, here - puts three
     * vector allocations on the audio thread on the first block after any
     * edit, which is exactly the block that must not be late. */
    if (orderDirty) rebuildOrder();
    for (size_t i = 0; i < order.size(); i++) {
        Module *m = mods[(size_t)order[i]];
        if (m) m->process();
    }
}

} /* namespace bs */
