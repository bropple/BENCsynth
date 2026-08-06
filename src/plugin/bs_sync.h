/*
 * Turning a rack into the two things the shared block carries, and back.
 *
 * The split that matters is between structure and values. Adding a module or
 * pulling a cable changes what the graph *is*, and the plugin has to rebuild -
 * which allocates and cuts every sounding voice. Turning a knob changes a
 * float. If both travelled the same way, every knob turn would reload the
 * patch, and a filter sweep would sound like someone repeatedly kicking the
 * power switch.
 *
 * So: a signature over the structure alone (module types and cables, position
 * deliberately excluded - dragging a module is cosmetic and must not rebuild
 * anything), and a flat vector of knob values in slot order. Same rack text on
 * both sides means the same build order means the same indices, which is what
 * makes the flat vector meaningful at all.
 */
#ifndef BS_SYNC_H
#define BS_SYNC_H

#include "bs_engine.h"

#include <cstdint>

namespace bs {

/* Structure only: module count, each module's type and knob count, and every
 * live cable. Two racks with the same signature have the same knob layout, so
 * a value vector from one applies to the other. */
uint64_t bs_structure_signature(const Engine *e);

/* Flattens every module's knob values, in slot order then knob order.
 * Returns how many were written, or 0 if they would not fit. */
uint32_t bs_flatten_params(const Engine *e, float *out, uint32_t cap);

/* Writes values back. Does nothing unless the count matches exactly, because a
 * mismatch means the two sides disagree about the rack and writing anyway
 * would scatter values into unrelated knobs. Returns true if applied. */
bool bs_apply_params(Engine *e, const float *in, uint32_t count);

} /* namespace bs */

#endif
