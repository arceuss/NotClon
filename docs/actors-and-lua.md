# Actors and Lua

NotClon can render the StepMania/OpenITG **XML actor layer** — the `lua/`,
`effects/`-style folders that classic modfiles use for foreground visuals:
sprites, tweened animations, masks, message-driven effects, and embedded Lua.

If a song folder has a `.sm` file next to the chart, its `#FGCHANGES` /
`#BGCHANGES` entries load these folders automatically. You can also load one
by hand:

```
notclon.exe --dir "charts/MySong" --actor effects@228
```

That loads `charts/MySong/effects/default.xml` as a foreground layer starting
at beat 228.

## The Lua version — read this before writing any Lua

NotClon embeds **Lua 5.1.5**.

| don't use | why | use instead |
|---|---|---|
| `goto` / labels | Lua 5.2+ | restructure with `while`/`repeat` |
| `a // b` | floor division is 5.3+ | `math.floor(a / b)` |
| `&` `\|` `~` `<<` `>>` | bitwise operators are 5.3+ | `math.floor`, multiplication |
| integer subtype (`math.type`) | 5.3+ | everything is a double |
| `\z` string escape | 5.2+ | concatenate lines |
| `utf8.*` | 5.3+ | not available |

Things that *do* work, because 5.1 has them natively: the `#` length operator
and the `%` modulo operator

One compatibility note for old scripts: stock Lua 5.1.5 dropped `table.getn`
and `table.setn`, but OpenITG-era modfiles use them everywhere — so NotClon
provides them as shims. `table.getn(t)` behaves as `#t`.

## What an actor file looks like

The format is the SM 3.9 one — an `<ActorFrame>` tree where attributes are
command chains and `%function(self) ... end` attributes are Lua:

```xml
<ActorFrame> <children>
  <Layer
    File="frame.png"
    OnCommand="x,320;y,240;sleep,0.6;decelerate,0.6;x,160;linear,0.3;diffusealpha,0"
  />
  <Layer
    Type="Quad"
    InitCommand="hidden,1"
    FlashMessageCommand="diffusealpha,1;linear,0.5;diffusealpha,0"
  />
</children> </ActorFrame>
```

