// A PLAYFIELD shader for NotClon.
//
//   notclon --dir "charts/My Song" --fxshader templates/modchart/shaders/playfield.frag
//
// Unlike a background shader, this one runs OVER the rendered frame with
// sampler0 bound to it — so it can warp, fold or tint the playfield itself.
// It replaces the frame, so whatever you don't write, you lose.
//
// Uniforms NotClon does not recognise become `fx.<name>` knobs your modchart
// can drive, same as `bg.<name>` for a background layer.

#version 330

in vec2 imageCoord;      // (0,0) BOTTOM-left here -- see README; a playfield
                         // pass indexes the frame directly, so this is the
                         // identity coordinate for sampler0
out vec4 fragColor;

uniform sampler2D sampler0;   // the rendered frame
uniform float time;
uniform float beat;
uniform vec2  resolution;
uniform vec2  field;     // the strike line, in imageCoord

// --- your knobs -------------------------------------------------------------
uniform float wobble;    // fx.wobble  — sideways ripple, in pixels
uniform float chroma;    // fx.chroma  — colour separation, in pixels
uniform float pinch;     // fx.pinch   — pull toward / push from the strike line

void main() {
    vec2 uv = imageCoord;

    // A ripple travelling up the screen, on the beat. Because `beat` is
    // fractional and nothing here accumulates, seeking anywhere gives the same
    // picture — which is what lets the editor preview match the encode.
    float w = sin(uv.y * 24.0 - beat * 6.2831853) * wobble / max(resolution.x, 1.0);

    // Pull the image toward the strike line, or push it away with a negative
    // percent. `field` follows the playfield, including any movex/movey.
    vec2 d = uv - field;
    uv += d * pinch * 0.25;
    uv.x += w;

    // Protect the playfield if you want the effect only at the edges:
    //   float guard = smoothstep(0.0, 0.3, abs(imageCoord.x - field.x));
    //   uv = mix(imageCoord, uv, guard);

    // Chromatic separation along the direction away from the strike line.
    vec2 off = normalize(d + 1e-6) * chroma / max(resolution.x, 1.0);
    vec3 col;
    col.r = texture(sampler0, uv + off).r;
    col.g = texture(sampler0, uv).g;
    col.b = texture(sampler0, uv - off).b;

    fragColor = vec4(col, 1.0);
}
