NotClon — alpha
===============

An offline Clone Hero chart renderer with OpenITG-style modcharting. It does
not touch the game: it draws the highway itself, applies mods, and pipes frames
to ffmpeg. Clone Hero has no modcharting API, so the alternative is editing
video by hand.

Nothing to install. Unzip and run.


Try it
------

The bundled template is a complete song, so this works straight away:

    notclon.exe --dir templates\modchart --preview 20

That writes preview.png. To render a video instead:

    notclon.exe --dir templates\modchart --out demo.mp4

To open the editor on it:

    notclon-editor.exe --mods templates\modchart\example.ncmod

`notclon.exe` on its own prints every option.


Your own songs
--------------

A song folder is a Clone Hero one: notes.chart plus its audio, named by CH's
stem convention (guitar/bass/rhythm/vocals/drums/keys/song/crowd, .ogg/.mp3/
.wav). Every stem found is mixed, exactly as CH layers them.

    notclon.exe --dir "path\to\My Song" --mods my.ncmod

A StepMania .sm can be converted:

    notclon.exe --import-sm "My Song\song.sm" --dir "My Song"

Start from templates\modchart — copy the pieces you want next to your chart.
docs\ covers the modchart format, the editor, shaders and actors.


What's in here
--------------

    notclon.exe          the renderer
    notclon-editor.exe   the modchart editor
    ffmpeg.exe           used for encoding and audio decoding
    SDL3.dll             window/input/audio for the editor
    assets\              highway, note and pump artwork
    docs\                the manual
    templates\modchart\  a working song to start from
    licenses\            third-party licences

ffmpeg.exe here is found in preference to any ffmpeg already on your PATH, so
the bundle behaves the same on every machine. Delete it and NotClon falls back
to whatever is installed.


Alpha
-----

Rough edges to expect:

  * A shader chain that uses feedback is exact when encoding but only
    approximate in the editor until it settles after a scrub. NotClon prints
    [FEEDBACK] when it loads one.
  * Lua in actor trees is partially wired: tweened command chains and message
    commands run, per-frame UpdateCommand stepping does not.
  * The `cover` mod parses and saves but does not render.


Licences
--------

ffmpeg.exe is a GPL build (it includes x264 and x265, which are GPL). It is a
separate program that NotClon runs — NotClon does not link it — but the binary
itself is distributed under the GPL and its licence is in licenses\. Source for
it is at https://github.com/BtbN/FFmpeg-Builds and https://ffmpeg.org.

SDL3 is zlib-licensed. Lua 5.1.5 is MIT; its COPYRIGHT is in licenses\.
Dear ImGui is MIT.
