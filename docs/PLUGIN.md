# Running BENCsynth inside a host

## The format question, and a correction

The goal is "a VST, mainly for LMMS". Those two do not go together, and it is
worth saying plainly before any code is written:

**LMMS cannot load VST3.** Its plugin support is LADSPA, LV2, and VST**2** —
the last of those through VeSTige, which on Linux is really a bridge for
*Windows* VST2 binaries under Wine. VST3 has no path into LMMS at all today.

That leaves four candidates, and the ranking is not close:

| Format | LMMS | Elsewhere | Verdict |
|---|---|---|---|
| **LV2** | native, works now | Ardour, Qtractor, Carla, REAPER, Mixbus | **first target** |
| **CLAP** | in development, not merged | Bitwig, REAPER, Studio One, FL | **second target** |
| VST3 | not supported | everywhere | via clap-wrapper, later |
| VST2 | supported | everywhere old | no — see below |

**VST2 is out on licensing, not on merit.** Steinberg withdrew the VST2 SDK in
2018 and no longer issues licences for it. Shipping a new VST2 plugin means
either using an SDK nobody can legally give you or reimplementing the ABI from
observation. For a project that has been careful about the OFL on its font and
carries a NOTICE file, that is not a trade worth making for one host's older
plugin path.

**CLAP is not ready for LMMS yet.** Support is a draft pull request
([LMMS/lmms#7199](https://github.com/LMMS/lmms/pull/7199)) — 137 commits,
tested only on Linux Mint, with known parameter-automation problems and a note
from its author not to expect a finished product. It will land; it has not
landed.

So: **LV2 first**, because it is the only format an instrument can use in LMMS
without a licensing problem, and it is a plain C API with no SDK to license.
**CLAP second**, because it is the better API and because
[clap-wrapper](https://github.com/free-audio/clap-wrapper) turns one CLAP into
VST3 and AU builds — which is how the VST3 that LMMS cannot load reaches every
host that can, without writing a VST3 at all.

## Which LMMS, though

This is the part that decides whether the plan is any good, and the answer is
not comfortable.

**LMMS's latest stable release is 1.2.2, from July 2020.** LV2 support is not
in it. LV2 landed in the 1.3 line, which after six years is still
`1.3.0-alpha.1` and nightlies.

| | LMMS 1.2.2 (stable, 2020) | LMMS 1.3-alpha / nightly |
|---|---|---|
| LADSPA | yes | yes |
| **LV2** | **no** | **yes — Linux, macOS and Windows** |
| VST2 | yes | yes |
| VST3 | no | no |
| CLAP | no | no (draft PR) |

Two things follow.

**LADSPA is not a fallback.** It has no MIDI and no notion of an instrument —
it is an effects API, audio in and audio out with float control ports. A
synthesizer cannot be a LADSPA plugin at all. So on stable 1.2.2 an
*instrument* has exactly one route, VST2, and that route is closed on
licensing.

**There is therefore no format that reaches stable LMMS.** Targeting LV2 means
telling people to run an LMMS 1.3 nightly. That is defensible — 1.2.2 predates
the LV2 work by years and 1.3 is where all development has happened since — but
it is the actual trade, and it should be written on the tin rather than
discovered by whoever tries to load the plugin into the version they already
have.

The good news is that when it does work, it works everywhere: the 1.3 line
carries LV2 on Linux, macOS and Windows, so one format covers all three
platforms rather than needing a different plugin per system.

### One thing to verify before committing

The LMMS LV2 wiki lists its supported features as *"Core (except CV ports),
URIDs, MIDI atoms, Buffer Size, Options, Worker"* — and **`state:interface` is
not on that list.** For most plugins that is a detail. For this one it is
central: the entire rack is state, a text blob, not a set of control ports.
Without the state extension a rack could not be saved inside an LMMS project,
and the plugin would come back empty every time the song was reopened.

The wiki is visibly incomplete, so this may simply be undocumented rather than
missing. It is the first thing to test with a stub plugin, before any real work
goes in — and if it is genuinely absent, the fallback is a file path in the
state and the rack loaded from disk, which is worse but workable.

## What is already done

The core was built plugin-shaped from the first commit, and three things that
every one of these formats needs are already in place and tested:

- **No raylib, no globals, no allocation in `process()`.** `make core` and
  `make test` build with raylib absent entirely, which is what keeps it true.
  A plugin links `libbencsynth.a` and nothing else.
- **A wait-free event queue with frame offsets.** `Engine::noteOn(note, vel,
  atFrame)` is exactly the shape a host's event list arrives in, and
  `Engine::render` works through the buffer applying events as it reaches them
  rather than dumping them all at the top. Timing lands to one 32-frame block.
- **State as a string.** `bs_patch_to_string` / `bs_patch_from_string` are the
  real serialiser and the file functions wrap them. A host is handed a blob and
  expects one back; it never sees a path.

`bs::Engine` is the whole surface: sample rate in, events in, `render()` out.

## What a wrapper still has to solve

### 1. Parameters

This is the real design problem, and it is worth being honest that it has no
clean answer.

A host wants a **flat, fixed, stable** list of automatable parameters, declared
once, identified by a number that never changes. A modular rack has none of
those properties: its parameters come and go as modules are added and deleted,
and "the cutoff knob" is not a stable identity when there may be zero VCFs or
four.

Three options, in decreasing order of how much I would want to use them:

- **A fixed macro bank.** Declare eight parameters, `MACRO 1..8`, at load
  time. Add a `MACRO` module to the rack whose outputs are those values as
  CV. The host automates macros; the patch decides what they do, by cable.
  This is honest about the mismatch instead of fighting it, it is one new
  module, and it makes the mapping visible on the panel rather than hidden in
  a dialog. It is the same answer hardware modular reached for CV inputs on
  the back panel.
- **Enumerate the current rack.** Publish one host parameter per rack knob and
  re-publish on every edit. LV2 cannot do this at all — its ports are declared
  in the Turtle file before the plugin is instantiated. CLAP can, through
  `params.rescan`, but hosts vary in how well they cope and an automation lane
  pointing at a knob that no longer exists has to fail gracefully.
- **Both.** Macros for LV2, enumeration for CLAP. Two behaviours to maintain
  and explain; only worth it if the macro bank proves too coarse in practice.

**Recommendation: the macro bank.** Build the `MACRO` module first — it is
useful in the standalone too, as a place to land an expression pedal or a mod
wheel — and let both plugin formats expose exactly those.

### 2. The editor

raylib owns its window: `InitWindow` creates one, and there is one per process.
A plugin editor is a child of a window the host owns. These are not compatible,
and this is the largest single piece of work.

Three routes:

- **Ship headless first.** An LV2 with no UI still loads in LMMS, still makes
  sound, and still saves its state — you build the rack in the standalone,
  save a `.bencsynth`, and load it in the plugin. That is a genuinely useful
  product and it is reachable in a few hundred lines. **Do this first.**
- **An external UI.** LV2 has `ui:external-ui` and CLAP has a floating-window
  mode; both let the plugin open its *own* top-level window rather than
  embedding. raylib can do that as-is. It is a worse experience than embedding
  and it is not supported by every host, but it is a fraction of the work.
- **Embed properly.** Requires raylib to attach to a host-supplied `Window` /
  `NSView` / `HWND`, which raylib does not support. It would mean either
  patching raylib's GLFW backend or reimplementing the drawing layer against a
  context the host provides. Everything in `src/gui` above `bs_gui.cpp` is
  already independent of how the context was made, so this is a platform-layer
  job rather than a rewrite — but it is not small.

### 3. Sample rate and block size

`Engine::init(sr)` and `setSampleRate` already do the right thing, and the
32-frame internal block is decoupled from whatever the host asks for. A host
that changes sample rate mid-session is handled; one that hands over a buffer
smaller than 32 frames is handled by the leftover logic in `render()`.

### 4. Threading

The plugin case is *simpler* than the standalone. There is no interface thread
mutating the rack, so the mutex in `Engine` is uncontended and could in
principle be removed for a headless build — but it costs nothing uncontended,
and leaving it in means one code path rather than two.

## Suggested order of work

1. ~~**`MACRO` module** — eight CV outputs, values settable from outside.~~
   **Done.** Eight knobs, eight jacks, 0–10 V out, and
   `Engine::setMacro` / `macroValue` writing to and reading from the knobs
   themselves rather than a shadow copy, so a host automating a parameter
   moves the knob that parameter *is*.
2. **Headless LV2** — a `bencsynth.lv2` bundle: `manifest.ttl`, `bencsynth.ttl`,
   and a `.so` with the twelve LV2 entry points. Atom port in for MIDI, two
   audio ports out, sixteen control ports for the macros, `state:interface` for
   the rack. Load it in LMMS; that is the milestone.
3. **CLAP** — the same core behind `clap_plugin`. Smaller than the LV2 because
   there is no Turtle to write, and it gets VST3 and AU free through
   clap-wrapper.
4. **Editor** — external-window first, embedded only if it proves necessary.

Nothing in steps 1–3 requires touching `src/core`. That was the point of
keeping it clean.

## Sources

- [LMMS VST plugin support](https://deepwiki.com/LMMS/lmms/6.1-vst-plugin-support)
- [Initial support for CLAP plugins — LMMS/lmms#7199](https://github.com/LMMS/lmms/pull/7199)
- [LV2 support — LMMS/lmms#562](https://github.com/LMMS/lmms/issues/562)
- [LMMS Lv2 wiki page](https://github.com/LMMS/lmms/wiki/Lv2)
- [VST3 support in LMMS](https://neomoon.one/vst3-support-in-lmms/)
