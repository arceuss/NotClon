# Getting started

## What you need

- Windows
- **[ffmpeg](https://ffmpeg.org/) on your PATH** — NotClon uses it to decode
  song audio and to encode video. Nothing works without it.
- To build from source: CMake, Ninja, and clang.

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build
```

Everything lands in `build/` together — the two executables, `SDL3.dll`, and a
copy of the art assets. That folder is self-contained: zip it, move it, run it
anywhere.

## A song folder

NotClon reads Clone Hero song folders:

```
charts/My Song/
    notes.chart      the chart
    song.ini         metadata and the audio delay
    song.ogg         audio
    mysong.ncmod     the modchart (optional)
```

Audio follows CH's naming — `song`, `guitar`, `bass`, `rhythm`, `drums`,
`keys`, `vocals`, `crowd`, in `.ogg`/`.mp3`/`.wav`. Every stem present is mixed
together, exactly as Clone Hero layers them.

## Render a video

```
build/notclon.exe --dir "charts/My Song" --out clip.mp4
```

Some options you'll actually use:

| flag | what it does |
|---|---|
| `--from`/`--to <beat>` | render just part of the song |
| `--w`/`--h`/`--fps` | output size and frame rate (default 1920×1080 60) |
| `--enc av1` | use the GPU encoder — much faster than the default x264 |
| `--preview <beat>` | render one frame to `preview.png` and stop |
| `--speed <n>` | note speed, same scale as CH's (default 10) |

Run `build/notclon.exe` with no arguments for the full list.

`--dir` also accepts the `notes.chart` file itself, so you can drag a chart
onto the exe or tab-complete to it.

## Try random mods

No modchart? Let NotClon make one:

```
build/notclon.exe --dir "charts/My Song" --randmods --out clip.mp4
```

It picks a few mods at every section of the song and re-rolls as the music
changes. Each run prints its seed:

```
modchart: RANDOM, seed 0, 3 at a time (70 entries) -- re-run with --randseed 0 ...
```

Pass `--randseed <n>` to get that exact modchart back, and `--randcount <n>`
for more or fewer mods at once. Small numbers stay readable; large ones get
wild fast.

## Make your own modchart

Open the editor:

```
build/notclon-editor.exe
```

Use **Open chart** to load a `notes.chart`, then add mods and scrub around. The
editor draws frames with the same renderer the encoder uses, so what you see is
what you'll get. See [editor.md](editor.md) and
[ncmod-format.md](ncmod-format.md).

## Coming from StepMania?

NotClon converts `.sm` files — chart, mods, and timing:

```
build/notclon.exe --import-sm "path/to/song.sm" --dir "charts/My Song"
```

A separate output directory receives the referenced audio, media, source `.sm`
and complete actor folders automatically. Use `--dump-mods <file.ncmod>` after
importing when the modfile drives song PlayerOptions from Lua/XML instead of
`#MODS` or `#ATTACKS`.

See [importing-sm.md](importing-sm.md).
