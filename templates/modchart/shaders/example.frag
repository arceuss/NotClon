// A background shader for NotClon.
//
//   notclon --dir "charts/My Song" --bgshader templates/modchart/shaders/example.frag
//
// Everything it needs is a uniform NotClon sets every frame, so the whole thing
// is a pure function of the song position — scrub anywhere and you get the same
// picture. Any uniform NotClon does not recognise becomes a `bg.<name>` knob
// your modchart can drive; the three at the bottom are examples.

#version 330

in vec2 imageCoord;      // (0,0) top-left of the frame, (1,1) bottom-right
out vec4 fragColor;

uniform float time;      // seconds into the song
uniform float beat;      // current beat, fractional
uniform float bpm;
uniform vec2  resolution;
uniform vec2  field;     // where the strike line is on screen, in imageCoord

// --- your knobs -------------------------------------------------------------
// Anything declared here and not on NotClon's list becomes a modchart knob.
// Write them in a .ncmod as `bg.<name>`, and remember the percent column is
// divided by 100 — so `bg.rings 400` gives rings = 4.0.
uniform float rings;     // bg.rings
uniform float warp;      // bg.warp
uniform float bright;    // bg.bright   <- starts at 0, so drive it or see black

void main() {
    // Centre the coordinates and fix the aspect so circles stay circular.
    vec2 uv = imageCoord * 2.0 - 1.0;
    uv.x *= resolution.x / max(resolution.y, 1.0);

    float r = length(uv);
    float a = atan(uv.y, uv.x);

    // Concentric rings pulsing on the beat. `beat` is fractional, so
    // fract(beat) ramps 0->1 once per beat and this lands on the music without
    // any timers or state.
    float pulse = 1.0 - fract(beat);
    float band  = sin(r * max(rings, 1.0) * 6.2831853 - time * 2.0 + pulse * 3.0);
    band = smoothstep(0.2, 1.0, band);

    // A little swirl, strongest at the edges.
    float swirl = sin(a * 3.0 + r * warp * 6.0 - time);

    vec3 col = mix(vec3(0.05, 0.02, 0.15),
                   vec3(0.25, 0.55, 1.00), band * 0.7 + swirl * 0.15);

    // Fade toward the playfield so the notes stay readable. `field` follows the
    // strike line, including any movex/movey the modchart applies.
    float gutter = smoothstep(0.0, 0.35, abs(imageCoord.x - field.x));
    col *= mix(0.25, 1.0, gutter);

    fragColor = vec4(col * bright, 1.0);
}
