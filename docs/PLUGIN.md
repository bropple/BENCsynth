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
| VST3 | not supported | everywhere | MIT since Oct 2025 — viable directly |
| VST2 | supported | everywhere old | no — see below |

**VST2 is out on licensing, not on merit.** Steinberg withdrew the VST2 SDK in
2018 and no longer issues licences for it. Shipping a new VST2 plugin means
either using an SDK nobody can legally give you or reimplementing the ABI from
observation. For a project that has been careful about the OFL on its font and
carries a NOTICE file, that is not a trade worth making for one host's older
plugin path.

**VST3 stopped being a licensing question on 29 October 2025.** Steinberg
relicensed the VST3 SDK (3.8) under **MIT**, replacing the old dual
GPLv3+/proprietary arrangement. There is no agreement to sign and no
proprietary licence to manage — `vst3sdk/LICENSE.txt` is a plain MIT grant, and
clap-wrapper's own licensing table lists it as such, with only Steinberg's
trademark guidelines applying to use of the VST name and logo. (ASIO went
GPLv3 in the same announcement, which is a separate matter and does not affect
us.)

That changes VST3 from "reachable through a wrapper" to a target we could write
directly, and it is the format with by far the widest host support — Ableton,
Cubase, FL Studio, Studio One, Bitwig, REAPER, Ardour and Mixbus all load it.
It does not change the LMMS answer: LMMS has zero VST3 code, so none of this
helps there.

The one place VST3 is genuinely worse than CLAP is the editor. `IPlugView`
embeds into a host-supplied parent window (HWND, NSView, X11) and VST3 has no
first-class floating mode, whereas CLAP supports plugin-created floating
windows outright. That matters here because raylib owns its own top-level
window — see the editor section.

