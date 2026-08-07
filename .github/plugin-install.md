# Installing the BENCsynth plugins on Windows

Three formats, one synthesizer. Pick whichever your DAW loads.

## CLAP — REAPER, Bitwig, Studio One

Copy **`bencsynth.clap`** and **`bencsynth.exe`** into:

    %LOCALAPPDATA%\Programs\Common\CLAP

Both files. The rack editor *is* `bencsynth.exe` — the plugin starts it as a
separate process — so without it you get a generic parameter list and no
cables. The plugin looks for it in `BENCSYNTH_EDITOR`, then beside itself,
then on `PATH`.

To open that folder without typing the path:

    explorer "$env:LOCALAPPDATA\Programs\Common\CLAP"

## VST3 — Ableton, Cubase, FL Studio, Studio One

Copy the **`bencsynth.vst3`** folder into:

    %LOCALAPPDATA%\Programs\Common\VST3

**Install the CLAP as well.** The VST3 is a shim: it finds `bencsynth.clap` in
the CLAP folder above and loads it at runtime. On its own it will appear in
your host with nothing to play.

## Then

Restart the host — they scan once at startup. BENCsynth appears among the
instruments. Click the plugin's UI button and the rack opens.

The **Rack** parameter steps through all 37 presets and saves with the project.
The eight **Macro** parameters are automatable; what each one does is decided
in the rack, by cable, on the MACRO module.

## If it does not appear

- **Wrong folder.** `%LOCALAPPDATA%` is `AppData\Local`, and `%APPDATA%` is
  `AppData\Roaming`. They are different directories and neither is `AppData`.
- **The host did not rescan.** Restart it.
- **VST3 with no CLAP.** See above.
