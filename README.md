<!-- markdownlint-disable-file MD033 -->

# SpaceCadetPinball

A fork of [k4zmu2a/SpaceCadetPinball](https://github.com/k4zmu2a/SpaceCadetPinball)
focused on an Emscripten / WebAssembly build, with both the Full Tilt
`CADET.DAT` and the 3D Pinball `PINBALL.DAT` tables bundled, mobile-friendly
touch controls, and persistent options + high scores that save in the browser.

**Play it:** <https://aburro.me/pinball/> (or
`https://aburro.me/pinball/?table=ft` / `?table=3dpb` for a specific table).

## Why this fork

The right flipper on every existing online port of 3DPB (that I know of) is
bound to `/`, which on a Spanish keyboard requires `Shift+7`, making the game
unplayable. This motivated an input rewrite, which then snowballed
into a proper web build.

## What's different from upstream

- **International keyboard support.** Bindings are stored as `SDL_Scancode`
  (physical key position) instead of `SDLK_*` keysyms, so the right flipper
  works on any layout (at least in theory).
- **Emscripten / WebAssembly build.** Custom HTML shell with on-page log,
  splash screen, and a download progress bar (since with both tables the download 
can be somewhat significant)
- **Both tables in one build.** Full Tilt (CADET.DAT) and 3D Pinball
  (PINBALL.DAT) are both bundled. A top-level `Table` menu switches between
  them at runtime, and `?table=ft|3dpb` URL parameters force a specific
  table on first load for shareable links.
- **TinySoundFont MIDI on the web.** SDL_mixer has no MIDI backend under
  Emscripten, so MIDI playback uses a baked-in GM SoundFont. 
- **Fixed-timestep physics on the web.** The browser drives the loop at
  display refresh rate, but the physics is stepped at a fixed 120 Hz via
  an accumulator. Also includes a resting-contact damping threshold in
  `maths::basic_collision` that "fixes" (more like dampens) upstream
  [issue #210](https://github.com/k4zmu2a/SpaceCadetPinball/issues/210) .
- **Persistent  high scores in the browser**. Using IDBFS, per-table high scores 
  can be saved for replayability. The high-scores are namespaced so the
  two tables keep separate leaderboards.
- **Mobile touch controls (rudimentary).** Tap the left or right half of the
screen for the flippers, hold the bottom-centre to pull the plunger.
Menus and dialogs can still be tapped normally.
- **Mobile sizing.** `object-fit: contain` so the playfield
  letterboxes instead of stretching on phones.
- **Hidden-tab pause.** Stops physics and audio when the tab isn't
  visible, so the game doesn't drain CPU/ battery while in the background.
- **Web metadata.** Viewport, theme-color, OpenGraph / Twitter
  card tags, apple-mobile-web-app capability, apple-touch-icon.

## Building the web version

The build needs `emsdk` (tested with 5.0.7) and Python 3.10+:

```sh
source /path/to/emsdk/emsdk_env.sh
cmake -B build-web -DCMAKE_TOOLCHAIN_FILE=$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake
cmake --build build-web -j
```

Game data is **not** included, but can easily be found on the Internet Archive
([here](https://archive.org/details/3d-pinball-space-cadet_multi) or [here](https://archive.org/details/full-tilt-pinball-1996-windows))
. Place `pinball.dat` (3DPB) and / or
`cadet.dat` (Full Tilt) plus their sound files in `game_resources/` before
building, they get baked into the `.data` bundle at link time. Full
Tilt sounds go under `game_resources/sound/`, the 3DPB ones in the root.
`gm.sf2` is also needed for MIDI playback. See below for how to generate
a trimmed version for lower web downloads.

### Generating a trimmed `gm.sf2`

The web build ships a SoundFont so MIDI music can be synthesised without
relying on the OS. A full General MIDI bank is roughly 3 MB, but the
bundled tracks (3DPB `pinball.mid` and Full Tilt `taba1/2/3.mds`) only
use seven instruments. `tools/trim_gm_sf2.py` keeps exactly those, drops
the rest, and produces a ~600 KB file that sounds identical, thus
saving several bytes of downloads.

You need any GM-128 SoundFont as input, like the
copy that ships with the [alula fork](https://github.com/alula/SpaceCadetPinball/blob/master/SpaceCadetPinball/gm.sf2).

```sh
# Writes to game_resources/gm.sf2 by default.
python3 tools/trim_gm_sf2.py /path/to/full_gm.sf2

# Or specify an explicit output path.
python3 tools/trim_gm_sf2.py /path/to/full_gm.sf2 game_resources/gm.sf2
```

The script reports which presets were kept and the size delta. The output is
deterministic, the same input always produces the same bytes.

Serve the resulting `bin/` over HTTP:

```sh
cd bin && python3 -m http.server 8000
```

Then open <http://localhost:8000/SpaceCadetPinball.html>.

## Credits

Based on:

- [k4zmu2a/SpaceCadetPinball](https://github.com/k4zmu2a/SpaceCadetPinball): the upstream decomp and SDL2 port.
- [alula/SpaceCadetPinball](https://github.com/alula/SpaceCadetPinball): the earlier Emscripten fork. Baseline web-build ideas (TinySoundFont
  backend approach, IDBFS link flag) originated there.

Original game by Cinematronics, Microsoft, and Maxis.