**CLAP is not ready for LMMS yet.** Support is a draft pull request
([LMMS/lmms#7199](https://github.com/LMMS/lmms/pull/7199)) — 137 commits,
tested only on Linux Mint, with known parameter-automation problems and a note
from its author not to expect a finished product. It will land; it has not
landed.

So: **LV2 first**, because it is the only format an instrument can use in LMMS
without a licensing problem, and it is a plain C API with no SDK to license.
**CLAP second**, because it is the better API, because it is the only format
with a floating-window editor mode that suits raylib, and because
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

### LMMS does not implement LV2 state — checked, not guessed

The wiki's feature list omits `state:interface`, and the wiki is incomplete
enough that this could have been an oversight. It is not. `src/core/lv2/Lv2Proc.cpp`
on master contains no reference to `LV2_State_Interface` and never calls
`extension_data` looking for one. The supported-feature list `Lv2Manager`
declares is `urid:map`, `urid:unmap`, `options`, `worker:schedule` and the three
`buf-size` block-length features. State is absent because there is no state
support to declare.

For most plugins that is a detail. For this one it is central: the whole rack
*is* state — a text blob, not a set of control ports. So **in LMMS specifically,
the rack does not survive a project save.** The eight macro ports do, being
ordinary control ports that LMMS models and automates like any other. Which
modules exist and how they are wired does not.

`state:interface` stays regardless. It is correct, it is what the extension is
for, and it works in Ardour, Qtractor, Carla and REAPER — most of where an LV2
actually goes. The gap is LMMS's, and it closes on its own the day LMMS
implements state.

Until then the workable fallback is loading the rack from a file instead of
from host state: an environment variable or a path convention read at
`instantiate`, needing nothing from the host. Worse than state, and unbuilt,
but it is the option that exists.

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

- **Ship headless first.** An LV2 with no UI still loads in LMMS and still
  makes sound — you build the rack in the standalone, save a `.bencsynth`, and
  load it in the plugin. (It saves its state anywhere that implements state;
  see above for why LMMS is not yet such a place.) That is a genuinely useful
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
2. ~~**Headless CLAP**~~ **Done.** Audio ports, note ports (CLAP notes and raw
   MIDI both), nine parameters, state, and a rack selector. 39 checks in
   `tools/clap_host.cpp`, built for Linux, Windows and macOS in CI.
3. **Headless LV2** — a `bencsynth.lv2` bundle: `manifest.ttl`, `bencsynth.ttl`,
   and a `.so` with the twelve LV2 entry points. Atom port in for MIDI, two
   audio ports out, sixteen control ports for the macros, `state:interface` for
   the rack. Load it in LMMS; that is the milestone.
3. **CLAP** — the same core behind `clap_plugin`. Smaller than the LV2 because
   there is no Turtle to write, and it gets VST3 and AU free through
   clap-wrapper.
4. **Editor** — external-window first, embedded only if it proves necessary.

Nothing in steps 1–3 requires touching `src/core`. That was the point of
keeping it clean.

## The CLAP

Built, tested, and the second of the two wrappers. `make clap-fetch` once for
the headers (MIT, vendored under `vendor/`, gitignored like raylib), then
`make clap`, `make clap-test`, `make clap-install`.

It goes where hosts already look — `~/.clap` on Linux,
`~/Library/Audio/Plug-Ins/CLAP` on macOS,
`%LOCALAPPDATA%\Programs\Common\CLAP` on Windows. On Linux and Windows a CLAP
is a single file; on macOS it is a bundle directory with an `Info.plist`, which
is why that branch of the Makefile does more.

**It is not limited to one rack.** CLAP has a stepped parameter type, so the
rack selector is parameter 9 and every preset is reachable from the host's own
parameter list, named through `value_to_text`, saved with the project, and
automatable. That is the thing the LV2 cannot do in LMMS, and it arrives here
for free because the format has a vocabulary for it.

Building a rack allocates, so a rack change cannot happen in `process()`. The
parameter event records the request and calls `host->request_callback`; the
rebuild happens in `on_main_thread`, which is exactly what that pair is for.
`Engine::clear()` takes the graph lock, so it is safe against a render in
flight — the same path the standalone's preset menu already uses.

### The macro parameters had to be fixed to have any effect

`Engine::setMacro` writes into the MACRO module's knobs and does nothing at all
when the rack has no MACRO module — which most racks do not. Read back through
`macroValue`, that means a host sets a parameter to 0.75, reads 0.0, and draws
automation that visibly does nothing. The LV2 has the same hole and never
showed it, because LV2 control ports are owned by the host and it never asks
the plugin what they are.

So the wrapper holds the authoritative macro values: it reports what the host
set, pushes into the rack whenever there is a MACRO module to receive it,
re-applies all eight after a rack change so switching presets does not silently
zero them, and re-reads them from the rack after loading state, since a
restored rack's knob positions are then the truth.

Found by `tools/clap_host.cpp`, which sets a parameter and reads it back — the
kind of check that only fails when something is actually wired wrong.

## The editor, in another process

The CLAP opens the real BENCsynth rack — cables, physics and all — as a
floating window, and it does that by starting a **second process**.

That is forced, not chosen. raylib keeps its window, GL context, input and
timing in one file-scope `CoreData CORE` (`rcore.c:373`), so a process gets
exactly one raylib window however many threads it has. Two plugin instances in
one project would fight over that single global. Threading cannot fix a
singleton; a process boundary gives each instance its own by construction. It
is also how LMMS hosts VST2, and why a crashing plugin there does not take the
DAW with it.

The editor is the standalone binary, run as `bencsynth --editor <name>`. It
opens no audio device — the plugin is already making the sound, and a second
one would be the same notes playing a few milliseconds out of step.

### What crosses the boundary

Four channels in a shared block, because the two directions cost different
amounts:

| | Direction | Carries | Cost on arrival |
|---|---|---|---|
| `edit` | editor → plugin | the whole rack, when its *structure* changed | rebuild, on the main thread |
| `param` | editor → plugin | every knob value, flattened | written in place, per block |
| `snap` | editor → plugin | the whole rack, on any change | stored, never rebuilt from |
| `host` | plugin → editor | the whole rack | editor catches up |

The split between `edit` and `param` is the one that matters. Adding a module
means the plugin must rebuild, which allocates and cuts every sounding voice —
unavoidable, and rare. Turning a knob is a float, and must not cost that: if
both travelled the same way, a filter sweep would sound like the patch being
reloaded sixty times a second.

So the structure signature covers module types, knob counts and cables —
**and deliberately not positions**. Dragging a module changes the rack text but
not the signature, so it travels as a snapshot and rebuilds nothing.

`snap` exists because the plugin never rebuilds for a knob turn or a drag. Its
own copy would have the right sound and the wrong layout, so what the host
saves is the editor's rack, not the plugin's.

Every channel is a seqlock — writer bumps the sequence odd, writes, bumps it
even; a reader seeing an odd or changed sequence tries again next frame. There
is no lock anywhere near the audio thread, so a hung or killed editor can never
block it. The worst it can do is stop publishing.

There is also a small SPSC ring for notes played *in* the editor window. It has
no audio device, so its on-screen keyboard and musical typing only make a sound
if the events reach the plugin. Draining it is not optional either: nothing in
editor mode calls `render()`, so anything left in the engine's own queue would
sit there until it filled and then be dropped forever.

### What the plugin tells the editor

The editor renders nothing that reaches a speaker, so everything it draws from
audio would otherwise be dead: a flat scope, a still meter, a load of 0% and a
sample rate that was a compiled-in constant rather than the host's.

Three things fix that, all travelling plugin → editor:

- **Telemetry** — sample rate, load, sounding and allocated voices. Plain
  relaxed atomics, not a seqlock: each is one word, read once a frame for a
  status line, and one frame stale is invisible.
- **Host notes** — a second ring, opposite direction to the editor's own. The
  editor applies them to its engine so the on-screen keyboard lights up while
  the DAW plays a part. Deliberately never echoed back, or a held key would
  bounce between the processes forever. The two rings have separate indices and
  a test enforces it.
- **A shadow render.** Scopes, meters and envelope displays are modules reading
  their own inputs, so there is no signal in the editor's process unless the
  editor makes one. It runs the same graph with the same notes and throws the
  audio away.

The shadow render is honest about what it is: the picture is the editor's own
render, not the plugin's output, so a waveform can sit at a different phase
than what you hear. Shape and level are right, which is what a scope is for. It
is paced by the frame clock and clamped to ~170 ms, so a window that stalls
comes back and catches up rather than rendering a second of audio in one go.

### Embedded, and floating where it has to be

Floating was the first attempt and it was not enough. Hosts built around an FX
rack — REAPER is one — embed the plugin's view in their own window and never
ask for a floating one, so a floating-only plugin gets no call at all and the
host falls back to its generic parameter list. That is exactly what happened.

On Windows both modes work and **embedded is preferred**, because it is what
hosts there actually do. The editor reparents its own window into the host's
with `SetParent`, which works across process boundaries, and drops its caption
and border — a title bar inside an FX rack reads as a bug. Size arrives through
the shared block, since the host resizes its window rather than ours.

That reparenting lives in `src/plugin/bs_embed.cpp` for a specific reason:
raylib and `<windows.h>` both define `Rectangle`, `CloseWindow` and
`ShowCursor`, and the documented workaround — `NOGDI` and `NOUSER` — removes
precisely the USER functions `SetParent` needs. So the two headers never share
a translation unit and a `void *` crosses between them.

X11 and Cocoa remain floating. XEmbed and cross-process `NSView` embedding are
the outstanding work there, and claiming support without them produces a window
the host cannot place.

Embedding is also most of what a VST3 needs, since `IPlugView` has no floating
mode at all — so this was the work that route wanted anyway.

### Testing it

`make ipc-test` — 21 protocol checks that need no window, then six that start
the real editor binary against a real shared block and wait for it to publish
the rack it was handed. The protocol half is where the subtle failures live: a
signature that changed when a module moved would rebuild the rack on every
drag, and nothing about that is visible on the page.

`make clap-test` additionally drives `gui.create` / `show` / `hide` / `destroy`
through the plugin interface, and with `BENCSYNTH_EDITOR` set it starts a real
editor from a real plugin instance.

Both run in CI under Xvfb.

### Finding the editor

The plugin and the standalone are separate files and nothing guarantees they
were installed together. The plugin tries `BENCSYNTH_EDITOR`, then the
directory the `.clap` itself is in, then `PATH`. If all three miss, `show`
fails and says so on stderr rather than leaving a host waiting for a window
that will never appear.

## VST3, by wrapping

`make vst3-fetch` once, then `make vst3`. It builds
[clap-wrapper](https://github.com/free-audio/clap-wrapper) against the CLAP and
VST3 SDKs and produces `build/bencsynth.vst3`.

There is no second port and no second copy of the synthesizer. The wrapper
builds a VST3 that looks for a CLAP **of the same name** in the standard CLAP
directories and loads it at runtime — including
`%LOCALAPPDATA%\Programs\Common\CLAP` on Windows, which is where
`make clap-install` already puts it. CI checks this rather than assuming it:
the built `bencsynth.so` inside the bundle exports `GetPluginFactory` and
contains **zero** `bs::` symbols. If DSP ever ends up in there, the two copies
would drift apart silently.

That property has a useful consequence: the wrapper and the plugin need not
share a toolchain. CI builds the VST3 with **MSVC** and the CLAP with
**MinGW**, and they work together, because the only thing the wrapper has to
agree with anyone about is the VST3 ABI.

Both SDKs are MIT — CLAP always was, and Steinberg relicensed VST3 in October
2025 — so this adds no licensing obligation beyond attribution.

Versions are pinned (`clap-wrapper v0.15.1`, CLAP `1.2.10`) rather than
tracked. The first attempt at this used CLAP 1.2.2 and failed to compile:
clap-wrapper's main follows the CLAP SDK closely enough that it wanted
`clap/ext/draft/gain-adjustment-metering.h`, which did not exist until later.
An unpinned pair breaks on someone else's schedule.

The VST3 SDK's submodules are fetched individually, because the full set drags
in vstgui4 — by far the largest part of it, and something a wrapper never
touches. That is 37 MB instead of several hundred.

**Install both.** A VST3 without its CLAP loads into the host and has nothing
to play.

## Installing it

```
make lv2-install
```

That is the whole thing. It copies the bundle into the user-level directory
from the LV2 filesystem hierarchy standard, which every host already searches.

| | User | System |
|---|---|---|
| **Linux** | `~/.lv2` | `/usr/local/lib/lv2`, `/usr/lib/lv2` |
| **macOS** | `~/Library/Audio/Plug-Ins/LV2` | `/Library/Audio/Plug-Ins/LV2` |
| **Windows** | `%APPDATA%\LV2` | `%COMMONPROGRAMFILES%\LV2` |

Then restart the host — LMMS scans once at startup, so a bundle dropped in
while it is running will not appear.

### Get the bundle for the platform you are on

CI publishes three, one per platform: `bencsynth-lv2-linux`,
`bencsynth-lv2-windows`, `bencsynth-lv2-macos`. They are not interchangeable.
The binary inside is `bencsynth.so`, `bencsynth.dll` or `bencsynth.dylib`
respectively, and `manifest.ttl` names that file specifically.

This is worth being careful about because the failure is silent in the worst
way. Put the Linux bundle in `%APPDATA%\LV2` and lilv finds the directory,
parses the Turtle, and asks Windows to load an ELF binary. That fails, and a
host that cannot load a plugin's binary has nothing to report — LMMS shows no
plugin and no error, exactly as if the folder were empty. If a bundle is in the
right place and nothing appears, check which file is in it before checking
anything else.

**Copy the whole `bencsynth.lv2` directory, not the `.so` inside it.** The
bundle is the unit LV2 deals in: the two `.ttl` files beside the binary are
what tell a host the plugin exists at all. A lone shared object is invisible,
and silently so.

`LV2_INSTALL_DIR=/elsewhere make lv2-install` overrides the destination. Note
that `LV2_PATH` **replaces** the default search path rather than extending it,
so anything set there must include the standard directories itself — exactly
what made CI report the plugin's class as plain `Plugin` until the system
directory went back on.

### There is no LV2 path setting in LMMS, and that is correct

LMMS's settings dialog has directory fields for VST, LADSPA, SF2 and GIG, and
none for LV2. Nothing is missing or disabled. `Lv2Manager` calls
`lilv_world_load_all()` and never touches `LV2_PATH`, so discovery belongs
entirely to lilv — which has the standard directories compiled in. There is no
path for LMMS to offer because LMMS does not do the looking.

Lilv's built-in Windows default is `%APPDATA%\LV2;%COMMONPROGRAMFILES%\LV2` —
semicolons, not colons — with the environment variables expanded by lilv at
startup. `%APPDATA%\LV2` really is the whole answer on Windows.

**Carla is not needed.** That advice predates LMMS's own LV2 support and is
still the top search result for the question. `lilv` is a dependency in LMMS's
`vcpkg.json`, so official Windows builds have LV2 compiled in.

### Whether LMMS will accept this plugin

`Lv2Proc::check()` refuses a plugin that requires a feature LMMS does not
support, requires an unsupported option, has more than two audio channels in or
out, has **no** audio output, or has more than one MIDI port in either
direction. BENCsynth requires only `urid:map`, has two audio outs and no audio
in, exactly one MIDI in, no MIDI out, and no CV ports. It clears every one of
those.

MIDI does reach it: atom ports were enabled in
[LMMS/lmms#5691](https://github.com/LMMS/lmms/pull/5691), and piano-roll notes
are forwarded to LV2 instruments as MIDI atoms.

### If LMMS does not show it

In rough order of likelihood:

1. **It is the wrong LMMS**, and on Windows this is stricter than it sounds.
   1.2.2 has no LV2 at all — but neither does the released **1.3.0-alpha.1**,
   which is from November 2020 and is still the only 1.3 tag that exists. The
   Windows build did not get its LV2 dependencies until **April 2026**, when
   `Use vcpkg for MinGW dependencies` (LMMS/lmms#8218) landed. So on Windows you
   need a **master build newer than April 2026**, not "a 1.3 alpha". This is the
   answer most of the time.

   Verified against the master mingw64 build log of 2026-08-04: it compiles the
   whole `src/core/lv2/` tree and produces `lv2instrument` and `lv2effect`. A
   build that has LV2 puts `lv2instrument.dll` in its `plugins/` directory —
   that file's presence is the fastest way to tell, and it is absent from
   alpha.1 entirely.

   To check a build without launching it, scan its binaries for a string that
   only exists in the LV2 code path:

   ```powershell
   $dir="C:\Program Files\LMMS"; Get-ChildItem $dir -Recurse -Include *.exe,*.dll | ForEach-Object { $t=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($_.FullName)); if ($t -match 'LMMS_LV2_DEBUG') { "HIT: "+$_.Name } }
   ```

   No hits, no LV2, and no directory or environment variable will change that.
2. **The `.so` was copied instead of the bundle directory.**
3. **`LV2_PATH` is set and omits the standard directories.**
4. **Something in the bundle was rejected.** Set `LMMS_LV2_DEBUG` in the
   environment and LMMS lists what it turned down and why.

`lv2ls` from `lilv-utils` is the quickest independent check: if it prints
`https://github.com/bropple/BENCsynth`, the bundle is discoverable and any
remaining problem is on the host's side.

## Sources

- [LMMS VST plugin support](https://deepwiki.com/LMMS/lmms/6.1-vst-plugin-support)
- [Initial support for CLAP plugins — LMMS/lmms#7199](https://github.com/LMMS/lmms/pull/7199)
- [LV2 support — LMMS/lmms#562](https://github.com/LMMS/lmms/issues/562)
- [LMMS Lv2 wiki page](https://github.com/LMMS/lmms/wiki/Lv2)
- [VST3 support in LMMS](https://neomoon.one/vst3-support-in-lmms/)
- [LV2 filesystem hierarchy standard](https://lv2plug.in/pages/filesystem-hierarchy-standard.html)
- [Steinberg relicenses VST3 under MIT, 29 Oct 2025](https://www.soundonsound.com/news/steinberg-adopt-mit-license-vst3)
- [clap-wrapper licensing table](https://github.com/free-audio/clap-wrapper)
- [Use vcpkg for MinGW dependencies — LMMS/lmms#8218](https://github.com/LMMS/lmms/pull/8218)
  (April 2026: when Windows builds first got lilv, and therefore LV2)
- [Enable Lv2 Atom ports — LMMS/lmms#5691](https://github.com/LMMS/lmms/pull/5691)
- lilv `meson.build` (the compiled-in default search path) and LMMS
  `src/core/lv2/Lv2Manager.cpp`, `Lv2Proc.cpp`, `vcpkg.json`, all on master
