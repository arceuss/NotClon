# Modchart template

A working starting point. Copy this folder's contents next to your
`notes.chart` and start editing.

```
charts/My Song/
    notes.chart
    song.ogg
    example.ncmod          <- the modchart
    lua/default.xml        <- an actor tree (sprites, animations)
    shaders/example.frag   <- a background shader
```

Try each piece on its own first:

```
notclon --dir "charts/My Song" --mods example.ncmod
notclon --dir "charts/My Song" --actor lua
notclon --dir "charts/My Song" --bgshader shaders/example.frag --mods example.ncmod
notclon --dir "charts/My Song" --fxshader shaders/playfield.frag --mods example.ncmod
```

## What's here

**`example.ncmod`** — a modchart with an opening, a build, a one-beat `tilt`
stab that undoes itself with `len=`, a drop, a bar of `stealth`, and a clean
landing. Every knob is listed in [../../docs/ncmod-format.md](../../docs/ncmod-format.md).

**`lua/default.xml`** — an actor tree showing the three things actors do:
tweened command chains, message-driven effects, and a Lua chunk. Load it with
`--actor lua`, or let a `.sm`'s `#FGCHANGES` load it automatically.

**`shaders/playfield.frag`** — a *playfield* shader. Where a background shader
draws behind everything, this one runs over the rendered frame and warps the
playfield itself: a beat-synced ripple, a pinch toward the strike line, and
chromatic separation. Its uniforms become `fx.wobble`, `fx.chroma` and
`fx.pinch`. Load it with `--fxshader`.

**`shaders/example.frag`** — a background shader. Beat-synced rings, a swirl,
and a gutter that dims the area around the playfield so notes stay readable.
Its `rings`, `warp` and `bright` uniforms become `bg.rings`, `bg.warp` and
`bg.bright` knobs — uncomment the last block of `example.ncmod` to drive them.

Note that `bright` starts at 0. An undriven knob keeps the shader's own
default, so a shader whose brightness defaults to zero renders black until your
modchart turns it up. NotClon warns about undriven knobs when it loads one.

## Shader uniforms

NotClon sets these every frame. Anything else you declare becomes a knob your
modchart can drive — `bg.<name>` for a background layer, `fx.<name>` for a
playfield layer.

| uniform | type | value |
|---|---|---|
| `time` | float | seconds into the song |
| `beat` | float | current beat, fractional |
| `bpm` | float | current tempo |
| `resolution`, `res` | vec2 | output size in pixels |
| `textureSize`, `imageSize` | vec2 | same, for ported shaders that expect them |
| `field` | vec2 | the strike line's position, in `imageCoord` |
| `sampler0` | sampler2D | the layer's texture (white if none) |

Varyings: `imageCoord` and `textureCoord`, both `(0,0)` at the **top-left**.

`#version 120` and `#version 330` both work, so a shader written against older
GLSL runs unchanged. Note that one layer is one pass with one input: multi-pass
effects that expect their own intermediate buffers will link but not work.

## Everything is a function of the song position

There are no timers and no accumulated state anywhere in NotClon — a frame is
computed purely from where you are in the song. That's what lets you scrub
anywhere in the editor and see exactly what the encoder will write, and it's
worth keeping in mind when writing shaders: drive things from `time` and
`beat`, never from a counter.
