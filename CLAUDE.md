# BENCsynth — working notes for Claude

A polyphonic virtual analog modular synth. C++17 + raylib, physics-based patch
cables. Ships as a standalone program and as CLAP / VST3 / AU / LV2 plugins.

Thirty-three module types, thirty-eight racks.

This file is for the things that are **expensive to re-derive** — the release
sequence, and the traps that have actually cost a build. It is not a tour of
the code; `ARCHITECTURE.md` and `docs/PLUGIN.md` are that, and the Makefile
documents its own targets.

---

## Untracked on purpose

- **`style/`** — the BENCO style guide. Gitignored, and must stay that way.
- **`ROADMAP.txt`** — working notes, deliberately untracked. Read it at the
  start of a session; it is ordered by what to pick up next.
- `patches/`, `vendor/`, `shots/`, `build/`.

The mascot is **S. Tarr** (the star). `../BENCmouth` is a reference for GUI
style only — do not copy code from it.

---

## Before you commit anything

```sh
make && make test                     # 582 checks
make clap  && make clap-test          #  74
make lv2   && make lv2-test           #  27
make ipc-test && xvfb-run -a --server-args="-screen 0 1400x900x24" \
    ./bencsynth-ipc-test ./bencsynth  #  46
```

Screenshots: `./bencsynth --shot NAME.png --frames 90` under `xvfb-run`.
**raylib's `TakeScreenshot` ignores the directory** and writes to the working
directory — pass a bare filename and move it afterwards.

---

## Cutting a release

The workflow builds **from the tag**, and on a tag push it also reads its own
definition from the tag. So a bug in `release.yml` means deleting and
re-pointing the tag, not just pushing a fix to main.

```sh
# 1. everything above passes, main is pushed, and CI on that exact commit is green
gh run list --branch main --limit 1

# 2. update .github/release-notes.md   <- the release body comes from this file
# 3. tag and push
git tag -a v0.X.Y -m "..." && git push origin v0.X.Y

# 4. watch it
gh run watch "$(gh run list --workflow release --limit 1 --json databaseId -q '.[0].databaseId')"
```

To move a tag after a failed run (nothing was published, so nothing is lost):

```sh
git tag -d v0.X.Y && git push origin :refs/tags/v0.X.Y
git tag -a v0.X.Y -m "..." && git push origin v0.X.Y
```

Seven assets across three platforms. `publish` asserts all seven exist before
creating the release — if that step fails, a glob did not match.

**Verify what actually shipped.** Download the artifacts and check them; do not
infer from a green run. The Linux binary can be run right here, which is a real
end-to-end test because it was built on a different machine:

```sh
gh release download vX --pattern '*linux*' && tar xzf *x86_64.tar.gz
xvfb-run -a ./bencsynth-*/bencsynth --shot t.png --frames 60
```

NSIS compresses its string table, so the installer's payload is **not**
readable with `strings`. Verify it from the build log's staging listing
instead, or install it.

---

## Formats: what builds them, what ships them, where they land

| Format | Build | Ships in | Installs to |
|---|---|---|---|
| standalone | `make` | `*-<os>-x86_64.zip` / `.tar.gz` / `*-macos-universal.dmg` | anywhere; keep `assets/` beside it |
| CLAP | `make clap` | every plug-in pack | `~/Library/Audio/Plug-Ins/CLAP`, `%LOCALAPPDATA%\Programs\Common\CLAP`, `~/.clap` |
| VST3 | `make vst3` | Windows + Linux packs | `…/VST3`, `~/.vst3` |
| **AU** | `make au` | **`*-macos-plugins.dmg`** | `~/Library/Audio/Plug-Ins/Components` |
| LV2 | `make lv2` | Linux pack | `~/.lv2` |

The CLAP is the synth. VST3 and AU are shims that load it at runtime, so
**both always install together** — a shim on its own does nothing. The editor
is the standalone binary and must sit beside the CLAP (or in `/Applications`
on macOS), or the plug-in opens to a row of sliders.

`make au-fetch` clones clap-wrapper **and** Apple's AudioUnitSDK;
`make vst3-fetch` clones clap-wrapper and the VST3 SDK. Neither implies the
other, which is why `cmake/CMakeLists.txt` guards each wrapper on its own SDK.

### Validating the AU (only possible on a real Mac)

