A polyphonic virtual modular synthesizer. Panels in a rack, jacks you drag
patch cables between, and the cables hang.

**Windows only for this release.** CI builds and tests Linux and macOS on every
push, so they are not unknown — but nobody has sat in front of them yet, and a
release ought to mean someone has. They follow.

### Which file

| | |
|---|---|
| **The program** | `bencsynth-*-windows-x86_64.zip` — unzip and run `bencsynth.exe`. |
| **The plugins** | `bencsynth-*-windows-plugins.zip` — CLAP and VST3, with `INSTALL.md`. |

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

The **Rack** parameter steps through all 29 presets and saves with your
project. The eight **Macro** parameters are automatable.

### Getting started

Press **Z** or click the keys. **RACKS** on the toolbar loads a preset; each
one comes with a NOTES panel explaining what it does and one thing to try.
Right-click a module's title bar for its menu, right-click the empty rack to
add a module, and drag between jacks to patch. Scroll to zoom.

Start with **CLASSIC**, then **PIANO**, then **GRAND TOUR** — which uses every
module type at once and is more of a demonstration than an instrument.
