/*
 * BENCsynth - the rack
 *
 * A patch is a set of modules, a set of cables, and the order to run them in.
 *
 * Cables are not copied through: connecting an output to an input points the
 * input at the output's buffer. Nothing is moved per block, an unpatched jack
 * costs nothing, and a cable is exactly what it looks like on screen - a
 * declaration that this jack reads that one.
 */

#ifndef BS_PATCH_H
#define BS_PATCH_H

#include "bs_module.h"

namespace bs {

struct Cable {
    int  id;
    int  src,  srcPort;    /* an output jack */
    int  dst,  dstPort;    /* an input jack  */
    int  color;            /* index into the cable palette, purely cosmetic */
    bool alive;
};

class Patch {
public:
    Patch();
    ~Patch();

    /* Takes ownership. Returns the module id, which is also its slot. */
    int  add(Module *m, float x, float y);
    void removeModule(int moduleId);
    void clear();

    Module *module(int id) const
    {
        return (id >= 0 && id < (int)mods.size()) ? mods[(size_t)id] : 0;
    }
    int slotCount() const { return (int)mods.size(); }

    /* An input jack takes one cable. Connecting a second replaces the first,
     * which is how a patch bay behaves and removes a whole class of "why is
     * nothing coming out" from the interaction. Outputs fan out freely.
     * Returns the new cable id, or -1 if the request made no sense. */
    int  connect(int src, int srcPort, int dst, int dstPort);
    void disconnect(int cableId);
    int  cableAtInput(int dst, int dstPort) const;

    const std::vector<Cable> &cableList() const { return cables; }
    Cable *cable(int id)
    {
        return (id >= 0 && id < (int)cables.size() && cables[(size_t)id].alive)
             ? &cables[(size_t)id] : 0;
    }

    void setSampleRate(float rate);
    float sampleRate() const { return sr; }

    /* Runs every module once, in dependency order. */
    void process();

    /* Bumped by anything that changes the shape of the graph, so callers that
     * cache derived structure - the engine's list of output modules, the
     * GUI's cable geometry - can notice without being told. */
    unsigned revision() const { return rev; }

    /* The nth module in evaluation order, or -1 past the end. Exposed for the
     * test that checks the sort actually sorts; nothing in the program needs
     * to know what order it runs in. */
    int evalOrderAt(int n) const
    {
        return (n >= 0 && n < (int)order.size()) ? order[(size_t)n] : -1;
    }

private:
    void rebuildOrder();
    void rebind(int dst, int dstPort);

    std::vector<Module *> mods;      /* removed slots are null    */
    std::vector<Cable>    cables;    /* removed slots are !alive  */
    std::vector<int>      order;     /* module ids, evaluation order */

    /* Compressed adjacency for the sort, kept between rebuilds only so the
     * allocation is reused rather than made afresh on every edit. */
    std::vector<int>      edgeStart, edgeTo;
    float    sr;
    unsigned rev;
    bool     orderDirty;

    Patch(const Patch &);
    Patch &operator=(const Patch &);
};

} /* namespace bs */

#endif /* BS_PATCH_H */
