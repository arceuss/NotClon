# NotClon

Make Clone Hero modchart videos.

Clone Hero has no official modcharting API. If you want a CH modchart, the
only route is video editing — recording gameplay and warping it frame by frame
in an editor, which is slow, lossy, and can only push pixels around after the
fact. NotClon aims to make that far less hellish: it *is* the renderer. It
loads a CH chart, applies StepMania/OpenITG-style mods (drunk, tornado, flip,
stealth, camera tilts, and so on) to the actual notes and highway in 3D, and
encodes the result straight to MP4 through ffmpeg. Because the playfield is
real geometry rather than recorded footage, you can mess with it in ways video
editing never could — per-note motion, the board tilting and rotating, notes
that vanish and reappear — and it stays pixel-sharp at any resolution.

Nothing is screen-recorded and nothing runs in real time — a 4K60 render is
just a slower loop. A GUI editor plays the song and previews with the exact
same renderer the encoder uses, so what you scrub is what you get.

## What it does

- Reproduces Clone Hero's 5-fret highway 1:1 (notes, sustains, frets, the works)
- OpenITG `ArrowEffects` mods, driven by a simple text modchart format (`.ncmod`)
- A GUI editor for authoring modcharts: audio playback, scrubbing, snap, live sliders
- Imports StepMania `.sm` files — notes, `#ATTACKS`/`#MODS`, stops — into playable CH charts
- Renders the SM/OpenITG XML actor layer (foreground effects, Lua)
- Encodes with x264 or NVENC (h264/hevc/av1)

## Requirements

- Windows
- [ffmpeg](https://ffmpeg.org/) on your PATH (audio decoding + video encoding)
- To build: CMake, Ninja, and clang

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build
```

Everything lands in `build/` — executables, DLLs, and assets together. You can
zip that folder and run it anywhere.

## Use

```
build/notclon.exe                        # prints all options
build/notclon-editor.exe                 # the editor; open a chart from the UI
```

Render a video:

```
build/notclon.exe --dir "charts/REM III" --out clip.mp4
```

Convert a StepMania file (chart, mods, and timing come across):

```
build/notclon.exe --import-sm "path/to/song.sm" --dir "charts/MySong"
```

When `--dir` is somewhere other than the `.sm` folder, the importer creates it
and copies the `.sm`, referenced audio/media, and complete scheduled actor
folders before writing the converted chart.

Dump a Lua/XML modfile's live song-level PlayerOptions into a `.ncmod`:

```
build/notclon.exe --dir "charts/MySong" --dump-mods "charts/MySong/dumped.ncmod"
```

The adjacent `.sm` supplies its actor schedule; `--actor <folder>` can select a
folder explicitly. Every PlayerOptions change is rounded to the nearest chart
tick, so a modfile that rewrites many values every update can produce a large
but directly loadable `.ncmod`. Scheduled `GAMESTATE:LaunchAttack` start/end
changes are included in the same stream.

A song folder is laid out like a Clone Hero song: `notes.chart`, `song.ini`,
audio named the CH way (`song.ogg`, `guitar.ogg`, …), plus a `.ncmod` modchart.
The editor's **Open chart** button loads a `notes.chart`; a `.ncmod` remembers
which chart it belongs to, so opening one brings back the whole session.

No modchart handy? `--randmods` generates one, re-rolled at every section of
the song, and prints its seed so you can get the same roll back.

## Docs

- [Getting started](docs/getting-started.md) — build, render, first steps
- [The `.ncmod` format](docs/ncmod-format.md) — the modchart file and every knob
- [The editor](docs/editor.md) — keys and workflow
- [Importing StepMania files](docs/importing-sm.md) — what converts and what doesn't
- [Actors and Lua](docs/actors-and-lua.md) — the XML/Lua effect layer and background shaders

A copy-and-edit starting point lives in [`templates/modchart/`](templates/modchart/):
a modchart, an actor tree and a background shader, each of which runs as-is.
