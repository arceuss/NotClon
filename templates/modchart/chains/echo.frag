// One pass of a chain (see echo.ncfx). Mixes the frame with the chain's own
// previous output, so anything that moves leaves a trail.
//
// `samplerPrev` is bound to the buffer this pass WRITES, which is what makes
// it the previous frame rather than this one.

#version 330

in vec2 imageCoord;
out vec4 fragColor;

uniform sampler2D sampler0;      // the rendered frame
uniform sampler2D samplerPrev;   // this buffer, one frame ago

// fx.decay -- how much of the last frame survives. 0 is off; near 100 smears
// into a long comet. Above 100 the feedback gains rather than fades and the
// screen saturates to white within a second, which is occasionally the point.
uniform float decay;

void main() {
    vec3 now  = texture(sampler0, imageCoord).rgb;
    vec3 past = texture(samplerPrev, imageCoord).rgb;
    // max(), not mix(): a trail should brighten where the two overlap rather
    // than wash the current frame out, and the notes stay readable through it.
    fragColor = vec4(max(now, past * clamp(decay, 0.0, 2.0)), 1.0);
}