Coordinates are in StepMania's virtual **640×480** screen space, origin at the
top-left. Supported commands include the tweens (`sleep`, `linear`,
`accelerate`, `decelerate`, `spring`, `bouncebegin`, `bounceend`), position and
size (`x`, `y`, `addx`, `addy`, `zoom`, `zoomto`, `zoomtowidth`,
`zoomtoheight`, `rotationx/y/z`), colour (`diffuse`, `diffusealpha`, `blend`),
alignment (`horizalign`, `vertalign`), visibility (`hidden`), the actor
effects (`wag`, `bounce`, `bob`, `spin`, `pulse`, `vibrate` with
`effectmagnitude`, `effectperiod`, `effectclock,bgm`), z-buffer masking
(`zwrite`, `ztest`, `clearzbuffer` — how the classic "notes vanish behind a
frame" trick works), and `queuecommand`/`playcommand`.

Messages work the SM way: `MESSAGEMAN:Broadcast('Flash')` runs every actor's
`FlashMessageCommand`.

## What the Lua can reach

The embedded Lua exposes a deliberately small surface:

- `GAMESTATE:GetSongBeat()` / `GetSongBeatVisible()` — the beat at the
  command's scheduled instant
- `GAMESTATE:GetCurMusicSeconds()` and
  `GAMESTATE:GetSongPosition():GetMusicSeconds()` — that instant in seconds
- `GAMESTATE:GetCurrentSong():GetSongDir()` — the song folder
- `STATSMAN:GetCurStageStats()` and the player-stage-stat accessors used by
  classic modfiles. Offline rendering has no live score, so dance points are
  reported as zero.
- `GAMESTATE:ApplyGameCommand(...)` is accepted so a runtime loop survives,
  but it does not mutate NotClon's playfield. Player mods still come from the
  imported Lua `mods` tables / `.ncmod`; a command-line render reports this
  limitation once when the method is used.
- `MESSAGEMAN:Broadcast('Name')` — trigger `NameMessageCommand`s
- `SCREEN_WIDTH`, `SCREEN_HEIGHT`, `SCREEN_CENTER_X`, and `SCREEN_CENTER_Y` —
  the virtual-screen constants (`640`, `480`, `320`, and `240`)
- `DISPLAY` and `PREFSMAN` compatibility methods used during actor setup
- `Trace(value)` — append a value to the actor diagnostic log
- `self` is persistent actor userdata. Position, rotation, zoom, colour,
  tween, effect, state, getter, texture and command methods return `self` where
  StepMania does, so chains work.
- `ActorFrameTexture` setup (`SetTextureName`, `SetWidth`, `SetHeight`,
  `Enable*`, `Create`, `GetTexture`) and `Sprite:SetTexture` work for the
  screen-capture chains used by classic NotITG files.
- Lua globals persist across every actor in the same tree. Actors can register
  themselves in a table during one command and be retrieved by a later command.
- `SCREENMAN` is accepted but inert: theme-UI and ActorProxy calls are safely
  absorbed, since NotClon has no StepMania screen/player tree to manipulate.

## Background shaders

A `.frag` file can draw behind the playfield:

```
notclon --dir "charts/My Song" --bgshader shaders/example.frag
```

NotClon sets `time`, `beat`, `bpm`, `resolution`/`res`,
`textureSize`/`imageSize`, `field` (the strike line's position) and `sampler0`
every frame. **Any other uniform you declare becomes a modchart knob** named
`bg.<uniform>`, so your `.ncmod` can animate it:

```
0     *-1    400    bg.rings      # rings = 4.0 -- the percent column is /100
0     *2     100    bg.bright
```

Varyings are `imageCoord` and `textureCoord`, `(0,0)` at the top-left. Both
`#version 120` and `#version 330` link, so a shader written against older GLSL
runs unchanged. A ready-to-edit example is in
[`templates/modchart/`](../templates/modchart/).

## Playfield shaders

A background shader draws *behind* everything. A **playfield** shader runs
*over* the rendered frame, with `sampler0` bound to it — so it can warp, fold
or tint the playfield itself:

```
notclon --dir "charts/My Song" --fxshader shaders/playfield.frag
```

Same file format and the same uniforms, with two differences:

- `sampler0` is the frame you are processing, not a layer texture. The pass
  **replaces** the frame, so whatever you do not write, you lose.
- Its uniforms register as **`fx.<name>`** knobs rather than `bg.<name>`, so a
  background and a playfield shader can both have a `speed` without colliding.

Playfield shaders run before the built-in post chain (`glow`, `aberration`,
`vignette`, `desat`, `shake`), so those still apply on top.

`field` tells you where the strike line is, which is how you keep the notes
readable while the rest of the frame goes wild — mask your displacement by
distance from it:

```glsl
float guard = smoothstep(0.0, 0.3, abs(imageCoord.x - field.x));
uv = mix(imageCoord, uv, guard);      // full effect at the edges, none on the notes
```

A working example is `templates/modchart/shaders/playfield.frag`.

### What a shader layer cannot do

One layer is **one pass with one input**: `sampler0` is the frame (or the
layer's texture), and that is the only sampler bound. Effects built as a chain
of passes writing into intermediate buffers — block-matching, JPEG-style
transforms, anything declaring `samplerPrev`, `samplerBest` and friends — will
compile and link here, and then read the frame through every one of those
samplers and produce nonsense.

Frame **feedback** is the deeper limit. NotClon computes each frame purely from
where you are in the song, which is what lets the editor preview match the
encode and lets you render an arbitrary range. An effect whose output depends
on the frames before it has no defined answer at a beat you jumped straight to.

Only `float` uniforms become knobs. `int` and `bool` uniforms keep whatever the
shader gives them.

## Current limitations

Honest list, so you don't fight the tool:

- `%function` bodies run for `InitCommand`, `OnCommand`, `*MessageCommand` and
  self-requeued `UpdateCommand` loops. Each queued event sees the beat for its
  own scheduled second, not the final preview beat. Forward rendering steps the
  loop once; a cold or backward seek reloads the tree and deterministically
  replays it from activation.
- Actor mutations made by Lua are captured into the same `Seg` timelines as
  XML command chains. ActorFrameTexture contents persist while rendering
  forward; a cold seek reconstructs command state but cannot reconstruct
  pixel feedback from frames that were never rendered, so feedback begins at
  the sought frame.
- Unsupported actor methods are absorbing no-ops that return `self`, keeping
  method chains alive. Command-line renders report each distinct method once
  as `actor: unsupported Actor method: <name>`; check this before assuming a
  Lua setter took effect.
- `GAMESTATE:ApplyGameCommand` does not apply per-frame PlayerOptions; use the
  imported Lua mod tables or `.ncmod` for playfield modifiers.
- `NoteCrossed*` message broadcasts (per-column note triggers) are not emitted.
- 3D perspective on actors is approximated: `rotationx/y` render as
  foreshortening rather than true vanishing-point perspective.

A template with a working actor tree, shader and modchart lives in
[`templates/modchart/`](../templates/modchart/).
