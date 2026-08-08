A polyphonic virtual modular synthesizer. Panels in a rack, jacks you drag
patch cables between, and the cables hang.

**Windows only for this release.** CI builds and tests Linux and macOS on every
push, so they are not unknown — but nobody has sat in front of them yet, and a
release ought to mean someone has. They follow.

### Which file

| | |
|---|---|
| **Everything** | `bencsynth-*-windows-setup.exe` — the installer. Program and plug-ins, and it upgrades an existing install rather than sitting beside it. No administrator rights needed. |
| The program alone | `bencsynth-*-windows-x86_64.zip` — unzip and run `bencsynth.exe`. |
| The plug-ins alone | `bencsynth-*-windows-plugins.zip` — CLAP and VST3, with `INSTALL.md` for placing them by hand. |

The installer is the one to take unless you have a reason not to. It puts each
format in the folder its hosts already search, and puts a copy of the program
beside the CLAP as its editor — which is the part that is easy to get wrong by
hand, and without which the plug-in opens to a row of sliders instead of the
rack.

Keep the `assets` folder beside the binary: it carries the interface font and
the wordmark. Without it the program still runs, in raylib's built-in face.

### As a plugin

The plugin opens the **real rack** — cables, physics, keyboard — inside your
DAW, not a row of sliders. Notes from the host light up the keyboard and drive
the scopes; turning a knob reaches the audio without interrupting it.

- **CLAP** for REAPER, Bitwig and Studio One.
- **VST3** for Ableton, Cubase, FL Studio and Studio One. It is a shim that
  loads the CLAP, so **install both**.

`INSTALL.md` in the plugin zip says which folder. Two things worth knowing
before you start: the editor is `bencsynth.exe` and has to sit beside the
`.clap`, and `%LOCALAPPDATA%` is not `%APPDATA%` — putting a plugin in the
wrong one fails with no message at all.

The **Rack** parameter steps through all 37 presets and saves with your
project. The eight **Macro** parameters are automatable.

So is any knob you choose: **right-click a knob → EXPOSE** and it takes one of
sixteen slots the host sees by name — `VCF CUTOFF` rather than `Param 12`. The
choice is saved with the project, and the host is told to re-read the names, so
they change as you change them.

Everything else saves too — the patch, where the rack was scrolled and how far
it was zoomed. You do not have to save inside BENCsynth for the DAW's project
to come back the way you left it.

### New since v0.1.0

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
