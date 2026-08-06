#include "bs_sync.h"

namespace bs {

/* FNV-1a. Not cryptographic and does not need to be - a collision means one
 * missed rebuild, and the next structural edit corrects it. */
static inline void mix(uint64_t &h, uint64_t v)
{
    h ^= v;
    h *= 1099511628211ull;
}

static inline void mixStr(uint64_t &h, const std::string &s)
{
    for (size_t i = 0; i < s.size(); i++) mix(h, (uint64_t)(unsigned char)s[i]);
    mix(h, 0xffu);
}

uint64_t bs_structure_signature(const Engine *e)
{
    uint64_t h = 14695981039346656037ull;
    const Patch &p = e->patch;

    mix(h, (uint64_t)p.slotCount());
    for (int i = 0; i < p.slotCount(); i++) {
        const Module *m = p.module(i);
        if (!m) { mix(h, 0xdeadu); continue; }
        mixStr(h, m->typeId);
        mix(h, (uint64_t)m->paramCount());
    }

    const std::vector<Cable> &cs = p.cableList();
    for (size_t i = 0; i < cs.size(); i++) {
        if (!cs[i].alive) continue;
        mix(h, (uint64_t)cs[i].src);
        mix(h, (uint64_t)cs[i].srcPort);
        mix(h, (uint64_t)cs[i].dst);
        mix(h, (uint64_t)cs[i].dstPort);
    }
    return h;
}

uint32_t bs_flatten_params(const Engine *e, float *out, uint32_t cap)
{
    const Patch &p = e->patch;
    uint32_t n = 0;
    for (int i = 0; i < p.slotCount(); i++) {
        const Module *m = p.module(i);
        if (!m) continue;
        for (int k = 0; k < m->paramCount(); k++) {
            if (n >= cap) return 0;
            out[n++] = m->params[(size_t)k].value;
        }
    }
    return n;
}

bool bs_apply_params(Engine *e, const float *in, uint32_t count)
{
    Patch &p = e->patch;

    /* Count first, apply second. A partial application is worse than none:
     * half the rack would take new values and half would keep old ones, with
     * no way to tell afterwards which half. */
    uint32_t n = 0;
    for (int i = 0; i < p.slotCount(); i++) {
        const Module *m = p.module(i);
        if (m) n += (uint32_t)m->paramCount();
    }
    if (n != count) return false;

    n = 0;
    for (int i = 0; i < p.slotCount(); i++) {
        Module *m = p.module(i);
        if (!m) continue;
        for (int k = 0; k < m->paramCount(); k++)
            m->params[(size_t)k].value = in[n++];
    }
    return true;
}

} /* namespace bs */
