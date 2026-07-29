# Importing StepMania files

```
build/notclon.exe --import-sm "path/to/song.sm" --dir "charts/My Song"
```

This converts and exits — it doesn't render anything. If the output directory
differs from the `.sm` folder, it is created and receives the source `.sm`,
referenced audio and media, plus each complete actor folder so its Lua/XML and
nested assets stay together.

Out come `notes.chart`, `song.ini`, and a `.ncmod` carrying the file's mods.
The import prints what it did:

```
Example legacy chart   [dance-single Challenge 10, 2 blocks]
  notes 510 (holds 52)   strum 466 / hopo 44 / tap 0
  bpm points 3   stops 1   last note 112.0s
  #MODS lines 185 -> 219 entries (219 scoped)
  ! stop at beat 224 (0.15s) -> wedge B 6250 over 3 ticks, residual 0 us
```

By default it takes the hardest `dance-single` chart; `--sm-diff <n>` picks a
different `#NOTES` block by index.

## Dumping a Lua/XML modfile

`#MODS` and `#ATTACKS` are already imported directly. For a modern chart whose
mods are driven from an actor tree instead, run its song-level PlayerOptions and
write the observed changes to a new `.ncmod`:

```
build/notclon.exe --dir "charts/My Song" --dump-mods "charts/My Song/dumped.ncmod"
```

The adjacent `.sm` provides the actor schedule. Use `--actor <folder>` when no
schedule is present. Changes retain their target and approach rate and are
rounded to the nearest chart tick; repeated writes to the same knob on that
tick collapse to the last one. `GAMESTATE:LaunchAttack` start/end rebuilds are
captured too. A framework that rewrites dozens of mods every update will
consequently produce a large file. Actor drawing, AFT contents and arbitrary
shader state are not `.ncmod` knobs and are not part of this dump.

## What converts

**Notes.** Taps, holds and rolls (rolls become plain holds). The four dance
panels map straight onto the first four frets — left, down, up, right become
green, red, yellow, blue. **Orange is never used**, which is the honest signal
that a chart came from four panels.

**Mods.** Both `#MODS` and `#ATTACKS` are read; they're the same format. Times,
lengths, approach rates, percentages and `No <mod>` are all handled, and
`hallway`/`distant`/`overhead` collapse onto the single `tilt` knob the way
StepMania collapses them internally.

**Timing.** BPM changes carry over directly. `#OFFSET` flips sign, because
StepMania and Clone Hero use opposite conventions, and the result is written to
both `notes.chart` and `song.ini` so the chart lines up in real Clone Hero.

**Stops** become a short, very slow BPM segment with every later note shifted
to compensate — audio-exact, usually to within a microsecond. NotClon also
freezes the highway during the stop, the way StepMania does.

**Backgrounds and effects.** `#BGCHANGES` stills and movies render behind the
highway, and `#FGCHANGES` actor folders (the `lua/` and `effects/` style
folders) are loaded and animated. See [actors-and-lua.md](actors-and-lua.md).

## What doesn't

- **Mines** are dropped — Clone Hero has no equivalent.
- **Negative stops (warps)** are dropped with a warning.
- Non-4-panel charts (`dance-double`, `pump-single`, …) warn and import
  anyway using the first four columns.
- A few mods import but don't render yet; the report lists them under
  *imported but NOT rendered* so nothing disappears silently.

## Notes vs HOPOs

Clone Hero turns closely-spaced notes into HOPOs automatically, and a dense
StepMania chart can come out mostly HOPO. The importer reads its own output
back through the real parser and reports the actual strum/HOPO/tap split, so
you can see what you got rather than guessing.

## After importing

Re-running the import **overwrites the imported `.ncmod`**. If you've hand-edited it,
keep a copy or re-apply your changes afterwards.

If you plan to use `piu` mode for a section, chart that section as **tap
notes** — see [ncmod-format.md](ncmod-format.md).
