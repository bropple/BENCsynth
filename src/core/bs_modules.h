/*
 * BENCsynth - the module set
 *
 * Only the three modules something outside the DSP has to reach into are
 * declared here. The rest exist solely through the registry in bs_module.h,
 * which is the point: adding a module means writing one class and one line in
 * a table, and nothing else in the program has to learn about it.
 */

#ifndef BS_MODULES_H
#define BS_MODULES_H

#include "bs_module.h"

namespace bs {

/* The output stage. Sums whatever reaches it, and keeps a peak and an RMS per
 * side for the panel meter - two numbers rather than one because they answer
 * different questions, and a synthesizer with a resonant filter in it makes
 * them disagree constantly. */
class ModuleOut : public Module {
public:
    ModuleOut();
    void process();
    void reset();

    const float *masterL() const { return bufL; }
    const float *masterR() const { return bufR; }
    int extraPanelHeight() const { return 74; }   /* the meter */

    float peak[2];
    float rms[2];
    bool  clipping;

private:
    float bufL[BS_BLOCK], bufR[BS_BLOCK];
    float sq[2];
    float clipHold;
};

/* An oscilloscope, because a modular without one is a modular you patch by
 * guessing. Writes into a ring the GUI reads without locking: the worst a
 * torn read can do is put one pixel in the wrong place for one frame. */
class ModuleScope : public Module {
public:
    enum { TRACE = 512 };

    ModuleScope();
    void process();
    void reset();
    int extraPanelHeight() const { return 108; }  /* the screen */

    float traceA[TRACE];
    float traceB[TRACE];
    int   traceLen;
    int   writePos;

private:
    float acc[2];
    int   accCount;
    int   decim;
};

/* The keyboard interface: the one module that is not a pure function of its
 * inputs, and the only place polyphony enters the patch. */
class ModuleKbd : public Module {
public:
    ModuleKbd();
    void process();
    void reset();
    void bindKeys(KeyboardState *k) { keys = k; }

private:
    KeyboardState *keys;
    Slew  glide[BS_MAX_POLY];
    float trigTimer[BS_MAX_POLY];
    bool  primed[BS_MAX_POLY];
};

} /* namespace bs */

#endif /* BS_MODULES_H */
