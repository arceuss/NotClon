# The editor

```
build/notclon-editor.exe
build/notclon-editor.exe --dir "charts/My Song"        # open a song straight away
build/notclon-editor.exe --mods mysong.ncmod           # ...or a modchart, which knows its chart
```

With no arguments it opens on a blank document — use **Open chart** to pick a
`notes.chart`. Since a `.ncmod` records the song it belongs to, opening the
modchart later brings the whole session back.

The preview is drawn by the same renderer that encodes video, so what you scrub
past is what lands in the MP4.

## Keys

| key | action |
|---|---|
| `Space` | play / pause |
| `←` `→` | change snap division (1/1 up to 1/192) |
| `↑` `↓` | step the playhead one snap division — **`↑` moves forward**, as in Moonscraper |
| mouse wheel over the preview | same as `↑`/`↓` |

Stepping snaps to the grid rather than nudging from wherever you stopped, so
repeated presses stay exact.

## Adding mods

The **Add at playhead** panel builds one line at a time: pick the mod, the
percent, the approach rate, and optionally a length. **Place at** decides where
the line lands:

- *exact tick* — wherever the playhead is
- *snap grid* — the nearest snap division (usually what you want; a mod that
  fires a few ticks off the beat reads as a glitch)
- *nearest note* — snapped to the closest note in the chart

**Add** places it. **Off here (0%)** places a line that turns the same mod back
off at the playhead.

## Tuning

Select a line in the table to open its inspector. Tick, mod, percent, approach
and length are all draggable, and the preview re-renders as you drag — that's
the point of tuning in place instead of deleting and re-adding. Dragging the
tick re-sorts the list, and the selection follows the line.

**Go to** jumps the playhead to the selected line. **Duplicate here** copies it
to the playhead. **Delete** removes it.

The **enabled** checkbox mutes a line while keeping its values, for A/B-ing.
Muted lines are saved as `#!` comments. To turn a mod off *in the chart*, add a
0% line instead — see [ncmod-format.md](ncmod-format.md).

## The knob panel

Shows every mod's live value at the playhead, so you can see what's actually in
force at any moment — including values still easing toward their target.

## Audio

The song plays through your default output device, and the playhead is read
*out of* the audio device rather than being pushed at it, so the two can't
drift apart. Any audio Clone Hero accepts works; all stems present in the
folder are mixed.
