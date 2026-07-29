# Actors and Lua

NotClon can render StepMania/OpenITG **actor folders** — the `lua/`,
`effects/`-style folders that classic modfiles use for foreground visuals:
sprites, tweened animations, masks, message-driven effects, and embedded Lua.
Stock SM5 `default.lua` ActorDef tables are preferred; legacy `default.xml`
files remain a compatibility fallback.

If a song folder has a `.sm` file next to the chart, its `#FGCHANGES` /
`#BGCHANGES` entries load these folders automatically. You can also load one
by hand:

```
notclon.exe --dir "charts/MySong" --actor effects@228
```

That loads `charts/MySong/effects/default.lua` (or its XML fallback) as a
foreground layer starting at beat 228.

## The Lua version — read this before writing any Lua

NotClon embeds **Lua 5.1.5** plus StepMania 5.1's `cmd(...)` parser extension.

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

The canonical format is an SM5.1 ActorDef table returned from `default.lua`:

```lua
return Def.ActorFrame{
    Def.Sprite{
        Texture="frame.png",
        OnCommand=function(self)
            self:x(320):y(240):sleep(0.6)
            self:decelerate(0.6):x(160):linear(0.3):diffusealpha(0)
        end,
    },
    Def.Quad{
        InitCommand=function(self) self:visible(false) end,
        FlashMessageCommand=function(self)
            self:diffusealpha(1):linear(0.5):diffusealpha(0)
        end,
    },
}
```

An unresolved chart texture uses `assets/_missing.png`; the diagnostic log
still prints the original path so the chart can be repaired.

Coordinates use StepMania's virtual **480-high, aspect-correct** screen space,
origin at the top-left: 640×480 at 4:3 and 854×480 at 16:9. Supported commands
include the tweens (`sleep`, `linear`,
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
- `GAMESTATE:GetPlayerState(...):GetPlayerOptions('ModsLevel_Song')` exposes
  live per-player options. `FromString`, direct modifier methods and
  `GAMESTATE:ApplyGameCommand(...)` mutate the CH fields the same frame.
- `GAMESTATE:LaunchAttack(start, length, mods[, player])` schedules an attack
  in song seconds and rebuilds PlayerOptions at its start and end. Its optional
  player argument follows the legacy API: 1 is P1 and 2 is P2.
- `MESSAGEMAN:Broadcast('Name')` — trigger `NameMessageCommand`s
- `SCREEN_WIDTH`, `SCREEN_HEIGHT`, `SCREEN_CENTER_X`, and `SCREEN_CENTER_Y` —
  the aspect-correct virtual-screen bounds and centres
- `DISPLAY` and `PREFSMAN` compatibility methods used during actor setup
- `Trace(value)` — append a value to the actor diagnostic log
- `self` is persistent actor userdata. Position, rotation, zoom, colour,
  tween, effect, state, getter, texture and command methods return `self` where
  StepMania does, so chains work.
- `ActorFrameTexture` setup (`SetTextureName`, `SetWidth`, `SetHeight`,
  `Enable*`, `Create`, `GetTexture`) allocates a real FBO. SM5 Lua AFTs render
  their children into that target; legacy NotITG XML AFT markers snapshot the
  framebuffer at their draw-order position. Named textures feed later
  sprites/AFTs in either case.
- Actor-valued Lua globals and `MESSAGEMAN` broadcasts cross actor-folder
  boundaries, matching the one screen-wide environment used by SM modfiles.
- `SCREENMAN:GetTopScreen():GetChild('PlayerP1'/'PlayerP2')` returns typed
  player sources with `Judgment` and `Combo` children; `ActorProxy:SetTarget`
  renders those sources or another actor subtree.
- BitmapText loads the supplied SM font INI/sheet, and ActorFrame FOV/vanishing
  point state uses a real perspective projection.

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

- Lua command functions and legacy XML `%function` bodies run for `InitCommand`,
  `OnCommand`, `*MessageCommand` and self-requeued `UpdateCommand` loops. Each
  queued event sees the beat for its
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
- Autoplay emits deterministic `Judgment` messages with `params.Player` and
  `params.TapNoteScore`; `--nobot` suppresses them. `NoteCrossed*` per-column
  messages are not emitted.
- Only actor-valued globals are mirrored between separately loaded folders;
  arbitrary scalar/table globals remain local to their Lua state.
- Player and NoteField proxy targets are captured to a full-frame texture
  before the proxy transform. Their 2D placement can overlap without a
  half-screen seam, but perspective, skew and 3D rotation transform that
  rasterized field rather than drawing the NoteField geometry again; pixels
  clipped at the outer capture edge cannot be recovered by a later proxy.
- Mirin, NotITG spline APIs, shader flags and general theme-object lookup are
  outside the stock-SM5 surface implemented here.

A template with a working actor tree, shader and modchart lives in
[`templates/modchart/`](../templates/modchart/).
