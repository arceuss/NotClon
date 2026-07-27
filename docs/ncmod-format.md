# The `.ncmod` modchart format

A modchart is a plain text file. One line per change:

```
# tick    *approach   percent   mod       [len=<ticks>]
0         *-1         0         glow
3840      *0.5        100       drunk
7680      *2          60        tornado
11520     *-1         0         drunk
15360     *-1         50        sudden    len=192
```

| column | meaning |
|---|---|
| **tick** | when it happens. `tick = beat × Resolution` (Resolution is in the chart's `[Song]` block, usually 192, so beat 20 = tick 3840). |
| **\*approach** | how fast the value moves to its target, in full swings per second. `*1` takes one second to go 0→100%; `*4` takes a quarter second. **`*-1` snaps instantly** — the usual choice. |
| **percent** | the target. 100 is full strength. Negative is allowed and usually mirrors the effect. |
| **mod** | which knob (see below). |
| **len=** | optional. How long this line stays in force, in ticks. |

Lines starting with `#` are comments. A few of them mean something:

| line | effect |
|---|---|
| `#chart <path>` | which song the modchart belongs to. The editor writes it automatically, so opening a `.ncmod` reopens its chart too. |
| `#bgshader <path>` | load a background shader, registering its uniforms as `bg.<name>` knobs. Repeatable. |
| `#fxshader <path>` | load a playfield shader, registering `fx.<name>` knobs. Repeatable. |
| `#fxchain <path>` | load a multi-pass shader chain. |

Shader paths resolve against the **`.ncmod`'s own folder**, so a modchart and
its `shaders/` folder move together. Carrying the pointer is what makes the
document stand on its own: a `fx.wobble` line is meaningless unless the shader
declaring `wobble` is loaded, so without it, reopening the file leaves the knob
driving nothing. Every one of these is a plain comment to any other reader.

## `len=` — timed effects that undo themselves

Without `len=`, a line holds until something else changes that knob:

```
0      *-1   100   drunk        # drunk stays at 100% forever
7680   *-1   0     drunk        # ...until you turn it off here
```

With `len=`, the value reverts **on its own** when the window closes:

```
0      *-1   90    stealth              # the baseline for this section
3840   *-1   50    stealth   len=48     # dips to 50% for 48 ticks, then back to 90%
```

This is how StepMania modfiles are written — short bursts stacked on a base you
never have to restore. It's what makes rapid alternation practical:

```
3840   *-1   50    stealth   len=48
3888   *-1   100   flip      len=48
3936   *-1   50    stealth   len=48
```

## Turning a mod off

There's no "delete from here". You schedule it back to zero — a line with
`0` percent. The editor's **Off here (0%)** button does exactly that.

That's different from the per-line **enabled** checkbox in the editor, which
just mutes a line while you're working. Muted lines are saved as `#!` comments,
so other tools see them as comments.

## The knobs

Percentages, where 100 = full. Most also take negatives.

**Note motion**

| mod | effect |
|---|---|
| `drunk` | notes sway side to side in a wave along the highway |
| `tornado` | notes spiral across the lanes |
| `beat` | notes pulse sideways on every beat |
| `bumpy` | notes rise and fall over the board |
| `tipsy` | each lane bobs on its own offset |
| `wave` | notes bunch and spread along the highway |
| `boost` / `brake` | notes accelerate / decelerate as they approach |
| `boomerang` | notes fly out, come back, then arrive |
| `expand` | the note spacing pulses in and out |
| `bumpyspeed` / `tipsyspeed` | make `bumpy`/`tipsy` patterns travel instead of sitting still |

**Lane swaps**

| mod | effect |
|---|---|
| `flip` | mirrors the lanes left to right |
| `invert` | swaps lanes in a fixed scramble |
| `swaptint` | tints a displaced note toward the colour of the fret it's over. NotClon's own addition — under `flip`/`invert` a green gem sitting on the orange fret is hard to read, and this helps. |

**Rotation and size**

| mod | effect |
|---|---|
| `dizzy` | each note spins on its own |
| `confusion` | everything spins together |
| `roll` | notes tumble forward as they travel |
| `twirl` | notes rotate on their vertical axis |
| `tiny` | shrinks the notes |
| `mini` | shrinks the whole playfield |

**Camera and field**

| mod | effect |
|---|---|
| `tilt` | tips the highway. Positive lays it toward the horizon, negative rears it up. |
| `wag` | the whole playfield rocks side to side on the beat |
| `reverse` | notes come from the other end |
| `centered` | moves the strike line toward the middle |
| `scrollspeed` | note speed multiplier. **Neutral is 100, not 0** — 200 is double speed. |
| `movex` / `movey` / `movez` | slide the whole playfield |
| `hide` | hides the playfield entirely |
| `hideboard` | hides just the highway — the neck, sidebars, lane strings and beat lines — leaving the notes and frets floating. The `--playfield` flag as a knob, so a section can drop the board mid-song. |

**Visibility**

| mod | effect |
|---|---|
| `stealth` | fades notes out. Around 50% they become white silhouettes; at 100% they're gone. |
| `hidden` | notes vanish as they near the strike line |
| `sudden` | notes only appear once they're close |
| `blink` | notes strobe |
| `randomvanish` | notes blank out in a band across the highway |
| `dark` | dims the frets |

**Post effects** (applied to the finished picture)

| mod | effect |
|---|---|
| `glow` | bloom on bright areas |
| `aberration` | colour fringing at the edges |
| `vignette` | darkens the corners |
| `desat` | drains colour |
| `shake` | shakes the frame |

**Not implemented yet** — these parse and save but don't render: `cover`. The
editor labels them "(stub)".

**Shader knobs.** A background shader's uniforms appear as `bg.<name>` and a
playfield shader's as `fx.<name>`, so a modchart drives them like any other
knob. Every uniform the shader declares and NotClon does not recognise becomes
one automatically — a ported shader with ten tweakables gives you ten knobs
without any extra work.

On top of those, each shader gets **two knobs named after its file**:

| knob | effect |
|---|---|
| `<prefix><stem>` | how much of the shader applies. `0` skips the pass entirely, so a loaded shader costs nothing until you schedule it. **Undriven means fully on**, so a shader named on the command line behaves as it always did. A playfield shader crossfades at intermediate values; a background layer is a straight on/off, because overriding its blend would throw away whatever the shader meant by its own alpha. |
| `<prefix><stem>.fov` | a scale on the coordinate the shader works in. `100` is the identity; higher sees more, so the pattern shrinks. |

So `shaders/example.frag` loaded as a background gives `bg.example` and
`bg.example.fov`. This is what lets you *load* a shader without turning it on —
the editor's shader picker inserts both at tick 0 with the amount at `0`.

The percent column is still divided by 100 — write `400` for `4.0`. That works
for `int` and `bool` uniforms too: `800` sets an `int blockSize` to `8`. A
`--fxchain`'s passes register their knobs the same way. See
[actors-and-lua.md](actors-and-lua.md).

`piu` exists too. Try it and see.
