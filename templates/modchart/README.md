# Modchart template

A complete song folder, not a fragment — it runs as-is:

```
notclon --dir templates/modchart
notclon-editor --mods templates/modchart/example.ncmod
```

```
templates/modchart/
    notes.chart            <- 60s of notes at 120bpm, with sustains
    song.ogg               <- 60s of silence, so the timeline is a real minute
    song.ini
    example.ncmod          <- the modchart, and it names the shader it drives
    lua/default.lua        <- an SM5.1 actor tree (sprites, animations)
    shaders/example.frag   <- a background shader
    shaders/playfield.frag <- a playfield shader
    chains/echo.ncfx       <- a multi-pass chain with feedback
```

The audio is silent on purpose: a chart with no audio at all gives you a
16-beat timeline to scrub, which is about eight seconds and not enough to
build anything in. A minute of silence gives you a minute.

To start your own, copy the pieces you want next to your real `notes.chart`
and audio, and point `#chart` at that folder.

Try each piece on its own:

```
notclon --dir templates/modchart --nomods
notclon --dir templates/modchart --actor lua
notclon --dir templates/modchart --bgshader shaders/example.frag
notclon --dir templates/modchart --fxchain chains/echo.ncfx
```

## Pointing at shaders from the modchart

`example.ncmod` carries its own pointers:

```
#chart templates/modchart
#bgshader shaders/example.frag
#fxshader shaders/playfield.frag
```

`#bgshader`, `#fxshader` and `#fxchain` work like `#chart`: they are comments
to anything that does not know about them, and relative paths resolve against
the **`.ncmod`'s own folder** — so a modchart and its `shaders/` travel
together, and reopening the document in the editor restores the whole effect
rather than leaving `fx.*` knobs driving nothing.

Command-line `--bgshader`/`--fxshader`/`--fxchain` still work and stack on top
of whatever the document names.

## What's here

**`example.ncmod`** — a modchart with an opening, a build, a one-beat `tilt`
stab that undoes itself with `len=`, a drop, a bar of `stealth`, and a clean
landing. Every knob is listed in [../../docs/ncmod-format.md](../../docs/ncmod-format.md).

**`lua/default.lua`** — an SM5.1 ActorDef showing the three things actors do:
tweened command functions, message-driven effects, and ordinary Lua. Load it with
`--actor lua`, or let a `.sm`'s `#FGCHANGES` load it automatically.

**`shaders/playfield.frag`** — a *playfield* shader. Where a background shader
draws behind everything, this one runs over the rendered frame and warps the
playfield itself: a beat-synced ripple, a pinch toward the strike line, and
chromatic separation. Its uniforms become `fx.wobble`, `fx.chroma` and
`fx.pinch`. Load it with `--fxshader`.

**`shaders/example.frag`** — a background shader. Beat-synced rings, a swirl,
and a gutter that dims the area around the playfield so notes stay readable.
Its `rings`, `warp` and `bright` uniforms become `bg.rings`, `bg.warp` and
`bg.bright` knobs, which `example.ncmod` drives from the first tick.

That it drives `bright` from tick 0 is the point: `bright` defaults to 0, and
an undriven knob keeps the shader's own default — so a background nobody drives
renders black. NotClon lists a shader's knobs when it loads one, and says which
are undriven.

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

Varyings: `imageCoord` and `textureCoord`.

For a **background** layer, `(0,0)` is the top-left. For a **playfield**
(`--fxshader`) or **chain** pass, `(0,0)` is the bottom-left, because those
resample the rendered frame and the frame's own first row is its bottom one --
`texture(sampler0, imageCoord)` is the identity, which is what a resampling
shader needs. `field` is handed to each pass in whichever of the two it uses,
so it always points at the strike line.

`#version 120` and `#version 330` both work, and a file with **no** `#version`
line is treated as 120 — so a shader written against older GLSL runs unchanged.

A single `--bgshader`/`--fxshader` layer is one pass with one input. For
anything that needs its own intermediate buffers, use a chain.

## Chains — multi-pass and feedback

`--fxchain` runs a sequence of passes over named buffers:

```
Blur = blur.frag   sampler0=@frame
Mix  = mix.frag    sampler0=@frame samplerBlur=Blur
out  = Mix
```

`@frame` is the rendered frame; anything else is a buffer name. Each pass
writes the buffer on its left. Any sampler your shader declares can be bound
by name, and any other unrecognised uniform — `float`, `int` or `bool` —
becomes an `fx.<name>` knob your modchart drives.

**Buffers persist between frames.** Reading a buffer written earlier in the
file gives you this frame; reading one written *later*, or one the pass writes
itself, gives you the **previous** frame. That is feedback, and it is all you
need for trails, echoes and datamosh-style block smearing:

```
Echo = echo.frag  sampler0=@frame samplerPrev=Echo
out  = Echo
```

See [chains/echo.ncfx](chains/echo.ncfx) for a working one.

The catch is real and worth knowing before you build on it: a chain with
feedback is **not** a pure function of the song position, unlike everything
else in NotClon. An encode renders every frame in order, so it is exact. The
editor is not — after you scrub, the buffers still hold what was on screen
before, and the preview is wrong until enough frames have passed. NotClon
prints `[FEEDBACK]` when it loads a chain that has any, so you always know
which kind you are looking at.

## Everything is a function of the song position

There are no timers and no accumulated state anywhere in NotClon — a frame is
computed purely from where you are in the song. That's what lets you scrub
anywhere in the editor and see exactly what the encoder will write, and it's
worth keeping in mind when writing shaders: drive things from `time` and
`beat`, never from a counter.