```sh
# from the mounted bencsynth-*-macos-plugins.dmg
open "/Volumes/BENCsynth Plug-Ins/Install Plug-Ins.command"
auval -v aumu BNCS BNCO          # what Logic runs before it will list a plugin
auval -a | grep -i bencsynth     # is it even registered
```

If it is not registered, `killall -9 AudioComponentRegistrar` and try again.

---

## Traps that have cost a build

Each of these has happened. They are cheap to re-introduce.

**`workflow_dispatch` checks out the branch, not the tag.** This shipped
v0.1.0's release with `main`'s code attached to it. All three checkouts are now
pinned to `${{ github.event.inputs.tag || github.ref }}`. Same for
`GITHUB_REF_NAME`, which is the *branch* on a dispatch — that once published a
release called "main".

**`otool -L` prints a header line per architecture.** On a universal binary
`tail -n +2` leaves `bencsynth (architecture arm64):` in the stream, and a
"self-contained" check then fails the build for linking against itself. Filter
to indented lines only.

**`grep -i 'Rl[A-Z]'` matches the `rle` in `strlen`.** Do not widen the
raylib-symbol check that way; it fails on libc.

**`-MMD` cannot be combined with two `-arch` flags.** Clang refuses outright.
That is why every plugin rule filters `-MMD -MP` out, and why the universal
executable is behind `MACOS_UNIVERSAL=1` rather than always on. CI sets it too
— a flag exercised only on a tag is a flag that breaks on a tag.

**The macOS executable is the plugin's editor.** The CLAP opens the rack by
running the standalone, and the plugin DMG carries it. An arm64 executable
inside a universal plugin means the plugin loads on an Intel Mac and never
opens a window, which reads as a broken plugin rather than a wrong download.
Keep them both universal.

**Never ship a `.app` or `.clap` in a zip.** GitHub's zip drops the executable
bit and the code signature seals file modes — losing them invalidates the
signature, and an invalid signature is exactly what macOS reports as
"damaged". Disk image on macOS, tarball on Linux. Zip is fine for Windows.

**Signing is not notarisation.** Ad-hoc signing satisfies arm64's requirement
and validates, but identifies nobody, so macOS still says it cannot check for
malicious software. That is expected. "Damaged" is a different thing and is a
real bug.

**One CMakeLists declares both wrappers, so each needs its SDK guarded.**
Without that, configuring for AU demands the VST3 tree and vice versa. The
guards test the same file clap-wrapper's own `guarantee_*sdk` looks for.

**`project()` must list `OBJCXX` on Apple.** Both wrappers have `.mm` sources.
Miss it and CMake configures happily, then fails at *generate* time with
"required internal CMake variable not set: CMAKE_OBJCXX_COMPILE_OBJECT", which
does not obviously mean "declare the language".

**`auval` cannot see a component on a GitHub runner.** Registration is
asynchronous and needs a real user session; six retries with the registrar
killed between them never worked, so the CI job warns and skips. The AU
*was* validated by hand on a real Mac and passed everything — but that is a
human step, and it stays one. Re-run `auval -v aumu BNCS BNCO` after any
change to the wrapper, the codes, or the CLAP's parameter list.

**Nothing outside `bs_version.h` may spell a version out.** The header has
always claimed to be the single place; it was not, and v0.2.2 shipped an AU
reporting 1.1.1 wrapping a CLAP reporting 0.1.0 under a tag saying 0.2.2.
The Makefile reads the header and feeds `@BS_VERSION@` into the plist and
`-DBS_VERSION` into cmake. `tools/check_version.sh` enforces it, and the
release job runs it against the tag — so a tag that disagrees with the source
now fails before anything is built.

**clap-wrapper's AUv2 view never calls `gui->show()`.** The line is in
`wrappedview.asinclude.mm`, commented out, twice. It calls `create` and
`set_parent` and stops. Anything a plugin does in `show()` — starting the
editor process, in our case — simply never happens under Logic and GarageBand.
`ensureEditor()` is called from both `set_parent` and `show` for this reason,
and `clap_host.cpp` has a test that attaches a parent and never shows.

**`%LOCALAPPDATA%` is not `%APPDATA%`.** A plugin in the wrong one fails with
no message at all.

**`makensis` on Linux:** backslashes are not path separators, and relative
source paths resolve against the *script's* directory, not the working
directory. Pass absolute paths for the icon and the output.

