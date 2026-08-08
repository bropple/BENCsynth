A polyphonic virtual modular synthesizer. Panels in a rack, jacks you drag
patch cables between, and the cables hang.

**All three platforms this time.** Windows was the only one for v0.1.0 and
v0.2.0, on the rule that a release is a claim someone has actually run the
thing. That has stopped being true, so Linux and macOS are back.

### Which file

**Windows**

| | |
|---|---|
| **Everything** | `bencsynth-*-windows-setup.exe` — the installer. Program and plug-ins, and it upgrades an existing install rather than sitting beside it. No administrator rights needed. |
| The program alone | `bencsynth-*-windows-x86_64.zip` — unzip and run `bencsynth.exe`. |
| The plug-ins alone | `bencsynth-*-windows-plugins.zip` — CLAP and VST3, with `INSTALL.md` for placing them by hand. |

**macOS** — universal, Apple Silicon and Intel.

| | |
|---|---|
| The program | `bencsynth-*-macos-universal.dmg` — drag it to Applications. |
| The plug-ins | `bencsynth-*-macos-plugins.dmg` — CLAP and the editor, with an `Install Plug-Ins.command` that puts them where hosts look. |

**Linux** — x86_64.

| | |
|---|---|
| The program | `bencsynth-*-linux-x86_64.tar.gz` — untar and run `./bencsynth`. |
| The plug-ins | `bencsynth-*-linux-plugins.tar.gz` — CLAP, LV2 and VST3, with `INSTALL.md`. |

On Windows the installer is the one to take unless you have a reason not to. It
puts each format in the folder its hosts already search, and puts a copy of the
program beside the CLAP as its editor — which is the part that is easy to get
wrong by hand, and without which the plug-in opens to a row of sliders instead
of the rack.

Keep the `assets` folder beside the binary: it carries the interface font and
the wordmark. Without it the program still runs, in raylib's built-in face.

**macOS will say it cannot check the app for malicious software.** That is
what unnotarised means, and this is unnotarised — notarisation needs a paid
Apple Developer account. The signature is real and validates; it just
identifies nobody. Right-click → Open, once, per download. If you get
"damaged" rather than "unidentified developer", that is a different thing and
a bug — please say so.

**Linux embeds the rack via X11.** Under Wayland that goes through XWayland,
which works; native Wayland has no cross-process window embedding at all, so
there is nothing to implement against there.

### As a plugin

The plugin opens the **real rack** — cables, physics, keyboard — inside your
DAW, not a row of sliders. Notes from the host light up the keyboard and drive
the scopes; turning a knob reaches the audio without interrupting it.

- **CLAP** for REAPER, Bitwig and Studio One.
- **VST3** for Ableton, Cubase, FL Studio and Studio One. It is a shim that
  loads the CLAP, so **install both**.

- **LV2** on Linux, for hosts that want it. It has no editor — see below.

If you are placing files by hand, `INSTALL.md` in each plug-in pack says which
folder. The one thing to get right on every platform is that **the editor is
the program itself**, and it has to sit beside the `.clap` — that is what opens
the rack instead of a row of sliders. The Windows installer and the macOS
`Install Plug-Ins.command` both do it for you.

On Windows, also note that `%LOCALAPPDATA%` is not `%APPDATA%`; putting a
plug-in in the wrong one fails with no message at all.

The **Rack** parameter steps through all 37 presets and saves with your
project. The eight **Macro** parameters are automatable.

So is any knob you choose: **right-click a knob → EXPOSE** and it takes one of
sixteen slots the host sees by name — `VCF CUTOFF` rather than `Param 12`. The
choice is saved with the project, and the host is told to re-read the names, so
they change as you change them.

Everything else saves too — the patch, where the rack was scrolled and how far
it was zoomed. You do not have to save inside BENCsynth for the DAW's project
to come back the way you left it.

### New since v0.2.0

- **Linux and macOS builds**, per the above.
- **The macOS executable is universal now.** The plug-ins always were, but the
  executable is the plug-in's editor — so a universal CLAP on an Intel Mac used
  to load and then never open its window.
- **DLY and CHORUS carry stereo through.** Both summed their input, so a rack
  that had placed its voices across the field lost them at the effect. Both
  gained an `IN R`; DLY gained a second delay line with `OUT R` and `WET R`.
  Patches saved by v0.2.0 load unchanged — the new ports are appended.

### New in v0.2.0

- **An installer**, which is the headline. It upgrades in place; the old
  version does not have to be removed first.
- **Thirteen more module types**, thirty in all: a plucked-string waveguide, a
  state-variable filter, a function generator, a quantizer, a clock, an
  eight-step sequencer, logic, a switch, a wavefolder, a bitcrusher, a chorus,
  a slew limiter and a stereo panner among them.
- **Eight more racks**, thirty-seven in all — including **PIANO** and
  **GRAND**, which are struck strings rather than a filter pretending.
- **Musical typing** while the plugin has focus, a keyboard you can fold away
  with the icon in the top right, and a rack list that scrolls and sorts.
- **Tempo sync** — CLK and DLY can follow the host's BPM in note values.
- **Five bugs** an audit turned up, three of them serious: a restored rack
  being wiped on activate, which made saved projects come back silent; clock
  divisions that did not divide; and an envelope's end-of-cycle that stuck on.

### Getting started

Press **Z** or click the keys. **RACKS** on the toolbar loads a preset; each
one comes with a NOTES panel explaining what it does and one thing to try.
Right-click a module's title bar for its menu, right-click the empty rack to
add a module, and drag between jacks to patch. Scroll to zoom.

Start with **CLASSIC**, then **PIANO**, then **GRAND TOUR** — which uses every
module type at once and is more of a demonstration than an instrument.