---

## Invariants that constrain changes

**One raylib window per process.** raylib keeps its window and GL context in a
single file-scope `CoreData CORE`. That is why the editor is a **separate
process** talking over shared memory, not a thread. Do not try to make it a
thread again.

**The `.clap` must not link raylib or GLFW.** The host process has neither. CI
fails on any undefined glfw/raylib symbol in the plugin. `src/plugin/bs_embed.cpp`
is editor-only for this reason.

**Append ports and parameters; never insert or reorder.** Patch files store
cables by port *index*, so appending keeps every saved patch working. Verify
compatibility by writing a patch with the previously shipped binary and loading
it with the new one — do not reason about it.

**The AU's four-character codes are permanent** — `aumu` / `BNCS` / `BNCO`. A
host remembers a plugin by type, subtype and manufacturer; change any of them
and every saved project stops finding it.

**A plugin that changes its own parameter must say so in the output event
list.** Otherwise the hardware moves the sound, the host's knob stays put, the
project saves the old value, and the next automation point snaps it back.

**ALSA reads into a buffer of its own.** One `snd_seq_event_input` per poll
loses everything queued behind the first event, and it looks exactly like MIDI
not working. Drain with `snd_seq_event_input_pending`. And join a reader thread
*before* closing the sequencer it reads through — closing it first is a
use-after-free that aborts on shutdown.

**A polyphonic oscillator's idle channels are not silent.** They free-run at
0 V, which is middle C. Any measurement of a polyphonic rack needs a gated VCA
or the silent voices drown the one playing — this cost an hour on the MPE work.

**Measure a decay over longer than you think.** The bow was reported as
sustaining on the strength of a 1.6-1.8 s window; over eight seconds every note
was decaying and the bottom of the keyboard died. Anything that settles slowly
needs a window longer than it settles in, or the test is measuring the attack.

**The rack text is capped at 192 KB and crosses on every change.** Nothing
bulky can travel that way - embedded sample audio goes in the plugin's *state*,
which is a separate uncapped stream written only when the host saves.

**Goertzel drifts over a long window.** It reported 261 Hz for a note an octave
up. Use direct sin/cos correlation for pitch over more than a few thousand
samples.

**The editor owns the rack; the plugin must not write knobs.** The editor
publishes every knob every frame, so anything the plugin writes into the rack
is overwritten by a frame already in flight — silently. MIDI learn was built
the wrong way round first and passed locally while failing in CI, which is what
a race looks like. The plugin reports what it saw; the editor turns the knob.

**Changing `ShmBlock` means bumping `BS_SHM_VERSION`.** The handshake refuses
a mismatched pair rather than reading the block at the wrong offsets, which is
the only reason that number exists. And the block is `memset` to zero, so any
field whose zero is a real value — `learnMacro`, where 0 is macro one — has to
be initialised explicitly at both creation sites.

**`bs_button` honours `ui->suppress`.** A modal panel that sets it to block the
rack blocks its own buttons too — the browser did exactly that and its OK
button was dead while row clicks worked, because those are hit-tested by hand.
Set it in `main()` before the rack draws, the way the about overlay does.

**CLAP parameter IDs are permanent.** There are 16 exposed slots whose *names*
change via `rescan(INFO|TEXT)`; the IDs never do.

**Unpatched inputs read as silence by design**, and an input takes exactly one
cable — a second silently replaces the first. Right at a patch bay, never right
in a preset. `Patch::connect` counts replacements and the preset sweep insists
on zero, which found four such cables in one run after one of them had already
broken GRAND.

**Wayland cannot embed a plugin window at all.** Not our gap; XWayland covers
it. Windows uses `SetParent`, X11 uses `XReparentWindow`, macOS cannot reparent
across processes at all and so ships pixels into an `IOSurface`.

---

## Measure, don't reason

This project has a strong track record of measurement catching what argument
missed: FFT spectra for string inharmonicity, tick-gap timing for the clock,
L/R correlation for stereo width, `XQueryTree` for embedding, Xvfb + xdotool
for UI. Build a throwaway probe against `libbencsynth.a`:

```sh
g++ -O2 -std=c++17 -Isrc/core -o /tmp/probe probe.cpp libbencsynth.a -lm
```

When a change is supposed to affect the sound, **put the number in the commit
message**, and say so plainly when the number is unimpressive.
