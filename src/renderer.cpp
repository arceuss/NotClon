// Implementation of the shared render core. See renderer.h for why it exists.

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

#include "renderer.h"
#include "actor.h"
#include "background.h"
#include "gh3_tables.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace nc {

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
const char* SCENE_VS = R"(#version 330
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
out vec2 vUV;
out vec4 vCol;
out float vEngineDistance;
uniform mat4 uMVP;
// Rigid-body offset for the whole playfield. With --playfield the board is
// gone, so nothing anchors the frets to the bottom of the frame and the
// assembly can be placed anywhere. A modchart will drive this later.
uniform vec3 uOffset;
uniform float uCurve;
uniform vec4 uFadePlane;
void main() {
    vec3 p = aPos + uOffset;
    if (uCurve != 0.0) {
        float radius = sign(uCurve)*15.0 - uCurve*((15.0-2.0)/3.0);
        float d = abs(p.x);
        float dOverRadius = d/radius;
        float sinc = d < 0.001
            ? 1.0-(dOverRadius*dOverRadius)/6.0
            : sin(dOverRadius)/d;
        p = vec3(sinc*(radius+p.y)*p.x,
                 cos(d/radius)*(radius+p.y)-radius, p.z);
    }
    vUV = aUV; vCol = aCol;
    vEngineDistance = dot(vec4(p,1.0),uFadePlane);
    gl_Position = uMVP * vec4(p, 1.0);
}
)";

const char* SCENE_FS = R"(#version 330
in vec2 vUV;
in vec4 vCol;
in float vEngineDistance;
out vec4 oCol;
uniform sampler2D uTex;
uniform float uPremul;   // 1 for Unity sprite blending (One, 1-SrcAlpha)
uniform vec2 uFadeRange;
void main() {
    vec4 c = texture(uTex, vUV) * vCol;
    if (uFadeRange.y > uFadeRange.x)
        c.a *= 1.0-smoothstep(uFadeRange.x,uFadeRange.y,vEngineDistance);
    if (c.a < 0.002) discard;
    if (uPremul > 0.5) c.rgb *= c.a;
    oCol = c;
}
)";

// ITG's glow pass -- Sprite.cpp:536-541 draws the sprite a second time under
// DISPLAY->SetTextureModeGlow(), which keeps the texture's ALPHA and replaces
// its RGB with a flat colour. That is the silhouette of the arrow filled with
// solid white, and it is what makes stealth/hidden/sudden read as a fade
// instead of a hard on/off (see the comment above GetAlpha in mods.h).
//
// A separate program rather than a branch in SCENE_FS: every existing layer's
// float codegen goes through SCENE_FS, and the pinned hashes are the only thing
// proving the rest of the frame is untouched.
const char* NOTE_GLOW_FS = R"(#version 330
in vec2 vUV;
in vec4 vCol;
out vec4 oCol;
uniform sampler2D uTex;
void main() {
    float a = texture(uTex, vUV).a * vCol.a;
    if (a < 0.002) discard;
    oCol = vec4(vCol.rgb * a, a);   // premultiplied, to match BLEND_SPRITE
}
)";

// CloneHero/SustainGlow reduced. The whammy texture is created black and
// NotClon's bot never whammies, so every tex2Dlod in GetWhammyAtPosition
// returns 0 and gain(0, 0.5) == 0 -- which pins whammyAmount at 1.0 and
// collapses innerDist/outerDist to 0.5 / 1.0. Two of the three remaining
// inputs are exactly linear in z and ride in the vertex stream; only smin()
// is per-pixel, because it is not separable in (a, e).
const char* SUSTAIN_GLOW_FS = R"(#version 330
in vec2 vUV;     // x: lateral -1..1   y: endDistance, exact per vertex
in vec4 vCol;    // rgb: NoteColors.Sustains[lane]   a: world z * 10
out vec4 oCol;
void main() {
    float a = 1.0 - abs(vUV.x);
    float e = vUV.y;
    const float k = 0.6;
    float h = clamp(0.5 + 0.5 * (e - a) / k, 0.0, 1.0);   // smin
    float d = mix(e, a, h) - k * h * (1.0 - h);
    d = clamp(d, 0.0, 1.0);
    d = min(vCol.a, d);
    oCol = vec4(vCol.rgb * smoothstep(0.5, 1.0, d), 1.0);
}
)";

// The actor layer's own program. Ortho over SM's virtual 640x480, y DOWN --
// actor coordinates put (0,0) at the top-left, which is why the projection
// flips y rather than the geometry doing it.
const char* ACTOR_VS = R"(#version 330
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
layout(location=3) in vec2 aLocal;
out vec2 vUV; out vec4 vCol; out vec2 vLocal;
uniform float uDepth;
uniform float uInvertY;
// Half the virtual screen. Height is pinned at 480 (240 here); width follows
// the output aspect -- SM5 and current NotITG both run widescreen this way,
// and charts scale themselves by SCREEN_WIDTH/640. Defaults keep 4:3.
uniform vec2 uVirtHalf = vec2(320.0, 240.0);
void main() {
    vUV = aUV; vCol = aCol; vLocal = aLocal;
    gl_Position = vec4(aPos.x / uVirtHalf.x - 1.0,
                       (1.0 - aPos.y / uVirtHalf.y) * uInvertY,
                       uDepth, 1.0);
}
)";

const char* ACTOR_FS = R"(#version 330
in vec2 vUV; in vec4 vCol; in vec2 vLocal;
out vec4 oCol;
uniform sampler2D uTex;
uniform float uHasTex;
uniform float uGlow;
uniform vec2 uFadeX;
uniform vec2 uFadeY;
void main() {
    vec4 c = vCol;
    if (uHasTex > 0.5) {
        vec4 texel = texture(uTex, vUV);
        if (uGlow > 0.5) c.a *= texel.a;
        else c *= texel;
    }
    if (uFadeX.x > 0.0) c.a *= clamp(vLocal.x / uFadeX.x, 0.0, 1.0);
    if (uFadeX.y > 0.0) c.a *= clamp((1.0-vLocal.x) / uFadeX.y, 0.0, 1.0);
    if (uFadeY.x > 0.0) c.a *= clamp(vLocal.y / uFadeY.x, 0.0, 1.0);
    if (uFadeY.y > 0.0) c.a *= clamp((1.0-vLocal.y) / uFadeY.y, 0.0, 1.0);
    // SM's fixed-function alpha test: glAlphaFunc(GL_GREATER, 0.01)
    // (RageDisplay_OGL.cpp:1881). It applies to MASK draws too -- that is what
    // lets a mostly-transparent mask image stamp depth only where it has ink.
    // The fixture mask is a frame-shaped border at alpha 1-63/255; the
    // window it leaves open starts at the judgment line.
    if (c.a <= 0.01) discard;
    oCol = c;
}
)";

// `cover`: ITG's one and only consumer of m_fCover is
// BrightnessOverlay::SetActualBrightness (Background.cpp:958-984), which sets
// the overlay quads to GetBrightnessColor(1 - cover). That helper
// (Background.cpp:124-138) blends a black quad at alpha (1-brightness) under a
// grey clamp quad at alpha ClampOutputPercent -- which is 0 in every fallback
// theme in the collection (openitg .../fallback/metrics.ini:2726, and SM5's
// _fallback:259), so the whole thing reduces to RGBA(0,0,0,1-brightness).
// PREFSMAN->m_fBGBrightness (the player's background-dimming preference, 0.8 by
// default) multiplies brightness; NotClon has no such preference and shows
// backgrounds undimmed, so its neutral base is 1 and the quad is simply black
// at alpha == cover. Own program, not SCENE_FS -- the pinned hashes are what
// prove the rest of the frame is untouched, and this pass is skipped whole at 0.
const char* COVER_FS = R"(#version 330
out vec4 oCol;
uniform float uAlpha;
void main() { oCol = vec4(0.0, 0.0, 0.0, uAlpha); }
)";

// The PIU playfield's program. Its geometry is authored in StepMania's virtual
// 640x480 space with y DOWN from the top-left. uMVP normally supplies that
// ortho projection, but also lets the Player actor transform the whole field.
// Shares SCENE_VS's attribute layout so it can reuse vao_/vbo_.
const char* PIU_VS = R"(#version 330
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
out vec2 vUV; out vec4 vCol;
uniform mat4 uMVP;
void main() {
    vUV = aUV; vCol = aCol;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* PIU_FS = R"(#version 330
in vec2 vUV; in vec4 vCol;
out vec4 oCol;
uniform sampler2D uTex;
void main() {
    vec4 c = texture(uTex, vUV) * vCol;
    if (c.a < 0.002) discard;
    c.rgb *= c.a;                      // premultiplied, as every sprite here is
    oCol = c;
}
)";

// GH3 highway sprites. The vertex shader is PIU_VS plus one varying: the
// vscreen y, which the fragment shaders need because GH3's highway fade
// (gHighwayEndFade..gHighwayStartFade, 305..335 in the 1P vscreen) is
// PER-PIXEL -- smoothstep((y-end)/(start-end)) in the game's shaders. Folding
// it into vertex alpha is only right when the geometry is tessellated finely
// (the whammy strip); on a single tall quad like a sidebar or lane string it
// bleeds the 30px band down the sprite's whole length, which reads as the
// element "fading too early".
const char* GH3_SPRITE_VS = R"(#version 330
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
out vec2 vUV; out vec4 vCol; out float vScrY;
uniform mat4 uMVP;
void main() {
    vUV = aUV; vCol = aCol; vScrY = aPos.y;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* GH3_SPRITE_FS = R"(#version 330
in vec2 vUV; in vec4 vCol; in float vScrY;
out vec4 oCol;
uniform sampler2D uTex;
uniform vec2 uFade;    // (endY, startY) of the fade-in band; equal = disabled
void main() {
    vec4 c = texture(uTex, vUV) * vCol;
    // The highway fade-in band (gHighwayEndFade..gHighwayStartFade,
    // 305..335): gems and fretbars materialise softly over the first 30px
    // below the horizon instead of popping in. Sidebars and strings do NOT
    // take it -- the real capture shows the sidebar at full brightness at
    // y=305, so their material templates lack the fade params; their tips
    // die by their own art alone.
    if (uFade.y > uFade.x) {
        float t = clamp((vScrY - uFade.x) / (uFade.y - uFade.x), 0.0, 1.0);
        c.a *= t * t * (3.0 - 2.0 * t);
    }
    if (c.a < 0.002) discard;
    c.rgb *= c.a;                      // premultiplied, as every sprite here is
    oCol = c;
}
)";

// GH3's whammy (sustain) tail. Ported from the game's own compiled D3D9
// shaders -- the `WhammyBar` technique of material WhammyBar_UI in
// DATA/FXFILES/MaterialLibrary.bin.xen, shader indices 683/684 (solid) and
// 685/686 (glow). See devdocs/spec/gh3-shaders.md for the disassembly.
//
// The geometry is built in screen space on the CPU here, so the parts of
// GH3's vertex shader that place the strip (the lane centreline and the
// smoothstep highway fade) are folded into the vertices; what genuinely
// needs a shader is the per-pixel work below, which is why CH's ribbons
// never needed one.
const char* GH3_WHAMMY_FS = R"(#version 330
in vec2 vUV; in vec4 vCol; in float vScrY;
out vec4 oCol;
uniform sampler2D uTex;
uniform vec2 uEdge;      // PS c1.xy: (alpha scale, profile slope)
uniform int uGlow;
void main() {
    vec4 t = texture(uTex, vUV);
    vec4 c;
    if (uGlow == 0) {
        // PS 684. U runs ACROSS the ribbon, so the tile's bottom row is the
        // tube's cross-section shading; V is 1 down the body and ramps only
        // over the rounded tip. Material colour and vertex colour are white,
        // so every colour comes from the texture.
        c = vec4(t.rgb * vCol.rgb, t.a * vCol.a);
    } else {
        // PS 686. A triangular cross-strip profile smoothstepped over
        // 1/(1+slope) of the width -- with slope -0.3 that saturates across
        // the middle 70% and falls off over the outer 15% each side. RGB is
        // ONE fixed texel of the same sheet (0.9, 0.9), not the tile, which
        // is what makes the glow a flat lane-coloured bloom.
        float prof = 1.0 - abs(2.0 * vUV.x - 1.0);
        float g = clamp(prof / (1.0 + uEdge.y), 0.0, 1.0);
        float edge = g * g * (3.0 - 2.0 * g);
        // PS 686 applies the vertex colour twice, to both rgb and alpha.
        c = vec4(texture(uTex, vec2(0.9)).rgb * vCol.rgb * vCol.rgb,
                 edge * vUV.y * t.a * vCol.a * vCol.a * uEdge.x);
    }
    // The highway fade, per pixel like the game's own shaders. The glow pass
    // multiplies (vcol.a * fade) into alpha twice, so the band squares there.
    float ft = clamp((vScrY - 305.0) / 30.0, 0.0, 1.0);
    float fade = ft * ft * (3.0 - 2.0 * ft);
    c.a *= (uGlow == 0) ? fade : fade * fade;
    if (c.a < 0.002) discard;
    // Premultiplying folds GH3's SRC_ALPHA,ONE additive glow into ONE,ONE.
    c.rgb *= c.a;
    oCol = c;
}
)";

// The source-engine modes use their original meshes and material textures.
// Material indices are exported as a vertex attribute because both source
// projects assign several Unity materials to one mesh.
const char* ENGINE_VS = R"(#version 330
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in float aMaterial;
out vec3 vNormal;
out vec3 vWorld;
out vec2 vUV;
out float vEngineDistance;
flat out int vMaterial;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform float uCurve;
uniform vec4 uFadePlane;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    if (uCurve != 0.0) {
        float radius = sign(uCurve)*15.0-uCurve*((15.0-2.0)/3.0);
        float d = abs(world.x);
        float dOverRadius = d/radius;
        float sinc = d < 0.001
            ? 1.0-(dOverRadius*dOverRadius)/6.0
            : sin(dOverRadius)/d;
        world.xy = vec2(sinc*(radius+world.y)*world.x,
                        cos(d/radius)*(radius+world.y)-radius);
    }
    vNormal = normalize(transpose(inverse(mat3(uModel))) * aNormal);
    vWorld = world.xyz;
    vUV = aUV;
    vEngineDistance = dot(world,uFadePlane);
    vMaterial = int(aMaterial + 0.5);
    gl_Position = uMVP * world;
}
)";

const char* ENGINE_FS = R"(#version 330
in vec3 vNormal;
in vec3 vWorld;
in vec2 vUV;
in float vEngineDistance;
flat in int vMaterial;
out vec4 oCol;
uniform sampler2D uTex;
uniform sampler2D uTex2;
uniform sampler2D uTex3;
uniform sampler2D uAo;
uniform vec2 uAoSize;
uniform float uUseAo;
uniform vec3 uColor;
uniform vec3 uCameraPos;
uniform float uAlpha;
uniform float uScroll;
uniform int uKind;
uniform int uMaterialFilter;
uniform vec2 uFadeRange;
uniform float uMaterialState;
uniform vec3 uRandom;
uniform float uTime;
uniform float uWaviness;
uniform float uGroove;
uniform int uHitLightCount;
uniform vec3 uHitLightPos[5];
uniform vec3 uHitLightColor[5];

float hlslMod(float x, float y) {
    return x-y*trunc(x/y);
}

vec2 hlslMod(vec2 x, float y) {
    return x-vec2(y)*trunc(x/vec2(y));
}

vec2 gradientNoiseDirection(vec2 p) {
    p = hlslMod(p,289.0);
    float x = hlslMod((34.0*p.x+1.0)*p.x,289.0)+p.y;
    x = hlslMod((34.0*x+1.0)*x,289.0);
    x = fract(x/41.0)*2.0-1.0;
    return normalize(vec2(x-floor(x+0.5),abs(x)-0.5));
}

float gradientNoise(vec2 uv, float scale) {
    vec2 p = uv*scale;
    vec2 ip = floor(p);
    vec2 fp = fract(p);
    float d00 = dot(gradientNoiseDirection(ip),fp);
    float d01 = dot(gradientNoiseDirection(ip+vec2(0,1)),fp-vec2(0,1));
    float d10 = dot(gradientNoiseDirection(ip+vec2(1,0)),fp-vec2(1,0));
    float d11 = dot(gradientNoiseDirection(ip+vec2(1,1)),fp-vec2(1,1));
    fp = fp*fp*fp*(fp*(fp*6.0-15.0)+10.0);
    return mix(mix(d00,d01,fp.y),mix(d10,d11,fp.y),fp.x)+0.5;
}

vec4 overlayBlend(vec4 base, vec4 blend) {
    vec4 low = 2.0*base*blend;
    vec4 high = 1.0-2.0*(1.0-base)*(1.0-blend);
    return mix(low,high,step(vec4(0.5),base));
}

vec3 hueDegrees(vec3 inputColor, float offset) {
    vec4 K = vec4(0.0,-1.0/3.0,2.0/3.0,-1.0);
    vec4 P = mix(vec4(inputColor.bg,K.wz),vec4(inputColor.gb,K.xy),
                 step(inputColor.b,inputColor.g));
    vec4 Q = mix(vec4(P.xyw,inputColor.r),vec4(inputColor.r,P.yzx),
                 step(P.x,inputColor.r));
    float D = Q.x-min(Q.w,Q.y);
    float E = 1e-4;
    float V = D == 0.0 ? Q.x : Q.x+E;
    vec3 hsv = vec3(abs(Q.z+(Q.w-Q.y)/(6.0*D+E)),D/(Q.x+E),V);
    float hue = hsv.x+offset/360.0;
    hsv.x = hue < 0.0 ? hue+1.0 : (hue > 1.0 ? hue-1.0 : hue);
    vec4 K2 = vec4(1.0,2.0/3.0,1.0/3.0,3.0);
    vec3 P2 = abs(fract(hsv.xxx+K2.xyz)*6.0-K2.www);
    return hsv.z*mix(K2.xxx,clamp(P2-K2.xxx,0.0,1.0),hsv.y);
}

vec3 yargSH(vec3 n) {
    vec4 n4 = vec4(n,1.0);
    vec3 gi = vec3(
        dot(vec4(8.85245754e-5,-0.00562569872,-0.0147181451,0.168671688),n4),
        dot(vec4(0.000139226191,0.0412316322,-0.0231422633,0.209188499),n4),
        dot(vec4(0.000238774664,0.126589864,-0.0398332104,0.284822226),n4));
    vec4 vB = n.xyzz*n.yzzx;
    gi += vec3(
        dot(vec4(6.95378185e-5,-0.0115551241,0.0378950816,-0.00017671744),vB),
        dot(vec4(0.000111176807,-0.0184599347,0.0531771407,-0.000256823143),vB),
        dot(vec4(0.000199187634,-0.0330873542,0.0676154494,-0.000369618792),vB));
    gi += vec3(0.0240153596,0.0329115465,0.0381791368)*
          (n.x*n.x-n.y*n.y);
    return gi;
}

vec3 yargBrdf(vec3 diffuseColor, vec3 specularColor, float smoothness,
              vec3 emission, float occlusion) {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(vec3(-0.187260912,0.719388326,-0.668889967));
    vec3 V = normalize(uCameraPos-vWorld);
    vec3 H = normalize(L+V);
    float pr = 1.0-smoothness;
    float roughness = max(pr*pr,0.0078125);
    float roughness2 = max(roughness*roughness,0.00006103515625);
    float normalizationTerm = roughness*4.0+2.0;
    float NoH = clamp(dot(N,H),0.0,1.0);
    float LoH = clamp(dot(L,H),0.0,1.0);
    float d = NoH*NoH*(roughness2-1.0)+1.00001;
    float specularTerm = roughness2/
        (d*d*max(0.1,LoH*LoH)*normalizationTerm);
    vec3 lightColor = vec3(1.8204746,1.8001208,2.0);
    vec3 radiance = lightColor*max(dot(N,L),0.0);
    vec3 direct = (diffuseColor+specularColor*specularTerm)*radiance;
    // YARG fret hit light (EffectLight mode Normal): one URP point light
    // per recently hit fret. The light's culling mask (87) excludes the
    // layer-7 track and trims, so kinds 6/7 skip the loop. Attenuation is
    // URP RealtimeLights.hlsl DistanceAttenuation: 1/d^2 windowed by
    // (1-(d^2/r^2)^2)^2 with range 0.3. Zero lights = untouched output.
    if (uKind != 6 && uKind != 7) {
        for (int i = 0; i < uHitLightCount; ++i) {
            vec3 toLight = uHitLightPos[i]-vWorld;
            float distSqr = max(dot(toLight,toLight),1e-6);
            vec3 Lp = toLight*inversesqrt(distSqr);
            float factor = distSqr*(1.0/0.09);
            float window = clamp(1.0-factor*factor,0.0,1.0);
            window *= window;
            vec3 Hp = normalize(Lp+V);
            float NoHp = clamp(dot(N,Hp),0.0,1.0);
            float LoHp = clamp(dot(Lp,Hp),0.0,1.0);
            float dp = NoHp*NoHp*(roughness2-1.0)+1.00001;
            float specP = roughness2/
                (dp*dp*max(0.1,LoHp*LoHp)*normalizationTerm);
            vec3 radP = uHitLightColor[i]*(window/distSqr)*
                        max(dot(N,Lp),0.0);
            direct += (diffuseColor+specularColor*specP)*radP;
        }
    }
    float NoV = clamp(dot(N,V),0.0,1.0);
    float fresnel = pow(1.0-NoV,4.0);
    float reflectivity = max(specularColor.r,
                             max(specularColor.g,specularColor.b));
    float grazingTerm = clamp(smoothness+reflectivity,0.0,1.0);
    vec3 envFactor = (1.0/(roughness2+1.0))*
                     mix(specularColor,vec3(grazingTerm),fresnel);
    const vec3 environment = vec3(0.181303382,0.226914212,0.307360709);
    vec3 indirect = yargSH(N)*diffuseColor+environment*envFactor;
    float screenOcclusion = mix(1.0,texture(uAo,gl_FragCoord.xy/uAoSize).r,
                                uUseAo);
    float indirectOcclusion = min(occlusion,screenOcclusion);
    float directOcclusion = 0.75+0.25*screenOcclusion;
    return indirect*indirectOcclusion+direct*directOcclusion+emission;
}

vec3 yargMetallic(vec3 albedo, float metallic, float smoothness,
                  vec3 emission, float occlusion) {
    vec3 diffuseColor = albedo*(0.96*(1.0-metallic));
    vec3 specularColor = mix(vec3(0.04),albedo,metallic);
    return yargBrdf(diffuseColor,specularColor,smoothness,emission,occlusion);
}

vec3 yargSpecular(vec3 albedo, vec3 specularColor, float smoothness,
                  vec3 emission, float occlusion) {
    float reflectivity = max(specularColor.r,
                             max(specularColor.g,specularColor.b));
    return yargBrdf(albedo*(1.0-reflectivity),specularColor,smoothness,
                    emission,occlusion);
}

// Unity ShaderGraph Blend node, mode 21 (VividLight): color burn below 0.5,
// dodge above, then a final lerp by Opacity. Epsilon denominators keep the
// shine texture's black texels from producing NaNs (Unity emits inf there;
// the alpha-gated opacity hides it, GLSL mix would propagate it).
vec3 vividLight(vec3 base, vec3 blend, float opacity) {
    vec3 burn = 1.0-(1.0-base)/max(2.0*blend,vec3(1e-4));
    vec3 dodge = base/max(1.0-2.0*(blend-0.5),vec3(1e-4));
    // The dodge path is singular as blend -> 1 (the fret shine sits right on
    // it); the result feeds an albedo, so keep it in range.
    return clamp(mix(base,mix(burn,dodge,step(0.5,blend)),opacity),
                 0.0,1.0);
}

// Gameplay/Notes/RectangularNote.shadergraph. BaseColor chain:
// Blend_Normal(BaseMap, max(noise, MinDarkness), opacity 1)  -> the noise wins
// Blend_Multiply(that, _Color)
// Blend_VividLight(that, ShineMap, _Shine_Amount(0.1) * ShineMap.A)
// Emission = _Color * SampleGradient(noise.r) * _Emission(0.1), the gradient
// being black@0.2794 -> white@1 (SampleGradient node 63e932 over the noise).
vec3 yargRectangularNote() {
    vec2 center = vec2(0.25,-0.06)+0.1*uRandom.yz;
    vec2 uv = vUV*vec2(1.1,1.3);
    vec2 delta = uv-center;
    vec2 q = uv+vec2(delta.y,-delta.x)*dot(delta,delta)*center+
             vec2(0.0,0.52);
    float warp = (0.43+(gradientNoise(q,-2.8)-6.47)*
                         (-2.82-0.43)/(10.94-6.47))*(-1.06);
    float jitter = 1.01+(uRandom.x+3.18)*(-1.11-1.01)/(13.24+3.18);
    vec2 p = q+vec2(warp)+vec2(uTime*(1.0+jitter)*0.4);
    float noise = gradientNoise(p,-1.11);
    vec4 dark = max(texture(uTex3,vec2(noise)),vec4(0.15));
    vec3 colored = dark.rgb*uColor;
    vec4 shine = texture(uTex2,vUV);
    vec3 baseColor = vividLight(colored,shine.rgb,0.1*shine.a);
    const float gradientStart = 18311.0/65535.0;
    float emissionMask = clamp((dark.r-gradientStart)/(1.0-gradientStart),
                               0.0,1.0);
    // Smoothness block is unconnected in the graph -> ShaderGraph's inline
    // default 0.5 (the material's _Smoothness override is never referenced).
    return yargMetallic(baseColor,0.0,0.5,uColor*emissionMask*0.1,1.0);
}

float pow5(float x) {
    float x2 = x*x;
    return x2*x2*x;
}

float disneyDiffuse(float nv, float nl, float lh, float perceptualRoughness) {
    float fd90 = 0.5 + 2.0*lh*lh*perceptualRoughness;
    float lightScatter = 1.0 + (fd90-1.0)*pow5(1.0-nl);
    float viewScatter = 1.0 + (fd90-1.0)*pow5(1.0-nv);
    return lightScatter*viewScatter;
}

float smithJointGGX(float nl, float nv, float roughness) {
    float lambdaV = nl*(nv*(1.0-roughness)+roughness);
    float lambdaL = nv*(nl*(1.0-roughness)+roughness);
    return 0.5/(lambdaV+lambdaL+1e-5);
}

float ggxTerm(float nh, float roughness) {
    float a2 = roughness*roughness;
    float d = (nh*a2-nh)*nh+1.0;
    return (1.0/3.14159265359)*a2/(d*d+1e-7);
}

vec3 fresnelTerm(vec3 f0, float cosA) {
    return f0+(vec3(1.0)-f0)*pow5(1.0-cosA);
}

vec3 moonStandard(vec3 albedo, float metallic, float smoothness,
                  vec3 emission) {
    const vec3 ambient = vec3(0.212,0.227,0.259);
    const vec3 dielectric = vec3(0.220916301);
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCameraPos-vWorld);
    float nv = abs(dot(N,V));
    float pr = 1.0-smoothness;
    float roughness = max(pr*pr,0.002);
    vec3 specColor = mix(dielectric,albedo,metallic);
    vec3 diffColor = albedo*(1.0-0.220916301)*(1.0-metallic);
    vec3 direct = vec3(0.0);
    for (int i = 0; i < 2; ++i) {
        vec3 L = vec3(0.0,i == 0 ? -0.6427876097 : 0.6427876097,
                      -0.7660444431);
        vec3 H = normalize(L+V);
        float nl = clamp(dot(N,L),0.0,1.0);
        float nh = clamp(dot(N,H),0.0,1.0);
        float lh = clamp(dot(L,H),0.0,1.0);
        float diffuse = disneyDiffuse(nv,nl,lh,pr)*nl;
        float specular = smithJointGGX(nl,nv,roughness)*
                         ggxTerm(nh,roughness)*3.14159265359;
        specular = max(0.0,sqrt(max(1e-4,specular))*nl);
        direct += diffColor*diffuse+
                  specular*fresnelTerm(specColor,lh);
    }
    return diffColor*ambient+direct+emission;
}

vec3 moonLambert(vec3 albedo, vec3 emission) {
    vec3 N = normalize(vNormal);
    vec3 L0 = vec3(0.0,-0.6427876097,-0.7660444431);
    vec3 L1 = vec3(0.0, 0.6427876097,-0.7660444431);
    float nl = max(dot(N,L0),0.0)+max(dot(N,L1),0.0);
    return albedo*(vec3(0.212,0.227,0.259)+vec3(nl))+emission;
}

void main() {
    if (uMaterialFilter >= 0 && vMaterial != uMaterialFilter) discard;
    vec4 c = vec4(1.0);
    if (uKind == 0 || (uKind >= 8 && uKind <= 10)) {
        if (vMaterial == 0) {
            c = vec4(moonStandard(vec3(0.8),0.0,0.5,vec3(1.0)),1.0);
        } else if (vMaterial == 1) {
            float tap = uKind == 10 ? 1.0 : 0.0;
            c = vec4(moonStandard(uColor,1.0,mix(0.214,0.646,tap),vec3(0.0)),
                     mix(1.0,0.459,tap));
        } else if (uKind == 9) {
            c = vec4(moonLambert(vec3(1.0),vec3(0.0)),1.0);
        } else {
            c = vec4(moonStandard(vec3(0.0),0.0,0.5,vec3(0.0)),1.0);
        }
    } else if (uKind >= 14 && uKind <= 17) { // Moonscraper SP note/marker
        bool hopo = uKind == 15;
        bool tap = uKind == 16;
        bool marker = uKind == 17;
        if (vMaterial == 0) {
            c = hopo
              ? vec4(moonLambert(vec3(1.0),vec3(0.0)),1.0)
              : vec4(moonStandard(vec3(0.0),0.0,0.5,vec3(0.0)),1.0);
        } else if (vMaterial == 1) {
            c = vec4(moonStandard(vec3(0.0),0.0,0.5,vec3(0.0)),1.0);
        } else if (vMaterial == 2) {
            c = vec4(moonStandard(vec3(0.22,0.92,1.0),0.0,0.5,
                                  vec3(0.0)),1.0);
        } else if (vMaterial == 3) {
            vec3 base = marker ? vec3(0.1544118,0.96501005,1.0) : uColor;
            c = vec4(moonStandard(base,1.0,tap ? 0.646 : 0.214,
                                  vec3(0.0)),tap ? 0.459 : 1.0);
        } else {
            c = vec4(moonStandard(vec3(0.8),0.0,0.5,vec3(1.0)),1.0);
        }
    } else if (uKind == 11 || uKind == 13 ||
               uKind == 18 || uKind == 19) { // Moonscraper open note
        vec3 base = vMaterial == 0 ? vec3(0.08,0.8,0.1)
                  : vMaterial == 1 ? vec3(0.0)
                  : vMaterial == 2 ? vec3(0.533777,0.088109,0.540437)
                  : vec3(1.0);
        if ((uKind == 18 || uKind == 19) && vMaterial == 2) {
            base = uKind == 18 ? vec3(1.0,0.0,0.972414)
                               : vec3(1.0,0.1544118,0.9766736);
            vec3 glowTex = vec3(0.0,0.2965517,1.0);
            vec3 albedo = base + glowTex*0.138;
            c = vec4(moonLambert(albedo,glowTex*albedo),1.0);
        } else if (uKind == 13 && vMaterial == 2) {
            base += vec3(0.174);
            c = vec4(moonLambert(base,base),1.0);
        } else {
            c = vec4(moonStandard(base,0.0,0.5,vec3(0.0)),1.0);
        }
    } else if ((uKind >= 1 && uKind <= 4) || uKind == 12) { // YARG note variants
        // vMaterial is the UNITY SUBMESH index: the loader permutes ufbx
        // slots into face first-use order, which is how Unity's importer
        // numbers submeshes (engine_mesh.cpp). RectangularTheme.prefab
        // m_Materials, in that order:
        //   NormalNote [NoteMetal, NoteMiddle, NoteGlow]
        //   HOPONote   [NoteMetal, NoteHOPOGlow, NoteGlow, NoteMiddle]
        //   TapNote    [NoteMetal, NoteMiddle, NoteGlow, NoteTapCenter]
        // so submesh 0 = Note_Sides (the big wrap), 2 = Note_Base, and the
        // lane-coloured NoteMiddle lands on the Note_Color top pad.
        // NoteGlow.mat is visually identical to NoteMetal.mat (white,
        // metallic 0, smoothness 0.5, _EMISSION_DISABLED), so one metal
        // branch covers submeshes 0+2; the runtime SetColorWithEmission
        // colours only the NoteMiddle slot {white under SP, gold metal}.
        // Kinds 4/12: slot 0 = Open(Hopo)Metal, slot 1 = Open(Hopo)Middle.
        bool starPower = uMaterialState > 0.5;
        bool metalSlot = ((uKind >= 1 && uKind <= 3) &&
                          (vMaterial == 0 || vMaterial == 2)) ||
                         ((uKind == 4 || uKind == 12) && vMaterial == 0);
        if (metalSlot) {
            // SP gold #FFD700, srgbToLinear'd like every Color property.
            vec3 metalColor = starPower ? vec3(1.0,0.6795425,0.0)
                                        : vec3(1.0);
            vec3 art = (uKind == 12 ? texture(uTex2,vUV).rgb
                                    : texture(uTex,vUV).rgb)*metalColor;
            float metallic = (uKind == 4 || uKind == 12) ? 1.0 : 0.0;
            float smoothness = (uKind == 4 || uKind == 12) ? 0.245 : 0.5;
            c = vec4(yargMetallic(art,metallic,smoothness,
                                  vec3(0.0),1.0),1.0);
        } else if ((uKind == 1 && vMaterial == 1) ||
            (uKind == 2 && vMaterial == 3) ||
            (uKind == 3 && vMaterial == 1)) {
            c = vec4(yargRectangularNote(),1.0);
        } else if (uKind == 2 && vMaterial == 1) {
            // NoteHOPOGlow: no BaseMap (white), no EmissionMap, _EMISSION with
            // _EmissionColor (sqrt(2))^3; SetColorWithEmission does not reach it.
            c = vec4(yargMetallic(vec3(1.0),0.0,0.0,
                                  vec3(1.4142135),1.0),1.0);
        } else if (uKind == 3 && vMaterial == 3) {
            // NoteTapCenter: _SHINE, ShineAmount 0.2, colored with x0
            // multiplier -> _Color = lane/SP color, emission stays black.
            vec3 base = texture(uTex,vUV).rgb*uColor;
            vec4 shine = texture(uTex2,vUV);
            vec3 albedo = vividLight(base,shine.rgb,0.2*shine.a);
            c = vec4(yargMetallic(albedo,0.0,0.0,vec3(0.0),1.0),1.0);
        } else if (uKind == 4) {
            vec3 art = texture(uTex,vUV).rgb;
            // OpenMiddle: _EMISSION with EmissionMap = the same Note_Full.png,
            // _Color = color, _EmissionColor = color x8.
            c = vec4(yargMetallic(art*uColor,0.0,0.0,
                                  art*uColor*8.0,1.0),1.0);
        } else {
            // OpenHopoMiddle: realColor = color + (1,1,1) (Addition 1),
            // EmissionMap = Note_Full_HOPO.png, x8. uColor already carries
            // srgbToLinear(srgb + 1) -- NoteGroup.cs:82 adds BEFORE Unity's
            // colorspace conversion at SetColor time.
            vec3 realColor = uColor;
            vec3 art = texture(uTex,vUV).rgb;
            vec3 emissionMap = texture(uTex2,vUV).rgb;
            c = vec4(yargMetallic(art*realColor,0.0,0.0,
                                  emissionMap*realColor*8.0,1.0),1.0);
        }
    } else if (uKind == 5) {          // YARG fret
        vec4 art = texture(uTex, vUV);
        if (vMaterial == 0) {
            // RectangularFretOuter: VividLight(_Color*BaseTex,
            //   lerp(Hue(_Color,+20 deg), white, 0.1) * ShineMap, ShineMap.A)
            vec3 base = art.rgb*uColor;
            vec4 shine = texture(uTex2,vUV);
            vec3 shineColor = mix(hueDegrees(uColor,20.0),vec3(1.0),0.1)*
                              shine.rgb;
            vec3 albedo = vividLight(base,shineColor,shine.a);
            c = vec4(yargMetallic(albedo,0.0,0.0,vec3(0.0),1.0),1.0);
        } else if (vMaterial == 1) {
            // RectangularFretInner: BaseColor = lerp(MainTex, _Color, Fade),
            // Emission = lerp(black, _Color, Fade) * 5 (the graph's inline
            // Color node is (0,0,0,0)). Glows lane colour only while held.
            vec3 albedo = mix(art.rgb,uColor,uMaterialState);
            vec3 emission = 5.0*uMaterialState*uColor;
            c = vec4(yargMetallic(albedo,0.0,0.0,emission,1.0),1.0);
        } else {
            // FretMetal: plain YargLit, emission disabled.
            c = vec4(yargMetallic(art.rgb,0.0,0.0,vec3(0.0),1.0),1.0);
        }
    } else if (uKind == 6) {          // YARG default highway material
        // Track.shadergraph: baseUV = uv*(1,3); L2 scrolls at 0.9x, L4 at 1x;
        // both through TimedHorizontalWavyUV (uv.x += sin(50*(uv.y+t*0.1))*A).
        // color = lerp(lerp(L1, L2tex*L2Color, L2tex.a*L2Color.a),
        //              L4tex*L4color, L4tex.a*L4color.a)
        // (the lerp factors are the multiplied textures' own alpha channels),
        // edge fade = smoothstep(0,.5,u)*smoothstep(1,.5,u) as a scalar over
        // all channels, occlusion = FadeTex (indirect only). Solo/starpower
        // states are never set offline.
        vec2 baseUV = vUV*vec2(1.0,3.0);
        float u = vUV.x;
        vec2 l2uv = baseUV+vec2(0.0,uScroll*0.9);
        vec2 l4uv = baseUV+vec2(0.0,uScroll);
        if (uWaviness != 0.0) {
            l2uv.x += sin(50.0*(l2uv.y+uTime*0.1))*0.01*uWaviness;
            l4uv.x += sin(50.0*(l4uv.y+uTime*0.1))*0.01*uWaviness;
        }
        vec4 l2tex = texture(uTex2,l2uv);
        vec4 l4tex = texture(uTex3,l4uv);
        // Groove state (ScoreMultiplier == MaxMultiplier): the layer colors
        // lerp toward the HighwayPreset groove palette, and the wavy UV
        // amount fades in with it.
        vec4 l1c = mix(vec4(0.0588235,0.0588235,0.0588235,1.0),
                       vec4(0.0,0.0352941,0.2,1.0),uGroove);
        vec4 l2c = mix(vec4(0.29411766,0.29411766,0.29411766,0.1490196),
                       vec4(0.1372549,0.2,0.7686275,0.1490196),uGroove);
        vec4 l4c = mix(vec4(0.3301887,0.3301887,0.3301887,1.0),
                       vec4(0.172549,0.2862745,0.6196078,1.0),uGroove);
        vec3 base = mix(l1c.rgb,
                        l2tex.rgb*l2c.rgb,
                        l2tex.a*l2c.a);
        base = mix(base,
                   l4tex.rgb*l4c.rgb,
                   l4tex.a*l4c.a);
        float edge = smoothstep(0.0,0.5,u)*smoothstep(1.0,0.5,u);
        float occlusion = texture(uTex,baseUV+vec2(0.0,-0.5)).r;
        c = vec4(yargMetallic(base*edge,0.0,0.0,vec3(0.0),occlusion),
                 1.0);
    } else if (uKind == 7) {          // YARG track trim
        vec3 albedo = texture(uTex,vUV).rgb*uColor;
        c = vec4(yargMetallic(albedo,0.0,0.0,vec3(0.0),1.0),1.0);
    } else {                          // plain lit material
        c = vec4(yargMetallic(uColor,0.0,0.0,vec3(0.0),1.0),1.0);
    }
    c.a *= uAlpha;
    if (uFadeRange.y > uFadeRange.x)
        c.a *= 1.0-smoothstep(uFadeRange.x,uFadeRange.y,vEngineDistance);
    if (c.a < 0.002) discard;
    if (uKind != 6) c.rgb *= c.a;
    oCol = c;
}
)";

const char* ENGINE_GLOW_FS = R"(#version 330
flat in int vMaterial;
out vec4 oCol;
uniform int uMaterialFilter;
uniform float uGlowPower;
uniform float uGlowAlpha;
uniform vec3 uGlowColor;
void main() {
    if (vMaterial != uMaterialFilter) discard;
    oCol = vec4(uGlowColor*uGlowPower,uGlowAlpha);
}
)";

const char* YARG_EFFECT_FS = R"(#version 330
in vec2 vUV;
in vec4 vCol;
in float vEngineDistance;
out vec4 oCol;
uniform sampler2D uTex;
uniform vec3 uEmission;
uniform float uVisibility;
uniform float uSweepTime;
uniform vec2 uFadeRange;

float sweepMask(float t, float v) {
    float f = 0.0;
    if (t < 1.0/3.0)
        f = (1.0-cos(3.0*3.14159265*t))*0.5;
    else if (t < 2.0/3.0)
        f = 1.0;
    else if (t < 1.0)
        f = (1.0+cos(3.0*3.14159265*t+2.0/3.0))*0.5;

    float p = (t < 1.0/3.0 || t > 2.0/3.0) ? 0.0 : 3.0*t-1.0;
    float g = 0.0;
    if (v <= 3.0*p && (1.0-v) <= 3.0*(1.0-p))
        g = (1.0+cos(3.1415*(v+1.0-3.0*p)))*0.5;
    return 0.25*f+0.75*g;
}

void main() {
    vec4 texel = texture(uTex,vUV);
    float a = texel.a*vCol.a*vCol.a*uVisibility;
    if (uSweepTime >= 0.0) a *= sweepMask(uSweepTime,vUV.y);
    if (uFadeRange.y > uFadeRange.x)
        a *= 1.0-smoothstep(uFadeRange.x,uFadeRange.y,vEngineDistance);
    if (a < 0.002) discard;
    oCol = vec4(texel.rgb*vCol.rgb+uEmission,a);
}
)";

const char* YARG_NORMAL_FS = R"(#version 330
in vec3 vNormal;
out vec4 oCol;
void main() { oCol = vec4(normalize(vNormal),1.0); }
)";

const char* YARG_AO_ESTIMATE_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uNormal;
uniform sampler2D uDepth;
uniform vec2 uSize;

vec3 positionAt(vec2 uv, float depth) {
    float aspect = uSize.x/uSize.y;
    return vec3((2.0*uv.x-1.0)*25.0*aspect,
                -depth,(2.0*uv.y-1.0)*25.0);
}

void main() {
    vec2 uv = gl_FragCoord.xy/uSize;
    float rawDepth = texture(uDepth,uv).r;
    if (rawDepth >= 1.0) {
        oCol = vec4(0.0,0.5,0.5,0.5);
        return;
    }
    float depth = 0.01+39.99*rawDepth;
    vec3 normal = texture(uNormal,uv).xyz;
    vec3 center = positionAt(uv,depth);
    vec2 pixel = uv*uSize;
    const float A[4] = float[4](0.0,0.33984375,0.75390625,0.56640625);
    const float B[4] = float[4](0.9296875,0.76171875,
                                0.13333330,0.015625);
    float accumulated = 0.0;
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
        float noise = fract(52.9829189*fract(dot(
            pixel+float(sampleIndex)*vec2(2.083,4.867),
            vec2(0.06711056,0.00583715))));
        float z = fract(A[sampleIndex]+noise)*2.0-1.0;
        float theta = (B[sampleIndex]+noise)*6.28318530718;
        float radial = sqrt(max(0.0,1.0-z*z));
        vec3 q = vec3(radial*cos(theta),radial*sin(theta),z);
        q *= sqrt(float(sampleIndex+1)/4.0);
        q = dot(q,-normal) < 0.0 ? q : -q;
        q *= 0.25;
        float aspect = uSize.x/uSize.y;
        vec2 sampleUv = clamp(uv+vec2(q.x/(50.0*aspect),q.z/50.0),
                              vec2(0.0),vec2(1.0));
        float sampleDepth = 0.01+39.99*texture(uDepth,sampleUv).r;
        float inside = abs(depth-sampleDepth) < 0.25 ? 1.0 : 0.0;
        vec3 delta = positionAt(sampleUv,sampleDepth)-center;
        accumulated += max(dot(delta,normal)-0.004*depth,0.0)/
                       (dot(delta,delta)+0.0001)*inside;
    }
    accumulated *= 0.25;
    float falloff = 1.0-depth/100.0;
    float occlusion = pow(clamp(accumulated*0.5*falloff*falloff/4.0,
                                0.0,1.0),0.6);
    oCol = vec4(occlusion,normal*0.5+0.5);
}
)";

const char* YARG_AO_BLUR_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform vec2 uDirection;

void addTap(vec2 uv, float offset, float gaussian, vec3 centerNormal,
            inout float value, inout float weight) {
    vec4 sampleValue = texture(uTex,uv+uDirection*uTexel*offset);
    vec3 sampleNormal = sampleValue.gba*2.0-1.0;
    float bilateral = smoothstep(0.8,1.0,dot(centerNormal,sampleNormal));
    float tapWeight = gaussian*bilateral;
    value += sampleValue.r*tapWeight;
    weight += tapWeight;
}

void main() {
    vec4 center = texture(uTex,vUV);
    vec3 centerNormal = center.gba*2.0-1.0;
    float value = 0.0;
    float weight = 0.0;
    addTap(vUV,0.0,0.2270270270,centerNormal,value,weight);
    addTap(vUV, 1.3846153846,0.3162162162,centerNormal,value,weight);
    addTap(vUV,-1.3846153846,0.3162162162,centerNormal,value,weight);
    addTap(vUV, 3.2307692308,0.0702702703,centerNormal,value,weight);
    addTap(vUV,-3.2307692308,0.0702702703,centerNormal,value,weight);
    oCol = vec4(value/max(weight,0.000001),center.gba);
}
)";

const char* YARG_AO_FINAL_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uTex;
uniform vec2 uTexel;

void addTap(vec2 offset, vec3 centerNormal,
            inout float value, inout float weight) {
    vec4 sampleValue = texture(uTex,vUV+offset*uTexel);
    vec3 sampleNormal = sampleValue.gba*2.0-1.0;
    float tapWeight = smoothstep(0.8,1.0,dot(centerNormal,sampleNormal));
    value += sampleValue.r*tapWeight;
    weight += tapWeight;
}

void main() {
    vec4 center = texture(uTex,vUV);
    vec3 centerNormal = center.gba*2.0-1.0;
    float value = center.r;
    float weight = 1.0;
    addTap(vec2(-1.0,-1.0),centerNormal,value,weight);
    addTap(vec2( 1.0,-1.0),centerNormal,value,weight);
    addTap(vec2(-1.0, 1.0),centerNormal,value,weight);
    addTap(vec2( 1.0, 1.0),centerNormal,value,weight);
    oCol = vec4(1.0-value/max(weight,0.000001));
}
)";

const char* MOON_OCCLUDER_FS = R"(#version 330
out vec4 oCol;
uniform float uAlpha;
void main() { oCol = vec4(0.0,0.0,0.0,uAlpha); }
)";

const char* MOON_BLUR_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform vec2 uDirection;
uniform float uShift;
uniform float uAlpha;
void main() {
    if (uShift < 0.0) {
        oCol = texture(uTex,vUV)*uAlpha;
        return;
    }
    vec4 blurred = vec4(0.0);
    for (int r = 0; r < 4; ++r) {
        vec2 offset = uDirection*uTexel*uShift*float(r);
        blurred += texture(uTex,vUV+offset);
        blurred += texture(uTex,vUV-offset);
    }
    oCol = blurred*(0.6/8.0);
}
)";

const char* YARG_BLOOM_PREFILTER_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uTex;
void main() {
    vec4 source = texture(uTex,vUV);
    vec3 color = min(source.rgb*source.a,vec3(65472.0));
    float brightness = max(color.r,max(color.g,color.b));
    float soft = clamp(brightness-0.5,0.0,1.0);
    soft = soft*soft/(2.0+1e-4);
    float contribution = max(brightness-1.0,soft)/max(brightness,1e-4);
    oCol = vec4(max(color*contribution,vec3(0.0)),1.0);
}
)";

const char* YARG_BLOOM_DOWN_H_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uTex;
uniform vec2 uTexel;
void main() {
    float x = uTexel.x*2.0;
    vec3 color = texture(uTex,vUV+vec2(-4.0*x,0.0)).rgb*0.01621622;
    color += texture(uTex,vUV+vec2(-3.0*x,0.0)).rgb*0.05405405;
    color += texture(uTex,vUV+vec2(-2.0*x,0.0)).rgb*0.12162162;
    color += texture(uTex,vUV+vec2(-1.0*x,0.0)).rgb*0.19459459;
    color += texture(uTex,vUV).rgb*0.22702703;
    color += texture(uTex,vUV+vec2( 1.0*x,0.0)).rgb*0.19459459;
    color += texture(uTex,vUV+vec2( 2.0*x,0.0)).rgb*0.12162162;
    color += texture(uTex,vUV+vec2( 3.0*x,0.0)).rgb*0.05405405;
    color += texture(uTex,vUV+vec2( 4.0*x,0.0)).rgb*0.01621622;
    oCol = vec4(color,1.0);
}
)";

const char* YARG_BLOOM_DOWN_V_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uTex;
uniform vec2 uTexel;
void main() {
    float y = uTexel.y;
    vec3 color = texture(uTex,vUV+vec2(0.0,-3.23076923*y)).rgb*0.07027027;
    color += texture(uTex,vUV+vec2(0.0,-1.38461538*y)).rgb*0.31621622;
    color += texture(uTex,vUV).rgb*0.22702703;
    color += texture(uTex,vUV+vec2(0.0, 1.38461538*y)).rgb*0.31621622;
    color += texture(uTex,vUV+vec2(0.0, 3.23076923*y)).rgb*0.07027027;
    oCol = vec4(color,1.0);
}
)";

const char* YARG_BLOOM_UP_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uHigh;
uniform sampler2D uLow;
void main() {
    vec3 highMip = texture(uHigh,vUV).rgb;
    vec3 lowMip = texture(uLow,vUV).rgb;
    oCol = vec4(mix(highMip,lowMip,0.4),1.0);
}
)";

// Gameplay/Sustain.shadergraph: two SustainWave sub-graph instances over the
// SustainLine ribbon (u = remaining world-unit length, v = 1 -> 0 across).
// wave = IsActive ? amp*cos(2*pi*(t*LineSpeed - posWS.y)/LineFreq)/2 : 0 with
// LineSpeed 2, LineFreq 0.875; amp = Min + Whammy*(Max-Min) and whammy is
// always 0 for a renderer, so amp = Min: +0.2 (SustainSecondary.png) and
// -0.1333 (Sustain.png). squeeze = 1 - 0.6667*(1-cos(2*pi*3*t))/2.
// alpha = (A+B).a * saturate(u); BaseColor = _Color*(A+B).rgb;
// Emission = _EmissionColor (colour x1 waiting, x3 while hitting).
// Open sustains use SustainLine_Full instead: BaseColor = _Color flat,
// alpha = min(MainTex.a, 1), same emission.
const char* YARG_SUSTAIN_FS = R"(#version 330
uniform sampler2D uTex;
uniform sampler2D uTex2;
uniform float uEmissionScale;
uniform float uOpen;
uniform float uActive;
uniform float uTime;
uniform vec2 uFadeRange;
in vec2 vUV;
in vec4 vCol;
in float vEngineDistance;
out vec4 oCol;
vec4 sustainWave(sampler2D tex, float amp, float squeeze) {
    float wave = uActive > 0.5
        ? amp*cos(6.2831853*(uTime*2.0 - 0.01)/0.875)*0.5 : 0.0;
    float v = clamp(squeeze*wave + 2.0*vUV.y - 0.5, 0.0, 1.0);
    return texture(tex, vec2(vUV.x, v));
}
void main() {
    vec4 c;
    if (uOpen > 0.5) {
        c = vec4(vCol.rgb + vCol.rgb*uEmissionScale,
                 min(texture(uTex, vUV).a, 1.0)*vCol.a);
    } else {
        float squeeze = 1.0-0.6667*(1.0-cos(6.2831853*3.0*uTime))*0.5;
        vec4 sum = sustainWave(uTex2, 0.2, squeeze) +
                   sustainWave(uTex, -0.1333, squeeze);
        c = vec4(vCol.rgb*sum.rgb + vCol.rgb*uEmissionScale,
                 sum.a*clamp(vUV.x,0.0,1.0)*vCol.a);
    }
    if (uFadeRange.y > uFadeRange.x)
        c.a *= 1.0-smoothstep(uFadeRange.x,uFadeRange.y,vEngineDistance);
    if (c.a < 0.002) discard;
    oCol = c;
})";

// Custom/Beatline.shader + highways.hlsl: alpha = pow(1-(2u-1)^2, 0.5) *
// BeatLine.A * Color.a -- a semicircular profile shaving the quad's side
// edges; BaseColor = tex * Color. Transparent, straight alpha.
const char* YARG_BEATLINE_FS = R"(#version 330
uniform sampler2D uTex;
uniform vec2 uFadeRange;
in vec2 vUV;
in vec4 vCol;
in float vEngineDistance;
out vec4 oCol;
void main() {
    vec4 texel = texture(uTex, vUV);
    float profile = sqrt(max(1.0-(2.0*vUV.x-1.0)*(2.0*vUV.x-1.0), 0.0));
    float a = profile*texel.a*vCol.a;
    if (uFadeRange.y > uFadeRange.x)
        a *= 1.0-smoothstep(uFadeRange.x,uFadeRange.y,vEngineDistance);
    if (a < 0.002) discard;
    oCol = vec4(texel.rgb*vCol.rgb, a);
})";

// Resources/HighwaysAlphaMask.shader: YARG's highway elements do NOT fade
// individually. This pass re-renders every renderer (except FadeExclude
// layers -- the sustains) with alpha = fade(z-distance from camera) into an
// R8 target using BlendOp Max, and the uber post pass takes
// min(highway.a, mask). fadeStart/fadeEnd are the camera-forward projections
// of ZeroFadePosition-FadeLength / ZeroFadePosition, exactly YARG's own
// threshold-vs-measurement mix.
const char* YARG_MASK_FS = R"(#version 330
uniform vec2 uFadeRange;
in float vEngineDistance;
out vec4 oCol;
void main() {
    float a = 1.0;
    if (uFadeRange.y > uFadeRange.x)
        a = 1.0-smoothstep(uFadeRange.x,uFadeRange.y,vEngineDistance);
    oCol = vec4(a);
})";

const char* LINEAR_COMPOSE_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uTex;
uniform sampler2D uBloom;
uniform sampler2D uMask;
uniform sampler2D uGrain;
uniform vec2 uGrainScale;
uniform vec2 uGrainOffset;
uniform float uAlpha;
uniform float uPost;
vec3 linearToSrgb(vec3 c) {
    vec3 low = c*12.92;
    vec3 high = 1.055*pow(max(c,vec3(0.0)),vec3(1.0/2.4))-0.055;
    return mix(high,low,lessThanEqual(c,vec3(0.0031308)));
}

vec3 mulRows(vec3 r0, vec3 r1, vec3 r2, vec3 v) {
    return vec3(dot(r0,v),dot(r1,v),dot(r2,v));
}

float acesSaturation(vec3 rgb) {
    float lo = min(rgb.r,min(rgb.g,rgb.b));
    float hi = max(rgb.r,max(rgb.g,rgb.b));
    return (max(hi,1e-4)-max(lo,1e-4))/max(hi,1e-2);
}

float acesYc(vec3 rgb) {
    float k = rgb.b*(rgb.b-rgb.g)+rgb.g*(rgb.g-rgb.r)+
              rgb.r*(rgb.r-rgb.b);
    float chroma = sqrt(max(k,0.0));
    return (rgb.r+rgb.g+rgb.b+1.75*chroma)/3.0;
}

float acesHue(vec3 rgb) {
    if (rgb.r == rgb.g && rgb.g == rgb.b) return 0.0;
    float hue = degrees(atan(sqrt(3.0)*(rgb.g-rgb.b),
                             2.0*rgb.r-rgb.g-rgb.b));
    return hue < 0.0 ? hue+360.0 : hue;
}

float acesSigmoid(float x) {
    float t = max(1.0-abs(x/2.0),0.0);
    float y = 1.0+(x >= 0.0 ? 1.0 : -1.0)*(1.0-t*t);
    return y/2.0;
}

float acesGlow(float yc, float gain, float mid) {
    if (yc <= (2.0/3.0)*mid) return gain;
    if (yc >= 2.0*mid) return 0.0;
    return gain*(mid/yc-0.5);
}

vec3 acesTonemap(vec3 color) {
    vec3 aces = mulRows(
        vec3(0.4397010,0.3829780,0.1773350),
        vec3(0.0897923,0.8134230,0.0967616),
        vec3(0.0175440,0.1115440,0.8707040),color);
    float saturation = acesSaturation(aces);
    float glowShape = acesSigmoid((saturation-0.4)/0.2);
    aces *= 1.0+acesGlow(acesYc(aces),0.05*glowShape,0.08);

    float hue = acesHue(aces);
    float centeredHue = hue > 180.0 ? hue-360.0 : hue;
    float hueWeight = smoothstep(0.0,1.0,
                                  1.0-abs(2.0*centeredHue/135.0));
    hueWeight *= hueWeight;
    aces.r += hueWeight*saturation*(0.03-aces.r)*(1.0-0.82);

    vec3 acescg = max(vec3(0.0),mulRows(
        vec3(1.4514393161,-0.2365107469,-0.2149285693),
        vec3(-0.0765537734,1.1762296998,-0.0996759264),
        vec3(0.0083161484,-0.0060324498,0.9977163014),aces));
    float luma = dot(acescg,vec3(0.272229,0.674082,0.0536895));
    acescg = mix(vec3(luma),acescg,0.96);
    const float a = 0.0245786;
    const float b = 0.000090537;
    const float cc = 0.983729;
    const float d = 0.4329510;
    const float e = 0.238081;
    vec3 linearCv = (acescg*(acescg+a)-b)/
                    (acescg*(cc*acescg+d)+e);

    vec3 xyz = mulRows(
        vec3(0.6624541811,0.1340042065,0.1561876870),
        vec3(0.2722287168,0.6740817658,0.0536895174),
        vec3(-0.0055746495,0.0040607335,1.0103391003),linearCv);
    float oldY = max(xyz.y,0.0);
    if (oldY > 0.0) xyz *= pow(oldY,0.9811)/oldY;
    linearCv = mulRows(
        vec3(1.6410233797,-0.3248032942,-0.2364246952),
        vec3(-0.6636628587,1.6153315917,0.0167563477),
        vec3(0.0117218943,-0.0082844420,0.9883948585),xyz);
    luma = dot(linearCv,vec3(0.272229,0.674082,0.0536895));
    linearCv = mix(vec3(luma),linearCv,0.93);

    xyz = mulRows(
        vec3(0.6624541811,0.1340042065,0.1561876870),
        vec3(0.2722287168,0.6740817658,0.0536895174),
        vec3(-0.0055746495,0.0040607335,1.0103391003),linearCv);
    xyz = mulRows(
        vec3(0.98722400,-0.00611327,0.0159533),
        vec3(-0.00759836,1.00186000,0.0053302),
        vec3(0.00307257,-0.00509595,1.0816800),xyz);
    return mulRows(
        vec3(3.2409699419,-1.5373831776,-0.4986107603),
        vec3(-0.9692436363,1.8759675015,0.0415550574),
        vec3(0.0556300797,-0.2039769589,1.0569715142),xyz);
}

void main() {
    vec4 c = texture(uTex,vUV);
    vec4 m = texture(uMask,vUV);
    // HighwaysAlphaMask composite: min(highway alpha, mask), THEN premultiply
    // -- using the unmasked coverage here would keep the fade band at full
    // brightness against the background.
    float a = min(c.a, m.r)*uAlpha;
    if (c.a > 0.0) {
        vec3 color = max(c.rgb/c.a,vec3(0.0));
        if (uPost > 0.5) {
            color += 0.3*texture(uBloom,vUV).rgb;
            // URP film grain (UberPP order: bloom -> grade -> grain -> ACES).
            // Medium01 texture (alpha channel, via swizzle), intensity 0.25*4,
            // response 0.792, scale = pixels/512 (1 texel = 1 screen pixel).
            float grain = texture(uGrain,vUV*uGrainScale+uGrainOffset).w;
            grain = (grain-0.5)*2.0;
            float lum = dot(color,vec3(0.2126729,0.7151522,0.0721750));
            lum = 1.0-sqrt(max(lum,0.0));
            lum = mix(1.0,lum,0.792);
            color += color*grain*lum;
            color = acesTonemap(color);
        }
        c.rgb = linearToSrgb(color)*a;
    }
    c.a = a;
    oCol = c;
}
)";

const char* POST_VS = R"(#version 330
layout(location=0) in vec2 aPos;
out vec2 vUV;
void main() { vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }
)";

const char* POST_FS = R"(#version 330
in vec2 vUV;
out vec4 oCol;
uniform sampler2D uTex;
uniform float uTime;
uniform float uAberration, uGlow, uVignette, uDesat, uShake;
void main() {
    vec2 fuv = vUV;
    vec2 uv = fuv + uShake * vec2(sin(uTime*57.0), cos(uTime*43.0));
    vec3 c = texture(uTex, uv).rgb;
    if (uAberration > 0.0) {
        vec2 d = (uv - 0.5) * uAberration * 0.04;
        c.r = texture(uTex, uv + d).r;
        c.b = texture(uTex, uv - d).b;
    }
    if (uGlow > 0.0) {
        vec3 g = vec3(0.0);
        for (int i = 0; i < 8; ++i) {
            float a = float(i) / 8.0 * 6.2831853;
            g += max(texture(uTex, uv + vec2(cos(a), sin(a)) * 0.011).rgb - 0.55, 0.0);
        }
        c += g * uGlow * 0.30;
    }
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(c, vec3(l), uDesat);
    c *= 1.0 - uVignette * smoothstep(0.30, 0.95, length(fuv - 0.5));
    oCol = vec4(c, 1.0);
}
)";

// ---------------------------------------------------------------------------
Tex gl_loadTex(const std::string& path, bool repeat, bool flipY,
               bool srgb, bool mipmaps) {
    Tex t;
    int n = 0;
    stbi_set_flip_vertically_on_load(flipY ? 1 : 0);
    unsigned char* d = stbi_load(path.c_str(), &t.w, &t.h, &n, 4);
    if (!d) { fprintf(stderr, "cannot load texture %s\n", path.c_str()); exit(1); }
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA,
                 t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
    if (mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    stbi_image_free(d);
    return t;
}

static Mat4 engineIdentity() {
    Mat4 m{};
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    return m;
}

static Mat4 engineTranslate(float x, float y, float z) {
    Mat4 m = engineIdentity();
    m.m[12] = x; m.m[13] = y; m.m[14] = z;
    return m;
}

static Mat4 engineScale(float x, float y, float z) {
    Mat4 m{};
    m.m[0] = x; m.m[5] = y; m.m[10] = z; m.m[15] = 1.0f;
    return m;
}

static Mat4 engineRotateX(float degrees) {
    Mat4 m = engineIdentity();
    const float a = degrees * 3.14159265f / 180.0f;
    const float c = cosf(a), s = sinf(a);
    m.m[5] = c; m.m[6] = s; m.m[9] = -s; m.m[10] = c;
    return m;
}

static Mat4 engineRotateY(float degrees) {
    Mat4 m = engineIdentity();
    const float a = degrees * 3.14159265f / 180.0f;
    const float c = cosf(a), s = sinf(a);
    m.m[0] = c; m.m[2] = -s; m.m[8] = s; m.m[10] = c;
    return m;
}

static Mat4 engineRotateZ(float degrees) {
    Mat4 m = engineIdentity();
    const float a = degrees * 3.14159265f / 180.0f;
    const float c = cosf(a), s = sinf(a);
    m.m[0] = c; m.m[1] = s; m.m[4] = -s; m.m[5] = c;
    return m;
}

static Mat4 engineNoteRotation(float rx, float ry, float rz) {
    if (rx == 0.0f && ry == 0.0f && rz == 0.0f) return engineIdentity();
    const float d = 3.14159265f / 180.0f;
    const float cX = cosf(rx*d), sX = sinf(rx*d);
    const float cY = cosf(ry*d), sY = sinf(ry*d);
    const float cZ = cosf(rz*d), sZ = sinf(rz*d);
    Mat4 m = engineIdentity();
    m.m[0] = cZ*cY;
    m.m[1] = cZ*sY*sX+sZ*cX;
    m.m[2] = cZ*sY*cX-sZ*sX;
    m.m[4] = -sZ*cY;
    m.m[5] = -sZ*sY*sX+cZ*cX;
    m.m[6] = -sZ*sY*cX-cZ*sX;
    m.m[8] = -sY;
    m.m[9] = cY*sX;
    m.m[10] = cY*cX;
    return m;
}

static Mat4 engineQuaternion(float x, float y, float z, float w) {
    Mat4 m = engineIdentity();
    const float xx=x*x, yy=y*y, zz=z*z;
    const float xy=x*y, xz=x*z, yz=y*z;
    const float wx=w*x, wy=w*y, wz=w*z;
    m.m[0] = 1.0f - 2.0f*(yy+zz);
    m.m[1] = 2.0f*(xy+wz);
    m.m[2] = 2.0f*(xz-wy);
    m.m[4] = 2.0f*(xy-wz);
    m.m[5] = 1.0f - 2.0f*(xx+zz);
    m.m[6] = 2.0f*(yz+wx);
    m.m[8] = 2.0f*(xz+wy);
    m.m[9] = 2.0f*(yz-wx);
    m.m[10]= 1.0f - 2.0f*(xx+yy);
    return m;
}

static Mat4 engineModel(float x, float y, float z,
                        float qx, float qy, float qz, float qw,
                        float sx, float sy, float sz) {
    return mat_mul(engineTranslate(x, y, z),
                   mat_mul(engineQuaternion(qx, qy, qz, qw),
                           engineScale(sx, sy, sz)));
}

static float yargRandomSigned(unsigned& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return float(state & 0x00ffffffu)*(2.0f/16777215.0f)-1.0f;
}

static Mat4 engineView(int style) {
    float eye[3], at[3];
    if (style == 1) {
        // The camera is under Highway at y=4, while the strike child is at
        // local y=-4. In strike-relative space the camera is therefore -5.3,
        // not its serialized local -9.3.
        eye[0] = 0.0f; eye[1] = -5.3f; eye[2] = -4.52f;
        const float pitch = 71.28001f * 3.14159265f / 180.0f;
        at[0] = 0.0f;
        at[1] = eye[1] + sinf(pitch) * 10.0f;
        at[2] = eye[2] + cosf(pitch) * 10.0f;
    } else {
        eye[0] = 0.0f; eye[1] = 2.66f; eye[2] = -4.86f;
        const float pitch = 24.12f * 3.14159265f / 180.0f;
        at[0] = 0.0f;
        at[1] = eye[1] - sinf(pitch) * 10.0f;
        at[2] = eye[2] + cosf(pitch) * 10.0f;
    }
    return mat_lookAt(eye, at);
}

static float yargLaneScale(const Mat4& camera, float aspect) {
    // HighwayCameraRendering first fits the 16:9 reference width, measures the
    // visible x=+/-1, z=[screen bottom, zero-fade] track bounds, then applies
    // the tighter of the screen-width and single-player 72%-height limits.
    const float firstScale = aspect / (16.0f / 9.0f);
    Mat4 firstPass = engineIdentity();
    firstPass.m[0] = firstScale;
    firstPass.m[5] = firstScale;
    firstPass.m[13] = -1.0f + firstScale;
    const Mat4 measured = mat_mul(firstPass, camera);

    const float pitch = 24.12f * 3.14159265f / 180.0f;
    const float tanHalfFov = tanf(55.0f * 3.14159265f / 360.0f);
    const float rayY = -sinf(pitch) - tanHalfFov * cosf(pitch);
    const float rayZ =  cosf(pitch) - tanHalfFov * sinf(pitch);
    const float bottomZ = -4.86f + (-2.66f / rayY) * rayZ;

    float minX = 1.0e9f, maxX = -1.0e9f;
    float minY = 1.0e9f, maxY = -1.0e9f;
    for (float x : {-1.0f, 1.0f}) {
        for (float z : {bottomZ, 3.0f}) {
            const float clipX = measured.m[0]*x + measured.m[8]*z +
                                measured.m[12];
            const float clipY = measured.m[1]*x + measured.m[9]*z +
                                measured.m[13];
            const float clipW = measured.m[3]*x + measured.m[11]*z +
                                measured.m[15];
            if (clipW == 0.0f) continue;
            const float viewportX = clipX / clipW * 0.5f + 0.5f;
            const float viewportY = clipY / clipW * 0.5f + 0.5f;
            minX = std::min(minX, viewportX);
            maxX = std::max(maxX, viewportX);
            minY = std::min(minY, viewportY);
            maxY = std::max(maxY, viewportY);
        }
    }
    const float trackWidth = maxX - minX;
    const float trackHeight = maxY - minY;
    if (trackWidth <= 0.0f || trackHeight <= 0.0f) return firstScale;
    const float widthFactor = std::min(1.0f, 1.0f / trackWidth);
    const float heightFactor = 0.72f / trackHeight;
    return firstScale * std::min(widthFactor, heightFactor);
}

static Mat4 engineCamera(int style, float aspect, bool applyViewport = true) {
    const float projectionAspect = style == 1
                                 ? aspect * (0.78f / 0.98f)
                                 : aspect;
    Mat4 camera = mat_mul(mat_perspective(55.0f, projectionAspect,
                                         style == 1 ? 0.3f : 0.01f,
                                         style == 1 ? 60.0f : 40.0f),
                          engineView(style));
    for (int row = 0; row < 4; ++row) camera.m[row * 4] = -camera.m[row * 4];
    if (style == 1 && applyViewport) {
        // Active 3D camera rect: x=.122, width=.78, y=0, height=.98.
        Mat4 viewport = engineIdentity();
        viewport.m[0] = 0.78f;
        viewport.m[5] = 0.98f;
        viewport.m[12] = 2.0f * 0.122f + 0.78f - 1.0f;
        viewport.m[13] = -0.02f;
        camera = mat_mul(viewport, camera);
    } else if (style == 2) {
        const float laneScale = yargLaneScale(camera, aspect);
        Mat4 viewport = engineIdentity();
        viewport.m[0] = laneScale;
        viewport.m[5] = laneScale;
        viewport.m[13] = -1.0f + laneScale;
        camera = mat_mul(viewport, camera);
    }
    return camera;
}

// ---------------------------------------------------------------------------
void Renderer::buildCamera() {
    float eye[3] = {ch::CAM_X, ch::CAM_Y, ch::CAM_Z};
    float pitch = ch::CAM_PITCH_DEG * 3.14159265f / 180.0f;
    float fwd[3] = {0.0f, -sinf(pitch), cosf(pitch)};
    float at[3]  = {eye[0] + fwd[0]*10, eye[1] + fwd[1]*10, eye[2] + fwd[2]*10};
    view_ = mat_lookAt(eye, at);
    mvp_ = mat_mul(mat_perspective(ch::CAM_FOV, float(W)/float(H),
                                   ch::CAM_NEAR, ch::CAM_FAR),
                   view_);
    mvp2_ = mat_mul(mat_perspective(ch::CAM_FOV, float(W)*0.5f/float(H),
                                    ch::CAM_NEAR, ch::CAM_FAR),
                    view_);
    // Unity is left-handed; this right-handed lookAt puts +x on the left.
    // Negating clip-space x once lets every Unity coordinate be used verbatim
    // instead of sprinkling sign flips through the geometry.
    for (Mat4* m : {&mvp_, &mvp2_}) {
        m->m[0] = -m->m[0]; m->m[4] = -m->m[4];
        m->m[8] = -m->m[8]; m->m[12] = -m->m[12];
    }
}

void Renderer::makeFbos() {
    auto mk = [&](GLuint& f, GLuint& t, int w, int h,
                  GLint internal = GL_RGBA) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "framebuffer incomplete\n"); exit(1);
        }
    };
    mk(fbo_, colorTex_, W, H);
    // Depth for the actor z-mask. Attached to the scene FBO only.
    glGenRenderbuffers(1, &depthRb_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depthRb_);
    // The post pass needs its own target: an offline run has no usable default
    // framebuffer (the window is 64x64 and hidden), so resolving into it would
    // clip every frame to a corner and read back black.
    mk(postFbo_, postTex_, W, H);
    mk(fxFbo_, fxTex_, W, H);      // --fxshader's intermediate; see renderer.h

    mk(moonSceneFbo_,moonSceneTex_,W,H,GL_RGBA8);
    glGenFramebuffers(1,&moonSceneMsaaFbo_);
    glGenRenderbuffers(1,&moonSceneMsaaColor_);
    glBindRenderbuffer(GL_RENDERBUFFER,moonSceneMsaaColor_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER,8,GL_RGBA8,W,H);
    glGenRenderbuffers(1,&moonSceneDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER,moonSceneDepth_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER,8,GL_DEPTH_COMPONENT24,W,H);
    glBindFramebuffer(GL_FRAMEBUFFER,moonSceneMsaaFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER,moonSceneMsaaColor_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER,moonSceneDepth_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,"moon scene framebuffer incomplete\n"); exit(1);
    }
    moonSceneW_ = W;
    moonSceneH_ = H;

    const int blurW = std::max(1, W / 2);
    const int blurH = std::max(1, H / 2);
    mk(moonGlowFbo_, moonGlowTex_, blurW, blurH, GL_RGBA8);
    glGenRenderbuffers(1, &moonGlowDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER, moonGlowDepth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, blurW, blurH);
    glBindFramebuffer(GL_FRAMEBUFFER, moonGlowFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, moonGlowDepth_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "moon glow framebuffer incomplete\n"); exit(1);
    }
    for (int i = 0; i < 2; ++i)
        mk(moonBlurFbo_[i], moonBlurTex_[i], blurW, blurH, GL_RGBA8);
    moonGlowW_ = blurW;
    moonGlowH_ = blurH;

    mk(yargFbo_,yargTex_,W,H,GL_RGBA16F);
    glGenRenderbuffers(1,&yargDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER,yargDepth_);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,W,H);
    glBindFramebuffer(GL_FRAMEBUFFER,yargFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER,yargDepth_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,"linear engine framebuffer incomplete\n"); exit(1);
    }
    yargW_ = W;
    yargH_ = H;
    const int bloomBaseMax = std::max(std::max(1,W >> 1),
                                      std::max(1,H >> 1));
    yargBloomMips_ = std::clamp(
        int(floorf(log2f(float(bloomBaseMax))))-1,1,6);
    for (int i = 0; i < 6; ++i) {
        const int bloomW = std::max(1,W >> (i+1));
        const int bloomH = std::max(1,H >> (i+1));
        mk(yargBloomDownFbo_[i],yargBloomDownTex_[i],bloomW,bloomH,
           GL_RGBA16F);
        mk(yargBloomUpFbo_[i],yargBloomUpTex_[i],bloomW,bloomH,
           GL_RGBA16F);
    }

    glGenTextures(1,&yargNormalTex_);
    glBindTexture(GL_TEXTURE_2D,yargNormalTex_);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8_SNORM,W,H,0,GL_RGBA,GL_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenTextures(1,&yargAoDepth_);
    glBindTexture(GL_TEXTURE_2D,yargAoDepth_);
    glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT32F,W,H,0,
                 GL_DEPTH_COMPONENT,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1,&yargNormalFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER,yargNormalFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                           yargNormalTex_,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,
                           yargAoDepth_,0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,"YARG normals framebuffer incomplete\n"); exit(1);
    }
    for (int i = 0; i < 3; ++i)
        mk(yargAoFbo_[i],yargAoTex_[i],W,H,GL_RGBA8);
    glGenTextures(1,&yargAoTex_[3]);
    glBindTexture(GL_TEXTURE_2D,yargAoTex_[3]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_R8,W,H,0,GL_RED,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1,&yargAoFbo_[3]);
    glBindFramebuffer(GL_FRAMEBUFFER,yargAoFbo_[3]);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                           yargAoTex_[3],0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,"YARG AO framebuffer incomplete\n"); exit(1);
    }

    // URP High asset renders 4x MSAA; the scene is drawn into this target and
    // resolved into yargTex_ before the bloom chain.
    glGenRenderbuffers(1,&yargMsaaColor_);
    glBindRenderbuffer(GL_RENDERBUFFER,yargMsaaColor_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER,4,GL_RGBA16F,W,H);
    glGenRenderbuffers(1,&yargMsaaDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER,yargMsaaDepth_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER,4,GL_DEPTH_COMPONENT24,
                                     W,H);
    glGenFramebuffers(1,&yargMsaaFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER,yargMsaaFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER,yargMsaaColor_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER,yargMsaaDepth_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,"YARG MSAA framebuffer incomplete\n"); exit(1);
    }

    // HighwaysAlphaMask target: R8. Its depth attachment reuses the AO depth
    // texture -- that texture is dead once the AO chain has run, and the mask
    // pass clears it before drawing.
    glGenTextures(1,&yargMaskTex_);
    glBindTexture(GL_TEXTURE_2D,yargMaskTex_);
    glTexImage2D(GL_TEXTURE_2D,0,GL_R8,W,H,0,GL_RED,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1,&yargMaskFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER,yargMaskFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                           yargMaskTex_,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,
                           yargAoDepth_,0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,"YARG mask framebuffer incomplete\n"); exit(1);
    }
}

void Renderer::destroyFbos() {
    if (depthRb_) { glDeleteRenderbuffers(1, &depthRb_); depthRb_ = 0; }
    if (moonGlowDepth_) {
        glDeleteRenderbuffers(1, &moonGlowDepth_); moonGlowDepth_ = 0;
    }
    if (moonSceneDepth_) {
        glDeleteRenderbuffers(1,&moonSceneDepth_); moonSceneDepth_ = 0;
    }
    if (moonSceneMsaaColor_) {
        glDeleteRenderbuffers(1,&moonSceneMsaaColor_); moonSceneMsaaColor_ = 0;
    }
    if (yargDepth_) { glDeleteRenderbuffers(1,&yargDepth_); yargDepth_ = 0; }
    if (yargMsaaColor_) {
        glDeleteRenderbuffers(1,&yargMsaaColor_); yargMsaaColor_ = 0;
    }
    if (yargMsaaDepth_) {
        glDeleteRenderbuffers(1,&yargMsaaDepth_); yargMsaaDepth_ = 0;
    }
    if (fbo_)     { glDeleteFramebuffers(1, &fbo_);     fbo_ = 0; }
    if (postFbo_) { glDeleteFramebuffers(1, &postFbo_); postFbo_ = 0; }
    if (fxFbo_)   { glDeleteFramebuffers(1, &fxFbo_);   fxFbo_ = 0; }
    if (moonGlowFbo_) {
        glDeleteFramebuffers(1, &moonGlowFbo_); moonGlowFbo_ = 0;
    }
    if (moonSceneFbo_) {
        glDeleteFramebuffers(1,&moonSceneFbo_); moonSceneFbo_ = 0;
    }
    if (moonSceneMsaaFbo_) {
        glDeleteFramebuffers(1,&moonSceneMsaaFbo_); moonSceneMsaaFbo_ = 0;
    }
    if (yargFbo_) { glDeleteFramebuffers(1,&yargFbo_); yargFbo_ = 0; }
    if (yargMsaaFbo_) {
        glDeleteFramebuffers(1,&yargMsaaFbo_); yargMsaaFbo_ = 0;
    }
    if (yargMaskFbo_) {
        glDeleteFramebuffers(1,&yargMaskFbo_); yargMaskFbo_ = 0;
    }
    if (yargNormalFbo_) {
        glDeleteFramebuffers(1,&yargNormalFbo_); yargNormalFbo_ = 0;
    }
    glDeleteFramebuffers(4,yargAoFbo_);
    for (GLuint& fbo : yargAoFbo_) fbo = 0;
    glDeleteFramebuffers(6,yargBloomDownFbo_);
    glDeleteFramebuffers(6,yargBloomUpFbo_);
    for (int i = 0; i < 6; ++i) {
        yargBloomDownFbo_[i] = yargBloomUpFbo_[i] = 0;
    }
    glDeleteFramebuffers(2, moonBlurFbo_);
    moonBlurFbo_[0] = moonBlurFbo_[1] = 0;
    if (fxTex_)   { glDeleteTextures(1, &fxTex_);       fxTex_ = 0; }
    if (colorTex_) { glDeleteTextures(1, &colorTex_); colorTex_ = 0; }
    if (postTex_)  { glDeleteTextures(1, &postTex_);  postTex_ = 0; }
    if (moonGlowTex_) { glDeleteTextures(1, &moonGlowTex_); moonGlowTex_ = 0; }
    if (moonSceneTex_) { glDeleteTextures(1,&moonSceneTex_); moonSceneTex_ = 0; }
    if (yargTex_) { glDeleteTextures(1,&yargTex_); yargTex_ = 0; }
    if (yargMaskTex_) {
        glDeleteTextures(1,&yargMaskTex_); yargMaskTex_ = 0;
    }
    if (yargNormalTex_) {
        glDeleteTextures(1,&yargNormalTex_); yargNormalTex_ = 0;
    }
    if (yargAoDepth_) { glDeleteTextures(1,&yargAoDepth_); yargAoDepth_ = 0; }
    glDeleteTextures(4,yargAoTex_);
    for (GLuint& texture : yargAoTex_) texture = 0;
    glDeleteTextures(6,yargBloomDownTex_);
    glDeleteTextures(6,yargBloomUpTex_);
    for (int i = 0; i < 6; ++i) {
        yargBloomDownTex_[i] = yargBloomUpTex_[i] = 0;
    }
    glDeleteTextures(2, moonBlurTex_);
    moonBlurTex_[0] = moonBlurTex_[1] = 0;
    moonSceneW_ = moonSceneH_ = 0;
}

bool Renderer::init(int w, int h, const std::string& A) {
    W = w; H = h;

    texHighway_ = gl_loadTex(A + "highway/spr_highway_gh6.png", true);  // Repeat
    texSide_    = gl_loadTex(A + "highway/sidebar.png", false);
    texString_  = gl_loadTex(A + "highway/Guitarstring_wor_remake2.png", false);
    texBeat_    = gl_loadTex(A + "highway/beatline.png", false);
    texNotes_   = gl_loadTex(A + "notes/spr_newnotes_strip4.png", false);
    texSustain_ = gl_loadTex(A + "notes/spr_sustain_strip6.png", false);

    // PIU art. Loaded unconditionally -- a texture that is never bound costs
    // nothing to draw, and lazy-loading inside drawFrame would put a file read
    // on the render path.
    {
        const char* piuArt[3] = {"DownLeft", "UpLeft", "Center"};
        for (int i = 0; i < 3; ++i) {
            const std::string n = A + "pump/" + piuArt[i] + " ";
            texPiuTap_[i]      = gl_loadTex(n + "Tap Note 3x2.png", false);
            texPiuRecep_[i]    = gl_loadTex(n + "Ready Receptor 1x3.png", false);
            texPiuHoldBody_[i] = gl_loadTex(n + "Hold Body Active 6x1.png", false);
            texPiuHoldCap_[i]  = gl_loadTex(n + "Hold BottomCap Active 6x1.png", false);
        }
        texPiuFlash_   = gl_loadTex(A + "pump/flash.png", false);
    }

    // Taiko art, same rule. Lane order is ch::NOTE_TINT's -- green, red,
    // yellow, blue, orange -- with purple in slot 5 for open notes. Red and
    // blue are the skin's own don and ka; the other four are recolours
    // (devtools/taiko_extract.py).
    {
        const char* taikoLane[6] = {"green", "red", "yellow",
                                    "blue", "orange", "purple"};
        for (int i = 0; i < 6; ++i) {
            const std::string c = taikoLane[i];
            texTaikoNote_[i] = gl_loadTex(
                A + "taiko/taiko_note_" + c + "_strip4.png", false);
            texTaikoBig_[i]  = gl_loadTex(
                A + "taiko/taiko_big_" + c + "_strip4.png", false);
            texTaikoRoll_[i] = gl_loadTex(
                A + "taiko/taiko_roll_" + c + "_strip3.png", false);
        }
        texTaikoJudge_     = gl_loadTex(A + "taiko/taiko_judge.png", false);
        texTaikoLane_      = gl_loadTex(A + "taiko/taiko_lane.png", false);
        texTaikoLaneGogo_  = gl_loadTex(A + "taiko/taiko_lane_gogo.png", false);
        texTaikoLaneFlash_ = gl_loadTex(A + "taiko/taiko_lane_flash.png", false);
        texTaikoFrame_     = gl_loadTex(A + "taiko/taiko_frame.png", false);
        texTaikoDrumBg_    = gl_loadTex(A + "taiko/taiko_drum_bg.png", false);
        texTaikoDrum_      = gl_loadTex(A + "taiko/taiko_drum.png", false);
        texTaikoDrumDon_   = gl_loadTex(A + "taiko/taiko_drum_don.png", false);
        texTaikoDrumKa_    = gl_loadTex(A + "taiko/taiko_drum_ka.png", false);
        texTaikoBar_       = gl_loadTex(A + "taiko/taiko_bar.png", false);
    }

    // BMS art. Three kinds, indexed by ch::BMS_LANE_ART.
    {
        const char* kind[3] = {"scratch", "white", "black"};
        for (int i = 0; i < 3; ++i) {
            const std::string k = kind[i];
            texBmsNote_[i]    = gl_loadTex(A + "bms/bms_note_" + k + ".png", false);
            texBmsLnStart_[i] = gl_loadTex(A + "bms/bms_ln_start_" + k + ".png", false);
            texBmsLnBody_[i]  = gl_loadTex(A + "bms/bms_ln_body_" + k + ".png", false);
            texBmsLnEnd_[i]   = gl_loadTex(A + "bms/bms_ln_end_" + k + ".png", false);
            texBmsBeam_[i]    = gl_loadTex(A + "bms/bms_beam_" + k + ".png", false);
        }
        texBmsJudge_   = gl_loadTex(A + "bms/bms_judgeline.png", false);
        texBmsMeasure_ = gl_loadTex(A + "bms/bms_measure.png", false);
        texBmsLaneGlow_ = gl_loadTex(A + "bms/bms_lane_glow.png", false);
        texBmsKeyboard_ = gl_loadTex(A + "bms/bms_keyboard.png", false);
        texBmsLeftCol_  = gl_loadTex(A + "bms/bms_leftcol.png", false);
        texBmsTurntable_ = gl_loadTex(A + "bms/bms_turntable.png", false);
        texBmsKeyFlash_[0] = gl_loadTex(A + "bms/bms_keyflash_white.png", false);
        texBmsKeyFlash_[1] = gl_loadTex(A + "bms/bms_keyflash_black.png", false);
    }
    texAnim_    = gl_loadTex(A + "notes/spr_note_anim_strip16.png", false);
    texOpen_    = gl_loadTex(A + "notes/spr_open_notes_strip5.png", false);
    // Starpower sheets share the note strip's geometry exactly: every one is
    // cut at 128px per frame with pivot (0.5, 0.16), so the star quads reuse
    // the plain note extents unchanged.
    texStarCap_     = gl_loadTex(A + "notes/spr_star_notes_cap_strip4.png", false);
    texStarBody_    = gl_loadTex(A + "notes/spr_star_notes_strip1.png", false);
    texStarBodyTap_ = gl_loadTex(A + "notes/spr_star_notes_strip2.png", false);
    texStarBottom_  = gl_loadTex(A + "notes/spr_star_notes_strip4.png", false);
    texSpOpen_      = gl_loadTex(A + "notes/spr_sp_highlight_strip16.png", false);
    texOpenAnim_    = gl_loadTex(A + "notes/spr_highlight_strip16.png", false);
    texOpenSustain_ = gl_loadTex(A + "notes/spr_open_sustain_strip2.png", false);
    texFretB_   = gl_loadTex(A + "frets/spr_newtargets_bottom_strip12.png", false);
    texFretH_   = gl_loadTex(A + "frets/spr_newtargets_head_strip6.png", false);
    texLift_    = gl_loadTex(A + "frets/spr_targets_lift.png", false);
    texHLight_  = gl_loadTex(A + "frets/Head_Lights.png", false);
    texFretOpen_[0] = gl_loadTex(A + "frets/1_Open.png", false);
    texFretOpen_[1] = gl_loadTex(A + "frets/2_Open.png", false);
    texFretOpen_[2] = gl_loadTex(A + "frets/3_Open.png", false);
    // YARG is a Linear-colorspace Unity project: albedo textures import with
    // sRGBTexture 1 and Unity decodes them to linear before the BRDF. Match
    // the .meta flag per texture -- data masks (Fade/Solo/effect trim) are 0.
    // The CH/Moonscraper paths keep srgb=false (their lighting math already
    // works in display space; touching it would move the pinned hashes).
    auto engineTex = [&](const char* name, bool repeat, bool flipY,
                         bool mipmaps, bool srgb = false) {
        std::string path = A+"engine/"+name;
        if (FILE* f = fopen(path.c_str(),"rb")) {
            fclose(f);
        } else {
            fprintf(stderr,"missing engine texture %s (using _missing.png)\n",
                    path.c_str());
            path = A+"_missing.png";
        }
        return gl_loadTex(path,repeat,flipY,srgb,mipmaps);
    };
    texMoonHighway_  = engineTex("moon_highway.png",true,false,false);
    texMoonRail_     = engineTex("moon_rail.png",true,false,false);
    texMoonBeat_     = engineTex("moon_beat.png",false,false,true);
    texMoonBeatWeak_ = engineTex("moon_beat_weak.png",false,false,true);
    texMoonMeasure_  = engineTex("moon_measure.png",false,false,true);
    texMoonIndicator_= engineTex("moon_indicator.png",false,false,false);
    texMoonStrike_   = engineTex("moon_strike.png",false,false,false);
    texMoonSustainFretted_ = engineTex("moon_sustain_fretted.png",false,false,false);
    texMoonSustainOpen_ = engineTex("moon_sustain_open.png",false,false,false);
    texMoonSpTail_ = engineTex("moon_sp_tail.png",false,false,false);
    texYargNote_       = engineTex("yarg_note.png",false,true,true,true);
    texYargNoteShine_  = engineTex("yarg_note_shine.png",false,true,true,true);
    texYargNoteShader_ = engineTex("yarg_note_shader.png",false,true,true,true);
    texYargOpenNote_   = engineTex("yarg_open_note.png",false,true,true,true);
    texYargOpenHopo_   = engineTex("yarg_open_hopo.png",false,true,true,true);
    texYargFret_       = engineTex("yarg_fret.png",false,true,true,true);
    texYargFretShine_  = engineTex("yarg_fret_shine.png",false,true,true,true);
    texYargTrackFade_  = engineTex("yarg_track_fade.png",false,true,true);
    texYargTrackSmall_ = engineTex("yarg_track_small.png",true,true,true,true);
    texYargTrackSide_  = engineTex("yarg_track_side.png",true,true,true,true);
    texYargTrackTrim_  = engineTex("yarg_track_trim.png",true,true,true,true);
    texYargSoloTrack_ = engineTex("yarg_solo_track.png",true,true,true);
    texYargSoloRail_ = engineTex("yarg_solo_rail.png",true,true,true);
    texYargSoloTransitionTrack_ =
        engineTex("yarg_solo_transition_track.png",false,true,true);
    texYargSoloTransitionRailLeft_ =
        engineTex("yarg_solo_transition_rail_left.png",false,true,true);
    texYargSoloTransitionRailRight_ =
        engineTex("yarg_solo_transition_rail_right.png",false,true,true);
    texYargSpTrim_ = engineTex("yarg_sp_trim.png",true,true,true);
    texYargBeatline_   = engineTex("yarg_beatline.png",false,true,true,true);
    texYargSustain_    = engineTex("yarg_sustain.png",true,true,true,true);
    texYargSustainSecondary_ = engineTex("yarg_sustain_secondary.png",true,true,true,true);
    texYargOpenSustain_ = engineTex("yarg_open_sustain.png",true,true,true,true);
    texYargFretHitFlash_ = engineTex("yarg_fret_hit_flash.png",false,true,true,true);
    texYargFretHitRing_ = engineTex("yarg_fret_hit_ring.png",false,true,true,true);
    // URP Medium01 film grain (see yarg_grain.SOURCE.txt). The PNG is RGB;
    // URP samples the single-channel alpha import, so swizzle A <- R.
    texYargGrain_      = engineTex("yarg_grain.png",true,false,false);
    glBindTexture(GL_TEXTURE_2D,texYargGrain_.id);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_R,GL_ONE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_G,GL_ONE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_B,GL_ONE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_A,GL_RED);
    // GH3 board sprites (see gh3.SOURCE.txt). Plain display-space 2D art like
    // the CH/Moonscraper sets: no srgb, no flip, no mipmaps.
    texGh3Fretbar_[0] = engineTex("gh3_fretbar_small.png",false,false,false);
    texGh3Fretbar_[1] = engineTex("gh3_fretbar_medium.png",false,false,false);
    texGh3Fretbar_[2] = engineTex("gh3_fretbar_large.png",false,false,false);
    texGh3String_     = engineTex("gh3_string.png",false,false,false);
    texGh3Sidebar_    = engineTex("gh3_sidebar.png",false,false,false);
    {
        const char* gcol[5] = {"green","red","yellow","blue","orange"};
        for (int i = 0; i < 5; ++i) {
            const std::string c = gcol[i];
            texGh3NowbarMid_[i] =
                engineTex(("gh3_nowbar_mid_"+c+".png").c_str(),false,false,false);
            texGh3NowbarLip_[i] =
                engineTex(("gh3_nowbar_lip_"+c+".png").c_str(),false,false,false);
            texGh3NowbarHead_[i] =
                engineTex(("gh3_nowbar_head_"+c+".png").c_str(),false,false,false);
            texGh3NowbarDown_[i] =
                engineTex(("gh3_nowbar_down_"+c+".png").c_str(),false,false,false);
            texGh3NowbarHeadLit_[i] =
                engineTex(("gh3_nowbar_head_"+c+"_lit.png").c_str(),false,false,false);
            texGh3Gem_[i] =
                engineTex(("gh3_gem_"+c+".png").c_str(),false,false,false);
            texGh3GemHammer_[i] =
                engineTex(("gh3_gem_"+c+"_hammer.png").c_str(),false,false,false);
            texGh3Star_[i] =
                engineTex(("gh3_star_"+c+".png").c_str(),false,false,false);
            texGh3StarHammer_[i] =
                engineTex(("gh3_star_"+c+"_hammer.png").c_str(),false,false,false);
            texGh3Tap_[i] =
                engineTex(("gh3_tap_"+c+".png").c_str(),false,false,false);
            texGh3TapSp_[i] =
                engineTex(("gh3_tap_sp_"+c+".png").c_str(),false,false,false);
            // Clamp, not repeat: the native tail maps ONE tile -- cap at the
            // far tip, body clamped to the tile's opaque bottom edge.
            texGh3Whammy_[i] =
                engineTex(("gh3_whammy_"+c+".png").c_str(),false,false,false);
        }
        texGh3NowbarNeck_ = engineTex("gh3_nowbar_neck.png",false,false,false);
        texGh3Open_       = engineTex("gh3_open_strum.png",false,false,false);
        texGh3OpenHopo_   = engineTex("gh3_open_hopo.png",false,false,false);
        texGh3OpenSp_     = engineTex("gh3_open_strum_sp.png",false,false,false);
        texGh3OpenHopoSp_ = engineTex("gh3_open_hopo_sp.png",false,false,false);
        texGh3WhammySp_   = engineTex("gh3_whammy_sp.png",false,false,false);
        texGh3WhammyDead_ = engineTex("gh3_whammy_dead.png",false,false,false);
        texGh3OpenSus_    = engineTex("gh3_open_sustain.png",false,false,false);
        texGh3OpenSusDead_= engineTex("gh3_open_sustain_dead.png",false,false,false);
    }
    {   // 1x1 white: vertex colour becomes the fill (GH3's black highway).
        unsigned char white[4] = {255, 255, 255, 255};
        glGenTextures(1, &texWhite_.id);
        glBindTexture(GL_TEXTURE_2D, texWhite_.id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    actorFont_  = gl_loadTex(A + "fonts/_eurostile normal (mipmaps) 16x16.png", false);
    actorMissing_ = gl_loadTex(A + "_missing.png", false, false);
    int rawFontWidth[256];
    for (int& width : rawFontWidth) width = 32;
    int addToWidths = 0, advanceExtra = 1;
    if (FILE* f = fopen((A + "fonts/_eurostile normal.ini").c_str(), "rb")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            int key = 0, value = 0;
            if (sscanf(line, "AddToAllWidths=%d", &value) == 1) addToWidths = value;
            else if (sscanf(line, "AdvanceExtraPixels=%d", &value) == 1) advanceExtra = value;
            else if (sscanf(line, "%d=%d", &key, &value) == 2 && key >= 0 && key < 256)
                rawFontWidth[key] = value;
        }
        fclose(f);
    }
    for (int i = 0; i < 256; ++i) {
        actorFontWidth_[i] = rawFontWidth[i] + addToWidths;
        actorFontAdvance_[i] = actorFontWidth_[i] + advanceExtra;
    }

    prog_ = gl_program(SCENE_VS, SCENE_FS, "scene");
    post_ = gl_program(POST_VS, POST_FS, "post");
    glow_ = gl_program(SCENE_VS, NOTE_GLOW_FS, "noteGlow");
    susGlow_ = gl_program(SCENE_VS, SUSTAIN_GLOW_FS, "sustainGlow");
    actor_   = gl_program(ACTOR_VS, ACTOR_FS, "actor");
    piu_     = gl_program(PIU_VS, PIU_FS, "piu");
    gh3Sprite_ = gl_program(GH3_SPRITE_VS, GH3_SPRITE_FS, "gh3sprite");
    gh3Whammy_ = gl_program(GH3_SPRITE_VS, GH3_WHAMMY_FS, "gh3whammy");
    engine_  = gl_program(ENGINE_VS, ENGINE_FS, "engine");
    engineGlow_ = gl_program(ENGINE_VS, ENGINE_GLOW_FS, "engineGlow");
    yargEffect_ = gl_program(SCENE_VS,YARG_EFFECT_FS,"yargEffect");
    moonOccluder_ = gl_program(SCENE_VS, MOON_OCCLUDER_FS, "moonOccluder");
    moonBlur_ = gl_program(POST_VS, MOON_BLUR_FS, "moonBlur");
    yargBloomPrefilter_ = gl_program(POST_VS,YARG_BLOOM_PREFILTER_FS,
                                     "yargBloomPrefilter");
    yargBloomDownH_ = gl_program(POST_VS,YARG_BLOOM_DOWN_H_FS,
                                 "yargBloomDownH");
    yargBloomDownV_ = gl_program(POST_VS,YARG_BLOOM_DOWN_V_FS,
                                 "yargBloomDownV");
    yargBloomUp_ = gl_program(POST_VS,YARG_BLOOM_UP_FS,"yargBloomUp");
    yargNormalProg_ = gl_program(ENGINE_VS,YARG_NORMAL_FS,"yargNormal");
    yargAoEstimate_ = gl_program(POST_VS,YARG_AO_ESTIMATE_FS,
                                 "yargAoEstimate");
    yargAoBlur_ = gl_program(POST_VS,YARG_AO_BLUR_FS,"yargAoBlur");
    yargAoFinal_ = gl_program(POST_VS,YARG_AO_FINAL_FS,"yargAoFinal");
    yargSustain_ = gl_program(SCENE_VS,YARG_SUSTAIN_FS,"yargSustain");
    yargBeatline_ = gl_program(SCENE_VS,YARG_BEATLINE_FS,"yargBeatline");
    yargMaskMesh_ = gl_program(ENGINE_VS,YARG_MASK_FS,"yargMaskMesh");
    linearCompose_ = gl_program(POST_VS, LINEAR_COMPOSE_FS, "linearCompose");
    cover_   = gl_program(POST_VS, COVER_FS, "cover");
    locPremul_ = glGetUniformLocation(prog_, "uPremul");

    glGenVertexArrays(1, &vao_); glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ch::Vtx), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ch::Vtx), (void*)(3*sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(ch::Vtx), (void*)(5*sizeof(float)));
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);

    glGenVertexArrays(1, &avao_); glBindVertexArray(avao_);
    glGenBuffers(1, &avbo_); glBindBuffer(GL_ARRAY_BUFFER, avbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 10*sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 10*sizeof(float), (void*)(2*sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 10*sizeof(float), (void*)(4*sizeof(float)));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 10*sizeof(float), (void*)(8*sizeof(float)));
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2); glEnableVertexAttribArray(3);

    loadEngineMesh(moonNote_, A + "engine/moonscraper_note.obj");
    loadEngineMesh(moonOpen_, A + "engine/moonscraper_open.obj");
    loadEngineMesh(moonSp_, A + "engine/moon_sp.obj");
    loadEngineMesh(yargNormal_, A + "engine/yarg_normal.fbx");
    loadEngineMesh(yargHopo_, A + "engine/yarg_hopo.fbx");
    loadEngineMesh(yargTap_, A + "engine/yarg_tap.fbx");
    loadEngineMesh(yargOpen_, A + "engine/yarg_open_default.fbx");
    loadEngineMesh(yargFret_, A + "engine/yarg_fret.fbx");
    loadEngineMesh(yargTrack_, A + "engine/yarg_track.fbx");
    loadEngineMesh(yargTrackTrim_, A + "engine/yarg_track_trim.fbx");

    const float quad[6] = {-1,-1, 3,-1, -1,3};
    glGenVertexArrays(1, &qvao_); glBindVertexArray(qvao_);
    glGenBuffers(1, &qvbo_); glBindBuffer(GL_ARRAY_BUFFER, qvbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    makeFbos();
    buildCamera();

    // Every CH highway layer is ZWrite Off: pure painter's algorithm.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    return true;
}

void Renderer::loadEngineMesh(EngineGpu& gpu, const std::string& path) {
    EngineMesh mesh;
    if (!mesh.load(path)) {
        fprintf(stderr, "engine mesh: could not load %s\n", path.c_str());
        return;
    }
    gpu.count = GLsizei(mesh.vertices.size());
    glGenVertexArrays(1, &gpu.vao);
    glBindVertexArray(gpu.vao);
    glGenBuffers(1, &gpu.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 GLsizeiptr(mesh.vertices.size() * sizeof(EngineMeshVertex)),
                 mesh.vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(EngineMeshVertex),
                          (void*)offsetof(EngineMeshVertex, x));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(EngineMeshVertex),
                          (void*)offsetof(EngineMeshVertex, nx));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(EngineMeshVertex),
                          (void*)offsetof(EngineMeshVertex, u));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(EngineMeshVertex),
                          (void*)offsetof(EngineMeshVertex, material));
    for (int i = 0; i < 4; ++i) glEnableVertexAttribArray(i);
}

void Renderer::drawEngineMesh(const EngineGpu& gpu, const Mat4& camera,
                              const Mat4& model, int kind, const float* color,
                              float alpha, GLuint texture, GLuint texture2,
                              GLuint texture3, float scroll,
                              int materialFilter, float materialState,
                              const float* random, float shaderTime) {
    if (gpu.count == 0 || alpha <= 0.0f) return;
    glUseProgram(engine_);
    glUniformMatrix4fv(glGetUniformLocation(engine_, "uMVP"), 1, GL_FALSE,
                       camera.m);
    glUniformMatrix4fv(glGetUniformLocation(engine_, "uModel"), 1, GL_FALSE,
                       model.m);
    glUniform3f(glGetUniformLocation(engine_, "uColor"),
                color[0] * fieldTint_[0], color[1] * fieldTint_[1],
                color[2] * fieldTint_[2]);
    glUniform1f(glGetUniformLocation(engine_, "uAlpha"),
                alpha * fieldTint_[3]);
    glUniform1f(glGetUniformLocation(engine_, "uScroll"), scroll);
    glUniform1i(glGetUniformLocation(engine_, "uKind"), kind);
    glUniform1i(glGetUniformLocation(engine_, "uMaterialFilter"), materialFilter);
    glUniform1f(glGetUniformLocation(engine_, "uMaterialState"),materialState);
    // YARG fret hit lights (drawEngine refreshes the members every frame;
    // count is 0 outside the YARG style, keeping every other path's output
    // untouched).
    glUniform1i(glGetUniformLocation(engine_, "uHitLightCount"),
                hitLightCount_);
    glUniform3fv(glGetUniformLocation(engine_, "uHitLightPos"), 5,
                 hitLightPos_);
    glUniform3fv(glGetUniformLocation(engine_, "uHitLightColor"), 5,
                 hitLightColor_);
    glUniform3f(glGetUniformLocation(engine_, "uRandom"),
                random ? random[0] : 0.0f,
                random ? random[1] : 0.0f,
                random ? random[2] : 0.0f);
    glUniform1f(glGetUniformLocation(engine_, "uTime"),shaderTime);
    glUniform1f(glGetUniformLocation(engine_, "uWaviness"),yargGroove_);
    glUniform1f(glGetUniformLocation(engine_, "uGroove"),yargGroove_);
    glUniform1f(glGetUniformLocation(engine_, "uUseAo"),engineUseAo_ ? 1.0f : 0.0f);
    const bool yarg = (kind >= 1 && kind <= 7) || kind == 12;
    glUniform3f(glGetUniformLocation(engine_, "uCameraPos"),
                0.0f, yarg ? 2.66f : -5.3f, yarg ? -4.86f : -4.52f);
    glUniform1f(glGetUniformLocation(engine_, "uCurve"), yarg ? 0.5f : 0.0f);
    if (yarg) {
        // No per-element fade for YARG meshes: the HighwaysAlphaMask pass
        // re-renders the geometry and the composite takes min(alpha, mask).
        // The plane stays fed so vEngineDistance is valid for that pass.
        glUniform4f(glGetUniformLocation(engine_, "uFadePlane"),
                    0.0f,0.0f,1.0f,4.86f);
        glUniform2f(glGetUniformLocation(engine_, "uFadeRange"),
                    0.0f,0.0f);
    } else {
        // Moonscraper's highway does not run to the horizon: a background
        // quad fades everything out at a fixed world height. In its scene the
        // fade anchor ("Max", y 15.75) hangs offset 2.28 below -> centre
        // 13.47, and the shader's spread is 2% of the 11.75-unit camera span
        // -> +-0.1175. The plane (0,1,0,0) measures plain world y, which is
        // what moon content scrolls along.
        glUniform4f(glGetUniformLocation(engine_, "uFadePlane"),0,1,0,0);
        glUniform2f(glGetUniformLocation(engine_, "uFadeRange"),
                    13.35f,13.59f);
    }
    const GLuint textures[3] = {texture, texture2, texture3};
    const char* names[3] = {"uTex", "uTex2", "uTex3"};
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glUniform1i(glGetUniformLocation(engine_, names[i]), i);
    }
    glActiveTexture(GL_TEXTURE0+3);
    glBindTexture(GL_TEXTURE_2D,yargAoTex_[3]);
    glUniform1i(glGetUniformLocation(engine_,"uAo"),3);
    glUniform2f(glGetUniformLocation(engine_,"uAoSize"),
                float(std::max(1,yargW_)),float(std::max(1,yargH_)));
    if (yarg) {
        const float det =
            model.m[0]*(model.m[5]*model.m[10]-model.m[6]*model.m[9])-
            model.m[4]*(model.m[1]*model.m[10]-model.m[2]*model.m[9])+
            model.m[8]*(model.m[1]*model.m[6]-model.m[2]*model.m[5]);
        glEnable(GL_CULL_FACE);
        glCullFace(det < 0.0f ? GL_BACK : GL_FRONT);
    }
    glBindVertexArray(gpu.vao);
    glDrawArrays(GL_TRIANGLES, 0, gpu.count);
    if (yarg) glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::drawEngine(const Chart& chart, double beat, const RenderOpts& o,
                          const Mods& mods, float songTime, float scrollNow,
                          float noteSpeed, float bpm, int style, float alpha,
                          float mx, float my, float mz, int vpX, int vpW,
                          const Mat4* mvpOverride) {
    if (alpha <= 0.0f || mods.hide != 0.0f) return;
    engineUseAo_ = !o.noPost;

    GLint engineTargetFbo = 0;
    GLint engineTargetViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&engineTargetFbo);
    glGetIntegerv(GL_VIEWPORT,engineTargetViewport);

    const bool moonIsolated = style == 1;
    const bool yargLinear = style == 2;
    const float fieldAlpha = fieldTint_[3];
    const float compositeAlpha = alpha * fieldAlpha;
    if (moonIsolated || yargLinear) fieldTint_[3] = 1.0f;
    if (moonIsolated) {
        // Each field is its own engine WINDOW: the scene FBO takes the
        // field's rect and the camera fits that rect's aspect, exactly a
        // real Moonscraper/YARG window that size. The old model rendered
        // full-frame with an NDC place squeeze -- YARG's 16:9 reference
        // fit then shrank the scene AGAIN, the "tiny second-player
        // highway". Engine fields hard-clip at their rect; charts that
        // push fields through each other drive the actor-proxy
        // mvpOverride path, which keeps its own target.
        const int targetW = std::max(1, mvpOverride
                                     ? engineTargetViewport[2] : vpW);
        const int targetH = std::max(1,engineTargetViewport[3]);
        if (targetW != moonSceneW_ || targetH != moonSceneH_) {
            glBindTexture(GL_TEXTURE_2D,moonSceneTex_);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,targetW,targetH,0,
                         GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            glBindRenderbuffer(GL_RENDERBUFFER,moonSceneMsaaColor_);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER,8,GL_RGBA8,
                                             targetW,targetH);
            glBindRenderbuffer(GL_RENDERBUFFER,moonSceneDepth_);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER,8,
                                             GL_DEPTH_COMPONENT24,
                                             targetW,targetH);
            moonSceneW_ = targetW;
            moonSceneH_ = targetH;
        }
        glBindFramebuffer(GL_FRAMEBUFFER,moonSceneMsaaFbo_);
        glViewport(0,0,targetW,targetH);
        // Direct path: opaque black -- the rect IS this field's window.
        // Proxy path (mvpOverride): alpha 0, content only. The composite
        // blends premultiplied over the shared target, so an opaque clear
        // there makes the SECOND moon proxy erase the first player's field
        // (Testify both-moon: one highway vanished); yarg proxies never
        // suffered this because their scene clear is already alpha 0.
        glClearColor(0,0,0, mvpOverride ? 0.0f : 1.0f);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    if (yargLinear) {
        const int targetW = std::max(1, mvpOverride
                                     ? engineTargetViewport[2] : vpW);
        const int targetH = std::max(1,engineTargetViewport[3]);
        if (targetW != yargW_ || targetH != yargH_) {
            glBindTexture(GL_TEXTURE_2D,yargTex_);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,targetW,targetH,0,
                         GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            glBindRenderbuffer(GL_RENDERBUFFER,yargDepth_);
            glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,
                                  targetW,targetH);
            glBindRenderbuffer(GL_RENDERBUFFER,yargMsaaColor_);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER,4,GL_RGBA16F,
                                             targetW,targetH);
            glBindRenderbuffer(GL_RENDERBUFFER,yargMsaaDepth_);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER,4,
                                             GL_DEPTH_COMPONENT24,
                                             targetW,targetH);
            glBindTexture(GL_TEXTURE_2D,yargMaskTex_);
            glTexImage2D(GL_TEXTURE_2D,0,GL_R8,targetW,targetH,0,
                         GL_RED,GL_UNSIGNED_BYTE,nullptr);
            for (int i = 0; i < 6; ++i) {
                const int bloomW = std::max(1,targetW >> (i+1));
                const int bloomH = std::max(1,targetH >> (i+1));
                for (GLuint texture : {yargBloomDownTex_[i],
                                       yargBloomUpTex_[i]}) {
                    glBindTexture(GL_TEXTURE_2D,texture);
                    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,bloomW,bloomH,0,
                                  GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
                }
            }
            glBindTexture(GL_TEXTURE_2D,yargNormalTex_);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8_SNORM,targetW,targetH,0,
                         GL_RGBA,GL_BYTE,nullptr);
            glBindTexture(GL_TEXTURE_2D,yargAoDepth_);
            glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT32F,targetW,targetH,0,
                         GL_DEPTH_COMPONENT,GL_FLOAT,nullptr);
            for (int i = 0; i < 3; ++i) {
                glBindTexture(GL_TEXTURE_2D,yargAoTex_[i]);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,targetW,targetH,0,
                             GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            }
            glBindTexture(GL_TEXTURE_2D,yargAoTex_[3]);
            glTexImage2D(GL_TEXTURE_2D,0,GL_R8,targetW,targetH,0,
                         GL_RED,GL_UNSIGNED_BYTE,nullptr);
            const int bloomBaseMax = std::max(std::max(1,targetW >> 1),
                                              std::max(1,targetH >> 1));
            yargBloomMips_ = std::clamp(
                int(floorf(log2f(float(bloomBaseMax))))-1,1,6);
            yargW_ = targetW;
            yargH_ = targetH;
        }
        glBindFramebuffer(GL_FRAMEBUFFER,yargMsaaFbo_);
        glViewport(0,0,targetW,targetH);
        glClearColor(0,0,0,0);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    if (moonIsolated || yargLinear) alpha = 1.0f;

    // Each field keeps SINGLE-PLAYER pixel proportions, centred on its
    // half: build the single-player camera (full-frame aspect -- YARG's
    // 16:9 reference fit at a half aspect letterboxes the track tiny),
    // then widen NDC x by W/vpW because the rect FBO's NDC spans only
    // vpW pixels. For the moon this is algebraically the accepted old
    // look (aspect vpW/H x the old NDC place squeeze); identity when
    // vpW == W. Fields wider than their rect clip at its edge.
    Mat4 fieldWiden = engineIdentity();
    if (!mvpOverride && vpW != W) fieldWiden.m[0] = float(W) / float(vpW);
    Mat4 camera = mvpOverride ? *mvpOverride
                              : mat_mul(fieldWiden,
                                        engineCamera(style,
                                                     float(W) / float(H)));

    float zoom = 1.0f - 0.5f * mods.mini;
    if (mods.tilt > 0.0f) zoom *= 1.0f - 0.1f * mods.tilt;
    else if (mods.tilt < 0.0f) zoom *= 1.0f + 0.1f * mods.tilt;
    const float wag = mods.wag * 21.0f * sinf(float(beat) * 3.14159265f);
    Mat4 root = engineScale(zoom, zoom, zoom);
    if (style == 1) {
        root = mat_mul(engineRotateY(-wag),
                       mat_mul(engineRotateX(-30.0f * mods.tilt), root));
        root = mat_mul(engineTranslate(o.px + mx, o.pz + mz, o.py + my), root);
    } else {
        root = mat_mul(engineRotateZ(wag),
                       mat_mul(engineRotateX(30.0f * mods.tilt), root));
        // CH pivots the field mods (mini zoom, tilt, wag) about the STRIKE
        // LINE; the YARG world's origin sits 2 units past it (strike z=-2),
        // so an origin pivot makes mini slide the strikeline toward
        // mid-track -- the "mini percentage feels off vs CH" symptom.
        // Conjugate by the strike translation; identity when mods are 0.
        root = mat_mul(engineTranslate(0.0f, 0.0f, -2.0f),
                       mat_mul(root, engineTranslate(0.0f, 0.0f, 2.0f)));
        root = mat_mul(engineTranslate(o.px + mx, o.py + my, o.pz + mz), root);
    }
    camera = mat_mul(camera, root);
    Mat4 glowCamera = camera;
    if (style == 1 && !mvpOverride)
        glowCamera = mat_mul(fieldWiden,
                             mat_mul(engineCamera(1,float(W)/float(H),false),
                                     root));

    static const float moonColors[6][3] = {
        {0.07450981f,1.0f,0.0f}, {1.0f,0.0f,0.0f},
        {1.0f,0.972549f,0.0f}, {0.0f,0.5019608f,1.0f},
        {1.0f,0.7254902f,0.0f}, {0.75f,0.75f,0.75f}
    };
    static const float moonNoteColors[5][3] = {
        {0.1397059f,1.0f,0.16360286f}, {1.0f,0.125f,0.125f},
        {0.9884381f,1.0f,0.16176468f},
        {0.14705884f,0.43529424f,1.0f},
        {1.0f,0.6267748f,0.1544118f}
    };
    static const float moonTapColors[5][3] = {
        {0.0f,1.0f,0.027777672f}, {1.0f,0.0f,0.0f},
        {1.0f,0.9724138f,0.0f}, {0.0f,0.33793116f,1.0f},
        {1.0f,0.64137936f,0.0f}
    };
    static const float moonSustainColors[5][3] = {
        {0.11724126f,1.0f,0.0f}, {1.0f,0.0f,0.0f},
        {0.986207f,1.0f,0.0f}, {0.0f,0.46206903f,1.0f},
        {1.0f,0.7241379f,0.0f}
    };
    static const float moonOpenSustain[3] = {0.9915822f,0.3897059f,1.0f};
    // ColorProfile.Defaults.cs lane colors are authored in sRGB; YARG is a
    // Linear project, and Unity converts Color properties sRGB->linear when
    // uploading (same reason the albedo textures load GL_SRGB8_ALPHA8).
    // These are srgbToLinear(hex/255): green #79D304, red #FF1D23, yellow
    // #FFE900, blue #00BFFF, orange #FF8400, open purple #C800FF.
    static const float yargColors[6][3] = {
        {0.1912017f,0.6514056f,0.0012141f},
        {1.0f,0.0122865f,0.0168074f},
        {1.0f,0.8148466f,0.0f},
        {0.0f,0.5209956f,1.0f},
        {1.0f,0.23074f,0.0f},
        {0.5775804f,0.0f,1.0f}
    };
    // Open-HOPO middles: NoteGroup.cs:82 does color + (1,1,1) in sRGB space
    // BEFORE Unity's SetColor sRGB->linear conversion, so the sum converts
    // as a whole: srgbToLinear(purple/white + 1).
    static const float yargOpenHopoColor[3] = {3.7963657f,1.0f,4.9538458f};
    static const float yargOpenHopoSp[3] = {4.9538458f,4.9538458f,
                                            4.9538458f};
    const float (*colors)[3] = style == 1 ? moonColors : yargColors;
    const float receptorAlpha = alpha*fminf(1.0f,fmaxf(0.0f,1.0f-mods.dark));
    // YARG gameplay state, closed-form of songTime (the renderer seeks).
    // Only the groove survives: the overdrive bar, combo meter and sunburst
    // are deliberately hidden (user decision -- only the SP phrase display
    // renders), and the groove keeps only the track colour change. Groove =
    // ScoreMultiplier == MaxMultiplier (4x = combo 30; chords count gems+1),
    // TrackMaterial lerps it in at dt*5 -> 1-exp(-5*dt).
    yargGroove_ = 0.0f;
    if (style == 2 && !o.noBot) {
        int combo = 0;
        double tGroove = -1.0;
        for (const Note& note : chart.notes) {
            int gems = note.open ? 1 : 0;
            for (int lane = 0; lane < 5; ++lane)
                if (note.frets & (1 << lane)) ++gems;
            if (gems == 0) continue;
            combo += gems + (gems >= 2 ? 1 : 0);
            if (combo >= 30) {
                tGroove = chart.beatToSec(note.beat);
                break;
            }
        }
        if (tGroove >= 0.0 && songTime > tGroove)
            yargGroove_ = 1.0f-expf(-5.0f*float(songTime-tGroove));
    }
    const float laneStep = style == 1 ? 1.0f : 0.4f;
    // Highway is parented at y=4; the default scene's camYMax is local 11.75.
    const float moonVisibleEnd = 4.0f + 11.75f;
    const float moonBoardEnd = 62.6f;
    auto laneX = [&](int lane) { return (float(lane) - 2.0f) * laneStep; };
    auto modX = [&](int lane, float yOffset) {
        float x = laneX(lane)+GetXPos(mods,lane,yOffset,songTime,
                                      float(beat),bpm)*laneStep/64.0f;
        if (mods.tiny != 0.0f) x *= GetTinyColScale(mods);
        return x;
    };
    auto moonPitch = [&](float x, float y, float z) {
        const float clipY = camera.m[1]*x + camera.m[5]*y +
                            camera.m[9]*z + camera.m[13];
        const float clipW = camera.m[3]*x + camera.m[7]*y +
                            camera.m[11]*z + camera.m[15];
        const float screenY = clipW != 0.0f
                            ? clipY / clipW * 0.5f + 0.5f
                            : 0.0f;
        return -75.0f - screenY * 30.0f;
    };
    const bool hasStops = !chart.stops.empty();
    auto ssec = [&](double t) { return hasStops ? chart.scrollSec(t) : t; };
    const float yPerUnit = ARROW_SIZE * 1.6f;
    auto modYOffset = [&](double sec, int lane) {
        const float raw = float(ssec(sec) - scrollNow) * noteSpeed;
        const float in = raw * yPerUnit;
        return ApplyYMods(mods,lane,in,float(beat));
    };
    auto scrollOffset = [&](double sec, int lane = -1) {
        const float raw = float(ssec(sec) - scrollNow) * noteSpeed;
        const float in = raw * yPerUnit;
        const float adjusted = ApplyYMods(mods,lane < 0 ? 0 : lane,in,float(beat));
        const float z = adjusted == in ? raw : adjusted / yPerUnit;
        // YARG's default NoteSpeed is 6 at NotClon's default speed 10.
        //
        // Moonscraper: 1.7, NOT the authentic hyperspeed-5 mapping (0.5).
        // Its world is exact -- strikeline y=0, camera span 11.75, note pool
        // Max at 15.75 -- but the editor's default hyperspeed shows 3.15 s of
        // chart where CH shows NOTE_CULL_FAR/noteSpeed = 0.787 s, so notes
        // crawl at a quarter of CH's perceived speed. 1.7 makes the trip from
        // the fade line (y 13.35) to the strikeline take 13.35/17 = 0.785 s
        // at default speed: CH's window, Moonscraper's look.
        return ApplyScrollZ(mods,z,lane) * (style == 1 ? 1.7f : 0.6f);
    };
    auto noteRotation = [&](const Note& note, float yOffset) {
        return engineNoteRotation(
            -GetRotationX(mods,yOffset),GetRotationY(mods,yOffset),
            -GetRotationZ(mods,float(note.beat),float(beat)));
    };

    auto quadXY = [&](float x0, float y0, float x1, float y1, float z,
                      float u0, float v0, float u1, float v1,
                      const float* color, float a) {
        ch::Vtx q[4] = {
            {x0,y0,z,u0,v0,color[0],color[1],color[2],a},
            {x1,y0,z,u1,v0,color[0],color[1],color[2],a},
            {x1,y1,z,u1,v1,color[0],color[1],color[2],a},
            {x0,y1,z,u0,v1,color[0],color[1],color[2],a}
        };
        const int ix[6] = {0,1,2,0,2,3};
        for (int i : ix) v_.push_back(q[i]);
    };
    auto quadXZ = [&](float x0, float z0, float x1, float z1, float y,
                      float u0, float v0, float u1, float v1,
                      const float* color, float a) {
        ch::Vtx q[4] = {
            {x0,y,z0,u0,v0,color[0],color[1],color[2],a},
            {x1,y,z0,u1,v0,color[0],color[1],color[2],a},
            {x1,y,z1,u1,v1,color[0],color[1],color[2],a},
            {x0,y,z1,u0,v1,color[0],color[1],color[2],a}
        };
        const int ix[6] = {0,1,2,0,2,3};
        for (int i : ix) v_.push_back(q[i]);
    };
    auto moonBillboard = [&](float cy, float width, float height,
                             const float* color, float a) {
        const Mat4 view = engineView(1);
        const float upY = view.m[5], upZ = view.m[9];
        const float hx = width * 0.5f, hy = height * 0.5f;
        ch::Vtx q[4] = {
            {-hx,cy-upY*hy,-upZ*hy,0,0,color[0],color[1],color[2],a},
            { hx,cy-upY*hy,-upZ*hy,1,0,color[0],color[1],color[2],a},
            { hx,cy+upY*hy, upZ*hy,1,1,color[0],color[1],color[2],a},
            {-hx,cy+upY*hy, upZ*hy,0,1,color[0],color[1],color[2],a}
        };
        const int ix[6] = {0,1,2,0,2,3};
        for (int i : ix) v_.push_back(q[i]);
    };
    auto moonFretQuad = [&](float cx, float cy, float width, float height,
                            float z, const float* color, float a) {
        const float c = cosf(12.0f * 3.14159265f / 180.0f);
        const float s = sinf(12.0f * 3.14159265f / 180.0f);
        const float x0 = cx - width * 0.5f, x1 = cx + width * 0.5f;
        const float dy0 = -height * 0.5f, dy1 = height * 0.5f;
        ch::Vtx q[4] = {
            {x0,cy+dy0*c,z+dy0*s,0,0,color[0],color[1],color[2],a},
            {x1,cy+dy0*c,z+dy0*s,1,0,color[0],color[1],color[2],a},
            {x1,cy+dy1*c,z+dy1*s,1,1,color[0],color[1],color[2],a},
            {x0,cy+dy1*c,z+dy1*s,0,1,color[0],color[1],color[2],a}
        };
        const int ix[6] = {0,1,2,0,2,3};
        for (int i : ix) v_.push_back(q[i]);
    };
    struct EngineSustainRow {
        float x, pos, bump, visible, u, rise;
    };
    auto lerpSustainRow = [](const EngineSustainRow& a,
                             const EngineSustainRow& b,float t) {
        return EngineSustainRow{
            a.x+(b.x-a.x)*t,a.pos+(b.pos-a.pos)*t,
            a.bump+(b.bump-a.bump)*t,
            a.visible+(b.visible-a.visible)*t,a.u+(b.u-a.u)*t,
            a.rise+(b.rise-a.rise)*t
        };
    };
    auto appendEngineSustain = [&](double authoredStartSec, double endSec,
                                   int lane, float halfWidth,
                                   int widthSubdivisions, const float* color,
                                   float a,
                                   std::vector<ch::Vtx>* out = nullptr) {
        double startSec = authoredStartSec;
        if (!o.noBot && songTime >= authoredStartSec) startSec = songTime;
        if (startSec >= endSec) return;

        // Must match scrollOffset's per-style scale or sustain tails detach
        // from their heads.
        const float styleScale = style == 1 ? 1.7f : 0.6f;
        float length = float(ssec(endSec)-ssec(startSec))*noteSpeed*styleScale;
        if (style == 1 && !o.noBot && songTime >= authoredStartSec) {
            if (length <= 0.2f) return;
            startSec += (endSec-startSec)*double(0.2f/length);
            length = float(ssec(endSec)-ssec(startSec))*noteSpeed*styleScale;
        }
        if (length <= 0.0f) return;
        const float rowStep = style == 1 ? 0.1f : ch::SUS_STEP_Z;
        const int rowCount = std::clamp(int(ceilf(length/rowStep))+1,2,192);
        EngineSustainRow rows[192];
        for (int i = 0; i < rowCount; ++i) {
            const float t = float(i)/float(rowCount-1);
            const double sec = startSec+(endSec-startSec)*double(t);
            const float in = modYOffset(sec,lane);
            rows[i] = {
                modX(lane,in),scrollOffset(sec,lane),
                GetYPosBump(mods,lane,in,float(beat),bpm)*laneStep/64.0f,
                fmaxf(GetAlpha(mods,in,songTime),GetGlow(mods,in,songTime)),
                style == 1 ? length*t : length*(1.0f-t),t
            };
        }

        // YARG spawns a TrackElement at (3+2+2)/NoteSpeed ahead of the strike
        // (z <= 5) and removes it at z < -4 (pos = z+2). The composite mask
        // fades everything past z ~3.4, so nothing further needs drawing.
        const float visibleStart = style == 1 ? -8.47f : -2.0f;
        const float visibleEnd = style == 1 ? moonVisibleEnd : 7.0f+length;
        float moonU = 0.0f;
        for (int row = 0; row+1 < rowCount; ++row) {
            EngineSustainRow r0 = rows[row], r1 = rows[row+1];
            const float dp = r1.pos-r0.pos;
            float t0 = 0.0f, t1 = 1.0f;
            if (fabsf(dp) < 0.000001f) {
                if (r0.pos < visibleStart || r0.pos > visibleEnd) continue;
            } else {
                const float a0 = (visibleStart-r0.pos)/dp;
                const float a1 = (visibleEnd-r0.pos)/dp;
                t0 = fmaxf(0.0f,fminf(a0,a1));
                t1 = fminf(1.0f,fmaxf(a0,a1));
                if (t1 <= t0) continue;
            }
            const EngineSustainRow source0 = r0, source1 = r1;
            r0 = lerpSustainRow(source0,source1,t0);
            r1 = lerpSustainRow(source0,source1,t1);
            if (style == 1) {
                r0.u = moonU;
                const float dx = r1.x-r0.x;
                const float dy = r1.pos-r0.pos;
                const float dz = r1.bump-r0.bump;
                moonU += sqrtf(dx*dx+dy*dy+dz*dz);
                r1.u = moonU;
                ch::Vtx q[4] = {
                    {r0.x-halfWidth,r0.pos,r0.bump,r0.u,0,
                     color[0],color[1],color[2],a*r0.visible},
                    {r0.x+halfWidth,r0.pos,r0.bump,r0.u,1,
                     color[0],color[1],color[2],a*r0.visible},
                    {r1.x+halfWidth,r1.pos,r1.bump,r1.u,1,
                     color[0],color[1],color[2],a*r1.visible},
                    {r1.x-halfWidth,r1.pos,r1.bump,r1.u,0,
                     color[0],color[1],color[2],a*r1.visible}
                };
                const int ix[6] = {0,1,2,0,2,3};
                for (int index : ix) v_.push_back(q[index]);
                continue;
            }
            for (int width = 0; width < widthSubdivisions; ++width) {
                std::vector<ch::Vtx>& dst = out ? *out : v_;
                const float w0 = float(width)/float(widthSubdivisions);
                const float w1 = float(width+1)/float(widthSubdivisions);
                const float x00 = r0.x-halfWidth+2.0f*halfWidth*w0;
                const float x01 = r0.x-halfWidth+2.0f*halfWidth*w1;
                const float x10 = r1.x-halfWidth+2.0f*halfWidth*w0;
                const float x11 = r1.x-halfWidth+2.0f*halfWidth*w1;
                ch::Vtx q[4] = {
                    {x00,0.01f+0.01f*r0.rise+r0.bump,-2.001f+r0.pos,
                     r0.u,1.0f-w0,color[0],color[1],color[2],a*r0.visible},
                    {x01,0.01f+0.01f*r0.rise+r0.bump,-2.001f+r0.pos,
                     r0.u,1.0f-w1,color[0],color[1],color[2],a*r0.visible},
                    {x11,0.01f+0.01f*r1.rise+r1.bump,-2.001f+r1.pos,
                     r1.u,1.0f-w1,color[0],color[1],color[2],a*r1.visible},
                    {x10,0.01f+0.01f*r1.rise+r1.bump,-2.001f+r1.pos,
                     r1.u,1.0f-w0,color[0],color[1],color[2],a*r1.visible}
                };
                const int ix[6] = {0,1,2,0,2,3};
                for (int index : ix) dst.push_back(q[index]);
            }
        }
    };
    auto sceneSetup = [&]() {
        glUseProgram(prog_);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "uMVP"), 1, GL_FALSE,
                           camera.m);
        glUniform3f(glGetUniformLocation(prog_, "uOffset"), 0, 0, 0);
        glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
        glUniform1f(glGetUniformLocation(prog_, "uCurve"),
                    style == 2 ? 0.5f : 0.0f);
        if (style == 2) {
            const float pitch = 24.12f * 3.14159265f / 180.0f;
            const float fy = -sinf(pitch), fz = cosf(pitch);
            const float fadeStart = fabsf(2.66f*fy + (-4.86f-1.75f)*fz);
            const float fadeEnd = fabsf(2.66f*fy + (-4.86f-3.0f)*fz);
            glUniform4f(glGetUniformLocation(prog_, "uFadePlane"),
                        0.0f,0.0f,1.0f,4.86f);
            glUniform2f(glGetUniformLocation(prog_, "uFadeRange"),
                        fadeStart,fadeEnd);
        } else {
            // Same fade as the mesh path: Moonscraper's board quad runs to
            // y 62.6, and without this it draws the full length -- the
            // "highway too long" symptom. Values derived in drawEngineMesh.
            glUniform4f(glGetUniformLocation(prog_, "uFadePlane"),0,1,0,0);
            glUniform2f(glGetUniformLocation(prog_, "uFadeRange"),
                        13.35f,13.59f);
        }
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glActiveTexture(GL_TEXTURE0);
    };
    auto drawYargEffect = [&](GLuint texture, const float* emission,
                              float visibility, float sweepTime) {
        if (v_.empty()) return;
        glUseProgram(yargEffect_);
        glUniformMatrix4fv(glGetUniformLocation(yargEffect_,"uMVP"),1,
                           GL_FALSE,camera.m);
        glUniform3f(glGetUniformLocation(yargEffect_,"uOffset"),0,0,0);
        glUniform1f(glGetUniformLocation(yargEffect_,"uCurve"),0.5f);
        glUniform4f(glGetUniformLocation(yargEffect_,"uFadePlane"),
                    0.0f,0.0f,1.0f,4.86f);
        // No per-element fade: the alpha mask owns it at composite time.
        glUniform2f(glGetUniformLocation(yargEffect_,"uFadeRange"),
                    0.0f,0.0f);
        glUniform3f(glGetUniformLocation(yargEffect_,"uEmission"),
                    emission[0]*fieldTint_[0],emission[1]*fieldTint_[1],
                    emission[2]*fieldTint_[2]);
        glUniform1f(glGetUniformLocation(yargEffect_,"uVisibility"),visibility);
        glUniform1f(glGetUniformLocation(yargEffect_,"uSweepTime"),sweepTime);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,texture);
        glUniform1i(glGetUniformLocation(yargEffect_,"uTex"),0);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER,vbo_);
        glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(v_.size()*sizeof(ch::Vtx)),
                     v_.data(),GL_STREAM_DRAW);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glDrawArrays(GL_TRIANGLES,0,GLsizei(v_.size()));
        glDepthMask(GL_TRUE);
        glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        v_.clear();
    };
    auto beginMoonMeshPass = [&]() {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_CULL_FACE);
        // Moon's source OBJ is CCW; the projection's X reflection flips it.
        glCullFace(GL_FRONT);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    };
    auto endMoonMeshPass = [&]() {
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
    };
    auto drawMoonOpaque = [&](const EngineGpu& mesh, const Mat4& model, int kind,
                              const float* color, float a, bool open) {
        beginMoonMeshPass();
        glDepthMask(GL_TRUE);
        if (kind >= 14 && kind <= 17) {
            for (int material = 0; material < 4; ++material) {
                if (kind == 16 && material == 3) continue;
                drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,material);
            }
        } else if (open) {
            for (int material = 0; material < 4; ++material)
                drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,material);
        } else if (kind == 10) {
            drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,2);
        } else {
            drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,1);
            drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,2);
        }
        endMoonMeshPass();
    };
    auto drawMoonTransparent = [&](const EngineGpu& mesh, const Mat4& model,
                                   int kind, const float* color, float a,
                                   bool open) {
        if (kind >= 14 && kind <= 17) {
            beginMoonMeshPass();
            glDepthMask(GL_FALSE);
            if (kind == 16)
                drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,3);
            drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,4);
            endMoonMeshPass();
            return;
        }
        if (open) return;
        beginMoonMeshPass();
        glDepthMask(GL_FALSE);
        if (kind == 10) {
            drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,0);
            drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,1);
        } else {
            drawEngineMesh(mesh,camera,model,kind,color,a,0,0,0,0,0);
        }
        endMoonMeshPass();
    };

    std::vector<BeatLine> engineBeatLines;
    if (style == 1 || style == 2) {
        const bool moonHasAudio = style == 1 && o.audioDuration > 0.0;
        int lastTick = moonHasAudio
                     ? std::max(0,int(lrint(chart.secToBeat(o.audioDuration) *
                                           chart.resolution)))
                     : style == 1 ? chart.resolution * 16 : -1;
        for (const Note& note : chart.notes) {
            int endTick = note.tick;
            for (double sustain : note.sustain)
                endTick = std::max(endTick,
                                  note.tick + int(lrint(sustain * chart.resolution)));
            endTick = std::max(endTick,
                               note.tick + int(lrint(note.openSustain * chart.resolution)));
            if (!moonHasAudio)
                lastTick = std::max(lastTick,endTick +
                                    (style == 1 ? chart.resolution * 4 : 0));
        }
        for (size_t tsi = 0; tsi < chart.timesigs.size(); ++tsi) {
            const TimeSig& ts = chart.timesigs[tsi];
            const int regionEnd = tsi + 1 < chart.timesigs.size()
                                ? chart.timesigs[tsi + 1].tick : lastTick + 1;
            const int beatGap = chart.resolution * 4 / ts.den;
            const int measureGap = beatGap * ts.num;
            if (beatGap <= 0 || measureGap <= 0) continue;
            if (style == 1) {
                for (int measure = ts.tick; measure < regionEnd;
                     measure += measureGap) {
                    engineBeatLines.push_back({chart.tickToSec(measure), 0});
                    for (int b = 1; b < ts.num; ++b) {
                        const int tick = measure + b * beatGap;
                        if (tick < regionEnd)
                            engineBeatLines.push_back({chart.tickToSec(tick), 1});
                    }
                    for (int b = 0; b < ts.num; ++b) {
                        const int tick = measure + b * beatGap + beatGap / 2;
                        if (tick < regionEnd)
                            engineBeatLines.push_back({chart.tickToSec(tick), 2});
                    }
                }
            } else {
                const int strongRate = ts.den <= 4 ? 1 : ts.den / 4;
                int count = 0;
                for (int tick = ts.tick; tick < regionEnd; tick += beatGap) {
                    const int inMeasure = count % ts.num;
                    int lineStyle = 2;
                    if (ts.num == 1) {
                        lineStyle = count == 0 ? 0
                                  : count % strongRate == 0 ? 1 : 2;
                    } else if (inMeasure == 0) {
                        lineStyle = 0;
                    } else if (ts.den <= 4) {
                        lineStyle = 1;
                    } else if (inMeasure % strongRate == 0) {
                        lineStyle = inMeasure == ts.num - 1 ? 2 : 1;
                    }
                    engineBeatLines.push_back({chart.tickToSec(tick), lineStyle});
                    ++count;
                }
            }
        }
        std::sort(engineBeatLines.begin(), engineBeatLines.end(),
                  [](const BeatLine& a, const BeatLine& b) {
                      return a.sec < b.sec;
                  });
    }

    struct MoonGemDraw {
        const EngineGpu* mesh;
        Mat4 model;
        int kind;
        const float* color;
        float alpha;
        bool open;
    };
    std::vector<MoonGemDraw> moonGems;
    if (style == 1) {
        for (int i = int(chart.notes.size()) - 1; i >= 0; --i) {
            const Note& note = chart.notes[i];
            const double hitSec = chart.beatToSec(note.beat);
            if (!o.noBot && songTime >= hitSec) continue;
            const bool starPower = chart.phraseAt(PhraseType::StarPower,
                                                   note.tick) != nullptr;

            if (note.open) {
                const float in = modYOffset(hitSec,2);
                const float pos = scrollOffset(hitSec,2);
                if (pos >= -8.47f && pos <= moonVisibleEnd) {
                    const float visible = fmaxf(GetAlpha(mods,in,songTime),
                                                GetGlow(mods,in,songTime));
                    if (visible > 0.0f) {
                        const float x = modX(2,in);
                        const float pz = GetYPosBump(mods,2,in,float(beat),bpm) /
                                         64.0f;
                        const Mat4 model = mat_mul(
                            engineTranslate(x,pos,pz),
                            mat_mul(engineRotateX(moonPitch(x,pos,pz)),
                                    mat_mul(noteRotation(note,in),
                                            engineScale(0.7f,0.7f,0.7f))));
                        const NoteType openType = note.tap
                                                ? NoteType::Hopo
                                                : note.openType;
                        const int kind = starPower
                                       ? (openType == NoteType::Strum ? 18 : 19)
                                       : (openType == NoteType::Hopo ? 13 : 11);
                        moonGems.push_back({&moonOpen_,model,kind,colors[5],
                                            alpha*visible,true});
                    }
                }
            }

            for (int lane = 0; lane < 5; ++lane) {
                if (!(note.frets & (1 << lane))) continue;
                const float in = modYOffset(hitSec,lane);
                const float pos = scrollOffset(hitSec,lane);
                if (pos < -8.47f || pos > moonVisibleEnd) continue;
                const float visible = fmaxf(GetAlpha(mods,in,songTime),
                                            GetGlow(mods,in,songTime));
                if (visible <= 0.0f) continue;
                const float x = modX(lane,in);
                const float pz = GetYPosBump(mods,lane,in,float(beat),bpm) / 64.0f;
                const float noteZoom = GetZoom(mods,lane);
                const bool tap = note.type == NoteType::Tap;
                const int kind = starPower
                               ? (tap ? 16
                                  : note.type == NoteType::Hopo ? 15 : 14)
                               : (tap ? 10
                                  : note.type == NoteType::Hopo ? 9 : 8);
                const float* color = tap ? moonTapColors[lane]
                                         : moonNoteColors[lane];
                const float noteScale = starPower ? 0.6f : 0.45f;
                const Mat4 model = mat_mul(
                    engineTranslate(x,pos,pz),
                    mat_mul(engineRotateX(moonPitch(x,pos,pz)),
                            mat_mul(noteRotation(note,in),
                                    engineScale(noteScale*noteZoom,
                                                noteScale*noteZoom,
                                                noteScale*noteZoom))));
                moonGems.push_back({starPower ? &moonSp_ : &moonNote_,
                                    model,kind,color,
                                    alpha*visible,false});
            }
        }
        for (const Phrase& phrase : chart.phrases) {
            if (phrase.type != PhraseType::StarPower) continue;
            const float pos = scrollOffset(chart.tickToSec(phrase.tick));
            if (pos < -8.47f || pos > moonVisibleEnd) continue;
            const Mat4 model = mat_mul(
                engineTranslate(-3.0f,pos,0.0f),
                mat_mul(engineRotateX(-90.0f),
                        engineScale(0.6f,0.6f,0.6f)));
            moonGems.push_back({&moonSp_,model,17,moonColors[5],alpha,false});
        }
    }

    if (style == 2 && !o.noPost) {
        struct AoDraw {
            const EngineGpu* mesh;
            Mat4 model;
        };
        std::vector<AoDraw> aoDraws;
        aoDraws.reserve(chart.notes.size()*2+7);
        if (!o.playfield && mods.hideboard == 0.0f) {
            for (int side : {-1,1}) {
                const float qx = side < 0 ? 0.00073294336f : 0.0f;
                const float qy = side < 0 ? -0.99999976f : -1.0f;
                const float qz = side < 0 ? -0.00000003556937f : 0.0f;
                const float qw = side < 0 ? 0.0000029504597f : 0.0f;
                aoDraws.push_back({&yargTrackTrim_,
                    engineModel(side*1.015f,0.0f,7.74f,qx,qy,qz,qw,
                                -side*0.02045422f,0.02045422f,10.86853f)});
            }
        }
        for (int lane = 0; receptorAlpha > 0.0f && lane < 5; ++lane) {
            const float in = modYOffset(songTime,lane);
            const float pos = scrollOffset(songTime,lane);
            const float bump = GetYPosBump(mods,lane,in,float(beat),bpm);
            const Mat4 fretRoot = engineTranslate(
                modX(lane,in),bump*laneStep/64.0f,-2.0f+pos);
            const Mat4 fretParent = engineModel(
                0.0f,-0.0057729f,-0.0025355f,
                0.025898216f,0.0f,0.0f,0.9996646f,1.01f,1.2902225f,1.0f);
            const Mat4 fretMesh = engineModel(
                0.0f,0.0072f,0.0f,-0.0000021f,0.7098884f,0.7043143f,
                0.0000020f,0.19734741f,0.10530957f,0.0801998f);
            aoDraws.push_back({&yargFret_,
                               mat_mul(fretRoot,mat_mul(fretParent,fretMesh))});
        }
        for (int i = int(chart.notes.size())-1; i >= 0; --i) {
            const Note& note = chart.notes[i];
            const double hitSec = chart.beatToSec(note.beat);
            if (!o.noBot && songTime >= hitSec) continue;
            if (note.open) {
                const float in = modYOffset(hitSec,2);
                const float pos = scrollOffset(hitSec,2);
                if (pos >= -2.0f && pos <= 7.0f) {
                    const float x = modX(2,in);
                    const float bump = GetYPosBump(mods,2,in,float(beat),bpm);
                    const bool hopo = note.tap ||
                                      note.openType != NoteType::Strum;
                    const Mat4 openModel = mat_mul(
                        engineTranslate(x,0.032f+bump*laneStep/64.0f,
                                        -2.0f+pos+(hopo ? 0.030f : 0.0315f)),
                        mat_mul(noteRotation(note,in),
                                engineScale(0.07593973f,0.087584f,0.05f)));
                    aoDraws.push_back({&yargOpen_,openModel});
                }
            }
            for (int lane = 0; lane < 5; ++lane) {
                if (!(note.frets & (1 << lane))) continue;
                const float in = modYOffset(hitSec,lane);
                const float pos = scrollOffset(hitSec,lane);
                if (pos < -2.0f || pos > 7.0f) continue;
                const float x = modX(lane,in);
                const float bump = GetYPosBump(mods,lane,in,float(beat),bpm);
                const float noteZoom = GetZoom(mods,lane);
                const bool hopo = note.type == NoteType::Hopo;
                const bool tap = note.type == NoteType::Tap;
                const EngineGpu* mesh = tap ? &yargTap_
                                      : hopo ? &yargHopo_ : &yargNormal_;
                const float sx = (tap||hopo ? 0.045f : 0.05f)*noteZoom;
                const float sy = (tap||hopo ? 0.0325f : 0.0375f)*noteZoom;
                const float sz = (tap||hopo ? 0.065f : 0.07f)*noteZoom;
                const float py = (tap||hopo ? 0.073f : 0.0775f)+
                                 bump*laneStep/64.0f;
                const float pz = -2.0f+pos+(tap||hopo ? 0.0465f : 0.054f);
                const Mat4 model = mat_mul(
                    engineTranslate(x,py,pz),
                    mat_mul(engineQuaternion(0,0.7071068f,0.7071068f,0),
                            mat_mul(noteRotation(note,in),
                                    engineScale(sx,sy,sz))));
                aoDraws.push_back({mesh,model});
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER,yargNormalFbo_);
        glViewport(0,0,yargW_,yargH_);
        glClearColor(0,0,0,0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glUseProgram(yargNormalProg_);
        glUniformMatrix4fv(glGetUniformLocation(yargNormalProg_,"uMVP"),1,
                           GL_FALSE,camera.m);
        glUniform1f(glGetUniformLocation(yargNormalProg_,"uCurve"),0.5f);
        for (const AoDraw& draw : aoDraws) {
            glUniformMatrix4fv(glGetUniformLocation(yargNormalProg_,"uModel"),1,
                               GL_FALSE,draw.model.m);
            const Mat4& model = draw.model;
            const float determinant =
                model.m[0]*(model.m[5]*model.m[10]-model.m[6]*model.m[9])-
                model.m[4]*(model.m[1]*model.m[10]-model.m[2]*model.m[9])+
                model.m[8]*(model.m[1]*model.m[6]-model.m[2]*model.m[5]);
            glEnable(GL_CULL_FACE);
            glCullFace(determinant < 0.0f ? GL_BACK : GL_FRONT);
            glBindVertexArray(draw.mesh->vao);
            glDrawArrays(GL_TRIANGLES,0,draw.mesh->count);
        }
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glBindVertexArray(qvao_);

        glBindFramebuffer(GL_FRAMEBUFFER,yargAoFbo_[0]);
        glUseProgram(yargAoEstimate_);
        glUniform1i(glGetUniformLocation(yargAoEstimate_,"uNormal"),0);
        glUniform1i(glGetUniformLocation(yargAoEstimate_,"uDepth"),1);
        glUniform2f(glGetUniformLocation(yargAoEstimate_,"uSize"),
                    float(yargW_),float(yargH_));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,yargNormalTex_);
        glActiveTexture(GL_TEXTURE0+1);
        glBindTexture(GL_TEXTURE_2D,yargAoDepth_);
        glDrawArrays(GL_TRIANGLES,0,3);

        for (int pass = 0; pass < 2; ++pass) {
            glBindFramebuffer(GL_FRAMEBUFFER,yargAoFbo_[pass+1]);
            glUseProgram(yargAoBlur_);
            glUniform1i(glGetUniformLocation(yargAoBlur_,"uTex"),0);
            glUniform2f(glGetUniformLocation(yargAoBlur_,"uTexel"),
                        1.0f/float(yargW_),1.0f/float(yargH_));
            glUniform2f(glGetUniformLocation(yargAoBlur_,"uDirection"),
                        pass == 0 ? 1.0f : 0.0f,pass == 0 ? 0.0f : 1.0f);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,yargAoTex_[pass]);
            glDrawArrays(GL_TRIANGLES,0,3);
        }
        glBindFramebuffer(GL_FRAMEBUFFER,yargAoFbo_[3]);
        glUseProgram(yargAoFinal_);
        glUniform1i(glGetUniformLocation(yargAoFinal_,"uTex"),0);
        glUniform2f(glGetUniformLocation(yargAoFinal_,"uTexel"),
                    1.0f/float(yargW_),1.0f/float(yargH_));
        glBindTexture(GL_TEXTURE_2D,yargAoTex_[2]);
        glDrawArrays(GL_TRIANGLES,0,3);
        glActiveTexture(GL_TEXTURE0);
        glBindFramebuffer(GL_FRAMEBUFFER,yargMsaaFbo_);
        glViewport(0,0,yargW_,yargH_);
    }

    if (style == 1) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
    } else {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    const float white[3] = {1,1,1};
    const float moonAmbient[3] = {0.212f,0.227f,0.259f};
    std::vector<ch::Vtx> yargBeatLines;
    // Sustains split by live state: SustainWave animates and emission goes
    // x3 only while the bot is holding (YARG's _IsActive), and the batch
    // shares one uniform set, so idle and active ribbons draw separately.
    std::vector<ch::Vtx> yargFrettedSustains;
    std::vector<ch::Vtx> yargFrettedSustainsActive;
    std::vector<ch::Vtx> yargOpenSustains;
    std::vector<ch::Vtx> yargOpenSustainsActive;
    // HighwaysAlphaMask: every mask-eligible mesh draw (track, trims, frets,
    // notes) is recorded as it is issued and replayed with the fade shader
    // into the R8 mask target. Beatline/solo quads sit at the track's own z,
    // so their mask contribution is always a no-op; sustains are FadeExclude.
    struct MaskDraw { const EngineGpu* gpu; Mat4 model; };
    std::vector<MaskDraw> yargMaskDraws;
    std::vector<ch::Vtx> moonGlowOccluders;
    std::vector<ch::Vtx> moonGlowDepthBlockers;
    float moonIndicatorDt[5] = {-1,-1,-1,-1,-1};
    if (style == 1 && !o.noBot) {
        for (int lane = 0; lane < 5; ++lane)
            moonIndicatorDt[lane] = float(lastHit(lane,songTime));
        for (const Note& note : chart.notes) {
            const double hitSec = chart.beatToSec(note.beat);
            if (hitSec > songTime) break;
            if (note.open) {
                const float dt = float(songTime-hitSec);
                for (float& laneDt : moonIndicatorDt)
                    if (laneDt < 0.0f || dt < laneDt) laneDt = dt;
                if (note.openSustain > 0.0) {
                    const double endSec = chart.beatToSec(
                        note.beat+note.openSustain);
                    if (songTime < endSec)
                        for (float& laneDt : moonIndicatorDt) laneDt = 0.0f;
                }
            }
            for (int lane = 0; lane < 5; ++lane) {
                if (!(note.frets & (1 << lane)) ||
                    note.sustain[lane] <= 0.0) continue;
                const double endSec = chart.beatToSec(
                    note.beat+note.sustain[lane]);
                if (songTime < endSec) moonIndicatorDt[lane] = 0.0f;
            }
        }
    }
    if (!o.playfield && mods.hideboard == 0.0f) {
        if (style == 1) {
            sceneSetup();
            quadXY(-2.5f,-1.4f,2.5f,moonBoardEnd,0.01f,0,0,1,4,
                   moonAmbient,alpha);
            moonGlowDepthBlockers.insert(moonGlowDepthBlockers.end(),
                                         v_.begin(),v_.end());
            drawLayer(texMoonHighway_.id, ch::BLEND_NECK);
        } else {
            // Track FIRST: its shader sits in render queue 1950 (before the
            // default transparent queue), so YARG itself draws it before the
            // frets and notes. Drawing it last made it replace anything the
            // mods pushed below the track plane -- a per-lane negative movey
            // sank a fret under the surface and the track's LEQUAL pass
            // painted over it (moon never had this: it is painter's order
            // throughout). Track.shadergraph is SurfaceType TRANSPARENT with
            // ZWriteControl Auto -> depth writes OFF (the old "opaque,
            // depth-written" reading was wrong -- pixels matched over the
            // black clear, but the phantom track depth z-killed every
            // coplanar transparent layer above it, most visibly the
            // beatlines 0.002 up). Keep the colour path as-is; just do not
            // write depth, like YARG.
            const Mat4 track = engineModel(0.0f,-0.001f,15.0f,
                                           -0.000002f,0.7071068f,0.7071068f,
                                           0.000002f,1.01f,6.0f,1.0f);
            glDisable(GL_BLEND);
            glDepthMask(GL_FALSE);
            drawEngineMesh(yargTrack_,camera,track,6,white,alpha,
                           texYargTrackFade_.id,texYargTrackSmall_.id,
                           texYargTrackSide_.id,
                           scrollNow*noteSpeed*0.15f);
            glDepthMask(GL_TRUE);
            glEnable(GL_BLEND);
            yargMaskDraws.push_back({&yargTrack_,track});
            for (int side : {-1, 1}) {
                const float trimQx = side < 0 ? 0.00073294336f : 0.0f;
                const float trimQy = side < 0 ? -0.99999976f : -1.0f;
                const float trimQz = side < 0 ? -0.00000003556937f : 0.0f;
                const float trimQw = side < 0 ? 0.0000029504597f : 0.0f;
                const Mat4 trim = engineModel(side * 1.015f,0.0f,7.74f,
                                               trimQx,trimQy,trimQz,trimQw,
                                               -side * 0.02045422f,
                                               0.02045422f,10.86853f);
                drawEngineMesh(yargTrackTrim_, camera, trim, 7, white, alpha,
                               texYargTrackTrim_.id);
                yargMaskDraws.push_back({&yargTrackTrim_,trim});
            }
        }

        sceneSetup();
        const std::vector<BeatLine>& beatLines = engineBeatLines;
        for (const BeatLine& line : beatLines) {
            const float pos = scrollOffset(line.sec);
            if (style == 1) {
                if (pos < -8.47f || pos > moonVisibleEnd) continue;
                const float h = line.style == 0 ? 0.08f : 0.04f;
                moonBillboard(pos,5.0f,h,white,alpha);
                moonGlowOccluders.insert(moonGlowOccluders.end(),
                                         v_.begin(),v_.end());
                GLuint tex = line.style == 0 ? texMoonMeasure_.id
                           : line.style == 2 ? texMoonBeatWeak_.id
                                             : texMoonBeat_.id;
                drawLayer(tex, ch::BLEND_SPRITE);
            } else {
                const float z = -2.0f + pos;
                if (z <= -4.0f || z > 5.0f) continue;
                // BeatlineElement.cs:10-16: yScale/alpha 0.07/0.6 measure,
                // 0.05/0.4 strong, 0.03/0.3 weak; Beatline.prefab Mesh sits
                // at y = 0.002 over the track, scale x 2 (half-width 1).
                // The mesh is Quad.fbx's "Dense Quad" -- 11 faces across the
                // width -- so the per-vertex curve can bend the line along
                // the track's arc; a 2-triangle quad interpolates the
                // rail-to-rail chord and sags below the curled surface.
                const float thickness = line.style == 0 ? 0.07f
                                      : line.style == 2 ? 0.03f : 0.05f;
                const float lineAlpha = line.style == 0 ? 0.6f
                                      : line.style == 2 ? 0.3f : 0.4f;
                for (int seg = 0; seg < 11; ++seg) {
                    const float u0 = float(seg)/11.0f;
                    const float u1 = float(seg+1)/11.0f;
                    ch::quadFlat(v_,-1.0f+2.0f*u0,z-thickness*0.5f,
                                 -1.0f+2.0f*u1,z+thickness*0.5f,0.002f,
                                 u0,0,u1,1,white[0],white[1],white[2],
                                 alpha*lineAlpha);
                }
            }
        }
        if (style == 2) yargBeatLines = std::move(v_);
        if (style == 1) {
            sceneSetup();
            quadXY(-2.5f,-1.4f,-2.45f,moonBoardEnd,0.0f,0,0,1,16,white,alpha);
            quadXY(2.45f,-1.4f,2.5f,moonBoardEnd,0.0f,0,0,1,16,white,alpha);
            moonGlowOccluders.insert(moonGlowOccluders.end(),
                                     v_.begin(),v_.end());
            drawLayer(texMoonRail_.id, ch::BLEND_SPRITE);
        }
    }

    if (style == 1) {
        sceneSetup();
        const float tailColor[3] = {0.0f,0.91724133f,1.0f};
        for (const Phrase& phrase : chart.phrases) {
            if (phrase.type != PhraseType::StarPower || phrase.length <= 0)
                continue;
            const float start = scrollOffset(chart.tickToSec(phrase.tick));
            const float end = scrollOffset(chart.tickToSec(phrase.tick + phrase.length));
            if (end < -8.47f || start > moonVisibleEnd) continue;
            quadXY(-3.5f,std::max(start,-8.47f),-2.5f,
                   std::min(end,moonVisibleEnd),0.0f,0,0,1,1,
                   tailColor,alpha);
        }
        moonGlowOccluders.insert(moonGlowOccluders.end(),v_.begin(),v_.end());
        drawLayer(texMoonSpTail_.id,ch::BLEND_SPRITE);
    }

    if (style == 1) {
        for (const MoonGemDraw& gem : moonGems)
            drawMoonOpaque(*gem.mesh,gem.model,gem.kind,gem.color,
                           gem.alpha,gem.open);

        sceneSetup();
        for (int lane = 0; lane < 5; ++lane) {
            if (receptorAlpha <= 0.0f) continue;
            const float dt = !o.noBot ? moonIndicatorDt[lane] : -1.0f;
            if (dt >= 0.0f && dt < 0.2f / 2.2f) continue;
            const float in = modYOffset(songTime,lane);
            const float pos = scrollOffset(songTime,lane);
            const float bump = GetYPosBump(mods,lane,in,float(beat),bpm)/64.0f;
            moonFretQuad(modX(lane,in),pos,101.0f/120.0f,101.0f/120.0f,
                         bump,colors[lane],receptorAlpha);
        }
        moonGlowOccluders.insert(moonGlowOccluders.end(),v_.begin(),v_.end());
        drawLayer(texMoonStrike_.id, ch::BLEND_SPRITE);
    }

    // Sustain ribbons use each source renderer's own highway plane and art.
    std::vector<ch::Vtx> moonFrettedSustains;
    sceneSetup();
    for (int i = int(chart.notes.size()) - 1; i >= 0; --i) {
        const Note& note = chart.notes[i];
        const double hitSec = chart.beatToSec(note.beat);
        const bool starPower = style == 2 &&
            chart.phraseAt(PhraseType::StarPower,note.tick) != nullptr;
        for (int lane = 0; lane < 5; ++lane) {
            if (!(note.frets & (1 << lane)) || note.sustain[lane] <= 0.0) continue;
            const double endSec = chart.beatToSec(note.beat + note.sustain[lane]);
            const bool holding = !o.noBot && songTime >= hitSec &&
                                 songTime < endSec;
            appendEngineSustain(hitSec,endSec,lane,
                                style == 1 ? 0.5f : 0.4f,4,
                                style == 1 ? moonSustainColors[lane]
                                           : starPower ? white : colors[lane],
                                alpha,
                                style == 2 ? (holding
                                    ? &yargFrettedSustainsActive
                                    : &yargFrettedSustains) : nullptr);
        }
    }
    if (style == 1) {
        moonGlowOccluders.insert(moonGlowOccluders.end(),v_.begin(),v_.end());
        moonFrettedSustains = std::move(v_);
    }

    sceneSetup();
    for (int i = int(chart.notes.size()) - 1; i >= 0; --i) {
        const Note& note = chart.notes[i];
        if (!note.open || note.openSustain <= 0.0) continue;
        const bool starPower = style == 2 &&
            chart.phraseAt(PhraseType::StarPower,note.tick) != nullptr;
        const double hitSec = chart.beatToSec(note.beat);
        const double endSec = chart.beatToSec(note.beat + note.openSustain);
        const bool holding = !o.noBot && songTime >= hitSec &&
                             songTime < endSec;
        appendEngineSustain(hitSec,endSec,2,
                            style == 1 ? 2.0f : 1.0f,16,
                            style == 1 ? moonOpenSustain
                                       : starPower ? white : yargColors[5],
                            alpha,
                            style == 2 ? (holding
                                ? &yargOpenSustainsActive
                                : &yargOpenSustains) : nullptr);
    }
    if (style == 1) {
        moonGlowOccluders.insert(moonGlowOccluders.end(),v_.begin(),v_.end());
        drawLayer(texMoonSustainOpen_.id,ch::BLEND_SPRITE);
    }
    if (style == 1) {
        v_ = std::move(moonFrettedSustains);
        drawLayer(texMoonSustainFretted_.id,ch::BLEND_SPRITE);
    }

    // YARG fret hit light: EffectLight mode Normal holds full intensity for
    // _fadeOutRate = 100 ms then hard-cuts (EffectLight.cs:53-70,96-101).
    // Point Light: intensity 4, range 0.3, offset (0,0.220,-0.0046) from the
    // fret root (Light local (0,0.147,-0.0156) under Hit Effects
    // (0,0.073,0.011)); color = the fret's particle colour. Uploaded (and
    // zeroed) every frame so an editor style switch cannot leak stale
    // lights into the Moonscraper path.
    {
        int lightCount = 0;
        float lightPos[15] = {};
        float lightColor[15] = {};
        if (style != 1 && !o.noBot) {
            for (int lane = 0; lane < 5; ++lane) {
                const double dt = lastHit(lane,songTime);
                if (dt < 0.0 || dt >= 0.1) continue;
                const float yOff = modYOffset(songTime,lane);
                const float pos = scrollOffset(songTime,lane);
                const float bump = GetYPosBump(mods,lane,yOff,float(beat),
                                               bpm)*laneStep/64.0f;
                lightPos[lightCount*3+0] = modX(lane,yOff);
                lightPos[lightCount*3+1] = bump+0.220f;
                lightPos[lightCount*3+2] = -2.0046f+pos;
                for (int c = 0; c < 3; ++c)
                    lightColor[lightCount*3+c] = colors[lane][c]*4.0f;
                ++lightCount;
            }
        }
        hitLightCount_ = lightCount;
        std::memcpy(hitLightPos_,lightPos,sizeof(lightPos));
        std::memcpy(hitLightColor_,lightColor,sizeof(lightColor));
    }
    // YARG's rectangular fret hierarchy is a pair of nested source transforms.
    if (style != 1) {
        bool fretHeld[5] = {};
        if (!o.noBot) {
            for (const Note& note : chart.notes) {
                const double hitSec = chart.beatToSec(note.beat);
                for (int lane = 0; lane < 5; ++lane) {
                    if (!(note.frets & (1 << lane)) ||
                        note.sustain[lane] <= 0.0) continue;
                    const double endSec = chart.beatToSec(
                        note.beat+note.sustain[lane]);
                    if (songTime >= hitSec && songTime < endSec)
                        fretHeld[lane] = true;
                }
            }
            for (int lane = 0; lane < 5; ++lane) {
                const float dt = float(lastHit(lane,songTime));
                fretHeld[lane] = fretHeld[lane] ||
                                 (dt >= 0.0f && dt < 1.0f/60.0f);
            }
        }
        for (int lane = 0; lane < 5; ++lane) {
            if (receptorAlpha <= 0.0f) continue;
            const float in = modYOffset(songTime,lane);
            const float pos = scrollOffset(songTime,lane);
            const float bump = GetYPosBump(mods,lane,in,float(beat),bpm);
            const Mat4 fretRoot = engineTranslate(
                modX(lane,in),bump*laneStep/64.0f,-2.0f+pos);
            const Mat4 fretParent = engineModel(0.0f,-0.0057729f,-0.0025355f,
                                                 0.025898216f,0.0f,0.0f,
                                                 0.9996646f,1.01f,1.2902225f,1.0f);
            const Mat4 fretMesh = engineModel(0.0f,0.0072f,0.0f,
                                               -0.0000021f,0.7098884f,0.7043143f,
                                               0.0000020f,0.19734741f,
                                               0.10530957f,0.0801998f);
            const Mat4 fret = mat_mul(fretRoot,mat_mul(fretParent,fretMesh));
            // FretHit.anim pops the colour shell (mesh 0 = materials 0/1)
            // +0.38 raw units along the FBX node's local z over 133 ms
            // (keys 0 -> peak at 66.7 ms -> 0, auto tangents = a smoothstep
            // hump). FretSustained.anim is the same hump at 0.06 amplitude
            // with m_LoopTime 1, looping while the Sustain bool holds. The
            // base plate (material 2) never moves.
            float popZ = 0.0f;
            if (!o.noBot) {
                const float dt = float(lastHit(lane,songTime));
                auto hump = [](float t) {
                    float u = t/0.06666667f;
                    if (u < 0.0f || u >= 2.0f) return 0.0f;
                    u = u < 1.0f ? u : 2.0f-u;
                    return u*u*(3.0f-2.0f*u);
                };
                if (dt >= 0.0f && dt < 0.13333334f)
                    popZ = 0.38f*hump(dt);
                else if (fretHeld[lane] && dt >= 0.0f)
                    popZ = 0.06f*hump(fmodf(dt,0.13333334f));
            }
            const Mat4 shell = popZ != 0.0f
                ? mat_mul(fret,engineTranslate(0.0f,0.0f,popZ))
                : fret;
            drawEngineMesh(yargFret_,camera,shell,5,colors[lane],receptorAlpha,
                           texYargFret_.id,texYargFretShine_.id,0,0.0f,0);
            drawEngineMesh(yargFret_,camera,shell,5,colors[lane],receptorAlpha,
                           texYargFret_.id,texYargFretShine_.id,0,0.0f,1,
                           fretHeld[lane] ? 1.0f : 0.0f);
            drawEngineMesh(yargFret_,camera,fret,5,colors[lane],receptorAlpha,
                           texYargFret_.id,texYargFretShine_.id,0,0.0f,2);
            yargMaskDraws.push_back({&yargFret_,fret});
        }
    }

    if (style != 1) {
        for (int i = int(chart.notes.size()) - 1; i >= 0; --i) {
            const Note& note = chart.notes[i];
            const double hitSec = chart.beatToSec(note.beat);
            if (!o.noBot && songTime >= hitSec) continue;
            const bool starPower = chart.phraseAt(PhraseType::StarPower,
                                                   note.tick) != nullptr;

            if (note.open) {
                const float in = modYOffset(hitSec,2);
                const float pos = scrollOffset(hitSec,2);
                if (pos >= -2.0f && pos <= 7.0f) {
                    // The alpha-mask pass owns the z fade; per-element alpha
                    // is the appearance mods only.
                    const float fade = fmaxf(GetAlpha(mods,in,songTime),
                                             GetGlow(mods,in,songTime));
                    if (fade > 0.0f) {
                        const float x = modX(2,in);
                        const float bump = GetYPosBump(mods,2,in,float(beat),bpm);
                        const bool openHopo = note.tap ||
                                              note.openType != NoteType::Strum;
                        const Mat4 model = mat_mul(
                            engineTranslate(x,0.032f+bump*laneStep/64.0f,
                                            -2.0f+pos+
                                            (openHopo ? 0.030f : 0.0315f)),
                            mat_mul(noteRotation(note,in),
                                    engineScale(0.07593973f,0.087584f,0.05f)));
                        drawEngineMesh(yargOpen_,camera,model,
                                       openHopo ? 12 : 4,
                                       openHopo
                                           ? (starPower ? yargOpenHopoSp
                                                        : yargOpenHopoColor)
                                           : (starPower ? white : colors[5]),
                                       alpha*fade,
                                       texYargOpenNote_.id,texYargOpenHopo_.id,
                                       0,0.0f,-1,starPower ? 1.0f : 0.0f);
                        yargMaskDraws.push_back({&yargOpen_,model});
                    }
                }
            }

            for (int lane = 0; lane < 5; ++lane) {
                if (!(note.frets & (1 << lane))) continue;
                const float in = modYOffset(hitSec,lane);
                const float pos = scrollOffset(hitSec,lane);
                if (pos < -2.0f || pos > 7.0f) continue;
                const float fade = fmaxf(GetAlpha(mods,in,songTime),
                                         GetGlow(mods,in,songTime));
                if (fade <= 0.0f) continue;
                const float x = modX(lane,in);
                const float bump = GetYPosBump(mods,lane,in,float(beat),bpm);
                const float noteZoom = GetZoom(mods,lane);
                const bool hopo = note.type == NoteType::Hopo;
                const bool tap = note.type == NoteType::Tap;
                const EngineGpu& mesh = tap ? yargTap_ : (hopo ? yargHopo_ : yargNormal_);
                const int kind = tap ? 3 : (hopo ? 2 : 1);
                const float sx = (tap || hopo ? 0.045f : 0.05f) * noteZoom;
                const float sy = (tap || hopo ? 0.0325f : 0.0375f) * noteZoom;
                const float sz = (tap || hopo ? 0.065f : 0.07f) * noteZoom;
                const float py = (tap || hopo ? 0.073f : 0.0775f) +
                                 bump*laneStep/64.0f;
                const float pz = -2.0f+pos+(tap||hopo ? 0.0465f : 0.054f);
                const Mat4 model = mat_mul(
                    engineTranslate(x,py,pz),
                    mat_mul(engineQuaternion(0,0.7071068f,0.7071068f,0),
                            mat_mul(noteRotation(note,in),
                                    engineScale(sx,sy,sz))));
                unsigned randomState = unsigned(note.tick)*747796405u ^
                                       unsigned(lane+1)*2891336453u;
                if (randomState == 0) randomState = 1;
                float noteRandom[3] = {
                    yargRandomSigned(randomState),
                    yargRandomSigned(randomState),
                    yargRandomSigned(randomState)
                };
                drawEngineMesh(mesh,camera,model,kind,
                               starPower ? white : colors[lane],alpha*fade,
                               texYargNote_.id,texYargNoteShine_.id,
                               texYargNoteShader_.id,0.0f,-1,
                               starPower ? 1.0f : 0.0f,
                               noteRandom,songTime);
                yargMaskDraws.push_back({&mesh,model});
            }
        }
    }

    if (style == 2) {
        // Shared plumbing for the YARG transparent quad passes (beatlines
        // 2975, solo/sweep 2979, sustains 3000): SCENE_VS + per-pass FS,
        // straight-alpha blend, depth tested but not written. The z fade is
        // the alpha mask's job, so uFadeRange stays (0,0) here.
        auto setupYargQuadProg = [&](GLuint prog) {
            glUseProgram(prog);
            glUniformMatrix4fv(glGetUniformLocation(prog,"uMVP"),1,
                               GL_FALSE,camera.m);
            glUniform3f(glGetUniformLocation(prog,"uOffset"),0,0,0);
            glUniform1f(glGetUniformLocation(prog,"uCurve"),0.5f);
            glUniform4f(glGetUniformLocation(prog,"uFadePlane"),
                        0.0f,0.0f,1.0f,4.86f);
            glUniform2f(glGetUniformLocation(prog,"uFadeRange"),0.0f,0.0f);
        };
        auto drawYargQuads = [&](const std::vector<ch::Vtx>& verts) {
            if (verts.empty()) return;
            glBindVertexArray(vao_);
            glBindBuffer(GL_ARRAY_BUFFER,vbo_);
            glBufferData(GL_ARRAY_BUFFER,
                         GLsizeiptr(verts.size()*sizeof(ch::Vtx)),
                         verts.data(),GL_STREAM_DRAW);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glDrawArrays(GL_TRIANGLES,0,GLsizei(verts.size()));
            glDepthMask(GL_TRUE);
            glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        };
        auto sustainPass = [&](const std::vector<ch::Vtx>& verts, bool open,
                               bool active) {
            if (verts.empty()) return;
            setupYargQuadProg(yargSustain_);
            glUniform1i(glGetUniformLocation(yargSustain_,"uTex"),0);
            glUniform1i(glGetUniformLocation(yargSustain_,"uTex2"),1);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,open ? texYargOpenSustain_.id
                                             : texYargSustain_.id);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D,texYargSustainSecondary_.id);
            glUniform1f(glGetUniformLocation(yargSustain_,"uOpen"),
                        open ? 1.0f : 0.0f);
            glUniform1f(glGetUniformLocation(yargSustain_,"uActive"),
                        active ? 1.0f : 0.0f);
            glUniform1f(glGetUniformLocation(yargSustain_,"uEmissionScale"),
                        active ? 3.0f : 1.0f);
            glUniform1f(glGetUniformLocation(yargSustain_,"uTime"),songTime);
            drawYargQuads(verts);
            glActiveTexture(GL_TEXTURE0);
        };

        if (!o.playfield && mods.hideboard == 0.0f) {
            // Beatlines (queue 2975) sit between the track and the solo
            // overlays (2979).
            setupYargQuadProg(yargBeatline_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,texYargBeatline_.id);
            glUniform1i(glGetUniformLocation(yargBeatline_,"uTex"),0);
            drawYargQuads(yargBeatLines);


            struct SoloDraw {
                double start = 0.0, end = 0.0;
                bool startCap = true, endCap = true;
            };
            std::vector<SoloDraw> solos;
            for (const Phrase& phrase : chart.phrases) {
                if (phrase.type != PhraseType::Solo) continue;
                solos.push_back({chart.tickToSec(phrase.tick),
                                 chart.tickToSec(phrase.tick+phrase.length),
                                 true,true});
            }
            for (size_t i = 0; i+1 < solos.size(); ++i) {
                SoloDraw& current = solos[i];
                SoloDraw& next = solos[i+1];
                if (current.end == next.start) {
                    current.endCap = false;
                    next.startCap = false;
                } else {
                    const float visualSpeed = noteSpeed*0.6f;
                    const int caps = int(current.endCap)+int(next.startCap);
                    const double minGap = visualSpeed > 0.0f
                                        ? 0.5*double(caps)/visualSpeed : 0.0;
                    if (next.start-current.end < minGap) {
                        const double middle = (current.end+next.start)*0.5;
                        current.end = middle;
                        next.start = middle;
                        current.endCap = false;
                        next.startCap = false;
                    }
                }
            }

            const float soloTrackColor[3] = {
                0.0f,0.29803917f*fieldTint_[1],0.47450978f*fieldTint_[2]
            };
            const float soloRailColor[3] = {
                0.0f,0.29803923f*fieldTint_[1],0.47450978f*fieldTint_[2]
            };
            const float soloEmission[3] = {0.0f,0.11372549f,0.4392157f};
            auto soloBounds = [&](const SoloDraw& solo) {
                return std::pair<float,float>{
                    -2.0f+scrollOffset(solo.start),
                    -2.0f+scrollOffset(solo.end)
                };
            };

            sceneSetup();
            for (const SoloDraw& solo : solos) {
                const auto [z0,z1] = soloBounds(solo);
                quadXZ(-0.9915f,z0,0.9915f,z1,0.019f,
                       0,0,1,1,soloTrackColor,0.5019608f);
            }
            drawYargEffect(texYargSoloTrack_.id,soloEmission,1.0f,-1.0f);

            sceneSetup();
            for (const SoloDraw& solo : solos) {
                const auto [z0,z1] = soloBounds(solo);
                quadXZ(-1.07082f,z0,-1.019262f,z1,0.0184f,
                       0,0,1,1,soloRailColor,1.0f);
                quadXZ(1.019262f,z0,1.07082f,z1,0.0184f,
                       0,0,1,1,soloRailColor,1.0f);
            }
            drawYargEffect(texYargSoloRail_.id,soloEmission,1.0f,-1.0f);

            sceneSetup();
            for (const SoloDraw& solo : solos) {
                const auto [z0,z1] = soloBounds(solo);
                if (solo.startCap)
                    quadXZ(-0.9915f,z0-0.5f,0.9915f,z0,0.019f,
                           0,1,1,0,soloTrackColor,0.5019608f);
                if (solo.endCap)
                    quadXZ(-0.9915f,z1,0.9915f,z1+0.5f,0.019f,
                           1,0,0,1,soloTrackColor,0.5019608f);
            }
            drawYargEffect(texYargSoloTransitionTrack_.id,soloEmission,
                           1.0f,-1.0f);

            sceneSetup();
            for (const SoloDraw& solo : solos) {
                const auto [z0,z1] = soloBounds(solo);
                if (solo.startCap)
                    quadXZ(-1.07082f,z0-0.5f,-1.019262f,z0,0.0184f,
                           0,1,1,0,soloRailColor,1.0f);
                if (solo.endCap)
                    quadXZ(-1.07082f,z1,-1.019262f,z1+0.5f,0.0184f,
                           1,0,0,1,soloRailColor,1.0f);
            }
            drawYargEffect(texYargSoloTransitionRailLeft_.id,soloEmission,
                           1.0f,-1.0f);

            sceneSetup();
            for (const SoloDraw& solo : solos) {
                const auto [z0,z1] = soloBounds(solo);
                if (solo.startCap)
                    quadXZ(1.019262f,z0-0.5f,1.07082f,z0,0.0184f,
                           0,1,1,0,soloRailColor,1.0f);
                if (solo.endCap)
                    quadXZ(1.019262f,z1,1.07082f,z1+0.5f,0.0184f,
                           1,0,0,1,soloRailColor,1.0f);
            }
            drawYargEffect(texYargSoloTransitionRailRight_.id,soloEmission,
                           1.0f,-1.0f);

            if (!o.noBot) {
                const float sweepTrackColor[3] = {
                    fieldTint_[0],0.70932275f*fieldTint_[1],0.0f
                };
                const float sweepTrackEmission[3] = {0.5f,0.3529412f,0.0f};
                const float sweepTrimColor[3] = {
                    fieldTint_[0],0.70980394f*fieldTint_[1],0.0f
                };
                const float sweepTrimEmission[3] = {1.0f,0.70980394f,0.0f};
                for (const Phrase& phrase : chart.phrases) {
                    if (phrase.type != PhraseType::StarPower) continue;
                    const Note* last = nullptr;
                    const int phraseEnd = phrase.tick+phrase.length;
                    for (const Note& note : chart.notes) {
                        const bool inside = phrase.length == 0
                                          ? note.tick == phrase.tick
                                          : note.tick >= phrase.tick &&
                                            note.tick < phraseEnd;
                        if (inside) last = &note;
                        if (note.tick >= phraseEnd && phrase.length > 0) break;
                    }
                    if (!last) continue;
                    const float elapsed = songTime-float(chart.tickToSec(last->tick));
                    if (elapsed < 0.0f || elapsed >= 1.0f) continue;

                    sceneSetup();
                    quadXZ(-0.9915f,-3.0f,0.9915f,3.0f,0.019f,
                           0,0,1,1,sweepTrackColor,0.40784314f);
                    drawYargEffect(texYargSoloTrack_.id,sweepTrackEmission,
                                   1.0f,elapsed);

                    sceneSetup();
                    quadXZ(-1.0321515f,-3.0f,-0.9726615f,3.0f,0.019f,
                           0,0,1,1,sweepTrimColor,1.0f);
                    quadXZ(0.9726615f,-3.0f,1.0321515f,3.0f,0.019f,
                           0,0,1,1,sweepTrimColor,1.0f);
                    drawYargEffect(texYargSpTrim_.id,sweepTrimEmission,
                                   1.0f,elapsed);
                }
            }
        }
        // Sustains (queue 3000). Open = SustainLine_Full; fretted = the
        // two-texture SustainWave; held ribbons animate and emit x3.
        sustainPass(yargOpenSustains,true,false);
        sustainPass(yargOpenSustainsActive,true,true);
        sustainPass(yargFrettedSustains,false,false);
        sustainPass(yargFrettedSustainsActive,false,true);

        // Fret hit effects (EffectGroup under the fret at local
        // (0,0.073,0.011)): the flipbook flash (2x10 grid, random row per
        // hit, both frames over its life via the eased frameOverTime curve),
        // the 200-dot expanding ring, and the point light uploaded above.
        // Smoke, sparkles and shards are not ported.
        if (!o.noBot) {
            std::vector<ch::Vtx> flashQuads;
            std::vector<ch::Vtx> ringQuads;
            std::vector<ch::Vtx> sparkQuads;
            const float camUp[3] = {0.0f,0.9126300f,0.4087821f};
            auto addBillboard = [&](std::vector<ch::Vtx>& out,float cx,
                                    float cy,float cz,float hx,float hy,
                                    float u0,float v0,float u1,float v1,
                                    const float* color,float a) {
                ch::Vtx q[4] = {
                    {cx-hx,cy-camUp[1]*hy,cz-camUp[2]*hy,u0,v0,
                     color[0],color[1],color[2],a},
                    {cx+hx,cy-camUp[1]*hy,cz-camUp[2]*hy,u1,v0,
                     color[0],color[1],color[2],a},
                    {cx+hx,cy+camUp[1]*hy,cz+camUp[2]*hy,u1,v1,
                     color[0],color[1],color[2],a},
                    {cx-hx,cy+camUp[1]*hy,cz+camUp[2]*hy,u0,v1,
                     color[0],color[1],color[2],a}
                };
                const int ix[6] = {0,1,2,0,2,3};
                for (int i : ix) out.push_back(q[i]);
            };
            for (const Note& note : chart.notes) {
                const double hitSec = chart.beatToSec(note.beat);
                if (hitSec > songTime) break;
                const float dt = float(songTime-hitSec);
                if (dt >= 0.5f) continue;
                int lanes[6], laneCount = 0;
                if (note.open) lanes[laneCount++] = 5;
                for (int lane = 0; lane < 5; ++lane)
                    if (note.frets & (1 << lane)) lanes[laneCount++] = lane;
                for (int li = 0; li < laneCount; ++li) {
                    const int lane = lanes[li];
                    // Open notes query the mods at column 2 (the ArrowEffects
                    // port has five columns) and tint from colors[5].
                    const int modCol = lane == 5 ? 2 : lane;
                    const float in = modYOffset(songTime,modCol);
                    const float x = modX(modCol,in);
                    const float bump = GetYPosBump(mods,modCol,in,float(beat),
                                                   bpm)*laneStep/64.0f;
                    const float* color = lane == 5 ? colors[5]
                                                   : colors[lane];
                    unsigned seed = unsigned(note.tick)*747796405u ^
                                    unsigned(lane+1)*2891336453u;
                    if (seed == 0) seed = 1;
                    // Flash: child at (0.001,0.249,-0.077) under Hit
                    // Effects; startSize 0.7x0.6, lifetime random 0.3-0.4 s
                    // at simulationSpeed 2, colorOverLifetime alpha
                    // 0.706 -> 0 linear, SrcAlpha-additive.
                    const float life = (0.3f+0.1f*(yargRandomSigned(seed)*
                                                   0.5f+0.5f))*0.5f;
                    // Row is drawn from the stream unconditionally so the
                    // ring's dot angles below stay fixed after the flash
                    // window closes.
                    const int row = int((yargRandomSigned(seed)*0.5f+
                                         0.5f)*9.999f);
                    const float fdt = dt-0.01f;
                    if (fdt >= 0.0f && fdt < life) {
                        const float progress = fdt/life;
                        const int frame = progress > 0.5f ? 1 : 0;
                        const float u0 = frame*0.5f, u1 = u0+0.5f;
                        const float v1 = 1.0f-row*0.1f, v0 = v1-0.1f;
                        addBillboard(flashQuads,x,0.322f+bump,-2.066f,
                                     0.35f,0.30f,u0,v0,u1,v1,color,
                                     0.706f*(1.0f-progress));
                    }
                    // Ring: 200-particle burst, cone angle 90 / radius ~0 =
                    // a flat radial spray in the emitter's local XY plane
                    // (vertical, facing down-track). Ring child at
                    // (0,-0.031,0) with scale (0.2418,0.2418,0.1313);
                    // startSpeed 4 local -> 0.967 u/s world, startLifetime
                    // 0.5 s at simulationSpeed 1, startSize 0.05 local
                    // (~0.012 world) scaled by the eased sizeOverLifetime
                    // 0 -> 1 curve, alpha 0.294 x (1-t) colorOverLifetime.
                    if (dt < 0.5f) {
                        const float t = dt/0.5f;
                        const float radius = 0.967f*dt;
                        const float grow = 1.0f-(1.0f-t)*(1.0f-t);
                        const float dotHalf = 0.006f*grow;
                        const float dotAlpha = 0.294f*(1.0f-t);
                        for (int k = 0; k < 200; ++k) {
                            unsigned dotSeed = seed ^ (unsigned(k)*
                                                       0x9E3779B9u);
                            const float ang = 6.2831853f*
                                (yargRandomSigned(dotSeed)*0.5f+0.5f);
                            addBillboard(ringQuads,
                                         x+cosf(ang)*radius,
                                         0.042f+bump+sinf(ang)*radius,
                                         -1.989f,
                                         dotHalf,dotHalf,
                                         0.0f,0.0f,1.0f,1.0f,
                                         color,dotAlpha);
                        }
                    }
                    // Sparkles: rateOverTime 228.21 over duration 0.3 at
                    // simulationSpeed 2 -> ~68 particles across a real
                    // 0.15 s window, each living 0.15 s real. Emitter under
                    // Hit Effects at (0,-0.073,0.022) (track level, z fret
                    // +0.033), tilted -56.6 deg about X, so local +z rises
                    // at (0,0.834,0.551) and local +y at (0,0.551,-0.834);
                    // velocityOverLifetime z rand 0..1.5, y rand 0..0.1
                    // (x2 for the sim speed). White (allowColoring 0),
                    // straight alpha (ParticleTransparent), startSize rand
                    // 0..0.025, size holds then shrinks to 0 over the last
                    // half, alpha fades to 0.0176. Spawn spread = shape
                    // radius 0.3 x transform scale.x 0.437.
                    if (dt < 0.3f) {
                        for (int k = 0; k < 68; ++k) {
                            unsigned ss = seed ^ (unsigned(k+1)*
                                                  0x85EBCA6Bu);
                            const float birth = (yargRandomSigned(ss)*0.5f+
                                                 0.5f)*0.15f;
                            const float age = dt-birth;
                            if (age < 0.0f || age >= 0.15f) continue;
                            const float t = age/0.15f;
                            const float vz = (yargRandomSigned(ss)*0.5f+
                                              0.5f)*3.0f;
                            const float vy = (yargRandomSigned(ss)*0.5f+
                                              0.5f)*0.2f;
                            const float sx = x+yargRandomSigned(ss)*0.131f;
                            const float half = (yargRandomSigned(ss)*0.5f+
                                                0.5f)*0.0125f;
                            const float shrink = t < 0.5f
                                ? 1.0f : 1.0f-(t-0.5f)*2.0f;
                            const float sparkAlpha = 1.0f-0.98235f*t;
                            addBillboard(sparkQuads,
                                         sx,
                                         bump+(vz*0.834f+vy*0.551f)*age,
                                         -1.967f+(vz*0.551f-vy*0.834f)*age,
                                         half*shrink,half*shrink,
                                         0.0f,0.0f,1.0f,1.0f,
                                         white,sparkAlpha);
                        }
                    }
                }
            }
            sceneSetup();
            // Unity particles ZTest LEqual without writing: the ring's
            // below-track half and anything behind frets/notes culls
            // against the scene depth, which is most of why the ring is
            // near-imperceptible in real YARG.
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            v_ = std::move(ringQuads);
            drawLayer(texYargFretHitRing_.id,ch::BLEND_ADD);
            v_ = std::move(sparkQuads);
            drawLayer(texYargFretHitRing_.id,ch::BLEND_SPRITE);
            v_ = std::move(flashQuads);
            drawLayer(texYargFretHitFlash_.id,ch::BLEND_ADD);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
        }
    }

    if (style == 1) {
        for (const MoonGemDraw& gem : moonGems)
            drawMoonTransparent(*gem.mesh,gem.model,gem.kind,gem.color,
                                gem.alpha,gem.open);
    }

    if (style == 1 && !o.noBot) {
        sceneSetup();
        for (int lane = 0; lane < 5; ++lane) {
            if (receptorAlpha <= 0.0f) continue;
            const float dt = moonIndicatorDt[lane];
            if (dt < 0.0f || dt >= 0.2f / 2.2f) continue;
            const float in = modYOffset(songTime,lane);
            const float pos = scrollOffset(songTime,lane);
            const float bump = GetYPosBump(mods,lane,in,float(beat),bpm)/64.0f;
            moonFretQuad(modX(lane,in),pos,101.0f/120.0f,101.0f/120.0f,
                         bump,colors[lane],receptorAlpha);
        }
        moonGlowOccluders.insert(moonGlowOccluders.end(),v_.begin(),v_.end());
        drawLayer(texMoonStrike_.id, ch::BLEND_SPRITE);

        sceneSetup();
        for (int lane = 0; lane < 5; ++lane) {
            if (receptorAlpha <= 0.0f) continue;
            const float dt = moonIndicatorDt[lane];
            if (dt < 0.0f || dt >= 0.2f / 2.2f) continue;
            const float in = modYOffset(songTime,lane);
            const float pos = scrollOffset(songTime,lane);
            const float bump = GetYPosBump(mods,lane,in,float(beat),bpm)/64.0f;
            moonFretQuad(modX(lane,in),pos,100.0f/120.0f,100.0f/120.0f,
                         bump-0.2f+dt*2.2f,colors[lane],receptorAlpha);
        }
        moonGlowOccluders.insert(moonGlowOccluders.end(),v_.begin(),v_.end());
        drawLayer(texMoonIndicator_.id, ch::BLEND_SPRITE);
    }

    // The source camera's selective glow renders directly at half its pixel
    // rect. Opaque/MKGlow replacement passes write depth; Transparent passes
    // are black maskers with ZWrite Off.
    if (style == 1) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER,moonSceneMsaaFbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,moonSceneFbo_);
        glBlitFramebuffer(0,0,moonSceneW_,moonSceneH_,
                          0,0,moonSceneW_,moonSceneH_,
                          GL_COLOR_BUFFER_BIT,GL_NEAREST);
        if (!o.noPost) {
        const bool embedded = mvpOverride != nullptr;
        // The scene FBO is the field's own rect now, so the active-3D-rect
        // band is relative to it -- no vpX offset.
        const int glowDstX = embedded ? 0 : int(0.122f*float(vpW));
        const int glowDstY = 0;
        const int glowDstW = std::max(1,embedded ? engineTargetViewport[2]
                                                : int(0.78f*float(vpW)));
        const int glowDstH = std::max(1,embedded ? engineTargetViewport[3]
                                                : int(0.98f*float(
                                                      engineTargetViewport[3])));
        const int blurW = std::max(1,glowDstW/2);
        const int blurH = std::max(1,glowDstH/2);
        if (blurW != moonGlowW_ || blurH != moonGlowH_) {
            for (GLuint texture : {moonGlowTex_,moonBlurTex_[0],
                                   moonBlurTex_[1]}) {
                glBindTexture(GL_TEXTURE_2D,texture);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,blurW,blurH,0,
                             GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            }
            glBindRenderbuffer(GL_RENDERBUFFER,moonGlowDepth_);
            glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,
                                  blurW,blurH);
            moonGlowW_ = blurW;
            moonGlowH_ = blurH;
        }

        glBindFramebuffer(GL_FRAMEBUFFER,moonGlowFbo_);
        glViewport(0,0,blurW,blurH);
        glClearColor(0,0,0,0);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        if (!moonGlowDepthBlockers.empty()) {
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glUseProgram(moonOccluder_);
            glUniformMatrix4fv(glGetUniformLocation(moonOccluder_,"uMVP"),1,
                               GL_FALSE,glowCamera.m);
            glUniform3f(glGetUniformLocation(moonOccluder_,"uOffset"),0,0,0);
            glUniform1f(glGetUniformLocation(moonOccluder_,"uCurve"),0.0f);
            glUniform4f(glGetUniformLocation(moonOccluder_,"uFadePlane"),0,0,0,0);
            glUniform1f(glGetUniformLocation(moonOccluder_,"uAlpha"),0.0f);
            glBindVertexArray(vao_);
            glBindBuffer(GL_ARRAY_BUFFER,vbo_);
            glBufferData(GL_ARRAY_BUFFER,
                         GLsizeiptr(moonGlowDepthBlockers.size()*sizeof(ch::Vtx)),
                         moonGlowDepthBlockers.data(),GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES,0,
                         GLsizei(moonGlowDepthBlockers.size()));
        }
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(engineGlow_);
        glUniformMatrix4fv(glGetUniformLocation(engineGlow_,"uMVP"),1,GL_FALSE,
                           glowCamera.m);
        glUniform1f(glGetUniformLocation(engineGlow_,"uCurve"),0.0f);
        glUniform4f(glGetUniformLocation(engineGlow_,"uFadePlane"),0,0,0,0);
        auto materialPass = [](const MoonGemDraw& gem,int material) {
            if (gem.kind >= 14 && gem.kind <= 17) {
                if (gem.kind == 15 && material == 0) return 1;
                if ((gem.kind == 16 && material >= 3) ||
                    (gem.kind != 16 && material == 4))
                    return 2;
                return 0;
            }
            if ((gem.kind == 18 || gem.kind == 19) && material == 2)
                return 1;
            if (!gem.open &&
                (material == 0 || (material == 1 && gem.kind == 10)))
                return 2; // Transparent: black alpha one, no depth writes.
            if (material == 2 && (gem.kind == 9 || gem.kind == 13))
                return 1; // MKGlow: straight alpha and depth writes.
            return 0;     // Opaque: transparent black and depth writes.
        };
        auto drawMaskMaterials = [&](int pass) {
            glDepthMask(pass == 2 ? GL_FALSE : GL_TRUE);
            if (pass == 0) {
                glDisable(GL_BLEND);
            } else {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            }
            for (const MoonGemDraw& gem : moonGems) {
                const int materials = gem.kind >= 14 && gem.kind <= 17
                                    ? 5 : gem.open ? 4 : 3;
                glUniformMatrix4fv(glGetUniformLocation(engineGlow_,"uModel"),1,
                                   GL_FALSE,gem.model.m);
                for (int material = 0; material < materials; ++material) {
                    if (materialPass(gem,material) != pass) continue;
                    float power = 0.0f;
                    float maskAlpha = pass == 0 ? 0.0f : 1.0f;
                    float glowR = 1.0f, glowG = 1.0f, glowB = 1.0f;
                    if (pass == 1) {
                        if (gem.kind == 18 || gem.kind == 19) {
                            power = gem.kind == 18 ? 1.3f : 3.04f;
                            maskAlpha = 0.5f * gem.alpha * fieldTint_[3];
                            glowR = 0.066176474f;
                            glowG = 0.5749494f;
                        } else {
                            power = gem.open ? 0.91f : 0.8f;
                            maskAlpha = (gem.open ? 0.5f : 1.0f) * gem.alpha *
                                        fieldTint_[3];
                        }
                    }
                    glUniform1i(glGetUniformLocation(engineGlow_,
                                                     "uMaterialFilter"),material);
                    glUniform1f(glGetUniformLocation(engineGlow_,"uGlowPower"),
                                power);
                    glUniform1f(glGetUniformLocation(engineGlow_,"uGlowAlpha"),
                                maskAlpha);
                    glUniform3f(glGetUniformLocation(engineGlow_,"uGlowColor"),
                                glowR,glowG,glowB);
                    glBindVertexArray(gem.mesh->vao);
                    glDrawArrays(GL_TRIANGLES,0,gem.mesh->count);
                }
            }
        };
        drawMaskMaterials(0);
        drawMaskMaterials(1);
        drawMaskMaterials(2);

        if (!moonGlowOccluders.empty()) {
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_CULL_FACE);
            glUseProgram(moonOccluder_);
            glUniformMatrix4fv(glGetUniformLocation(moonOccluder_,"uMVP"),1,
                               GL_FALSE,glowCamera.m);
            glUniform3f(glGetUniformLocation(moonOccluder_,"uOffset"),0,0,0);
            glUniform1f(glGetUniformLocation(moonOccluder_,"uAlpha"),1.0f);
            glBindVertexArray(vao_);
            glBindBuffer(GL_ARRAY_BUFFER,vbo_);
            glBufferData(GL_ARRAY_BUFFER,
                         GLsizeiptr(moonGlowOccluders.size()*sizeof(ch::Vtx)),
                         moonGlowOccluders.data(),GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES,0,GLsizei(moonGlowOccluders.size()));
        }

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glUseProgram(moonBlur_);
        glUniform1i(glGetUniformLocation(moonBlur_,"uTex"),0);
        glUniform1f(glGetUniformLocation(moonBlur_,"uAlpha"),1.0f);
        glBindVertexArray(qvao_);
        glUniform2f(glGetUniformLocation(moonBlur_,"uTexel"),
                    1.0f/float(blurW),1.0f/float(blurH));
        auto blurPass = [&](int dst, GLuint src, float shift) {
            glBindFramebuffer(GL_FRAMEBUFFER,moonBlurFbo_[dst]);
            glViewport(0,0,blurW,blurH);
            glBindTexture(GL_TEXTURE_2D,src);
            glUniform1f(glGetUniformLocation(moonBlur_,"uShift"),shift);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE,GL_ZERO);
            glUniform2f(glGetUniformLocation(moonBlur_,"uDirection"),1,0);
            glDrawArrays(GL_TRIANGLES,0,3);
            glBlendFunc(GL_ONE,GL_ONE);
            glUniform2f(glGetUniformLocation(moonBlur_,"uDirection"),0,1);
            glDrawArrays(GL_TRIANGLES,0,3);
        };
        static const float shifts[] = {
            0.35f,0.0f,0.21805f,0.6139f,1.176f,1.8158f
        };
        GLuint blurSource = moonGlowTex_;
        int src = -1;
        for (int i = 0; i < int(sizeof(shifts)/sizeof(shifts[0])); ++i) {
            const int dst = i&1;
            blurPass(dst,blurSource,shifts[i]);
            blurSource = moonBlurTex_[dst];
            src = dst;
        }

        glBindFramebuffer(GL_FRAMEBUFFER,moonSceneFbo_);
        glViewport(glowDstX,glowDstY,glowDstW,glowDstH);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE,GL_ONE);
        glBindTexture(GL_TEXTURE_2D,moonBlurTex_[src]);
        glUniform1f(glGetUniformLocation(moonBlur_,"uShift"),-1.0f);
        glDrawArrays(GL_TRIANGLES,0,3);
        glViewport(0,0,moonSceneW_,moonSceneH_);
        }
    }

    if (moonIsolated) {
        glBindFramebuffer(GL_FRAMEBUFFER,GLuint(engineTargetFbo));
        // Composite into the field's rect only: the scene FBO is rect-sized
        // and its opaque black belongs to this field's window, not the
        // other player's half.
        glViewport(engineTargetViewport[0]+(mvpOverride ? 0 : vpX),
                   engineTargetViewport[1],
                   mvpOverride ? engineTargetViewport[2] : vpW,
                   engineTargetViewport[3]);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(moonBlur_);
        glUniform1i(glGetUniformLocation(moonBlur_,"uTex"),0);
        glUniform1f(glGetUniformLocation(moonBlur_,"uShift"),-1.0f);
        glUniform1f(glGetUniformLocation(moonBlur_,"uAlpha"),compositeAlpha);
        glBindVertexArray(qvao_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,moonSceneTex_);
        glDrawArrays(GL_TRIANGLES,0,3);
    }

    if (yargLinear) {
        // HighwaysAlphaMask: replay the recorded geometry with the fade
        // shader into the R8 mask target (BlendOp Max, like YARG's pass).
        glBindFramebuffer(GL_FRAMEBUFFER,yargMaskFbo_);
        glViewport(0,0,yargW_,yargH_);
        glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_MAX);
        glBlendFunc(GL_ONE,GL_ONE);
        glUseProgram(yargMaskMesh_);
        glUniformMatrix4fv(glGetUniformLocation(yargMaskMesh_,"uMVP"),1,
                           GL_FALSE,camera.m);
        glUniform3f(glGetUniformLocation(yargMaskMesh_,"uOffset"),0,0,0);
        glUniform1f(glGetUniformLocation(yargMaskMesh_,"uCurve"),0.5f);
        glUniform4f(glGetUniformLocation(yargMaskMesh_,"uFadePlane"),
                    0.0f,0.0f,1.0f,4.86f);
        {
            const float pitch = 24.12f * 3.14159265f / 180.0f;
            const float fy = -sinf(pitch), fz = cosf(pitch);
            glUniform2f(glGetUniformLocation(yargMaskMesh_,"uFadeRange"),
                        fabsf(2.66f*fy + (-4.86f-1.75f)*fz),
                        fabsf(2.66f*fy + (-4.86f-3.0f)*fz));
        }
        for (const MaskDraw& draw : yargMaskDraws) {
            glUniformMatrix4fv(glGetUniformLocation(yargMaskMesh_,"uModel"),1,
                               GL_FALSE,draw.model.m);
            const Mat4& model = draw.model;
            const float det =
                model.m[0]*(model.m[5]*model.m[10]-model.m[6]*model.m[9])-
                model.m[4]*(model.m[1]*model.m[10]-model.m[2]*model.m[9])+
                model.m[8]*(model.m[1]*model.m[6]-model.m[2]*model.m[5]);
            glEnable(GL_CULL_FACE);
            glCullFace(det < 0.0f ? GL_BACK : GL_FRONT);
            glBindVertexArray(draw.gpu->vao);
            glDrawArrays(GL_TRIANGLES,0,draw.gpu->count);
        }
        glDisable(GL_CULL_FACE);
        glBlendEquation(GL_FUNC_ADD);

        // Resolve the 4x MSAA scene into yargTex_ for the bloom chain.
        glBindFramebuffer(GL_READ_FRAMEBUFFER,yargMsaaFbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,yargFbo_);
        glBlitFramebuffer(0,0,yargW_,yargH_,0,0,yargW_,yargH_,
                          GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER,yargFbo_);

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glBindVertexArray(qvao_);
        glActiveTexture(GL_TEXTURE0);

        if (!o.noPost) {
        const int bloomW0 = std::max(1,yargW_ >> 1);
        const int bloomH0 = std::max(1,yargH_ >> 1);
        glBindFramebuffer(GL_FRAMEBUFFER,yargBloomDownFbo_[0]);
        glViewport(0,0,bloomW0,bloomH0);
        glUseProgram(yargBloomPrefilter_);
        glUniform1i(glGetUniformLocation(yargBloomPrefilter_,"uTex"),0);
        glBindTexture(GL_TEXTURE_2D,yargTex_);
        glDrawArrays(GL_TRIANGLES,0,3);

        for (int i = 1; i < yargBloomMips_; ++i) {
            const int sourceW = std::max(1,yargW_ >> i);
            const int sourceH = std::max(1,yargH_ >> i);
            const int bloomW = std::max(1,yargW_ >> (i+1));
            const int bloomH = std::max(1,yargH_ >> (i+1));

            glBindFramebuffer(GL_FRAMEBUFFER,yargBloomUpFbo_[i]);
            glViewport(0,0,bloomW,bloomH);
            glUseProgram(yargBloomDownH_);
            glUniform1i(glGetUniformLocation(yargBloomDownH_,"uTex"),0);
            glUniform2f(glGetUniformLocation(yargBloomDownH_,"uTexel"),
                        1.0f/float(sourceW),1.0f/float(sourceH));
            glBindTexture(GL_TEXTURE_2D,yargBloomDownTex_[i-1]);
            glDrawArrays(GL_TRIANGLES,0,3);

            glBindFramebuffer(GL_FRAMEBUFFER,yargBloomDownFbo_[i]);
            glUseProgram(yargBloomDownV_);
            glUniform1i(glGetUniformLocation(yargBloomDownV_,"uTex"),0);
            glUniform2f(glGetUniformLocation(yargBloomDownV_,"uTexel"),
                        1.0f/float(bloomW),1.0f/float(bloomH));
            glBindTexture(GL_TEXTURE_2D,yargBloomUpTex_[i]);
            glDrawArrays(GL_TRIANGLES,0,3);
        }

        for (int i = yargBloomMips_-2; i >= 0; --i) {
            const int bloomW = std::max(1,yargW_ >> (i+1));
            const int bloomH = std::max(1,yargH_ >> (i+1));
            glBindFramebuffer(GL_FRAMEBUFFER,yargBloomUpFbo_[i]);
            glViewport(0,0,bloomW,bloomH);
            glUseProgram(yargBloomUp_);
            glUniform1i(glGetUniformLocation(yargBloomUp_,"uHigh"),0);
            glUniform1i(glGetUniformLocation(yargBloomUp_,"uLow"),1);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,yargBloomDownTex_[i]);
            glActiveTexture(GL_TEXTURE0+1);
            const GLuint low = i == yargBloomMips_-2
                             ? yargBloomDownTex_[i+1]
                             : yargBloomUpTex_[i+1];
            glBindTexture(GL_TEXTURE_2D,low);
            glDrawArrays(GL_TRIANGLES,0,3);
        }
        }

        glBindFramebuffer(GL_FRAMEBUFFER,GLuint(engineTargetFbo));
        glViewport(engineTargetViewport[0]+(mvpOverride ? 0 : vpX),
                   engineTargetViewport[1],
                   mvpOverride ? engineTargetViewport[2] : vpW,
                   engineTargetViewport[3]);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(linearCompose_);
        glUniform1i(glGetUniformLocation(linearCompose_,"uTex"),0);
        glUniform1i(glGetUniformLocation(linearCompose_,"uBloom"),1);
        glUniform1i(glGetUniformLocation(linearCompose_,"uMask"),2);
        glUniform1i(glGetUniformLocation(linearCompose_,"uGrain"),3);
        glUniform1f(glGetUniformLocation(linearCompose_,"uAlpha"),compositeAlpha);
        glUniform1f(glGetUniformLocation(linearCompose_,"uPost"),
                    o.noPost ? 0.0f : 1.0f);
        // URP randomises the grain offset per frame; a renderer needs it
        // deterministic per beat, so hash a 1/240-beat counter instead.
        const unsigned grainTick = unsigned(floor(beat*240.0));
        glUniform2f(glGetUniformLocation(linearCompose_,"uGrainScale"),
                    float(yargW_)/512.0f,float(yargH_)/512.0f);
        glUniform2f(glGetUniformLocation(linearCompose_,"uGrainOffset"),
                    grainTick*0.61803399f-floorf(grainTick*0.61803399f),
                    grainTick*0.75487767f-floorf(grainTick*0.75487767f));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,yargTex_);
        glActiveTexture(GL_TEXTURE0+1);
        glBindTexture(GL_TEXTURE_2D,yargBloomMips_ == 1
                                   ? yargBloomDownTex_[0]
                                   : yargBloomUpTex_[0]);
        glActiveTexture(GL_TEXTURE0+2);
        glBindTexture(GL_TEXTURE_2D,yargMaskTex_);
        glActiveTexture(GL_TEXTURE0+3);
        glBindTexture(GL_TEXTURE_2D,texYargGrain_.id);
        glActiveTexture(GL_TEXTURE0);
        glDrawArrays(GL_TRIANGLES,0,3);
    }

    fieldTint_[3] = fieldAlpha;

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_);
    glUniform1f(glGetUniformLocation(prog_, "uCurve"),0.0f);
    glUniform2f(glGetUniformLocation(prog_, "uFadeRange"),0.0f,0.0f);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER,vbo_);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::resize(int w, int h) {
    if (w == W && h == H) return;
    W = w; H = h;
    destroyFbos();
    makeFbos();
    buildCamera();
}

void Renderer::buildHitTimes(const Chart& chart) {
    for (int i = 0; i < 5; ++i) hitTimes_[i].clear();
    hitTimesOpen_.clear();
    for (const auto& n : chart.notes) {
        double t = chart.beatToSec(n.beat);
        for (int lane = 0; lane < 5; ++lane)
            if (n.frets & (1 << lane)) hitTimes_[lane].push_back(t);
        // An open note pops EVERY fret -- NeckController's open branch runs
        // `for (i=0..4) FretAnimators[i].Play(false, true)` -- but it is not a
        // hit on any lane, so it is kept apart from hitTimes_. That
        // separation is the point: the head LIGHT follows the button state
        // (Fret_Animator: `if (isHeld) headLight.enabled = true`), and an
        // open note is played holding no frets at all. So opens raise the
        // buttons and light nothing, on both fields.
        if (n.open) hitTimesOpen_.push_back(t);
    }
}

double Renderer::lastOpenHit(double now) const {
    const auto& v = hitTimesOpen_;
    size_t lo = 0, hi = v.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (v[m] <= now) lo = m + 1; else hi = m; }
    const double best = (lo > 0) ? v[lo - 1] : -1e9;
    return now - best;
}

double Renderer::lastHit(int lane, double now) const {
    const auto& v = hitTimes_[lane];
    size_t lo = 0, hi = v.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (v[m] <= now) lo = m + 1; else hi = m; }
    double best = (lo > 0) ? v[lo - 1] : -1e9;
    return now - best;
}

void Renderer::applyFieldTint() {
    if (fieldTint_[0] == 1.0f && fieldTint_[1] == 1.0f &&
        fieldTint_[2] == 1.0f && fieldTint_[3] == 1.0f) return;
    for (ch::Vtx& vertex : v_) {
        vertex.r *= fieldTint_[0];
        vertex.g *= fieldTint_[1];
        vertex.b *= fieldTint_[2];
        vertex.a *= fieldTint_[3];
    }
}

// A bucket of pre-tinted silhouette quads through the note-glow program:
// the texture's alpha with the vertex colour's RGB, premultiplied. Used for
// the hold-body glow pass, whose alpha varies per ROW (GetGlow is a function
// of yOffset), so it cannot go through drawLayer's uniform-alpha repaint.
void Renderer::drawGlowLayer(GLuint tex) {
    if (v_.empty()) return;
    applyFieldTint();
    glUseProgram(glow_);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                 v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
    v_.clear();
    glUseProgram(prog_);
}

// One batch through the PIU program. Mirrors drawLayer, but the PIU path never
// wants the neck's straight-alpha blend, so there is no uPremul to set.
void Renderer::drawPiuLayer(GLuint tex, int blend) {
    if (v_.empty()) return;
    applyFieldTint();
    glUseProgram(piu_);
    glBlendFunc(blend == ch::BLEND_ADD ? GL_ONE : GL_ONE,
                blend == ch::BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(piu_, "uTex"), 0);
    glUniformMatrix4fv(glGetUniformLocation(piu_, "uMVP"), 1, GL_FALSE,
                       piuMvp_.m);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                 v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
    v_.clear();
    glUseProgram(prog_);
}

// Convert the absolute 480-high coordinates produced below into a Player's
// local coordinates, then apply the PlayerOptions whole-field transform. The
// outer caller supplies the Player's screen placement and actor transform, so
// wag rotates around that actor's origin instead of the receptor row.
static Mat4 piuFieldLocal(const Mods& mods, double beat, float centerX) {
    const float tilt = -30.0f * mods.tilt * 3.14159265f / 180.0f;
    const float wag = mods.wag * 21.0f * sinf(float(beat) * 3.14159265f) *
                      3.14159265f / 180.0f;
    float zoom = 1.0f - 0.5f * mods.mini;
    if (mods.tilt > 0.0f)      zoom *= 1.0f - 0.1f * mods.tilt;
    else if (mods.tilt < 0.0f) zoom *= 1.0f + 0.1f * mods.tilt;

    const float cx = cosf(tilt), sx = sinf(tilt);
    const float cz = cosf(wag),  sz = sinf(wag);
    Mat4 effect{};                                  // Rz * Rx, y-down space
    effect.m[0]  = zoom *  cz;
    effect.m[1]  = zoom *  sz * cx;
    effect.m[2]  = zoom * -sz * sx;
    effect.m[4]  = zoom * -sz;
    effect.m[5]  = zoom *  cz * cx;
    effect.m[6]  = zoom * -cz * sx;
    effect.m[9]  = zoom *  sx;
    effect.m[10] = zoom *  cx;
    effect.m[15] = 1.0f;

    Mat4 local{};
    local.m[0] = local.m[5] = local.m[10] = local.m[15] = 1.0f;
    local.m[12] = -centerX;
    local.m[13] = -240.0f;
    return mat_mul(effect, local);
}

// The GH3 field's camera mods, in real 3D. The 2D layout embeds LOSSLESSLY
// into a perspective view: every vertex's depth follows from its screen y
// alone (sprite scale is linear in the geometric fraction g and projective
// scale is 1/depth, so depth = 0.8/s(g), normalised to 1 at the strike
// line). Each vertex is unprojected into that view, rotated/zoomed about
// the strike line's centre, and reprojected. FOV 68 is GH3's own choice --
// the highway_prepass companion camera bakes tan 34 deg into its trapezoid
// params. With the mods neutral the map is SKIPPED, so the 2D render stays
// bit-identical: the embed is exact by construction, never an
// approximation of the 2D look.
void Renderer::gh3CamVtx(ch::Vtx& t) const {
    const float T = 0.67451f;             // tan(34 deg) -- FOV 68
    const float A = 1280.0f / 720.0f;
    float g = (t.y - 305.0f) / 350.0f;
    // s(g) hits zero at the lanes' own vanishing point (y ~ 146); clamp a
    // hair below so sidebar/string tips keep a finite depth.
    if (g < -0.40f) g = -0.40f;
    const float w = 0.8f / (0.25f + 0.55f * g);
    const float p[3] = { (t.x - 640.0f) / 640.0f * T * A * w,
                         (360.0f - t.y) / 360.0f * T * w,
                         -w };
    float q[3];
    for (int i = 0; i < 3; ++i)
        q[i] = gh3Cam_[i*3+0]*p[0] + gh3Cam_[i*3+1]*p[1] + gh3Cam_[i*3+2]*p[2]
             + gh3Cam_[9+i];
    const float d = q[2] < -0.05f ? -q[2] : 0.05f;
    t.x = 640.0f + q[0] / d / (T * A) * 640.0f;
    t.y = 360.0f - q[1] / d / T * 360.0f;
}

// The PIU playfield.
//
// NOT the CH highway wearing pump sprites: pump has no neck, no sidebars, no
// lane strings and no vanishing point. It is a FLAT field with a row of five
// square panels and notes rising into them, so this is a wholly separate draw
// path and the CH board is skipped entirely (see hidePlayfield in drawFrame).
//
// Everything here is computed in StepMania's own 640x480 virtual screen, y down
// from the top-left -- the space ArrowEffects was written for. GetXPos already
// returns pixels, yOffset already IS pixels, and ApplyScrollPos is GetYPos's
// reverse/centered handling verbatim. Nothing is converted to world units and
// back, so the mods land the way ITG intends rather than being translated twice.
void Renderer::drawPiu(const Chart& chart, double beat, const RenderOpts& o,
                       const Mods& mods, float songTime, float scrollNow,
                       float noteSpeed, float bpm, const Mat4& mvp) {
    piuMvp_ = mvp;
    // The receptor row: SCREEN_CENTER_Y + GRAY_ARROWS_Y_STANDARD, which
    // OpenITG's theme puts at -125 (metrics.ini:2942) -- above centre, with
    // notes rising into it. UPSCROLL, because StepMania spends the `reverse`
    // mod ON downscroll; if the baseline were already downscroll, reverse would
    // have nothing left to mean.
    // StepMania's widescreen convention: the virtual screen is always 480 tall
    // and as wide as the output aspect calls for. Hard-coding 640 would squash
    // every panel horizontally on a 16:9 render.
    const float VIRT_H = 480.0f;
    const float VIRT_W = logicalScreenWidth(W, H);
    const float CENTER_X = VIRT_W * 0.5f;
    // Pump's own column spacing, PUMP_COL_SPACING = 50 (GameManager.cpp:53),
    // NOT dance's 64. Every pixel the mods hand back is in 64-per-lane space
    // though, so all of it -- lane centres, GetXPos, the scroll axis -- is
    // scaled by 50/64 on the way out. Scaling UNIFORMLY is what keeps the
    // identity AGENTS.md insists on: flip's displacement is a whole number of
    // laneXPixels, so it stays a whole number of lanes here and still lands
    // dead centre. Using 50 for layout while leaving the mods at 64 would be
    // the LANE_W bug all over again -- flip would overshoot by 28%.
    //
    const float PANEL = 50.0f;
    const float HALF  = PANEL * 0.5f;
    const float PSCALE = PANEL / nc::ARROW_SIZE;      // 0.78125

    // `piu` is a transition, not a switch: the pad slides in from above and
    // shoves the CH highway out of the bottom (that half is applied to `my` in
    // drawFrame, before the matrices), so 40% means the swap is 40% done
    // rather than "pump, immediately". It enters from just above the frame,
    // not from a whole screen height up -- travelling VIRT_H left it invisible
    // until 70% and then it arrived all at once. PANEL of clearance is enough
    // to start hidden.
    const float piuT = mods.piu < 0.0f ? 0.0f : (mods.piu > 1.0f ? 1.0f : mods.piu);

    // SM5's own screen layout, not the CH highway's. `ReceptorArrowsYStandard`
    // is -144 from SCREEN_CENTER_Y and `ReceptorArrowsYReverse` is +144
    // (Themes/_fallback/metrics.ini:1157-1158), so the receptors sit at y=96
    // upscroll and y=384 under full reverse, and the throw between them is the
    // 288px fYReverseOffsetPixels every ArrowEffects call is handed.
    //
    // That throw is a PLAYER metric in a 480-tall screen -- it is where the
    // receptors live, not how far an arrow moves -- so unlike everything the
    // mods hand back it does NOT go through PSCALE. Routing it through
    // ApplyScrollPos and scaling it by the column spacing is what left 100%
    // reverse with the receptors at 68% of the frame instead of 80%, with the
    // notes cramped into what was left below them.
    const float PAD_Y  = 240.0f - 144.0f;
    const float SM_REV = 288.0f;
    float zoomRev = 1.0f - mods.mini * 0.5f;                  // ArrowEffects:609
    if (fabsf(zoomRev) < 0.01f) zoomRev = 0.01f;              // :613-614
    const float revBase = -SM_REV / zoomRev / 2.0f;
    auto reverseFor = [&](int lane) {
        return ReversePercentForCol(mods,lane);
    };
    auto revScaleFor = [&](int lane) {
        return nc::scale(reverseFor(lane),0.0f,1.0f,1.0f,-1.0f);
    };
    auto receptorYFor = [&](int lane) {
        float shift = nc::scale(reverseFor(lane),0.0f,1.0f,
                                revBase,-revBase);
        shift = nc::scale(mods.centered,0.0f,1.0f,shift,0.0f);
        const float target = PAD_Y+(shift-revBase);
        return target-(1.0f-piuT)*(target+PANEL);
    };
    // scale=+1 upscroll, -1 reversed (:621). Re-origined by -revBase so that
    // reverse 0 leaves the pad exactly at PAD_Y; `centered` then lands it on
    // SCREEN_CENTER_Y, 96 + 144, on its own.
    // Screen y for a note at arrow-space offset `yPx`.
    auto padY = [&](int lane,float yPx) {
        return receptorYFor(lane)+yPx*revScaleFor(lane)*PSCALE;
    };
    // GetYPos adds these after reverse, so reverse changes scroll direction
    // without reflecting tipsy, beaty or per-column movey themselves.
    auto noteY = [&](int lane, float yPx) {
        return padY(lane,yPx) +
               GetYPosOffset(mods, lane, yPx, float(beat), bpm) * PSCALE;
    };

    const bool hasStops = !chart.stops.empty();
    auto ssec = [&](double t) { return hasStops ? chart.scrollSec(t) : t; };

    // A rotated quad in screen space. The top edge (smaller y) takes vTop,
    // which is the sprite's own top row -- the textures load flipY (PNG row 0
    // at v=1), so vTop is the larger v.
    auto quad2 = [](std::vector<ch::Vtx>& out, float cx, float cy,
                    float hw, float hh, float rotDeg,
                    float u0, float vTop, float u1, float vBot,
                    float r, float g, float b, float a) {
        const float th = rotDeg * 3.14159265f / 180.0f;
        const float c = cosf(th), sn = sinf(th);
        auto P = [&](float dx, float dy, float u, float v) {
            ch::Vtx t;
            t.x = cx + dx * c - dy * sn;
            t.y = cy + dx * sn + dy * c;
            t.z = 0.0f; t.u = u; t.v = v;
            t.r = r; t.g = g; t.b = b; t.a = a;
            return t;
        };
        const ch::Vtx q0 = P(-hw, -hh, u0, vTop), q1 = P(hw, -hh, u1, vTop),
                      q2 = P( hw,  hh, u1, vBot), q3 = P(-hw, hh, u0, vBot);
        out.push_back(q0); out.push_back(q1); out.push_back(q2);
        out.push_back(q0); out.push_back(q2); out.push_back(q3);
    };

    // Batched per art: three textures cover five lanes, because UpRight and
    // DownRight are mirrors (NoteSkin.lua BaseRotY = 180).
    std::vector<ch::Vtx> vRecep[3], vGlow[3], vTap[3], vTapGlow[3],
                         vBody[3], vCap[3], vExpl[3], vFlash;

    // metrics.ini: AnimationIsBeatBased=0, TapNoteAnimationLength=0.25 -- six
    // frames per quarter second, on the clock rather than the beat, and global
    // rather than per-note.
    auto animFrameAt = [](float t, int frames) {
        const float cyc = t / ch::PIU_ANIM_LEN;
        int f = int((cyc - floorf(cyc)) * float(frames));
        return f < 0 ? 0 : (f >= frames ? frames - 1 : f);
    };
    const int tapFrame  = animFrameAt(songTime, ch::PIU_TAP_FRAMES);
    const int holdFrame = animFrameAt(songTime, ch::PIU_HOLD_FRAMES);

    float u0, vb, u1, vt;

    // ---- receptors ---------------------------------------------------------
    // ReceptorArrowRow.cpp:46-54: a receptor is an arrow evaluated at
    // fYOffset = 0. Which mods move it is a consequence, not a special case.
    const float pulse = ch::piuReceptorGlow(beat);
    for (int lane = 0; lane < 5; ++lane) {
        const int  art = ch::PIU_ART[lane];
        const bool mir = ch::PIU_MIRROR[lane];
        const float rx = CENTER_X + (nc::laneXPixels(lane) +
                         GetXPos(mods, lane, 0.0f, songTime, float(beat), bpm)) * PSCALE;
        const float ry = noteY(lane, 0.0f);
        // PressCommand from UpLeft Receptor.lua: linear .05 zoom .9, then
        // linear .1 back to 1.
        float zoom = GetZoom(mods);
        if (!o.noBot) {
            const float dt = float(lastHit(lane, songTime));
            if (dt >= 0.0f && dt < 0.05f)       zoom *= 1.0f - 0.1f * (dt / 0.05f);
            else if (dt >= 0.05f && dt < 0.15f) zoom *= 0.9f + 0.1f * ((dt - 0.05f) / 0.10f);
        }
        const float hw = HALF * zoom;
        ch::piuSheetUV(1, 3, 0, mir, u0, vb, u1, vt);
        quad2(vRecep[art], rx, ry, hw, hw, 0.0f, u0, vt, u1, vb, 1,1,1, 1);
        if (pulse > 0.0f) {
            ch::piuSheetUV(1, 3, 1, mir, u0, vb, u1, vt);
            quad2(vGlow[art], rx, ry, hw, hw, 0.0f, u0, vt, u1, vb, 1,1,1, pulse);
        }

        // ---- explosion -----------------------------------------------------
        // UpLeft Explosion.lua is the column's OWN tap sprite drawn additively
        // with a Glow command -- so it is the same art, blown up and faded out.
        if (!o.noBot) {
            const float dt = float(lastHit(lane, songTime));
            const float LIFE = 0.20f;
            if (dt >= 0.0f && dt < LIFE) {
                const float k = dt / LIFE;
                const float a = 1.0f - k;
                const float ez = hw * (1.0f + 0.6f * k);
                quad2(vFlash, rx, ry, ez * 1.6f, ez * 1.6f, 0.0f,
                      0.0f, 1.0f, 1.0f, 0.0f, 1,1,1, a * 0.5f);
                ch::piuSheetUV(3, 2, tapFrame, mir, u0, vb, u1, vt);
                quad2(vExpl[art], rx, ry, ez, ez, 0.0f, u0, vt, u1, vb, 1,1,1, a);
            }
        }
    }

    // No pad outline. The noteskin draws one (UpLeft Receptor.lua, under
    // `Condition = Var "Button" == "Center"`), but at this size it reads as a
    // faint box around the row and is not worth the trouble of matching its
    // art size to our column spacing.

    // ---- notes and holds ---------------------------------------------------
    const float noteZoom = GetZoom(mods);
    for (int i = int(chart.notes.size()) - 1; i >= 0; --i) {
        const Note& n = chart.notes[i];
        const double tHit = chart.beatToSec(n.beat);
        const float z0 = float(ssec(tHit) - scrollNow) * noteSpeed;
        const float yRaw = z0 * nc::ARROW_SIZE * 1.6f;
        // The bot consumes the HEAD at the strike line, not the note: a hold
        // is on screen for as long as its tail is, and dropping the whole
        // entry here is what made a PIU hold vanish the instant it was held.
        // The CH path never hit this because it draws sustains in a loop of
        // their own; this one folds head and body together, so the head is
        // skipped further down instead.
        const bool consumed = !o.noBot && yRaw <= 0.0f;
        double tLast = tHit;
        for (int L = 0; L < 5; ++L)
            if ((n.frets & (1 << L)) && n.sustain[L] > 0.0) {
                const double e = chart.beatToSec(n.beat + n.sustain[L]);
                if (e > tLast) tLast = e;
            }
        const bool hasSus = tLast > tHit;
        if (consumed && songTime >= tLast) continue;

        for (int lane = 0; lane < 5; ++lane) {
            if (!(n.frets & (1 << lane))) continue;
            const int  art = ch::PIU_ART[lane];
            const bool mir = ch::PIU_MIRROR[lane];
            const float yOff = ApplyYMods(mods,lane,yRaw,float(beat));

            // This path is already in ITG's Y-down screen space, so the
            // ArrowEffects rotation signs are used directly.
            const float rotZ = GetRotationZ(mods,float(n.beat),float(beat));
            const float rotX = GetRotationX(mods,yOff);
            const float rotY = GetRotationY(mods,yOff);
            const float fx = fabsf(cosf(rotY*3.14159265f/180.0f));
            const float fy = fabsf(cosf(rotX*3.14159265f/180.0f));
            const float baseAlpha = GetAlpha(mods,yOff,songTime);
            const float baseGlow = GetGlow(mods,yOff,songTime);
            if (!hasSus && baseAlpha <= 0.0f && baseGlow <= 0.0f) continue;

            const float sy = noteY(lane, yOff);
            // Off-screen head. Only fatal for a tap: a hold's head walks well
            // past the top of the frame while its ribbon is still crossing it,
            // and the body loop culls itself per segment anyway.
            const bool headOff = sy < -PANEL * 2.0f || sy > VIRT_H + PANEL * 2.0f;
            if (headOff && !hasSus) continue;
            const float sx = CENTER_X + (nc::laneXPixels(lane) +
                             GetXPos(mods, lane, yOff, songTime, float(beat), bpm)) * PSCALE;
            // ITG's bumpy is a GetZPos term -- depth toward the viewer. A flat
            // field has no depth to move through (it is invisible in ITG too
            // until tilt turns the field), so it is mapped to a small zoom:
            // nearer reads as bigger. A deviation, and a visible one is better
            // than a knob that silently does nothing.
            const float zoomB = 1.0f +
                GetZPos(mods, lane, yOff, float(beat), bpm) / 512.0f;
            const float hw = HALF * noteZoom * zoomB * fx;
            const float hh = HALF * noteZoom * zoomB * fy;

            // ---- the hold body, drawn BEFORE the head so the head caps it ---
            if (n.sustain[lane] > 0.0) {
                const float laneRevScale = revScaleFor(lane);
                const bool flipHold = reverseFor(lane) > 0.5f;
                const double tEnd = chart.beatToSec(n.beat + n.sustain[lane]);
                if (songTime < tEnd) {
                    const float zEnd = float(ssec(tEnd) - scrollNow) * noteSpeed;
                    const float yEndRaw = zEnd * nc::ARROW_SIZE * 1.6f;
                    // Held notes anchor at the receptor, exactly as CH's do.
                    const float yStart = (!o.noBot && songTime >= tHit) ? 0.0f : yRaw;
                    // Walk in ITG's own fYStep of 16px (NoteDisplay.cpp:983) so
                    // the ribbon follows every per-row mod instead of being a
                    // straight bar through a curve.
                    const int STEPS = 96;
                    const float span = yEndRaw - yStart;
                    if (span > 0.0f) {
                        const int nseg = int(span / 16.0f) + 1;
                        const int use = nseg > STEPS ? STEPS : nseg;
                        for (int k = 0; k < use; ++k) {
                            const float y0 = yStart + span * float(k) / float(use);
                            const float y1 = yStart + span * float(k + 1) / float(use);
                            const float ym = 0.5f * (y0 + y1);
                            const float my = ApplyYMods(mods, lane, ym, float(beat));
                            const float sy0 = noteY(
                                lane, ApplyYMods(mods, lane, y0, float(beat)));
                            const float sy1 = noteY(
                                lane, ApplyYMods(mods, lane, y1, float(beat)));
                            const float top = sy0 < sy1 ? sy0 : sy1;
                            const float bot = sy0 < sy1 ? sy1 : sy0;
                            if (bot < -PANEL || top > VIRT_H + PANEL) continue;
                            const float mx = CENTER_X + (nc::laneXPixels(lane) +
                                             GetXPos(mods, lane, my, songTime, float(beat), bpm)) * PSCALE;
                            const float ha = GetAlpha(mods, my, songTime);
                            // Every segment is BODY. The cap is a separate
                            // quad below, at its own height -- SM5 gives the
                            // bottom cap the span y_tail..y_tail+frameHeight
                            // (NoteDisplay.cpp:1061-1064) rather than handing
                            // it the leftovers. Making the last body segment
                            // the cap is what squashed it into a slanted stub
                            // as a hold ran out.
                            ch::piuSheetUV(6, 1, holdFrame, mir, u0, vb, u1, vt);
                            // FlipHoldBodyWhenReverse: the ribbon and its cap
                            // are art with a direction, and downscroll runs
                            // them the other way. Without this the cap tapers
                            // toward the receptor instead of away from it.
                            const float bodyZoom = 1.0f +
                                GetZPos(mods, lane, my, float(beat), bpm) / 512.0f;
                            quad2(vBody[art], mx, 0.5f * (top + bot),
                                  HALF * noteZoom * bodyZoom * fx,
                                  0.5f * (bot - top) + 0.5f, 0.0f,
                                  u0, flipHold ? vb : vt, u1, flipHold ? vt : vb,
                                  1,1,1, ha);
                        }
                    }

                    // ---- the bottom cap, at its OWN height ------------------
                    // 64px tall in the sheet, so one whole arrow past the tail
                    // (SM5 spans it y_tail..tail_plus_bottom). Fixed height is
                    // the point: a cap that shrinks with the remaining hold is
                    // the squashed stub this replaces.
                    {
                        const float myEnd = ApplyYMods(mods, lane, yEndRaw, float(beat));
                        const float syT = noteY(lane, myEnd);
                        // Away from the receptor, which flips with the scroll.
                        const float syF = syT + nc::ARROW_SIZE * PSCALE *
                                                   noteZoom * laneRevScale;
                        const float top = syT < syF ? syT : syF;
                        const float bot = syT < syF ? syF : syT;
                        if (bot >= -PANEL && top <= VIRT_H + PANEL) {
                            const float mx = CENTER_X + (nc::laneXPixels(lane) +
                                             GetXPos(mods, lane, myEnd, songTime,
                                                     float(beat), bpm)) * PSCALE;
                            const float ha = GetAlpha(mods, myEnd, songTime);
                            ch::piuSheetUV(6, 1, holdFrame, mir, u0, vb, u1, vt);
                            const float capZoom = 1.0f +
                                GetZPos(mods, lane, myEnd, float(beat), bpm) / 512.0f;
                            quad2(vCap[art], mx, 0.5f * (top + bot),
                                  HALF * noteZoom * capZoom * fx,
                                  0.5f * (bot - top), 0.0f,
                                  u0, flipHold ? vb : vt, u1, flipHold ? vt : vb,
                                  1,1,1, ha);
                        }
                    }
                }
            }

            // ---- the head ---------------------------------------------------
            // NoteSkin.lua:18-19 redirects Hold Head to Tap Note, so a hold's
            // head IS the tap sprite -- one code path covers both.
            ch::piuSheetUV(3, 2, tapFrame, mir, u0, vb, u1, vt);
            const bool drawHead = !consumed && !headOff;
            if (drawHead && baseAlpha > 0.0f)
                quad2(vTap[art], sx, sy, hw, hh, rotZ, u0, vt, u1, vb,
                      1,1,1, baseAlpha);
            if (drawHead && baseGlow > 0.0f)
                quad2(vTapGlow[art], sx, sy, hw, hh, rotZ, u0, vt, u1, vb,
                      1,1,1, baseGlow);
        }
    }

    // ---- draw, back to front ----------------------------------------------
    for (int i = 0; i < 3; ++i) { v_ = vBody[i]; drawPiuLayer(texPiuHoldBody_[i].id, ch::BLEND_SPRITE); }
    for (int i = 0; i < 3; ++i) { v_ = vCap[i];  drawPiuLayer(texPiuHoldCap_[i].id,  ch::BLEND_SPRITE); }
    for (int i = 0; i < 3; ++i) { v_ = vRecep[i]; drawPiuLayer(texPiuRecep_[i].id, ch::BLEND_SPRITE); }
    for (int i = 0; i < 3; ++i) { v_ = vGlow[i];  drawPiuLayer(texPiuRecep_[i].id, ch::BLEND_ADD); }
    for (int i = 0; i < 3; ++i) { v_ = vTap[i];   drawPiuLayer(texPiuTap_[i].id, ch::BLEND_SPRITE); }
    for (int i = 0; i < 3; ++i) { v_ = vTapGlow[i]; drawPiuLayer(texPiuTap_[i].id, ch::BLEND_ADD); }
    v_ = vFlash; drawPiuLayer(texPiuFlash_.id, ch::BLEND_ADD);
    for (int i = 0; i < 3; ++i) { v_ = vExpl[i];  drawPiuLayer(texPiuTap_[i].id, ch::BLEND_ADD); }
}

void Renderer::drawTaikoLayer(GLuint tex, int blend) {
    if (v_.empty()) return;
    applyFieldTint();
    glUseProgram(piu_);
    glBlendFunc(GL_ONE,
                blend == ch::BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(piu_, "uTex"), 0);
    glUniformMatrix4fv(glGetUniformLocation(piu_, "uMVP"), 1, GL_FALSE,
                       taikoMvp_.m);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                 v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
    v_.clear();
    glUseProgram(prog_);
}

// The taiko lane.
//
// drawPiu turned a quarter turn. Taiko is a FLAT field like pump's -- no neck,
// no vanishing point -- but its notes run right to left along one horizontal
// line into a target ring, so the axes swap: ArrowEffects' scroll offset drives
// screen X and its lateral GetXPos drives screen Y. Everything is computed in
// the skin's own authored 1920x1080 space, y down (ch::TAIKO_*, lifted from the
// skin's GameConfig.ini), and nothing is converted to world units and back, so
// the mods land the way ITG intends rather than being translated twice.
//
// Taiko has two note colours, red don and blue ka. This renders full grybo, so
// three things differ from the real game by design: green/yellow/orange/purple
// note art is generated (devtools/taiko_extract.py), a CHORD fans its notes
// about the line rather than stacking them into one invisible pile, and star
// power notes borrow the big-note art that taiko spends on finishers.
void Renderer::drawTaiko(const Chart& chart, double beat, const RenderOpts& o,
                         const Mods& mods, float songTime, float scrollNow,
                         float noteSpeed, float bpm, const Mat4& mvp) {
    const float taikoT =
        mods.taiko < 0.0f ? 0.0f : (mods.taiko > 1.0f ? 1.0f : mods.taiko);
    if (taikoT <= 0.0f) return;
    taikoMvp_ = mvp;

    const float HALF = ch::TAIKO_NOTE * 0.5f;

    // --- the axis map -------------------------------------------------------
    // Reverse still means "scroll the other way", so it flips the SCROLL axis,
    // which here is X: notes arrive from the left instead of the right. Per
    // column reverse (split/cross/alternate) still resolves per lane even
    // though the lanes share a line -- a chord can genuinely have half its
    // notes coming from each side, which is the honest reading of those mods
    // on a one-lane field rather than a special case.
    auto revScaleFor = [&](int lane) {
        return nc::scale(ReversePercentForCol(mods, lane), 0.0f, 1.0f,
                         1.0f, -1.0f);
    };
    // `centered` has no second axis to centre on here -- the line already is
    // the centre -- so it is spent on the target ring's X, sliding the strike
    // point to mid-lane and giving the notes the full width on both sides.
    auto judgeXFor = [&](int lane) {
        const float mid = ch::TAIKO_LANE_X + ch::TAIKO_LANE_W * 0.5f;
        const float base = ch::TAIKO_JUDGE_X;
        const float x = nc::scale(mods.centered, 0.0f, 1.0f, base, mid);
        // Under reverse the ring mirrors about the lane's midpoint, so the
        // notes still have the long side of the lane to travel down.
        return revScaleFor(lane) < 0.0f ? (2.0f * mid - x) : x;
    };
    // Screen X for a note at arrow-space offset yPx. GetYPosOffset carries
    // tipsy/beaty/movey, which ITG adds AFTER reverse -- so they do not flip
    // with the scroll direction, exactly as in drawPiu.
    auto noteX = [&](int lane, float yPx) {
        return judgeXFor(lane) + yPx * revScaleFor(lane) * ch::TAIKO_SCALE +
               GetYPosOffset(mods, lane, yPx, float(beat), bpm) *
                   ch::TAIKO_SCALE;
    };
    // Screen Y: the line, the chord's fan, then ArrowEffects' lateral term.
    auto noteY = [&](int lane, float yPx, float fan) {
        return ch::TAIKO_JUDGE_Y + fan +
               GetXPos(mods, lane, yPx, songTime, float(beat), bpm) *
                   ch::TAIKO_SCALE;
    };

    const bool hasStops = !chart.stops.empty();
    auto ssec = [&](double t) { return hasStops ? chart.scrollSec(t) : t; };

    auto quad = [&](std::vector<ch::Vtx>& out, float cx, float cy,
                    float hw, float hh, float rotDeg,
                    float u0, float u1, float a,
                    float vTop = 1.0f, float vBot = 0.0f) {
        const float th = rotDeg * 3.14159265f / 180.0f;
        const float c = cosf(th), sn = sinf(th);
        auto P = [&](float dx, float dy, float u, float v) {
            ch::Vtx t;
            t.x = cx + dx * c - dy * sn;
            t.y = cy + dx * sn + dy * c;
            t.z = 0.0f; t.u = u; t.v = v;
            t.r = 1.0f; t.g = 1.0f; t.b = 1.0f; t.a = a;
            return t;
        };
        // Textures load flipY, so the sprite's top row is v = 1.
        const ch::Vtx q0 = P(-hw, -hh, u0, vTop), q1 = P(hw, -hh, u1, vTop),
                      q2 = P( hw,  hh, u1, vBot), q3 = P(-hw, hh, u0, vBot);
        out.push_back(q0); out.push_back(q1); out.push_back(q2);
        out.push_back(q0); out.push_back(q2); out.push_back(q3);
    };
    // An axis-aligned rect given in top-left/size form, which is how every
    // constant in the skin's GameConfig.ini is written.
    auto rect = [&](std::vector<ch::Vtx>& out, float x, float y,
                    float w, float h, float a,
                    float vTop = 1.0f, float vBot = 0.0f) {
        quad(out, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, 0.0f,
             0.0f, 1.0f, a, vTop, vBot);
    };

    // Frame `f` of a 1 x n horizontal strip.
    auto stripU = [](int n, int f, float& u0, float& u1) {
        u0 = float(f) / float(n);
        u1 = float(f + 1) / float(n);
    };

    std::vector<ch::Vtx> vLane, vGogo, vFlash, vBar, vJudge, vDrumBg, vFrame,
                         vDrum, vDrumHit;
    std::vector<ch::Vtx> vRollBody[6], vRollCap[6], vNote[6], vBig[6];

    const bool board = !o.playfield && mods.hideboard == 0.0f;
    // Ticks are beats * resolution (Chart.cs's own scaling, the inverse of the
    // sustain conversion at load), which is all phraseAt wants.
    const bool inSp = chart.phraseAt(
        PhraseType::StarPower, int(beat * double(chart.resolution))) != nullptr;

    // ---- board -------------------------------------------------------------
    if (board) {
        rect(vFrame,  ch::TAIKO_FRAME_X,  ch::TAIKO_FRAME_Y,
                      ch::TAIKO_FRAME_W,  ch::TAIKO_FRAME_H,  taikoT);
        rect(vLane,   ch::TAIKO_LANE_X,   ch::TAIKO_LANE_Y,
                      ch::TAIKO_LANE_W,   ch::TAIKO_LANE_H,   taikoT);
        // Taiko's kiai lane, spent on the chart's star power phrases -- the
        // one thing in either game that means "this stretch is the payoff".
        if (inSp)
            rect(vGogo, ch::TAIKO_LANE_X, ch::TAIKO_LANE_Y,
                        ch::TAIKO_LANE_W, ch::TAIKO_LANE_H, taikoT);
        rect(vDrumBg, ch::TAIKO_DRUMBG_X, ch::TAIKO_DRUMBG_Y,
                      ch::TAIKO_DRUMBG_W, ch::TAIKO_DRUMBG_H, taikoT);

        // Bar lines, straight off chart.beatlines. Taiko draws measures only,
        // so the two beat styles are dropped rather than thinned.
        for (const BeatLine& bl : chart.beatlines) {
            if (bl.style != 0) continue;
            const float z = float(ssec(bl.sec) - scrollNow) * noteSpeed;
            const float yRaw = z * nc::ARROW_SIZE * 1.6f;
            const float bx = noteX(0, yRaw);
            if (bx < ch::TAIKO_LANE_X - ch::TAIKO_BAR_W ||
                bx > ch::TAIKO_VW + ch::TAIKO_BAR_W) continue;
            // Bar.png is 270 tall but the game CROPS it to the note cell
            // rather than scaling: source rect (0, 0, width, Game_Notes_Size[1])
            // drawn top-left at the scroll origin (CStage演奏ドラム画面.cs:1168).
            // So only the top 195 rows are ever seen, and the bar spans the
            // note cell -- which is the lane exactly.
            rect(vBar, bx - ch::TAIKO_BAR_W * 0.5f, ch::TAIKO_SCROLL_Y,
                 ch::TAIKO_BAR_W, ch::TAIKO_NOTE, taikoT,
                 1.0f, 1.0f - ch::TAIKO_NOTE / ch::TAIKO_BAR_H);
        }

        // The lane flash, and the drum's lit half. The bot's hit drives both:
        // Don (the drum's face) for an ordinary note, Ka (its rim) inside a
        // star power phrase, which is the taiko idiom for a note that wants
        // both hands.
        float sinceHit = -1.0f;
        if (!o.noBot)
            for (int lane = 0; lane < 5; ++lane) {
                const float dt = float(lastHit(lane, songTime));
                if (dt >= 0.0f && (sinceHit < 0.0f || dt < sinceHit))
                    sinceHit = dt;
            }
        const float HIT_LIFE = 0.18f;
        if (sinceHit >= 0.0f && sinceHit < HIT_LIFE) {
            const float k = 1.0f - sinceHit / HIT_LIFE;
            rect(vFlash, ch::TAIKO_LANE_X, ch::TAIKO_LANE_Y,
                 ch::TAIKO_LANE_W, ch::TAIKO_LANE_H, taikoT * k * 0.7f);
            rect(vDrumHit, ch::TAIKO_DRUM_X, ch::TAIKO_DRUM_Y,
                 ch::TAIKO_DRUM_W, ch::TAIKO_DRUM_H, taikoT * k);
        }
        rect(vDrum, ch::TAIKO_DRUM_X, ch::TAIKO_DRUM_Y,
                    ch::TAIKO_DRUM_W, ch::TAIKO_DRUM_H, taikoT);

        // The target ring, evaluated at yOffset 0 -- a receptor is an arrow
        // that never moved (ReceptorArrowRow.cpp:46-54), so whichever mods
        // displace it do so as a consequence rather than a special case.
        quad(vJudge, noteX(0, 0.0f), noteY(0, 0.0f, 0.0f), HALF, HALF, 0.0f,
             0.0f, 1.0f, taikoT);
    }

    // ---- notes -------------------------------------------------------------
    const float noteZoom = GetZoom(mods);
    float u0, u1;
    for (int i = int(chart.notes.size()) - 1; i >= 0; --i) {
        const Note& n = chart.notes[i];
        const double tHit = chart.beatToSec(n.beat);
        const float z0 = float(ssec(tHit) - scrollNow) * noteSpeed;
        const float yRaw = z0 * nc::ARROW_SIZE * 1.6f;
        const bool big = chart.phraseAt(PhraseType::StarPower, n.tick) != nullptr;

        // The bot consumes the HEAD at the strike point, not the note: a roll
        // stays on screen for as long as its tail does. Same shape as drawPiu,
        // which folds head and body into one pass.
        const bool consumed = !o.noBot && yRaw <= 0.0f;
        double tLast = tHit;
        for (int L = 0; L < 5; ++L)
            if ((n.frets & (1 << L)) && n.sustain[L] > 0.0) {
                const double e = chart.beatToSec(n.beat + n.sustain[L]);
                if (e > tLast) tLast = e;
            }
        if (n.open && n.openSustain > 0.0) {
            const double e = chart.beatToSec(n.beat + n.openSustain);
            if (e > tLast) tLast = e;
        }
        if (consumed && songTime >= tLast) continue;

        // The fan. A lone note rides the line; a chord spreads its members
        // evenly about it so five colours stay five colours. An open note is
        // the whole lane's note, so it takes the line by itself.
        int lanes[5], nLanes = 0;
        if (!n.open)
            for (int L = 0; L < 5; ++L)
                if (n.frets & (1 << L)) lanes[nLanes++] = L;
        if (n.open) { lanes[0] = 0; nLanes = 1; }

        for (int idx = 0; idx < nLanes; ++idx) {
            const int lane = lanes[idx];
            const int art = n.open ? 5 : lane;
            const float fan = n.open ? 0.0f
                : (float(idx) - float(nLanes - 1) * 0.5f) * ch::TAIKO_FAN;
            const double sus = n.open ? n.openSustain : n.sustain[lane];

            const float yOff = ApplyYMods(mods, lane, yRaw, float(beat));
            const float baseAlpha = GetAlpha(mods, yOff, songTime);
            const float baseGlow  = GetGlow(mods, yOff, songTime);
            if (sus <= 0.0 && baseAlpha <= 0.0f && baseGlow <= 0.0f) continue;

            // Already in a y-down screen space, so ArrowEffects' rotation
            // signs are used directly, as in drawPiu.
            const float rotZ = GetRotationZ(mods, float(n.beat), float(beat));
            const float rotX = GetRotationX(mods, yOff);
            const float rotY = GetRotationY(mods, yOff);
            const float fx = fabsf(cosf(rotY * 3.14159265f / 180.0f));
            const float fy = fabsf(cosf(rotX * 3.14159265f / 180.0f));
            // bumpy is a depth term and a flat field has no depth, so it is
            // mapped to a small zoom -- nearer reads as bigger. drawPiu makes
            // the same call and for the same reason.
            const float zoomB = 1.0f +
                GetZPos(mods, lane, yOff, float(beat), bpm) / 512.0f;
            const float hw = HALF * noteZoom * zoomB * fx;
            const float hh = HALF * noteZoom * zoomB * fy;

            const float sx = noteX(lane, yOff);
            const float sy = noteY(lane, yOff, fan);
            const bool headOff = sx < -ch::TAIKO_NOTE ||
                                 sx > ch::TAIKO_VW + ch::TAIKO_NOTE;

            // ---- the roll, drawn BEFORE the head so the head caps it -------
            if (sus > 0.0) {
                const double tEnd = chart.beatToSec(n.beat + sus);
                if (songTime < tEnd) {
                    const float zEnd = float(ssec(tEnd) - scrollNow) * noteSpeed;
                    const float yEndRaw = zEnd * nc::ARROW_SIZE * 1.6f;
                    // A held roll anchors at the ring, exactly as CH sustains
                    // and PIU holds do.
                    const float yStart =
                        (!o.noBot && songTime >= tHit) ? 0.0f : yRaw;
                    const float span = yEndRaw - yStart;
                    if (span > 0.0f) {
                        // Walk in ITG's own 16px step so the ribbon follows
                        // every per-row mod instead of being a straight bar
                        // drawn through a curve.
                        const int STEPS = 96;
                        const int nseg = int(span / 16.0f) + 1;
                        const int use = nseg > STEPS ? STEPS : nseg;
                        for (int k = 0; k < use; ++k) {
                            const float ya = yStart + span * float(k) / float(use);
                            const float yb = yStart + span * float(k + 1) / float(use);
                            const float ym = 0.5f * (ya + yb);
                            const float my = ApplyYMods(mods, lane, ym, float(beat));
                            const float xa = noteX(lane,
                                ApplyYMods(mods, lane, ya, float(beat)));
                            const float xb = noteX(lane,
                                ApplyYMods(mods, lane, yb, float(beat)));
                            const float lo = xa < xb ? xa : xb;
                            const float hi = xa < xb ? xb : xa;
                            if (hi < -ch::TAIKO_NOTE ||
                                lo > ch::TAIKO_VW + ch::TAIKO_NOTE) continue;
                            const float bodyZoom = 1.0f +
                                GetZPos(mods, lane, my, float(beat), bpm) / 512.0f;
                            stripU(3, 1, u0, u1);      // the body cell
                            quad(vRollBody[art],
                                 0.5f * (lo + hi), noteY(lane, my, fan),
                                 0.5f * (hi - lo) + 0.5f,
                                 HALF * noteZoom * bodyZoom * fy, 0.0f,
                                 u0, u1, GetAlpha(mods, my, songTime) * taikoT);
                        }
                    }
                    // The tail cap, at its own fixed size one cell past the
                    // end -- a cap that shrank with the remaining roll would
                    // squash into a stub as it ran out.
                    {
                        const float myEnd =
                            ApplyYMods(mods, lane, yEndRaw, float(beat));
                        const float xT = noteX(lane, myEnd);
                        if (xT >= -ch::TAIKO_NOTE * 2.0f &&
                            xT <= ch::TAIKO_VW + ch::TAIKO_NOTE * 2.0f) {
                            const float capZoom = 1.0f +
                                GetZPos(mods, lane, myEnd, float(beat), bpm) / 512.0f;
                            // Away from the ring, which flips with the scroll.
                            const float dir = revScaleFor(lane);
                            stripU(3, 2, u0, u1);      // the tail cell
                            quad(vRollCap[art],
                                 xT + HALF * noteZoom * capZoom * dir * 0.5f,
                                 noteY(lane, myEnd, fan),
                                 HALF * noteZoom * capZoom * 0.5f,
                                 HALF * noteZoom * capZoom * fy, 0.0f,
                                 dir < 0.0f ? u1 : u0, dir < 0.0f ? u0 : u1,
                                 GetAlpha(mods, myEnd, songTime) * taikoT);
                        }
                    }
                }
            }

            // ---- the head --------------------------------------------------
            // A roll's head is the tap sprite, the same one-path treatment
            // pump's noteskin gives its hold heads.
            if (consumed || headOff) continue;
            std::vector<ch::Vtx>& bucket = big ? vBig[art] : vNote[art];
            stripU(ch::TAIKO_NOTE_FRAMES, ch::taikoNoteFrame(beat, big),
                   u0, u1);
            if (baseAlpha > 0.0f)
                quad(bucket, sx, sy, hw, hh, rotZ, u0, u1, baseAlpha * taikoT);
            if (baseGlow > 0.0f)
                quad(bucket, sx, sy, hw, hh, rotZ, u0, u1, baseGlow * taikoT);
        }
    }

    // ---- draw, back to front -----------------------------------------------
    v_ = vFrame;   drawTaikoLayer(texTaikoFrame_.id,     ch::BLEND_SPRITE);
    v_ = vLane;    drawTaikoLayer(texTaikoLane_.id,      ch::BLEND_SPRITE);
    v_ = vGogo;    drawTaikoLayer(texTaikoLaneGogo_.id,  ch::BLEND_SPRITE);
    v_ = vFlash;   drawTaikoLayer(texTaikoLaneFlash_.id, ch::BLEND_ADD);
    v_ = vBar;     drawTaikoLayer(texTaikoBar_.id,       ch::BLEND_SPRITE);
    v_ = vJudge;   drawTaikoLayer(texTaikoJudge_.id,     ch::BLEND_SPRITE);
    for (int i = 0; i < 6; ++i) {
        v_ = vRollBody[i]; drawTaikoLayer(texTaikoRoll_[i].id, ch::BLEND_SPRITE);
    }
    for (int i = 0; i < 6; ++i) {
        v_ = vRollCap[i];  drawTaikoLayer(texTaikoRoll_[i].id, ch::BLEND_SPRITE);
    }
    for (int i = 0; i < 6; ++i) {
        v_ = vNote[i];     drawTaikoLayer(texTaikoNote_[i].id, ch::BLEND_SPRITE);
    }
    for (int i = 0; i < 6; ++i) {
        v_ = vBig[i];      drawTaikoLayer(texTaikoBig_[i].id,  ch::BLEND_SPRITE);
    }
    v_ = vDrumBg;  drawTaikoLayer(texTaikoDrumBg_.id, ch::BLEND_SPRITE);
    v_ = vDrum;    drawTaikoLayer(texTaikoDrum_.id,   ch::BLEND_SPRITE);
    // Don and Ka are opaque lit-state art, not glows: additive over the drum's
    // cream face blows Don's red out to white and the flash disappears.
    v_ = vDrumHit; drawTaikoLayer(inSp ? texTaikoDrumKa_.id : texTaikoDrumDon_.id,
                                  ch::BLEND_SPRITE);
}

void Renderer::drawBmsLayer(GLuint tex, int blend) {
    if (v_.empty()) return;
    applyFieldTint();
    glUseProgram(piu_);
    glBlendFunc(GL_ONE,
                blend == ch::BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(piu_, "uTex"), 0);
    glUniformMatrix4fv(glGetUniformLocation(piu_, "uMVP"), 1, GL_FALSE,
                       bmsMvp_.m);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                 v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
    v_.clear();
    glUseProgram(prog_);
}

// The LR2 beatmania lane.
//
// A flat 2D field on the drawPiu-shaped path. Where drawTaiko turned the axes a
// quarter turn, this one keeps them and flips only the scroll: BMS is
// DOWNSCROLL natively -- notes fall from the top of the lane onto a judge line
// at the bottom -- so the baseline direction is negative and `reverse` buys
// upscroll rather than the other way round. Everything is in LR2's own 640x480
// vscreen, y down, with every rect quoted from the default skin's CSV in
// ch::BMS_* (src/highway.h).
//
// beatmania colours notes by lane TYPE, not by lane: the scratch is red, white
// keys are white, black keys are blue. That is deliberately kept -- no grybo
// recolouring -- so three sprites cover all six lanes. NotClon's five frets map
// onto LR2 lanes 1..5 and an OPEN note takes the SCRATCH, which is the natural
// fit rather than a workaround: BMS's sixth lane is the one that is not a key.
void Renderer::drawBms(const Chart& chart, double beat, const RenderOpts& o,
                       const Mods& mods, float songTime, float scrollNow,
                       float noteSpeed, float bpm, const Mat4& mvp) {
    const float bmsT =
        mods.bms < 0.0f ? 0.0f : (mods.bms > 1.0f ? 1.0f : mods.bms);
    if (bmsT <= 0.0f) return;
    bmsMvp_ = mvp;

    // Scroll direction. -1 is BMS's own downscroll, so `reverse` runs it the
    // other way -- the opposite sign convention to drawPiu, whose ITG baseline
    // is upscroll precisely because StepMania spends `reverse` ON downscroll.
    auto dirFor = [&](int lane) {
        return nc::scale(ReversePercentForCol(mods, lane), 0.0f, 1.0f,
                         -1.0f, 1.0f);
    };
    // The judge line mirrors to the top of the field when the scroll flips, so
    // the notes always have the full lane to travel down.
    auto judgeYFor = [&](int lane) {
        const float rev = ReversePercentForCol(mods, lane);
        float y = nc::scale(rev, 0.0f, 1.0f, ch::BMS_JUDGE_Y, 0.0f);
        // `centered` pulls it to mid-field, the same use drawTaiko puts it to.
        return nc::scale(mods.centered, 0.0f, 1.0f, y, ch::BMS_JUDGE_Y * 0.5f);
    };
    auto noteY = [&](int lane, float yPx) {
        return judgeYFor(lane) + yPx * dirFor(lane) * ch::BMS_SCALE +
               GetYPosOffset(mods, lane, yPx, float(beat), bpm) * ch::BMS_SCALE;
    };
    // Lane centre plus ArrowEffects' lateral term.
    auto noteX = [&](int lane, int lr2Lane, float yPx) {
        const float cx = ch::BMS_LANE_X[lr2Lane] + ch::BMS_LANE_W[lr2Lane] * 0.5f;
        return cx + GetXPos(mods, lane, yPx, songTime, float(beat), bpm) *
                        ch::BMS_SCALE;
    };

    const bool hasStops = !chart.stops.empty();
    auto ssec = [&](double t) { return hasStops ? chart.scrollSec(t) : t; };

    auto quad = [&](std::vector<ch::Vtx>& out, float cx, float cy,
                    float hw, float hh, float rotDeg, float a,
                    float cr = 1.0f, float cg = 1.0f, float cb = 1.0f,
                    float vTop = 1.0f, float vBot = 0.0f) {
        const float th = rotDeg * 3.14159265f / 180.0f;
        const float c = cosf(th), sn = sinf(th);
        auto P = [&](float dx, float dy, float u, float v) {
            ch::Vtx t;
            t.x = cx + dx * c - dy * sn;
            t.y = cy + dx * sn + dy * c;
            t.z = 0.0f; t.u = u; t.v = v;
            t.r = cr; t.g = cg; t.b = cb; t.a = a;
            return t;
        };
        // Textures load flipY, so the sprite's top row is v = 1.
        const ch::Vtx q0 = P(-hw, -hh, 0.0f, vTop), q1 = P(hw, -hh, 1.0f, vTop),
                      q2 = P( hw,  hh, 1.0f, vBot), q3 = P(-hw, hh, 0.0f, vBot);
        out.push_back(q0); out.push_back(q1); out.push_back(q2);
        out.push_back(q0); out.push_back(q2); out.push_back(q3);
    };
    auto rect = [&](std::vector<ch::Vtx>& out, float x, float y,
                    float w, float h, float a,
                    float cr = 1.0f, float cg = 1.0f, float cb = 1.0f,
                    float vTop = 1.0f, float vBot = 0.0f) {
        quad(out, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, 0.0f, a,
             cr, cg, cb, vTop, vBot);
    };

    std::vector<ch::Vtx> vBg, vJudge, vMeasure, vGlow, vKb, vLeft, vTt;
    std::vector<ch::Vtx> vKeyFlash[2];
    std::vector<ch::Vtx> vBeam[3], vNote[3], vLnBody[3], vLnStart[3], vLnEnd[3];

    const bool board = !o.playfield && mods.hideboard == 0.0f;

    if (board) {
        // The lane surface, per lane -- NOT one flat black rect. White-key
        // lanes are lighter than the scratch and the black keys, the 2px gaps
        // between the lane rects carry a grey divider, and the strip is
        // bracketed in white. That is what makes a keyboard out of it; drawing
        // the whole strip in one black fill leaves a bare judge line with
        // nothing under it. Values in ch::BMS_LANE_FILL / BMS_DIVIDER.
        for (int L = 0; L < 6; ++L) {
            const float f = ch::BMS_LANE_FILL[L];
            rect(vBg, ch::BMS_LANE_X[L], 0.0f, ch::BMS_LANE_W[L],
                 ch::BMS_JUDGE_Y, bmsT, f, f, f);
        }
        for (int L = 0; L < 5; ++L) {
            const float gx = ch::BMS_LANE_X[L] + ch::BMS_LANE_W[L];
            rect(vBg, gx, 0.0f, ch::BMS_LANE_X[L + 1] - gx, ch::BMS_JUDGE_Y,
                 bmsT, ch::BMS_DIVIDER[0], ch::BMS_DIVIDER[1],
                 ch::BMS_DIVIDER[2]);
        }
        rect(vBg, ch::BMS_FIELD_X - ch::BMS_BORDER_W, 0.0f, ch::BMS_BORDER_W,
             ch::BMS_JUDGE_Y, bmsT, 1.0f, 1.0f, 1.0f);
        rect(vBg, ch::BMS_FIELD_X + ch::BMS_FIELD_W, 0.0f, ch::BMS_BORDER_W,
             ch::BMS_JUDGE_Y, bmsT, 1.0f, 1.0f, 1.0f);

        // The blue glow on the last stretch of lane before the judge line. It
        // follows the line when the scroll flips, and its gradient turns over
        // with it.
        {
            const bool down = dirFor(0) < 0.0f;
            const float jy = judgeYFor(0);
            rect(vGlow, ch::BMS_FIELD_X, down ? jy - ch::BMS_GLOW_H : jy,
                 ch::BMS_FIELD_W, ch::BMS_GLOW_H, bmsT,
                 1.0f, 1.0f, 1.0f, down ? 1.0f : 0.0f, down ? 0.0f : 1.0f);
        }

        // Measure bars. #SRC_LINE is 151x1 spanning the whole strip, and BMS
        // draws measures only -- the two finer beat styles are dropped rather
        // than thinned.
        for (const BeatLine& bl : chart.beatlines) {
            if (bl.style != 0) continue;
            const float z = float(ssec(bl.sec) - scrollNow) * noteSpeed;
            const float my = noteY(0, z * nc::ARROW_SIZE * 1.6f);
            if (my < -2.0f || my > ch::BMS_JUDGE_Y + 2.0f) continue;
            rect(vMeasure, ch::BMS_FIELD_X, my, ch::BMS_FIELD_W, 1.0f, bmsT);
        }

        // Key beams: the lane lights from the judge line back up the field
        // while it is being hit. The art is the full 315px of lane height at
        // the lane's own width, anchored on the line and running against the
        // scroll (#SRC_IMAGE,0,0,201,0,41,315 -> 33,321,41,-320).
        if (!o.noBot) {
            const float LIFE = 0.12f;
            for (int lane = 0; lane < 6; ++lane) {
                // The scratch is driven by OPEN notes, so it takes the open
                // clock -- lastOpenHit, which buildHitTimes keeps apart from
                // the per-fret ones precisely because an open is not a hit on
                // any lane. Reading lastHit(0) here instead lit the scratch
                // every time a GREEN note was hit.
                const int fret = lane == 0 ? 0 : lane - 1;
                const float dt = lane == 0
                    ? float(lastOpenHit(songTime))
                    : float(lastHit(fret, songTime));
                if (dt < 0.0f || dt >= LIFE) continue;
                const float a = (1.0f - dt / LIFE) * bmsT;
                const float d = dirFor(fret);
                const float jy = judgeYFor(fret);
                const float top = d < 0.0f ? jy - ch::BMS_JUDGE_Y : jy;
                rect(vBeam[ch::BMS_LANE_ART[lane]], ch::BMS_LANE_X[lane], top,
                     ch::BMS_LANE_W[lane], ch::BMS_JUDGE_Y, a);
            }
        }

        // The judge line, evaluated at yOffset 0 -- a receptor is an arrow that
        // never moved, so whichever mods displace it do so as a consequence.
        rect(vJudge, ch::BMS_FIELD_X, judgeYFor(0) - ch::BMS_NOTE_H * 0.5f,
             ch::BMS_FIELD_W, ch::BMS_NOTE_H, bmsT);

        // The controller under the line: the left frame edge with the
        // turntable housing, the disc, and the keyboard panel.
        rect(vLeft, ch::BMS_LEFT_X, ch::BMS_LEFT_Y,
             ch::BMS_LEFT_W, ch::BMS_LEFT_H, bmsT);
        rect(vTt, ch::BMS_TT_X, ch::BMS_TT_Y,
             ch::BMS_TT_W, ch::BMS_TT_H, bmsT);
        rect(vKb, ch::BMS_KB_X, ch::BMS_KB_Y,
             ch::BMS_KB_W, ch::BMS_KB_H, bmsT);

        // The keys light on the same press that raises the lane beam, so this
        // shares the beam's clock and lifetime. The scratch is absent by
        // design: its press timer drives the beam, and the keyboard has no
        // scratch key -- the turntable is the scratch.
        if (!o.noBot) {
            const float LIFE = 0.12f;
            for (int k = 0; k < 5; ++k) {
                const float dt = float(lastHit(k, songTime));
                if (dt < 0.0f || dt >= LIFE) continue;
                rect(vKeyFlash[ch::BMS_KEYFLASH_ART[k]],
                     ch::BMS_KEYFLASH_X[k], ch::BMS_KEYFLASH_Y[k],
                     ch::BMS_KEYFLASH_W[k], ch::BMS_KEYFLASH_H[k],
                     (1.0f - dt / LIFE) * bmsT);
            }
        }
    }

    // ---- notes -------------------------------------------------------------
    const float noteZoom = GetZoom(mods);
    for (int i = int(chart.notes.size()) - 1; i >= 0; --i) {
        const Note& n = chart.notes[i];
        const double tHit = chart.beatToSec(n.beat);
        const float z0 = float(ssec(tHit) - scrollNow) * noteSpeed;
        const float yRaw = z0 * nc::ARROW_SIZE * 1.6f;

        const bool consumed = !o.noBot && yRaw <= 0.0f;
        double tLast = tHit;
        for (int L = 0; L < 5; ++L)
            if ((n.frets & (1 << L)) && n.sustain[L] > 0.0) {
                const double e = chart.beatToSec(n.beat + n.sustain[L]);
                if (e > tLast) tLast = e;
            }
        if (n.open && n.openSustain > 0.0) {
            const double e = chart.beatToSec(n.beat + n.openSustain);
            if (e > tLast) tLast = e;
        }
        if (consumed && songTime >= tLast) continue;

        // An open note is the scratch and nothing else; otherwise every fret
        // in the mask draws in its own key lane.
        int lanes[5], nLanes = 0;
        if (n.open) { lanes[0] = 0; nLanes = 1; }
        else for (int L = 0; L < 5; ++L)
            if (n.frets & (1 << L)) lanes[nLanes++] = L;

        for (int idx = 0; idx < nLanes; ++idx) {
            const int lane = lanes[idx];
            const int lr2 = ch::bmsLaneFor(lane, n.open);
            const int art = ch::BMS_LANE_ART[lr2];
            const double sus = n.open ? n.openSustain : n.sustain[lane];

            const float yOff = ApplyYMods(mods, lane, yRaw, float(beat));
            const float baseAlpha = GetAlpha(mods, yOff, songTime);
            const float baseGlow  = GetGlow(mods, yOff, songTime);
            if (sus <= 0.0 && baseAlpha <= 0.0f && baseGlow <= 0.0f) continue;

            const float rotZ = GetRotationZ(mods, float(n.beat), float(beat));
            const float rotX = GetRotationX(mods, yOff);
            const float rotY = GetRotationY(mods, yOff);
            const float fx = fabsf(cosf(rotY * 3.14159265f / 180.0f));
            const float fy = fabsf(cosf(rotX * 3.14159265f / 180.0f));
            // bumpy is a depth term and a flat field has no depth, so it is
            // mapped to a small zoom, the same call drawPiu and drawTaiko make.
            const float zoomB = 1.0f +
                GetZPos(mods, lane, yOff, float(beat), bpm) / 512.0f;
            const float hw = ch::BMS_LANE_W[lr2] * 0.5f * noteZoom * zoomB * fx;
            const float hh = ch::BMS_NOTE_H * 0.5f * noteZoom * zoomB * fy;

            const float sx = noteX(lane, lr2, yOff);
            const float sy = noteY(lane, yOff);
            const bool headOff = sy < -ch::BMS_NOTE_H * 4.0f ||
                                 sy > ch::BMS_JUDGE_Y + ch::BMS_NOTE_H * 4.0f;

            // ---- the long note, drawn BEFORE the head so the head caps it ---
            if (sus > 0.0) {
                const double tEnd = chart.beatToSec(n.beat + sus);
                if (songTime < tEnd) {
                    const float zEnd = float(ssec(tEnd) - scrollNow) * noteSpeed;
                    const float yEndRaw = zEnd * nc::ARROW_SIZE * 1.6f;
                    // A held LN anchors at the judge line, as CH sustains do.
                    const float yStart =
                        (!o.noBot && songTime >= tHit) ? 0.0f : yRaw;
                    const float span = yEndRaw - yStart;
                    if (span > 0.0f) {
                        // ITG's own 16px walk, so the ribbon follows every
                        // per-row mod instead of being a straight bar through
                        // a curve.
                        const int STEPS = 96;
                        const int nseg = int(span / 16.0f) + 1;
                        const int use = nseg > STEPS ? STEPS : nseg;
                        for (int k = 0; k < use; ++k) {
                            const float ya = yStart + span * float(k) / float(use);
                            const float yb = yStart + span * float(k + 1) / float(use);
                            const float ym = 0.5f * (ya + yb);
                            const float my = ApplyYMods(mods, lane, ym, float(beat));
                            const float sa = noteY(lane,
                                ApplyYMods(mods, lane, ya, float(beat)));
                            const float sb = noteY(lane,
                                ApplyYMods(mods, lane, yb, float(beat)));
                            const float lo = sa < sb ? sa : sb;
                            const float hi = sa < sb ? sb : sa;
                            if (hi < -ch::BMS_NOTE_H ||
                                lo > ch::BMS_JUDGE_Y + ch::BMS_NOTE_H) continue;
                            const float bz = 1.0f +
                                GetZPos(mods, lane, my, float(beat), bpm) / 512.0f;
                            quad(vLnBody[art], noteX(lane, lr2, my),
                                 0.5f * (lo + hi),
                                 ch::BMS_LANE_W[lr2] * 0.5f * noteZoom * bz * fx,
                                 0.5f * (hi - lo) + 0.5f, 0.0f,
                                 GetAlpha(mods, my, songTime) * bmsT);
                        }
                    }
                    // The tail cap, at its own fixed height.
                    {
                        const float myEnd =
                            ApplyYMods(mods, lane, yEndRaw, float(beat));
                        const float syT = noteY(lane, myEnd);
                        if (syT >= -ch::BMS_NOTE_H * 4.0f &&
                            syT <= ch::BMS_JUDGE_Y + ch::BMS_NOTE_H * 4.0f) {
                            const float cz = 1.0f +
                                GetZPos(mods, lane, myEnd, float(beat), bpm) / 512.0f;
                            quad(vLnEnd[art], noteX(lane, lr2, myEnd), syT,
                                 ch::BMS_LANE_W[lr2] * 0.5f * noteZoom * cz * fx,
                                 ch::BMS_NOTE_H * 0.5f * noteZoom * cz * fy, 0.0f,
                                 GetAlpha(mods, myEnd, songTime) * bmsT);
                        }
                    }
                }
            }

            // ---- the head ---------------------------------------------------
            if (consumed || headOff) continue;
            // An LN's head is its own sprite in this skin, unlike pump's, which
            // redirects hold heads to the tap art.
            std::vector<ch::Vtx>& bucket = sus > 0.0 ? vLnStart[art] : vNote[art];
            if (baseAlpha > 0.0f)
                quad(bucket, sx, sy, hw, hh, rotZ, baseAlpha * bmsT);
            if (baseGlow > 0.0f)
                quad(bucket, sx, sy, hw, hh, rotZ, baseGlow * bmsT);
        }
    }

    // ---- draw, back to front -----------------------------------------------
    v_ = vBg;      drawBmsLayer(texWhite_.id, ch::BLEND_SPRITE);
    v_ = vGlow;    drawBmsLayer(texBmsLaneGlow_.id, ch::BLEND_ADD);
    for (int i = 0; i < 3; ++i) {
        v_ = vBeam[i];    drawBmsLayer(texBmsBeam_[i].id, ch::BLEND_ADD);
    }
    v_ = vMeasure; drawBmsLayer(texBmsMeasure_.id, ch::BLEND_SPRITE);
    for (int i = 0; i < 3; ++i) {
        v_ = vLnBody[i];  drawBmsLayer(texBmsLnBody_[i].id, ch::BLEND_SPRITE);
    }
    for (int i = 0; i < 3; ++i) {
        v_ = vLnEnd[i];   drawBmsLayer(texBmsLnEnd_[i].id, ch::BLEND_SPRITE);
    }
    for (int i = 0; i < 3; ++i) {
        v_ = vLnStart[i]; drawBmsLayer(texBmsLnStart_[i].id, ch::BLEND_SPRITE);
    }
    for (int i = 0; i < 3; ++i) {
        v_ = vNote[i];    drawBmsLayer(texBmsNote_[i].id, ch::BLEND_SPRITE);
    }
    v_ = vJudge;   drawBmsLayer(texBmsJudge_.id, ch::BLEND_SPRITE);
    // The hardware sits in front of everything -- it is the frame, not part of
    // the field it borders.
    v_ = vLeft;    drawBmsLayer(texBmsLeftCol_.id, ch::BLEND_SPRITE);
    v_ = vTt;      drawBmsLayer(texBmsTurntable_.id, ch::BLEND_SPRITE);
    v_ = vKb;      drawBmsLayer(texBmsKeyboard_.id, ch::BLEND_SPRITE);
    // blend=2 on their DST rows: additive, over the keyboard.
    for (int i = 0; i < 2; ++i) {
        v_ = vKeyFlash[i]; drawBmsLayer(texBmsKeyFlash_[i].id, ch::BLEND_ADD);
    }
}

void Renderer::drawGh3Layer(GLuint tex, int blend, bool fade) {
    if (v_.empty()) return;
    if (gh3CamOn_) for (auto& t : v_) gh3CamVtx(t);
    applyFieldTint();
    glUseProgram(gh3Sprite_);
    glBlendFunc(blend == ch::BLEND_ADD ? GL_ONE : GL_ONE,
                blend == ch::BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform2f(glGetUniformLocation(gh3Sprite_, "uFade"),
                fade ? 305.0f : 0.0f, fade ? 335.0f : 0.0f);
    glUniform1i(glGetUniformLocation(gh3Sprite_, "uTex"), 0);
    glUniformMatrix4fv(glGetUniformLocation(gh3Sprite_, "uMVP"), 1, GL_FALSE,
                       gh3Mvp_.m);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                 v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
    v_.clear();
    glUseProgram(prog_);
}

// The whammy tail's two passes. Both draw the same strip; the glow is the
// wider, softer one and is additive (GH3 sets SRC_ALPHA/ONE, which the
// premultiplying shader turns into ONE/ONE).
void Renderer::drawGh3Whammy(GLuint tex, bool glow) {
    if (v_.empty()) return;
    if (gh3CamOn_) for (auto& t : v_) gh3CamVtx(t);
    applyFieldTint();
    glUseProgram(gh3Whammy_);
    glBlendFunc(GL_ONE, glow ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(gh3Whammy_, "uTex"), 0);
    glUniform1i(glGetUniformLocation(gh3Whammy_, "uGlow"), glow ? 1 : 0);
    // PS c1.xy, from the glow pass's re-upload: alpha scale and profile slope.
    glUniform2f(glGetUniformLocation(gh3Whammy_, "uEdge"),
                glow ? 0.85f : 1.0f, glow ? -0.3f : 0.0f);
    glUniformMatrix4fv(glGetUniformLocation(gh3Whammy_, "uMVP"), 1, GL_FALSE,
                       gh3Mvp_.m);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                 v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
    v_.clear();
    glUseProgram(prog_);
}

// GH3's highway. Neversoft drew the whole field as 2D screen elements in a
// 1280x720 vscreen (y down, highway centred on x = 640): gems, frets and
// whammy tails are sprites over a highway quad that never moves -- its scroll
// is a texture V velocity -- and perspective is not projected but tabled: a
// pow() recurrence turns two coefficients into 1153 row heights, and a gem's
// time-until-strikeline picks a row. Ground truth is devtools/gh3refs
// (guitar_tweaks.q for every constant, highway_2d.q generate_pos_table for the
// tables, guitar_highway.q setup_highway for element placement); the port spec
// with the .q quotes is devdocs/spec/gh3-highway.md. Single-player constant
// set throughout -- GH3's own 2-player highway is a different tuning, not a
// viewport squeeze.
void Renderer::drawGh3(const Chart& chart, double beat, const RenderOpts& o,
                       const Mods& mods, float songTime, float scrollNow,
                       float noteSpeed, float bpm, const Mat4& mvp) {
    (void)beat; (void)songTime;
    const float gh3T = mods.gh3 < 0.0f ? 0.0f : (mods.gh3 > 1.0f ? 1.0f : mods.gh3);
    if (gh3T <= 0.0f) return;
    gh3Mvp_ = mvp;

    // Camera mods (tilt/wag/mini) as a REAL camera about the strike line's
    // centre, applied per vertex in gh3CamVtx. Angles and zoom shaping match
    // piuFieldLocal so the knobs feel identical across fields; the signs
    // flip because gh3CamVtx works in a y-up view space where piuFieldLocal's
    // matrix was written y-down. Neutral mods leave gh3CamOn_ false and the
    // map is skipped entirely -- the 2D render is bit-identical.
    gh3CamOn_ = mods.tilt != 0.0f || mods.wag != 0.0f || mods.mini != 0.0f;
    if (gh3CamOn_) {
        const float T = 0.67451f;
        const float tilt = 30.0f * mods.tilt * 3.14159265f / 180.0f;
        const float wag = -mods.wag * 21.0f * sinf(float(beat) * 3.14159265f) *
                          3.14159265f / 180.0f;
        float zoom = 1.0f - 0.5f * mods.mini;
        if (mods.tilt > 0.0f)      zoom *= 1.0f - 0.1f * mods.tilt;
        else if (mods.tilt < 0.0f) zoom *= 1.0f + 0.1f * mods.tilt;
        const float cx = cosf(tilt), sx = sinf(tilt);
        const float cz = cosf(wag),  sz = sinf(wag);
        // Rz(wag) * Rx(tilt), rows of the row-major 3x3, all scaled by zoom.
        const float M[9] = {
            zoom *  cz, zoom * -sz * cx, zoom *  sz * sx,
            zoom *  sz, zoom *  cz * cx, zoom * -cz * sx,
            0.0f,       zoom *  sx,      zoom *  cx };
        // Conjugate about the strike line's view-space point (0, yc, -1).
        const float pc[3] = { 0.0f, (360.0f - 655.0f) / 360.0f * T, -1.0f };
        for (int i = 0; i < 9; ++i) gh3Cam_[i] = M[i];
        for (int i = 0; i < 3; ++i)
            gh3Cam_[9 + i] = pc[i] - (M[i*3+0]*pc[0] + M[i*3+1]*pc[1] +
                                      M[i*3+2]*pc[2]);
    }

    // guitar_tweaks.q, *1 suffix. Derived values follow generate_pos_table:
    // the tweak-file gem_end_scale is dead there, and so is fretbar's -- both
    // are recomputed as start * (1 + widthOffsetFactor).
    const float PLAYLINE = 655.0f, HEIGHT = 350.0f, TOPW = 160.0f;
    const float WOFF = 2.2f;
    const float STRING_SX = 0.65000004f, STRING_SY = 0.8f;
    const float SIDEBAR_XOFF = 4.0f, SIDEBAR_SX = 0.3f, SIDEBAR_SY = 1.0f;
    const float START_Y = PLAYLINE - HEIGHT;                    // 305
    const float BOTW = TOPW * (1.0f + WOFF);                    // 512
    const float FRETBAR_S0 = 0.15f;
    const float FRETBAR_S1 = FRETBAR_S0 * (1.0f + WOFF);        // 0.48

    // The row table, generate_pos_table verbatim (spec section 4): rnd[0] = 1,
    // rnd[i] = pow(rnd[i-1] * fact, exp), normalised so the first 1024 sum to
    // 1, then rowY[i] = startY + HEIGHT * sum(rnd[0..i-1]). Row 1024 is the
    // strikeline exactly; 1025..1152 are the overshoot below it. float, not
    // double: the recurrence compounds 1151 times and GH3 ran it in single
    // precision. Constants are fixed, so built once.
    static float rowY[1153];
    static bool rowsBuilt = false;
    if (!rowsBuilt) {
        rowsBuilt = true;
        const int idx = int(HEIGHT) - 162;      // clamp(350-162, 0, 510) = 188
        const float fact = gh3::HEIGHT_PERSP_FACT[idx];
        const float expo = gh3::HEIGHT_PERSP_EXP[idx];
        static float rnd[1152];
        rnd[0] = 1.0f;
        for (int i = 1; i < 1152; ++i) rnd[i] = powf(rnd[i-1] * fact, expo);
        float sum = 0.0f;
        for (int i = 0; i < 1024; ++i) sum += rnd[i];
        const float norm = 1.0f / sum;
        for (int i = 0; i < 1152; ++i) rnd[i] *= norm;
        rowY[0] = START_Y;
        for (int i = 1; i <= 1152; ++i) rowY[i] = rowY[i-1] + HEIGHT * rnd[i-1];
    }

    // Time axis. GH3's native pacing (expert scroll_time 2.5s minus
    // destroy_time 1.0s = 1.5s top-to-strikeline) is deliberately NOT the
    // mapping -- the same call Moonscraper's 1.7 factor makes: the field
    // shows CH's visible window, NOTE_CULL_FAR / noteSpeed (0.787s at
    // default speed), so the chart pours at the same perceived rate as the
    // CH field at any speed, and hyperspeed translates exactly (noteSpeed
    // already carries the scrollspeed knob upstream). GH3's own 1.5s pace
    // is available as --speed 5.25.
    const float window = ch::NOTE_CULL_FAR / fmaxf(noteSpeed, 0.01f);
    const bool hasStops = !chart.stops.empty();
    auto ssec = [&](double t) { return hasStops ? chart.scrollSec(t) : t; };
    auto yAtRow = [&](float rowF) {
        rowF = fminf(1152.0f, fmaxf(0.0f, rowF));
        const int r = int(rowF) >= 1152 ? 1151 : int(rowF);
        return rowY[r] + (rowY[r+1] - rowY[r]) * (rowF - float(r));
    };
    // Lane geometry, spec section 4 step 4: straight edges, x linear in the
    // geometric fraction g = (y - startY) / HEIGHT (g runs past 1 below the
    // strikeline and the lerp extrapolates, which is exactly what straight
    // edges mean). Fade plane: alpha 0 at the highway top (y = 305), 1 at
    // 305 + highway_fade.
    auto geoAt  = [&](float y) { return (y - START_Y) / HEIGHT; };
    auto edgeL  = [&](float g) { return (640.0f - TOPW*0.5f) +
                                        ((640.0f - BOTW*0.5f) - (640.0f - TOPW*0.5f)) * g; };
    auto edgeR  = [&](float g) { return (640.0f + TOPW*0.5f) +
                                        ((640.0f + BOTW*0.5f) - (640.0f + TOPW*0.5f)) * g; };
    // The highway fade lives in the fragment shaders (GH3_SPRITE_FS /
    // GH3_WHAMMY_FS), per pixel, exactly like the game's own -- putting it in
    // vertex alpha bled the 30px band down the full length of tall sprites
    // like the sidebars and strings.
    auto push = [&](std::vector<ch::Vtx>& out, float x, float y, float u, float v,
                    float r, float g, float b, float a) {
        ch::Vtx t; t.x = x; t.y = y; t.z = 0.0f; t.u = u; t.v = v;
        t.r = r; t.g = g; t.b = b; t.a = a;
        out.push_back(t);
    };

    const bool board = !o.playfield && mods.hideboard == 0.0f;

    if (board) {
        // ---- highway quad: flat black over the venue/background (GH3's
        // fretboard art is venue geometry; black is the chosen stand-in, so
        // the m_velocityV texture scroll has nothing to show). Straight edges
        // + flat fill would be one trapezoid, but the fade band at the top
        // wants vertex alpha along the rows, so it stays a short strip.
        for (int row = 0; row < 1152; row += 16) {
            const int r1 = row + 16;
            const float y0 = rowY[row],          y1 = rowY[r1];
            const float g0 = geoAt(y0),          g1 = geoAt(y1);
            const float a0 = gh3T,               a1 = gh3T;
            push(v_, edgeL(g0), y0, 0.0f, 0.0f, 0,0,0, a0);
            push(v_, edgeR(g0), y0, 1.0f, 0.0f, 0,0,0, a0);
            push(v_, edgeR(g1), y1, 1.0f, 1.0f, 0,0,0, a1);
            push(v_, edgeL(g0), y0, 0.0f, 0.0f, 0,0,0, a0);
            push(v_, edgeR(g1), y1, 1.0f, 1.0f, 0,0,0, a1);
            push(v_, edgeL(g1), y1, 0.0f, 1.0f, 0,0,0, a1);
        }
        drawGh3Layer(texWhite_.id, ch::BLEND_SPRITE, true);

        // ---- fretbars. chart.beatlines already carries GH3's classes
        // (0 MEASURE -> thick, 1 -> medium, 2 WEAK -> thin), and GH3 adds thin
        // bars on the 8th-note midpoints when the BPM allows
        // (thin_fretbar_8note_params: up to 180). 1024x16 texture scaled
        // fretbar_start_scale -> 0.48 along the highway.
        std::vector<ch::Vtx> vBar[3];
        auto fretbar = [&](double sec, int weight) {
            // No fret_offset_tweak here: the checksum is unreferenced in the
            // binary, and gems and fretbars share ONE lead time
            // (guitar_gems.q:346,349). Shifting fretbars alone slides them
            // ~10 px against the gems near the strike line.
            const float dt = float(ssec(sec) - scrollNow);
            const float rowF = (1.0f - dt / window) * 1024.0f;
            if (rowF < 0.0f || rowF > 1152.0f) return;
            const float y = yAtRow(rowF);
            const float g = geoAt(y);
            const float s = FRETBAR_S0 + (FRETBAR_S1 - FRETBAR_S0) * g;
            const float hw = 1024.0f * s * 0.5f, hh = 16.0f * s * 0.5f;
            const float a = gh3T;
            push(vBar[weight], 640.0f - hw, y - hh, 0,0, 1,1,1, a);
            push(vBar[weight], 640.0f + hw, y - hh, 1,0, 1,1,1, a);
            push(vBar[weight], 640.0f + hw, y + hh, 1,1, 1,1,1, a);
            push(vBar[weight], 640.0f - hw, y - hh, 0,0, 1,1,1, a);
            push(vBar[weight], 640.0f + hw, y + hh, 1,1, 1,1,1, a);
            push(vBar[weight], 640.0f - hw, y + hh, 0,1, 1,1,1, a);
        };
        const auto& bl = chart.beatlines;
        for (size_t i = 0; i < bl.size(); ++i) {
            fretbar(bl[i].sec, bl[i].style == 0 ? 2 : (bl[i].style == 1 ? 1 : 0));
            if (i + 1 < bl.size() && bpm <= 180.0f)
                fretbar((bl[i].sec + bl[i+1].sec) * 0.5, 0);
        }
        for (int w = 0; w < 3; ++w) {
            v_ = vBar[w];
            drawGh3Layer(texGh3Fretbar_[w].id, ch::BLEND_SPRITE, true);
        }

        // ---- lane strings and sidebars: rotated sprites anchored
        // centre-bottom (setup_highway), lying along the straight lanes/edges.
        // Their pixel size is constant -- only the anchor and angle carry the
        // perspective -- and the top ends die in the fade band.
        auto rotSprite = [&](float ax, float ay, float dx, float dy,
                             float len, float hw, bool mirrorU,
                             float cr, float cg, float cb, float ca) {
            const float dl = sqrtf(dx*dx + dy*dy);
            const float nx = dx / dl, ny = dy / dl;
            const float px = -ny, py = nx;
            const float tx = ax + nx * len, ty = ay + ny * len;
            const float u0 = mirrorU ? 1.0f : 0.0f, u1 = mirrorU ? 0.0f : 1.0f;
            const float aB = ca, aT = ca;
            push(v_, ax - px*hw, ay - py*hw, u0, 1.0f, cr, cg, cb, aB);
            push(v_, ax + px*hw, ay + py*hw, u1, 1.0f, cr, cg, cb, aB);
            push(v_, tx + px*hw, ty + py*hw, u1, 0.0f, cr, cg, cb, aT);
            push(v_, ax - px*hw, ay - py*hw, u0, 1.0f, cr, cg, cb, aB);
            push(v_, tx + px*hw, ty + py*hw, u1, 0.0f, cr, cg, cb, aT);
            push(v_, tx - px*hw, ty - py*hw, u0, 0.0f, cr, cg, cb, aT);
        };
        for (int lane = 0; lane < 5; ++lane) {
            const float gts = TOPW / 5.0f, gbs = BOTW / 5.0f;
            const float sx = (640.0f - TOPW*0.5f) + gts*0.5f + gts*float(lane);
            const float ex = (640.0f - BOTW*0.5f) + gbs*0.5f + gbs*float(lane);
            const float c = 200.0f / 255.0f;
            rotSprite(ex, PLAYLINE, sx - ex, -HEIGHT, 512.0f * STRING_SY,
                      32.0f * STRING_SX * 0.5f, false, c, c, c, c * gh3T);
        }
        drawGh3Layer(texGh3String_.id, ch::BLEND_SPRITE);

        // Sidebar anchor: 25% past the bottom edge along the left-edge
        // direction, pulled left by sidebar_x_offset; the right side mirrors
        // about x = 640 with a negated x scale (spec section 4 step 7). The
        // sprite is then rotated by sidebar_angle = Atan2(x = highway_height,
        // y = stx - sbx), so its direction runs over the highway's HEIGHT --
        // not over the extended anchor drop, which leans it ~5 deg too
        // shallow and lifts the bars off the highway edge.
        const float stx = 640.0f - TOPW*0.5f, sbx = 640.0f - BOTW*0.5f;
        const float sbAx = (sbx + (sbx - stx)*0.25f) - SIDEBAR_XOFF;
        const float sbAy = PLAYLINE + HEIGHT*0.25f;
        // The sidebar pulse. GuitarEvent_Fretbar calls set_sidebar_flash and
        // then flips a global `beat_flip`, so the sidebars alternate between
        // the two endpoints of a colour PAIR on every fretbar -- and it is a
        // hard switch (SetScreenElementProps), not a tween. Which pair is
        // chosen depends on state: starpower / dying (crowd below
        // crowd_poor_medium * highway_flash_dying) / starready
        // (star_power >= 50) / normal. Offline there is no meter, no crowd
        // and no SP, so it is always `normal`: [255 255 255] on one beat and
        // [192 255 255] on the next. beat_flip starts at 0 and is flipped
        // AFTER the colour is applied, so the first fretbar shows endpoint 0.
        int barsFired = 0;
        for (const auto& b : chart.beatlines) {
            if (b.sec > songTime) break;
            ++barsFired;
        }
        const float sbR = (barsFired > 0 && (barsFired & 1) == 0)
                              ? 192.0f / 255.0f : 1.0f;
        rotSprite(sbAx, sbAy, stx - sbx, -HEIGHT,
                  512.0f * SIDEBAR_SY, 32.0f * SIDEBAR_SX * 0.5f, false,
                  sbR, 1, 1, gh3T);
        rotSprite((640.0f - sbAx) + 640.0f, sbAy, -(stx - sbx), -HEIGHT,
                  512.0f * SIDEBAR_SY, 32.0f * SIDEBAR_SX * 0.5f, true,
                  sbR, 1, 1, gh3T);
        drawGh3Layer(texGh3Sidebar_.id, ch::BLEND_SPRITE);
    }

    // ---- fret state, hoisted above the tails: GH3's z-order interleaves
    // them (dead tails 3.1, nowbar 3.6-3.9, live tails 4.0, gems 4.1,
    // pressed buttons raised to 4.6-4.9 by guitar_net.q's press anim).
    // setup_highway composites each button from mid/neck/head/lip, scale
    // 0.8, bottoms on the playline. Press mechanics are guitar_net.q's
    // mirror of the native logic: a hit pops the HEAD up button_up_pixels
    // and it sinks back linearly over button_sink_time, the shared neck
    // stretches (pixels + neck_lip_add) / neck_sprite_size with its bottom
    // pinned neck_lip_base above the anchor, and a held button swaps the
    // head to the _down art (guitar_net.q:911). The _lit (L) heads have
    // zero native references (gh3-whammy-idb.md section 3) -- loaded,
    // unwired.
    const float NOW_S = 0.8f;
    const float FRET_Y = PLAYLINE;
    bool laneSus[5] = {false, false, false, false, false};
    if (!o.noBot)
        for (const auto& n : chart.notes)
            for (int lane = 0; lane < 5; ++lane)
                if ((n.frets & (1 << lane)) && n.sustain[lane] > 0.0 &&
                    songTime >= chart.beatToSec(n.beat) &&
                    songTime <  chart.beatToSec(n.beat + n.sustain[lane]))
                    laneSus[lane] = true;
    float exL[5], pxL[5], fzL[5];
    bool heldL[5], fretRaised[5];
    for (int lane = 0; lane < 5; ++lane) {
        const float gbsF = BOTW / 5.0f;
        exL[lane] = (640.0f - BOTW*0.5f) + gbsF*0.5f + gbsF*float(lane);
        // The receptor rule, identical to the CH fret stack: an arrow at
        // fYOffset = 0, displaced by GetXPos (scaled into the strike line's
        // 1.6x lane pitch), pulled together by tiny about the highway
        // centre, and zoomed by 0.5^tiny per column.
        exL[lane] += GetXPos(mods, lane, 0.0f, songTime, float(beat), bpm) * 1.6f;
        if (mods.tiny != 0.0f)
            exL[lane] = 640.0f + (exL[lane] - 640.0f) * GetTinyColScale(mods);
        fzL[lane] = GetZoom(mods, lane);
        pxL[lane] = 0.0f;
        heldL[lane] = false;
        if (!o.noBot) {
            // The raise is NOTE-driven, not input-driven:
            // GuitarEvent_HitNotes_CFunc slams button_up_pixels into the lane
            // for every gem in the hit pattern, and ButtonCheckerPerFrame
            // decays it by (button_up_pixels * dt) / button_sink_time. A
            // sustain is the exception -- CheckNoteHoldPerFrame re-slams the
            // full 20 every frame it runs, so a held lane never sinks.
            // An open lights and raises ALL FIVE buttons here -- GH3 has no
            // separate open hit effect, so the whole nowbar simply reacts.
            // (CH differs: there the light is gated on real button state, and
            // an open is played holding nothing, so it pops unlit.)
            const float dtLane = float(lastHit(lane, songTime));
            const float dt = fminf(dtLane, float(lastOpenHit(songTime)));
            if (laneSus[lane]) pxL[lane] = 20.0f;
            else if (dt >= 0.0f && dt < 0.1f)
                pxL[lane] = 20.0f * (1.0f - dt / 0.1f);
            // Lit for the whole raise, opens included.
            heldL[lane] = laneSus[lane] || pxL[lane] > 0.0f;
        }
        // The whole button jumps z 3.6-3.9 -> 4.6-4.9 while raised, which is
        // what puts a popped fret in front of the gems.
        fretRaised[lane] = pxL[lane] > 0.0f;
    }
    auto fretSprite = [&](GLuint tex, float cx, float yBot, float w, float h) {
        const float hw = w * 0.5f;
        push(v_, cx-hw, yBot-h, 0,0, 1,1,1, gh3T);
        push(v_, cx+hw, yBot-h, 1,0, 1,1,1, gh3T);
        push(v_, cx+hw, yBot,   1,1, 1,1,1, gh3T);
        push(v_, cx-hw, yBot-h, 0,0, 1,1,1, gh3T);
        push(v_, cx+hw, yBot,   1,1, 1,1,1, gh3T);
        push(v_, cx-hw, yBot,   0,1, 1,1,1, gh3T);
        drawGh3Layer(tex, ch::BLEND_SPRITE);
    };
    auto drawFretLane = [&](int l) {
        // fzL zooms the whole button about its playline anchor.
        const float z = fzL[l];
        const float NOW_S = 0.8f * z;
        auto at = [&](float yBot) { return FRET_Y + (yBot - FRET_Y) * z; };
        fretSprite(texGh3NowbarMid_[l].id, exL[l], at(FRET_Y),
                   128.0f*NOW_S, 64.0f*NOW_S);
        fretSprite(texGh3NowbarNeck_.id, exL[l], at(FRET_Y - 5.0f),
                   64.0f*NOW_S, (pxL[l] + 16.0f) * z);
        // ButtonCheckerPerFrame picks the head art three ways, and only the
        // head element is ever re-materialled (lip/mid/neck keep whatever
        // setup_highway gave them): not fretted -> material_head; fretted and
        // raised -> material_head_LIT (the *L art -- this is what a popping
        // fret shows); fretted but sunk -> material_down. That last state
        // needs a fret held without a note under it, which an autoplay bot
        // never does, so `_down` stays unused here rather than being wrong.
        fretSprite((!heldL[l]        ? texGh3NowbarHead_[l]
                    : pxL[l] > 0.0f ? texGh3NowbarHeadLit_[l]
                                    : texGh3NowbarDown_[l]).id,
                   exL[l], at(FRET_Y - pxL[l]), 128.0f*NOW_S, 64.0f*NOW_S);
        fretSprite(texGh3NowbarLip_[l].id, exL[l], at(FRET_Y),
                   128.0f*NOW_S, 64.0f*NOW_S);
    };

    // ---- whammy tails. GH3 calls the sustain trail the whammy: a triangle
    // strip along the lane, width whammy_top_width widening with the
    // perspective toward the player, mapped with ONE 32x32 sys_whammy2d
    // tile. U runs ACROSS the ribbon (0 = left edge, 1 = right), so the
    // tile's bottom row supplies the tube's cross-section shading; V is 1
    // down the body and ramps only over the rounded far tip. No tiling and
    // no scroll -- whammy_units_per_second is a star-power earn rate, not a
    // render rate. The wibble is a whammy-INPUT history (128 floats, all 1.0
    // at rest) that modulates the half-width symmetrically; NotClon's bot
    // never whammies, so the straight constant-width ribbon is exact rather
    // than a simplification. Ground truth: devdocs/spec/gh3-shaders.md
    // (disassembled from the game's own MaterialLibrary) and
    // gh3-whammy-idb.md. Dead (grey) art replaces the colour when a tail's
    // head has gone past unhit, which only happens under --nobot here -- the
    // bot never drops a sustain.
    {
        const float W0 = 10.0f, WOFFW = WOFF;
        const float gts = TOPW / 5.0f, gbs = BOTW / 5.0f;
        auto laneX = [&](int lane, float g) {
            const float sx = (640.0f - TOPW*0.5f) + gts*0.5f + gts*float(lane);
            const float ex = (640.0f - BOTW*0.5f) + gbs*0.5f + gbs*float(lane);
            return sx + (ex - sx) * g;
        };
        auto rowFAt = [&](float dt) { return (1.0f - dt / window) * 1024.0f; };
        // The ArrowEffects pipeline, CH's laneState in GH3 clothes. Mods work
        // in SM pixels against a 64px lane pitch, so the scroll axis maps
        // through Y_PER_UNIT exactly as the CH field does and lateral pixels
        // scale by the highway's own lane pitch at that depth (32px at the
        // top -> 102.4 at the strike line, i.e. x0.5 -> x1.6).
        const float Y_PER_UNIT = ARROW_SIZE * 1.6f;
        const float tinyColG = GetTinyColScale(mods);
        auto pitchAt = [&](float g2) {
            return ((TOPW / 5.0f) + ((BOTW - TOPW) / 5.0f) * g2) / 64.0f;
        };
        // Scroll-axis mods (beat/bumpy/wave/boost/...): SM yOffset in, modded
        // row out.
        auto modDepth = [&](int ml, float dtIn, float& rowFOut, float& yOffOut) {
            const float yIn = dtIn * noteSpeed * Y_PER_UNIT;
            yOffOut = ApplyYMods(mods, ml, yIn, float(beat));
            float z = (yOffOut == yIn) ? dtIn * noteSpeed : yOffOut / Y_PER_UNIT;
            z = ApplyScrollZ(mods, z, ml);
            rowFOut = rowFAt(z / noteSpeed);
        };
        // Lateral mods (tornado/drunk/flip/movex/...) plus tiny's column
        // pull-together, contracted about the highway centre.
        auto modX = [&](float baseX, int ml, float g2, float yOff) {
            float cx = baseX +
                GetXPos(mods, ml, yOff, songTime, float(beat), bpm) * pitchAt(g2);
            if (mods.tiny != 0.0f) cx = 640.0f + (cx - 640.0f) * tinyColG;
            return cx;
        };
        std::vector<ch::Vtx> vWham[5], vWhamGlow[5], vWhamDead;
        std::vector<ch::Vtx> vOpenS, vOpenSGlow, vOpenSDead;
        auto tailStrip = [&](int lane /* -1 = open */, double tHitS, double tEndS,
                             bool held, std::vector<ch::Vtx>& out,
                             std::vector<ch::Vtx>* glowOut) {
            // Near end: exactly the gem's row -- the gem (z 4.1) covers the
            // blunt end, whose bottom lands inside the gem's 8.5% overhang
            // below its anchor. No whammy_offset_tweak: shifting the mover
            // -33ms pokes the tail ~20px out below the gem, the real game
            // shows no such peek, and the claim came from the stack-drift-
            // compromised decompile whose sibling constant fret_offset_tweak
            // proved to be unreferenced in the binary. While held the near
            // end pins just above the strikeline at row
            // 1024*whammy_cutoff/1152 ("burns down from the top only").
            // Far end: the shortened sustain end.
            float rowA = rowFAt(float(ssec(tEndS) - scrollNow));
            float rowB = held ? 1024.0f * 1100.0f / 1152.0f
                              : rowFAt(float(ssec(tHitS) - scrollNow));
            if (rowB < 0.0f || rowA > 1152.0f || rowA >= rowB) return;
            rowA = fmaxf(rowA, 0.0f);
            rowB = fminf(rowB, 1152.0f);
            // One strip row per 8 highway lines, exactly as sub_601D30 walks
            // its tables (rows = highway_lines >> 3).
            const int segs = int((rowB - rowA) / 8.0f) + 1;
            const int ml = lane < 0 ? 2 : lane;
            struct Row { float y, cx, hw, v, a; };
            std::vector<Row> rows;
            rows.reserve(size_t(segs) + 1);
            for (int sgi = 0; sgi <= segs; ++sgi) {
                const float rowN = rowA + (rowB - rowA) * float(sgi) / float(segs);
                // Mods bend the strip per row, exactly like CH's ribbons:
                // each sample's nominal scroll time runs through the same
                // Y-wave/scroll/X pipeline as a gem at that depth.
                const float dtN = (1.0f - rowN / 1024.0f) * window;
                float rowM, yOffR;
                modDepth(ml, dtN, rowM, yOffR);
                rowM = fminf(1152.0f, fmaxf(0.0f, rowM));
                Row r;
                r.y  = yAtRow(rowM);
                const float g = geoAt(r.y);
                // halfWidth = 0.5 * whammy_top_width * (1 + accum *
                // whammy_width_offset), and `accum` -- the running sum of the
                // per-row normalised distances -- IS the geometric fraction.
                // Open tails multiply the base width by 11.8: GH3+ patches
                // the strip renderer itself (WhammyShader_Resize at 0x603552
                // in gemMutation.cpp) to `mulss` that factor into the width
                // argument whenever the material is the open whammy texture.
                r.hw = (W0 + W0 * WOFFW * g) * 0.5f * (lane < 0 ? 11.8f : 1.0f);
                r.cx = modX(lane < 0 ? 640.0f : laneX(lane, g), ml, g, yOffR);
                // V is 1 down the whole body; only the rounded tip ramps.
                r.v  = fminf(1.0f, (rowN - rowA) / 32.0f);
                r.a  = gh3T;
                rows.push_back(r);
            }
            auto strip = [&](std::vector<ch::Vtx>& dst, float widthMul,
                             const std::vector<Row>& src) {
                for (size_t i = 1; i < src.size(); ++i) {
                    const Row& p = src[i-1];
                    const Row& q = src[i];
                    const float pl = p.cx - p.hw*widthMul, pr = p.cx + p.hw*widthMul;
                    const float ql = q.cx - q.hw*widthMul, qr = q.cx + q.hw*widthMul;
                    push(dst, pl, p.y, 0, p.v, 1,1,1, p.a);
                    push(dst, pr, p.y, 1, p.v, 1,1,1, p.a);
                    push(dst, qr, q.y, 1, q.v, 1,1,1, q.a);
                    push(dst, pl, p.y, 0, p.v, 1,1,1, p.a);
                    push(dst, qr, q.y, 1, q.v, 1,1,1, q.a);
                    push(dst, ql, q.y, 0, q.v, 1,1,1, q.a);
                }
            };
            strip(out, 1.0f, rows);
            // The glow is the SAME strip 3.5x wider -- VS 685 is byte-identical
            // to 685's solid twin plus one `mul r0.x, r0.x, c1.z` with
            // c1.z = 3.5, which scales the half-width. It is NOT a stretch
            // along the tail. The only length change is at the far tip, where
            // the 6 rows nearest the end are pushed out x1.4 from row 6's y
            // before the second draw.
            if (glowOut && held && rows.size() > 6) {
                std::vector<Row> g = rows;
                const float tipY = g[6].y;
                for (int i = 0; i < 6; ++i)
                    g[i].y = tipY + (g[i].y - tipY) * 1.4f;
                // Opens do NOT take the 3.5x: their body is already 11.8x
                // wide, and 3.5*11.8 puts the halo off both sides of the
                // screen. GH3+ inherits exactly that -- gemMutation.cpp
                // widens the body without touching the glow and leaves
                // "TODO: scale down glowing sprite" against it -- so this
                // follows the mod's intent rather than its known artifact.
                strip(*glowOut, lane < 0 ? 1.0f : 3.5f, g);
            }
        };
        // Tail length is CH's, by user choice: the full charted sustain,
        // ending at the exact point the CH ribbons end. GH3's authentic
        // whammy_shorten (every tail 0.25 beats short, nothing at or under
        // half a beat -- the famously stubby Neversoft tails) is
        // deliberately NOT applied; see AGENTS.md if it is ever wanted back.
        for (const auto& n : chart.notes) {
            const double tHit = chart.beatToSec(n.beat);
            for (int lane = 0; lane < 5; ++lane) {
                if (!(n.frets & (1 << lane)) || n.sustain[lane] <= 0.0) continue;
                const double tEnd =
                    chart.beatToSec(n.beat + n.sustain[lane]);
                if (songTime >= tEnd) continue;
                const bool held = !o.noBot && songTime >= tHit;
                const bool dead = o.noBot &&
                    float(ssec(tHit) - scrollNow) < 0.0f;
                tailStrip(lane, tHit, tEnd, held,
                          dead ? vWhamDead : vWham[lane],
                          dead ? nullptr : &vWhamGlow[lane]);
            }
            if (n.open && n.openSustain > 0.0) {
                const double tEnd =
                    chart.beatToSec(n.beat + n.openSustain);
                if (songTime < tEnd) {
                    const bool held = !o.noBot && songTime >= tHit;
                    const bool dead = o.noBot &&
                        float(ssec(tHit) - scrollNow) < 0.0f;
                    tailStrip(-1, tHit, tEnd, held,
                              dead ? vOpenSDead : vOpenS,
                              dead ? nullptr : &vOpenSGlow);
                }
            }
        }
        // GH3 z-order: dead tails (3.1) under the nowbar, live tails (4.0)
        // over it, glow as the tails' additive second pass, gems (4.1) above,
        // pressed buttons last (4.6-4.9, drawn after the gems below).
        v_ = vWhamDead;  drawGh3Whammy(texGh3WhammyDead_.id,  false);
        v_ = vOpenSDead; drawGh3Whammy(texGh3OpenSusDead_.id, false);
        for (int l = 0; l < 5; ++l)
            if (!fretRaised[l]) drawFretLane(l);
        for (int l = 0; l < 5; ++l) {
            v_ = vWham[l];
            drawGh3Whammy(texGh3Whammy_[l].id, false);
        }
        v_ = vOpenS;     drawGh3Whammy(texGh3OpenSus_.id, false);
        for (int l = 0; l < 5; ++l) {
            v_ = vWhamGlow[l];
            drawGh3Whammy(texGh3Whammy_[l].id, true);
        }
        v_ = vOpenSGlow; drawGh3Whammy(texGh3OpenSus_.id, true);

        // ---- gems. EVERY FRAME of note art is 128x64: static gem and hammer
        // art, star gems as a 4x4 flipbook, and the GH3+ taps as a 2x4
        // flipbook in the sys_BattleGEM_* slots. Frame layout and rate are
        // not guesses -- GH3's material graph carries a (cols, rows, fps)
        // triple per material (devtools/gh3refs/textures/scn/*.json), and
        // both animated sets run at 30 fps. Opens are 512x64 art, with the
        // SP-phrase opens a 4x4 sheet of that. Scale runs gem_start_scale ->
        // *(1 + widthOffsetFactor); 5 * 128 * 0.25 = 160 = highway_top_width
        // and 5 * 128 * 0.8 = 512 = the bottom width, so a gem exactly fills
        // its lane at both ends. Pivot is gem_y_just (0.83) except stars at
        // star_y_just (0.5), drawn at gem_star_scale 1.3. The bot consumes a
        // gem at its hit time; under --nobot they ride the overshoot rows out
        // the bottom like GH3 misses do.
        const float GEM_S0 = 0.25f, GEM_S1 = 0.25f * (1.0f + WOFF);
        // Every gem shares z 4.1, so their order is decided purely by
        // insertion: GH3 links a tie group newest-first, so the gem created
        // FIRST ends up on top. Gems stream in chart order, which makes the
        // NEARER gem win (free correct occlusion), and within one chord green
        // beats red beats ... beats open. A painter's renderer reproduces
        // that by drawing far->near and, inside an entry, open first then
        // orange..green last. Batching by texture would destroy it -- a far
        // star would paint over a near gem, which happens at every starpower
        // boundary -- so quads go into ONE ordered stream and the draw only
        // splits where the texture actually changes.
        std::vector<ch::Vtx> gemStream;
        std::vector<std::pair<GLuint, size_t>> gemRuns;
        for (int i = int(chart.notes.size()) - 1; i >= 0; --i) {
            const Note& n = chart.notes[i];
            const double tHit = chart.beatToSec(n.beat);
            if (!o.noBot && songTime >= tHit) continue;      // consumed
            const float dt = float(ssec(tHit) - scrollNow);
            // Flipbooks run on a PER-SPRITE clock: the AnimatedTexture_UI
            // shader stamps m_creationTime when the element binds, so every
            // animated gem starts at frame 0 as it spawns at the highway top
            // instead of sharing one global phase.
            const float elapsed = window - dt;
            const int spinF = ((int(elapsed * 30.0f) % 16) + 16) % 16;
            const int tapF  = ((int(elapsed * 30.0f) % 8) + 8) % 8;
            const bool isSP =
                chart.phraseAt(PhraseType::StarPower, n.tick) != nullptr;
            const float rot = GetRotationZ(mods, float(n.beat), float(beat));
            // Per-lane state through the mod pipeline, like CH's laneState:
            // culls per lane (a Y-wave can carry one lane of a chord off the
            // highway while another stays), returns the modded row and SM
            // yOffset for everything downstream.
            struct LaneEval { float y, g, s, cx, a, zoom; };
            auto laneEval = [&](int ml, LaneEval& e) {
                float rowM, yOff;
                modDepth(ml, dt, rowM, yOff);
                if (rowM < 0.0f || rowM > 1152.0f) return false;
                e.y = yAtRow(rowM);
                e.g = geoAt(e.y);
                e.s = GEM_S0 + (GEM_S1 - GEM_S0) * e.g;
                e.cx = modX(laneX(ml, e.g), ml, e.g, yOff);
                e.y += GetYPosBump(mods, ml, yOff, float(beat), bpm) *
                       pitchAt(e.g) * 0.5f;
                e.a = gh3T * GetAlpha(mods, yOff, songTime);
                e.zoom = GetZoom(mods, ml);
                return e.a > 0.002f;
            };
            // Seating: GH3's authentic gem_y_just (0.83) hangs 91.5% of the
            // gem above its position; CH seats notes at NOTE_PIVOT_Y = 0.16
            // from the sprite bottom (84% above), so the gem's base straddles
            // its chart position. CH's seating was chosen deliberately: the
            // authentic 0.83/0.5 become 0.68/0.35 in the same (1+j)/2
            // encoding -- identical split, 7.5% of the sprite lower.
            auto emit = [&](GLuint tex, const LaneEval& e, float cx, float w,
                            float h, float yJust, float u0, float v0,
                            float u1, float v1) {
                const float above = (yJust + 1.0f) * 0.5f * h;
                const float th = rot * 3.14159265f / 180.0f;
                const float c = cosf(th), sn = sinf(th);
                auto P = [&](float lx, float ly, float u, float v) {
                    lx *= e.zoom; ly *= e.zoom;
                    push(gemStream, cx + lx*c - ly*sn, e.y + lx*sn + ly*c,
                         u, v, 1,1,1, e.a);
                };
                const float hw = w * 0.5f;
                P(-hw, -above, u0, v0); P(hw, -above, u1, v0);
                P(hw, h-above, u1, v1); P(-hw, -above, u0, v0);
                P(hw, h-above, u1, v1); P(-hw, h-above, u0, v1);
                if (!gemRuns.empty() && gemRuns.back().first == tex)
                    gemRuns.back().second += 6;
                else
                    gemRuns.push_back({tex, size_t(6)});
            };
            if (n.open) {
                // An open is GH3+'s sixth lane, riding the yellow lane's
                // geometry (and lane 2's mod evaluation, like CH's open path
                // does). Its art is already 4x a lane gem's width, and it is
                // ALWAYS drawn at gem_star_scale (1.3) on top of that -- 5.2x
                // wide: a plain open via GH3+'s setGemScale detour
                // (gemLoading.cpp:16-34), an SP open via retail's star path,
                // which also swaps in star_y_just.
                LaneEval e;
                if (laneEval(2, e)) {
                    const bool hopo = n.openType == NoteType::Hopo;
                    const float ow = 512.0f*e.s*1.3f, oh = 64.0f*e.s*1.3f;
                    const float cx = e.cx - laneX(2, e.g) + 640.0f;
                    if (isSP) {
                        const float u0 = float(spinF % 4) * 0.25f;
                        const float v0 = float(spinF / 4) * 0.25f;
                        emit((hopo ? texGh3OpenHopoSp_ : texGh3OpenSp_).id, e,
                             cx, ow, oh, 0.35f, u0, v0, u0 + 0.25f, v0 + 0.25f);
                    } else {
                        emit((hopo ? texGh3OpenHopo_ : texGh3Open_).id, e,
                             cx, ow, oh, 0.68f, 0, 0, 1, 1);
                    }
                }
            }
            for (int lane = 4; lane >= 0; --lane) {
                if (!(n.frets & (1 << lane))) continue;
                LaneEval e;
                if (!laneEval(lane, e)) continue;
                if (n.type == NoteType::Tap) {
                    // Plain taps take the normal path (unscaled, gem seat),
                    // but GH3+ routes SP taps through retail's STAR path
                    // (makeStarTapGems, gemLoading.cpp:73-79), so those get
                    // gem_star_scale and the star seat like a star gem.
                    const float u0 = float(tapF % 2) * 0.5f;
                    const float v0 = float(tapF / 2) * 0.25f;
                    const float ts = isSP ? e.s * 1.3f : e.s;
                    emit((isSP ? texGh3TapSp_[lane] : texGh3Tap_[lane]).id, e,
                         e.cx, 128.0f*ts, 64.0f*ts,
                         isSP ? 0.35f : 0.68f, u0, v0, u0 + 0.5f, v0 + 0.25f);
                } else if (isSP) {
                    const float u0 = float(spinF % 4) * 0.25f;
                    const float v0 = float(spinF / 4) * 0.25f;
                    emit((n.type == NoteType::Hopo ? texGh3StarHammer_[lane]
                                                   : texGh3Star_[lane]).id, e,
                         e.cx, 128.0f*e.s*1.3f, 64.0f*e.s*1.3f, 0.35f,
                         u0, v0, u0 + 0.25f, v0 + 0.25f);
                } else {
                    emit((n.type == NoteType::Hopo ? texGh3GemHammer_[lane]
                                                   : texGh3Gem_[lane]).id, e,
                         e.cx, 128.0f*e.s, 64.0f*e.s, 0.68f, 0, 0, 1, 1);
                }
            }
        }
        {
            size_t off = 0;
            for (const auto& run : gemRuns) {
                v_.assign(gemStream.begin() + off,
                          gemStream.begin() + off + run.second);
                drawGh3Layer(run.first, ch::BLEND_SPRITE, true);
                off += run.second;
            }
        }

        // Pressed buttons ride above the gems (z 4.6-4.9).
        for (int l = 0; l < 5; ++l)
            if (fretRaised[l]) drawFretLane(l);
    }
}

void Renderer::drawActorQuad(float cx, float cy, float w, float h, float rotZDeg,
                             float r, float g, float b, float a,
                             GLuint tex, int blend, bool zWrite, bool zTest,
                             bool clearZ, float u0, float v0, float u1, float v1) {
    drawActorQuad3D(cx, cy, 0.0f, 0.0f, 0.0f, w, h,
                    0.0f, 0.0f, rotZDeg, 0.0f,
                    -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 0.0f,
                    r, g, b, a, tex, blend, zWrite, zTest, clearZ,
                    u0, v0, u1, v1);
}

void Renderer::drawActorQuad3D(float cx, float cy, float cz,
                               float localX, float localY, float w, float h,
                               float rotXDeg, float rotYDeg, float rotZDeg,
                               float skewX, float fovDeg,
                               float vanishX, float vanishY,
                               float fadeLeft, float fadeRight,
                               float fadeTop, float fadeBottom,
                               float cropLeft, float cropRight,
                               float cropTop, float cropBottom,
                               float r, float g, float b, float a,
                               GLuint tex, int blend, bool zWrite, bool zTest,
                               bool clearZ, float u0, float v0,
                               float u1, float v1, GLuint customProgram,
                               const std::vector<ActorShaderBinding>* customUniforms,
                               int imageW, int imageH,
                               int imageBackingW, int imageBackingH,
                               bool textureGlow,
                               const std::vector<ActorPolygonVertex>* polygon,
                               bool polygonTriangles, int cullMode,
                               float polygonZoomZ) {
    // blend,noeffect writes depth with NO colour, so it must still draw at
    // alpha 0 -- that is the whole point of a mask actor.
    if (a <= 0.0f && blend != 2) return;
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float rx = rotXDeg * 3.14159265f / 180.0f;
    const float ry = rotYDeg * 3.14159265f / 180.0f;
    const float rz = rotZDeg * 3.14159265f / 180.0f;
    const float cX = cosf(rx), sX = sinf(rx);
    const float cY = cosf(ry), sY = sinf(ry);
    const float cZ = cosf(rz), sZ = sinf(rz);
    const float m00 = cZ*cY;
    const float m01 = cZ*sY*sX+sZ*cX;
    const float m02 = cZ*sY*cX-sZ*sX;
    const float m10 = -sZ*cY;
    const float m11 = -sZ*sY*sX+cZ*cX;
    const float m12 = -sZ*sY*cX-cZ*sX;
    const float m20 = -sY;
    const float m21 = cY*sX;
    const float m22 = cY*cX;
    const float virtW = logicalScreenWidth(W, H);
    const float cameraDist = fovDeg > 0.0f
        ? virtW * 0.5f / tanf(fovDeg * 3.14159265f / 360.0f) : 0.0f;
    auto P = [&](float dx, float dy, float u, float v,
                 float lx, float ly, float* o) {
        float x = localX + dx;
        float y = localY + dy;
        const float z = 0.0f;
        x += skewX * y;
        float px = cx + x*m00 + y*m10 + z*m20;
        float py = cy + x*m01 + y*m11 + z*m21;
        const float pz = cz + x*m02 + y*m12 + z*m22;
        if (fovDeg > 0.0f) {
            const float denom = cameraDist - pz;
            const float scale = fabsf(denom) > 1e-5f ? cameraDist / denom : 1.0f;
            px = vanishX + (px - vanishX) * scale;
            py = vanishY + (py - vanishY) * scale;
        }
        o[0] = px;
        o[1] = py;
        o[2] = u; o[3] = v;
        o[4] = r; o[5] = g; o[6] = b; o[7] = a;
        o[8] = lx; o[9] = ly;
    };
    float q[6][10];
    P(-hw, -hh, u0, v0, 0, 0, q[0]); P(hw, -hh, u1, v0, 1, 0, q[1]);
    P(hw, hh, u1, v1, 1, 1, q[2]);   P(-hw, -hh, u0, v0, 0, 0, q[3]);
    P(hw, hh, u1, v1, 1, 1, q[4]);   P(-hw, hh, u0, v1, 0, 1, q[5]);
    const bool drawPolygon = polygon && !polygon->empty();
    const bool useFadeMesh = !drawPolygon &&
        (fadeLeft > 0.0f || fadeRight > 0.0f ||
         fadeTop > 0.0f || fadeBottom > 0.0f);
    std::vector<float> fadedQuadData;
    if (useFadeMesh) {
        // Sprite edge fades are five independently cropped rectangles: the
        // opaque inside, then left/right/top/bottom strips. Keep raw crop
        // values until each rectangle is clipped; negative crops deliberately
        // affect fade sizing before the eventual 0..1 clamp.
        float fadeSizeLeft = fadeLeft, fadeSizeRight = fadeRight;
        float fadeSizeTop = fadeTop, fadeSizeBottom = fadeBottom;
        const float horizontalRemaining = 1.0f - (cropLeft + cropRight);
        const float horizontalFade = fadeLeft + fadeRight;
        if (horizontalFade > 0.0f && horizontalRemaining < horizontalFade) {
            const float leftPart = fadeLeft / horizontalFade;
            fadeSizeLeft = leftPart * horizontalRemaining;
            fadeSizeRight = (1.0f - leftPart) * horizontalRemaining;
        }
        const float verticalRemaining = 1.0f - (cropTop + cropBottom);
        const float verticalFade = fadeTop + fadeBottom;
        if (verticalFade > 0.0f && verticalRemaining < verticalFade) {
            const float topPart = fadeTop / verticalFade;
            fadeSizeTop = topPart * verticalRemaining;
            fadeSizeBottom = (1.0f - topPart) * verticalRemaining;
        }

        const float baseLeft = std::clamp(cropLeft, 0.0f, 1.0f);
        const float baseRight = 1.0f - std::clamp(cropRight, 0.0f, 1.0f);
        const float baseTop = std::clamp(cropTop, 0.0f, 1.0f);
        const float baseBottom = 1.0f - std::clamp(cropBottom, 0.0f, 1.0f);
        const float baseWidth = baseRight - baseLeft;
        const float baseHeight = baseBottom - baseTop;
        fadedQuadData.reserve(5 * 6 * 10);

        auto appendRect = [&](float rawLeft, float rawRight,
                              float rawTop, float rawBottom,
                              float alphaTL, float alphaTR,
                              float alphaBL, float alphaBR) {
            if (rawLeft + rawRight >= 1.0f ||
                rawTop + rawBottom >= 1.0f ||
                baseWidth <= 0.0f || baseHeight <= 0.0f) return;
            const float rectLeft = std::clamp(rawLeft, 0.0f, 1.0f);
            const float rectRight = 1.0f - std::clamp(rawRight, 0.0f, 1.0f);
            const float rectTop = std::clamp(rawTop, 0.0f, 1.0f);
            const float rectBottom = 1.0f - std::clamp(rawBottom, 0.0f, 1.0f);
            if (rectLeft >= rectRight || rectTop >= rectBottom) return;
            const float x0 = (rectLeft - baseLeft) / baseWidth;
            const float x1 = (rectRight - baseLeft) / baseWidth;
            const float y0 = (rectTop - baseTop) / baseHeight;
            const float y1 = (rectBottom - baseTop) / baseHeight;
            auto append = [&](float lx, float ly, float vertexAlpha) {
                float vertex[10];
                P((lx - 0.5f) * w, (ly - 0.5f) * h,
                  u0 + (u1 - u0) * lx, v0 + (v1 - v0) * ly,
                  lx, ly, vertex);
                vertex[7] *= vertexAlpha;
                fadedQuadData.insert(fadedQuadData.end(), vertex, vertex + 10);
            };
            append(x0, y0, alphaTL); append(x1, y0, alphaTR);
            append(x1, y1, alphaBR); append(x0, y0, alphaTL);
            append(x1, y1, alphaBR); append(x0, y1, alphaBL);
        };

        appendRect(cropLeft + fadeLeft, cropRight + fadeRight,
                   cropTop + fadeTop, cropBottom + fadeBottom,
                   1.0f, 1.0f, 1.0f, 1.0f);
        if (fadeSizeLeft > 0.001f) {
            const float edgeAlpha = fadeSizeLeft / fadeLeft;
            appendRect(cropLeft, 1.0f - (cropLeft + fadeSizeLeft),
                       cropTop + fadeTop, cropBottom + fadeBottom,
                       0.0f, edgeAlpha, 0.0f, edgeAlpha);
        }
        if (fadeSizeRight > 0.001f) {
            const float edgeAlpha = fadeSizeRight / fadeRight;
            appendRect(1.0f - (cropRight + fadeSizeRight), cropRight,
                       cropTop + fadeTop, cropBottom + fadeBottom,
                       edgeAlpha, 0.0f, edgeAlpha, 0.0f);
        }
        if (fadeSizeTop > 0.001f) {
            const float edgeAlpha = fadeSizeTop / fadeTop;
            appendRect(cropLeft + fadeLeft, cropRight + fadeRight,
                       cropTop, 1.0f - (cropTop + fadeSizeTop),
                       0.0f, 0.0f, edgeAlpha, edgeAlpha);
        }
        if (fadeSizeBottom > 0.001f) {
            const float edgeAlpha = fadeSizeBottom / fadeBottom;
            appendRect(cropLeft + fadeLeft, cropRight + fadeRight,
                       1.0f - (cropBottom + fadeSizeBottom), cropBottom,
                       edgeAlpha, edgeAlpha, 0.0f, 0.0f);
        }
    }
    std::vector<float> polygonData;
    if (drawPolygon) {
        polygonData.reserve(polygon->size() * 10);
        for (const ActorPolygonVertex& vertex : *polygon) {
            const float data[10] = {
                vertex.x, vertex.y, vertex.u, vertex.v,
                r, g, b, a, vertex.z, 0.0f
            };
            polygonData.insert(polygonData.end(), data, data + 10);
        }
    }

    const GLuint actorProgram = customProgram ? customProgram : actor_;
    glUseProgram(actorProgram);
    glBindVertexArray(avao_);
    glBindBuffer(GL_ARRAY_BUFFER, avbo_);
    // blend: 0 normal, 1 add, 2 noeffect (colour write off -- used with zwrite
    // to lay a depth mask, which is exactly what the fixture mask does).
    if (blend == 1) glBlendFunc(GL_ONE, GL_ONE);
    else            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (blend == 2) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    // The z-mask. mask.png draws with blend,noeffect + zwrite + clearzbuffer to
    // lay a depth shape; chart1.png then draws with ztest so it only survives
    // inside it. That is what makes the taiko notation vanish at the judgment
    // line instead of scrolling on past.
    // The mask writes NEAR (-0.5) and the masked actor draws FARTHER (0.0)
    // under GL_LESS, so the masked actor survives only where the mask did NOT
    // write -- i.e. mask.png is an inverse stencil, opaque everywhere except
    // the window it opens. The fragment shader's alpha discard is what makes a
    // transparent texel leave the depth buffer alone, which is the whole
    // mechanism. Approximated: SM gives every actor a real Z, NotClon has only
    // these two planes, which is enough for a mask/masked pair.
    if (clearZ) { glDepthMask(GL_TRUE); glClear(GL_DEPTH_BUFFER_BIT); }
    // GREATER, with the mask nearer than what it reveals: the masked actor
    // survives exactly where the mask stamped depth, and is rejected against
    // the cleared far plane everywhere else. That makes the mask a REVEAL
    // window, which is what the taiko strip is -- it wipes in from the right.
    // OITG's mask mechanism, exactly (the earlier failure here was a wrong
    // probe claiming mask.png had no alpha -- it is a border at 1-63/255):
    //   * BLEND_NO_EFFECT masks automatically get SetZBias(1.0)
    //     (Actor.cpp:446-450), i.e. glDepthRange(0.0, 0.95), while everything
    //     else draws at (0.05, 1.0) -- same geometry z, NEARER stored depth.
    //     Emulated by the uDepth plane below: mask -0.1, others +0.1.
    //   * ztest,1 is ZTEST_WRITE_ON_PASS = GL_LEQUAL (RageDisplay_OGL.cpp:1494).
    //   * the alpha test culls transparent texels before the depth write, so
    //     the mask stamps only its inked border -- the occluder that swallows
    //     the taiko notation at the judgment line.
    // GL_DEPTH_TEST stays ENABLED for every actor -- depth WRITES only happen
    // while the test is enabled, and the mask has zwrite without ztest. OITG
    // does exactly this: SetZTestMode enables the test unconditionally and
    // maps ZTEST_OFF to GL_ALWAYS (RageDisplay_OGL.cpp:1488-1493).
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(zTest ? GL_LEQUAL : GL_ALWAYS);
    glDepthMask(zWrite ? GL_TRUE : GL_FALSE);
    if (drawPolygon && cullMode != 0) {
        glEnable(GL_CULL_FACE);
        glCullFace(cullMode == 1 ? GL_BACK : GL_FRONT);
    }
    glUniform1f(glGetUniformLocation(actorProgram, "uDepth"), blend == 2 ? -0.1f : 0.1f);
    glUniform1f(glGetUniformLocation(actorProgram, "uInvertY"),
                actorTargetYInverted_ ? -1.0f : 1.0f);
    const float actorVirtW = actorTargetW_ > 0 ? float(actorTargetW_)
                                               : logicalScreenWidth(W, H);
    const float actorVirtH = actorTargetH_ > 0 ? float(actorTargetH_) : 480.0f;
    glUniform2f(glGetUniformLocation(actorProgram, "uVirtHalf"),
                actorVirtW * 0.5f, actorVirtH * 0.5f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (customProgram) {
        glUniform1i(glGetUniformLocation(actorProgram, "sampler0"), 0);
        // NotITG convention: textureSize is the pow2 BACKING, imageSize the
        // content rect. Custom shaders convert with
        // img2tex(v) = v / textureSize * imageSize; binding both to the
        // content size makes that identity, so coords meant for the content
        // rect span the whole padded texture and feedback chains (Testify's
        // datamosh) accumulate the padding -- the pink L above and right of
        // the content (1 - 1920/2048, 1 - 1080/2048 of the frame).
        const float backingW = imageBackingW > 0 ? float(imageBackingW)
                                                 : float(imageW);
        const float backingH = imageBackingH > 0 ? float(imageBackingH)
                                                 : float(imageH);
        glUniform2f(glGetUniformLocation(actorProgram, "textureSize"),
                    backingW, backingH);
        glUniform2f(glGetUniformLocation(actorProgram, "imageSize"),
                    float(imageW), float(imageH));
        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        glUniform2f(glGetUniformLocation(actorProgram, "resolution"),
                    float(viewport[2]), float(viewport[3]));
        if (drawPolygon) {
            glUniform3f(glGetUniformLocation(actorProgram, "ncCenter"), cx, cy, cz);
            glUniform3f(glGetUniformLocation(actorProgram, "ncZoom"),
                        w, h, polygonZoomZ);
            glUniform3f(glGetUniformLocation(actorProgram, "ncRot0"), m00, m10, m20);
            glUniform3f(glGetUniformLocation(actorProgram, "ncRot1"), m01, m11, m21);
            glUniform3f(glGetUniformLocation(actorProgram, "ncRot2"), m02, m12, m22);
            glUniform1f(glGetUniformLocation(actorProgram, "ncSkewX"), skewX);
            glUniform1f(glGetUniformLocation(actorProgram, "ncCameraDist"), cameraDist);
            glUniform2f(glGetUniformLocation(actorProgram, "ncVanish"), vanishX, vanishY);
        }
        int textureUnit = 1;
        if (customUniforms) for (const ActorShaderBinding& uniform : *customUniforms) {
            const GLint location = glGetUniformLocation(actorProgram, uniform.name.c_str());
            if (location < 0) continue;
            if (uniform.integer) glUniform1i(location, int(uniform.value[0]));
            else if (uniform.components == 1) glUniform1f(location, uniform.value[0]);
            else if (uniform.components == 2)
                glUniform2f(location, uniform.value[0], uniform.value[1]);
            else if (uniform.components == 4)
                glUniform4f(location, uniform.value[0], uniform.value[1],
                            uniform.value[2], uniform.value[3]);
            else if (uniform.texture) {
                glActiveTexture(GL_TEXTURE0 + textureUnit);
                glBindTexture(GL_TEXTURE_2D, uniform.texture);
                glUniform1i(location, textureUnit++);
            }
        }
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.01f);
    } else {
        glUniform1i(glGetUniformLocation(actorProgram, "uTex"), 0);
        glUniform1f(glGetUniformLocation(actorProgram, "uHasTex"), tex ? 1.0f : 0.0f);
        glUniform1f(glGetUniformLocation(actorProgram, "uGlow"), textureGlow ? 1.0f : 0.0f);
        glUniform2f(glGetUniformLocation(actorProgram, "uFadeX"), 0.0f, 0.0f);
        glUniform2f(glGetUniformLocation(actorProgram, "uFadeY"), 0.0f, 0.0f);
    }
    if (drawPolygon) {
        glBufferData(GL_ARRAY_BUFFER,
                     GLsizeiptr(polygonData.size() * sizeof(float)),
                     polygonData.data(), GL_STREAM_DRAW);
        glDrawArrays(polygonTriangles ? GL_TRIANGLES : GL_TRIANGLE_STRIP,
                     0, GLsizei(polygon->size()));
    } else if (useFadeMesh) {
        if (!fadedQuadData.empty()) {
            glBufferData(GL_ARRAY_BUFFER,
                         GLsizeiptr(fadedQuadData.size() * sizeof(float)),
                         fadedQuadData.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, GLsizei(fadedQuadData.size() / 10));
        }
    } else {
        glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    if (customProgram) glDisable(GL_ALPHA_TEST);
    if (drawPolygon && cullMode != 0) glDisable(GL_CULL_FACE);
    if (blend == 2) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    // Hand the scene path back exactly what it expects.
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
}

void Renderer::drawActorText(const std::string& text,
                             float cx, float cy, float cz,
                             float zoomX, float zoomY,
                             float rotXDeg, float rotYDeg, float rotZDeg,
                             float skewX, float fovDeg,
                             float vanishX, float vanishY,
                             float r, float g, float b, float a,
                             int blend, bool zWrite, bool zTest, bool clearZ) {
    if (!actorFont_.id || text.empty()) return;

    std::vector<std::string> lines(1);
    for (char ch : text) {
        if (ch == '\n') lines.emplace_back();
        else lines.back() += ch;
    }
    int totalHeight = 21 * int(lines.size()) + 3 * int(lines.size() - 1);
    int lineY = int(lrintf(-float(totalHeight) * 0.5f));
    bool firstGlyph = true;
    for (const std::string& line : lines) {
        int lineWidth = 0;
        for (unsigned char ch : line) lineWidth += actorFontAdvance_[ch];
        int cursorX = int(lrintf(-float(lineWidth) * 0.5f));
        lineY += 21;
        const int glyphTop = lineY - 26;
        for (unsigned char ch : line) {
            int glyphWidth = actorFontWidth_[ch];
            const int advance = actorFontAdvance_[ch];
            int chop = 32 - glyphWidth;
            if ((chop % 2) == 1) { --chop; ++glyphWidth; }
            const float extraLeft = std::min(2.0f, chop * 0.5f);
            const float extraRight = std::min(1.0f, chop * 0.5f);
            const float drawWidth = glyphWidth + extraLeft + extraRight;
            if (drawWidth > 0.0f) {
                const int col = ch & 15, row = ch >> 4;
                const float u0 = (col * 32 + chop * 0.5f - extraLeft) / 512.0f;
                const float u1 = (col * 32 + 32 - chop * 0.5f + extraRight) / 512.0f;
                const float v0 = (row * 32) / 512.0f;
                const float v1 = (row * 32 + 32) / 512.0f;
                const float localX = (cursorX - extraLeft + drawWidth * 0.5f) * zoomX;
                const float localY = (glyphTop + 16.0f) * zoomY;
                drawActorQuad3D(cx, cy, cz, localX, localY,
                                drawWidth * zoomX, 32.0f * zoomY,
                                rotXDeg, rotYDeg, rotZDeg, skewX,
                                fovDeg, vanishX, vanishY, 0, 0, 0, 0,
                                0, 0, 0, 0,
                                r, g, b, a, actorFont_.id, blend,
                                zWrite, zTest, clearZ && firstGlyph,
                                u0, v0, u1, v1);
                firstGlyph = false;
            }
            cursorX += advance;
        }
        lineY += 3;
    }
}

void Renderer::drawActorPlayerSource(int pn, float x, float y, float z,
                                     float zoomX, float zoomY, float zoomZ,
                                     float rotXDeg, float rotYDeg, float rotZDeg,
                                     float skewX, float fovDeg,
                                     float vanishX, float vanishY,
                                     float r, float g, float b, float a) {
    if (pn < 1 || pn > 2 || a <= 0.0f || !actorChart_ || !actorOpts_ ||
        !actorFields_[pn - 1]) return;

    // Convert the highway's camera space to the actor layer's pixel space
    // without dividing by clip W. At the normal 45-degree actor camera this
    // reproduces the ordinary field projection exactly, but the vertices keep
    // their depth so proxy rotation and perspective act on the highway itself.
    const float sourceHalfW = logicalScreenWidth(W, H) * 0.5f;
    const float sourceHalfH = 240.0f;
    const float targetHalfW = actorTargetW_ > 0 ? actorTargetW_ * 0.5f
                                                : sourceHalfW;
    const float targetHalfH = actorTargetH_ > 0 ? actorTargetH_ * 0.5f
                                                : sourceHalfH;
    const float baseDistance = targetHalfW /
        tanf(45.0f * 3.14159265f / 360.0f);
    const float viewZ0 = view_.m[14];
    if (fabsf(viewZ0) < 1e-5f) return;
    const float sceneCot = 1.0f / tanf(ch::CAM_FOV * 3.14159265f / 360.0f);
    // ALWAYS the full aspect, even with two fields. The halved one belongs to
    // the DIRECT two-player layout, where each field is rendered with the
    // full-screen camera and then squeezed into half the width by drawField's
    // placement matrix -- mvp2_ pre-compensates for that squeeze. A proxied
    // field supplies its own MVP, so drawField skips the placement step
    // entirely and the field is drawn at full proportions for the actor to
    // position. Carrying the halved aspect into this path stretched every
    // proxied playfield to twice its width.
    const float sceneAspect = float(W) / float(H);

    Mat4 cameraToActor{};
    cameraToActor.m[0] = -sourceHalfW * sceneCot / (sceneAspect * -viewZ0);
    cameraToActor.m[5] = -sourceHalfH * sceneCot / -viewZ0;
    cameraToActor.m[10] = -baseDistance / viewZ0;
    cameraToActor.m[14] = baseDistance;
    cameraToActor.m[15] = 1.0f;

    Mat4 local{};
    local.m[0] = zoomX;
    local.m[4] = skewX * zoomY;
    local.m[5] = zoomY;
    local.m[10] = zoomZ;
    local.m[15] = 1.0f;

    const float rx = rotXDeg * 3.14159265f / 180.0f;
    const float ry = rotYDeg * 3.14159265f / 180.0f;
    const float rz = rotZDeg * 3.14159265f / 180.0f;
    const float cX = cosf(rx), sX = sinf(rx);
    const float cY = cosf(ry), sY = sinf(ry);
    const float cZ = cosf(rz), sZ = sinf(rz);
    Mat4 model{};
    model.m[0] = cZ*cY;
    model.m[1] = cZ*sY*sX+sZ*cX;
    model.m[2] = cZ*sY*cX-sZ*sX;
    model.m[4] = -sZ*cY;
    model.m[5] = -sZ*sY*sX+cZ*cX;
    model.m[6] = -sZ*sY*cX-cZ*sX;
    model.m[8] = -sY;
    model.m[9] = cY*sX;
    model.m[10] = cY*cX;
    model.m[12] = x;
    model.m[13] = y;
    model.m[14] = z;
    model.m[15] = 1.0f;

    Mat4 projection{};
    const float invertY = actorTargetYInverted_ ? -1.0f : 1.0f;
    if (fovDeg != 0.0f) {
        const float actorFov = fovDeg > 0.0f ? fovDeg : 45.0f;
            const float cameraDistance = targetHalfW /
                tanf(actorFov * 3.14159265f / 360.0f);
        const float vx = fovDeg > 0.0f ? vanishX : targetHalfW;
        const float vy = fovDeg > 0.0f ? vanishY : targetHalfH;
        projection.m[0] = cameraDistance / targetHalfW;
        projection.m[5] = -cameraDistance / targetHalfH * invertY;
        projection.m[8] = 1.0f - vx / targetHalfW;
        projection.m[9] = (-1.0f + vy / targetHalfH) * invertY;
        projection.m[11] = -1.0f;
        projection.m[12] = -cameraDistance;
        projection.m[13] = cameraDistance * invertY;
        // Constant clip z: NDC z = -5/w, monotone in distance from the
        // actor camera. Without ANY z row every proxied vertex sat at NDC
        // z = 0, so depth ordering degenerated to painter's order -- fine
        // for CH/moon (painter's by design), fatal for the yarg scene,
        // whose transparent track draws AFTER the notes and relies on
        // LEQUAL: at equal depth it painted over frets and notes ("under
        // the highway"), and fret meshes self-overdrew into dark slivers.
        // Magnitude 5: discrimination is 5*dw/w^2 -- a 0.07-world note-
        // over-track gap at the strikeline (w~690 at 480p targets) clears
        // ~440 ulps of 24-bit depth; -0.5 left only ~44. Clipping would
        // need w < 5 pixel units, already degenerate for x/y.
        projection.m[14] = -5.0f;
        projection.m[15] = cameraDistance;
    } else {
        projection.m[0] = 1.0f / targetHalfW;
        projection.m[5] = -invertY / targetHalfH;
        projection.m[12] = -1.0f;
        projection.m[13] = invertY;
        // Ortho: w is constant 1, so a constant z restores no ordering.
        // Put actor-space z in the numerator instead; nearer vertices have
        // larger z_actor (cameraToActor maps distance to decreasing z), so
        // -1e-4 keeps LEQUAL = nearer-wins. +-1000 px of depth spans only
        // +-0.1 NDC -- no clipping short of |z| > 10^4 px.
        projection.m[10] = -1e-4f;
        projection.m[15] = 1.0f;
    }

    const Mat4 actorModel = mat_mul(model, local);
    const Mat4 actorView = mat_mul(cameraToActor,
                                   actorFields_[pn - 1]->viewEff);
    const Mat4 actorMvp = mat_mul(projection, mat_mul(actorModel, actorView));
    const Mat4 piuMvp = mat_mul(
        projection,
        mat_mul(actorModel,
                piuFieldLocal(actorFields_[pn - 1]->mods, actorBeat_, sourceHalfW)));
    // GH3's 1280x720 vscreen into the actor source's 480-tall frame: scaled
    // by 2/3 (1280 maps onto the full 853 logical width) and centred, the
    // same role piuFieldLocal's translate plays for the pump pad. The
    // field's own tilt/wag/mini and whole-field moves compose inside
    // drawGh3's path from this player's mods.
    Mat4 gh3Local{};
    gh3Local.m[0] = gh3Local.m[5] = 480.0f / 720.0f;
    gh3Local.m[10] = gh3Local.m[15] = 1.0f;
    gh3Local.m[12] = -640.0f * (480.0f / 720.0f);
    gh3Local.m[13] = -360.0f * (480.0f / 720.0f);
    const Mat4 gh3Mvp = mat_mul(projection, mat_mul(actorModel, gh3Local));
    const Mods& playerMods = actorFields_[pn - 1]->mods;
    const int engineStyle = playerMods.moonscraper >= playerMods.yarg ? 1 : 2;
    const Mat4 sourceEngineView = engineView(engineStyle);
    const float engineViewZ0 = sourceEngineView.m[14];
    Mat4 engineCameraToActor{};
    const float engineCot = 1.0f / tanf(55.0f * 3.14159265f / 360.0f);
    engineCameraToActor.m[0] = -targetHalfH * engineCot / -engineViewZ0;
    engineCameraToActor.m[5] = -targetHalfH * engineCot / -engineViewZ0;
    engineCameraToActor.m[10] = -baseDistance / engineViewZ0;
    engineCameraToActor.m[14] = baseDistance;
    engineCameraToActor.m[15] = 1.0f;
    Mat4 engineLaneFit = engineIdentity();
    if (engineStyle == 2) {
        const float targetAspect = targetHalfW / targetHalfH;
        Mat4 sourceCamera = mat_mul(
            mat_perspective(55.0f,targetAspect,0.01f,40.0f),
            sourceEngineView);
        for (int row = 0; row < 4; ++row)
            sourceCamera.m[row*4] = -sourceCamera.m[row*4];
        const float laneScale = yargLaneScale(sourceCamera,targetAspect);
        const float oneMinusScale = 1.0f-laneScale;
        engineLaneFit.m[0] = laneScale;
        engineLaneFit.m[5] = laneScale;
        engineLaneFit.m[9] = -oneMinusScale*targetHalfH/baseDistance;
        engineLaneFit.m[13] = oneMinusScale*targetHalfH;
    }
    const Mat4 engineMvp = mat_mul(
        projection,
        mat_mul(actorModel,
                mat_mul(engineLaneFit,
                        mat_mul(engineCameraToActor,sourceEngineView))));
    const float oldTint[4] = {
        fieldTint_[0], fieldTint_[1], fieldTint_[2], fieldTint_[3]
    };
    fieldTint_[0] = r; fieldTint_[1] = g;
    fieldTint_[2] = b; fieldTint_[3] = a;
    drawField(*actorChart_, actorBeat_, *actorOpts_, *actorFields_[pn - 1],
              0, W, actorScrollNow_, actorSongTime_, actorNowSec_, actorBpm_,
              &actorMvp, &piuMvp, &engineMvp, &gh3Mvp);
    for (int i = 0; i < 4; ++i) fieldTint_[i] = oldTint[i];
}

// The sustain glow: its own program, additive, no texture. Mirrors drawLayer
// but cannot share it -- the fragment shader is different and there is nothing
// to bind.
void Renderer::drawSustainGlow() {
    if (v_.empty()) return;
    applyFieldTint();
    glUseProgram(susGlow_);
    glBlendFunc(GL_ONE, GL_ONE);                 // SustainGlow.shader Blend One One
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                 v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
    v_.clear();
    glUseProgram(prog_);                         // restore for the fret buckets
}

void Renderer::drawLayer(GLuint tex, int blend, float glow) {
    if (v_.empty()) return;
    applyFieldTint();
    switch (blend) {
        case ch::BLEND_NECK: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
        case ch::BLEND_ADD:  glBlendFunc(GL_ONE, GL_ONE); break;
        default:             glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
    }
    glUniform1f(locPremul_, blend == ch::BLEND_NECK ? 0.0f : 1.0f);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                 v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));

    // Second pass, same quad: ITG's Sprite draws diffuse then glow back to back
    // for one sprite (Sprite.cpp:520-541), so this is interleaved per quad
    // rather than batched into a bucket. Skipped entirely at glow 0, which is
    // every frame with no appearance mod live -- that is what keeps this
    // hash-neutral.
    if (glow > 0.0f) {
        for (ch::Vtx& vt : v_) {
            vt.r = fieldTint_[0];
            vt.g = fieldTint_[1];
            vt.b = fieldTint_[2];
            vt.a = glow * fieldTint_[3];
        }
        glUseProgram(glow_);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                     v_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
        glUseProgram(prog_);
    }
    v_.clear();
}

// ---------------------------------------------------------------------------
// One player's evaluated modchart state and the matrices built from it.
// Split out of drawFrame so a second playfield is a second call, not a
// second copy of five hundred lines.
Renderer::FieldEval Renderer::evalField(const Chart& chart, double beat,
                                        const RenderOpts& o,
                                        const ModDoc* doc, int plr) {
    (void)plr;
    FieldEval E;
    Mods& mods = E.mods; PostFx& fx = E.fx;
    float& mx = E.mx; float& my = E.my; float& mz = E.mz;
    float (&bgKnobs)[MAX_BG_UNIFORMS] = E.bgKnobs;
    unsigned& bgDriven = E.bgDriven;
    float& noteSpeed = E.noteSpeed;
    float& piuT = E.piuT;
    Mat4& mvpEff = E.mvpEff;
    Mat4& viewEff = E.viewEff;
    const float bpm = float(chart.bpmAt(beat));
    (void)bpm;
    // bg.<name> knob values for shader background layers. The trailing evalAt
    // argument defaults to null, so the editor's call sites are untouched; the
    // mask says which slots the document actually drives -- undriven uniforms
    // are never set, so a shader's own defaults survive (modfile.h).
    // The strike line in screen UV, for shaders that want to protect or
    // target the playfield. Filled by the background block below and reused
    // by the --fxshader pass; the 0.5,0.5 default is screen centre.
    // OITG composition, not a switch: the .ncmod is the offline stand-in
    // for m_StoredPlayerOptions (the base the engine rebuilds from,
    // GameState.cpp:1350), and the Lua runtime's live PlayerOptions overlay
    // ONLY the knobs Lua has actually written. The old either/or here made
    // any Lua modfile disregard the whole document -- an ncmod's
    // moonscraper/yarg style knob silently fell back to CH. Once Lua
    // touches a knob it shadows the doc for that knob from then on
    // (including "no X" = Lua-owned zero), matching live-state semantics.
    {
        float v[MOD_SLOTS];
        if (doc) {
            doc->evalAt(chart, beat * chart.resolution, v);
            if (bg_) memcpy(bgKnobs, v + MOD_BG_BASE,
                            MAX_BG_UNIFORMS * sizeof(float));
            bgDriven = doc->bgUsedMask();
        } else {
            for (int i = 0; i < MOD_SLOTS; ++i) v[i] = modDefault(i);
        }
        const bool livePlayerOptions = actors_ &&
            actors_->overlayPlayerValues(plr, fieldSec_, v);
        if (doc || livePlayerOptions)
            modValuesToState(v, float(beat), mods, fx, mx, my, mz);
        else
            modchartAt(beat, mods, fx);
    }
    if (o.noMods) { mods = Mods{}; mx = my = mz = 0; bgDriven = 0; }

    // `piu` is a transition, not a switch. The pad slides in from the top of
    // the virtual screen and SHOVES the CH highway out of the bottom -- both
    // driven by the same piuT, so 40% is 40% of the way through the swap. This
    // has to happen HERE, before mvp_/mvpEff are built from my, not next to the
    // hidePlayfield test further down: down there my has already been baked
    // into the matrices and the subtraction is a no-op.
    piuT = mods.piu < 0.0f ? 0.0f : (mods.piu > 1.0f ? 1.0f : mods.piu);
    // Measured, not guessed: banding the frame at 99% shows the last of the
    // board leaving the bottom band at 2.38 units, so 2.5 has it fully gone by
    // ~95% -- just before the pad finishes seating, which is what keeps 99%
    // from still showing a sliver and 100% from being a pop.
    if (piuT > 0.0f && mods.hide == 0.0f) my -= piuT * 2.5f;
    if (o.noPost) { fx = PostFx{}; fx.glow = 0; fx.vignette = 0; fx.aberration = 0; }

    // xmod: ITG multiplies fYOffset by the scroll speed (ArrowEffects.cpp:135)
    // and z is our offset domain, so the multiplier folds into the note speed.
    // Deviation, documented: ITG applies boost/wave to the PRE-scale offset;
    // folding here makes them see the post-scale one. * 1.0f is exact, so the
    // default is bit-identical.
    noteSpeed = o.noteSpeed * mods.scrollspeed;

    // ---- whole-field transform: tilt, mini, wag ---------------------------
    // OITG applies perspective mods to the whole notefield actor
    // (Player.cpp:706-739): rotationX = SCALE(tilt,-1,+1,+30,-30), zoom =
    // (1 - 0.5*mini) * SCALE(|tilt|,0,1,1,0.9), about the field origin. The
    // highway analogue is a world transform about the strike line, which sits
    // on the world origin. Screen-X rotations negate between ITG's Y-down
    // screen and our Y-up world, so world degrees = -fTiltDegrees = +30*tilt:
    // distant (+) lays the neck down away, hallway (-) rears it up. OITG's
    // SetY nudge (:730-735) is skipped -- it re-centres a rotation made about
    // the receptor row, and rotating about the strike line already is that.
    // wag is a legacy Actor effect: rotZ of the field,
    // wag% * 21 degrees on a 2-beat bgm-clock sine.
    const Mat4& baseMvp = o.doc2 ? mvp2_ : mvp_;
    mvpEff = baseMvp;
    viewEff = view_;
    {
        const float tiltDeg = 30.0f * mods.tilt;
        float zoom = 1.0f - 0.5f * mods.mini;
        if (mods.tilt > 0.0f)      zoom *= 1.0f - 0.1f * mods.tilt;       // :725
        else if (mods.tilt < 0.0f) zoom *= 1.0f + 0.1f * mods.tilt;       // :727
        const float wagDeg = mods.wag * 21.0f * sinf(float(beat) * 3.14159265f);
        if (tiltDeg != 0.0f || zoom != 1.0f || wagDeg != 0.0f) {
            const float tx = tiltDeg * 3.14159265f / 180.0f;
            const float tz = wagDeg  * 3.14159265f / 180.0f;
            const float cx_ = cosf(tx), sx_ = sinf(tx);
            const float cz_ = cosf(tz), sz_ = sinf(tz);
            Mat4 F{};                                   // Rz * Rx, uniform zoom
            F.m[0]  = zoom *  cz_;
            F.m[1]  = zoom *  sz_;
            F.m[4]  = zoom * -sz_ * cx_;
            F.m[5]  = zoom *  cz_ * cx_;
            F.m[6]  = zoom *  sx_;
            F.m[8]  = zoom *  sz_ * sx_;
            F.m[9]  = zoom * -cz_ * sx_;
            F.m[10] = zoom *  cx_;
            F.m[15] = 1.0f;
            mvpEff = mat_mul(baseMvp, F);
            viewEff = mat_mul(view_, F);
        }
    }

    return E;
}

// Draw one playfield at the position and scale of its camera rectangle without
// using that rectangle as a clip region. Modcharts move/rotate the two fields
// through each other; clipping each source to its half first cuts notes off at
// the centre seam. Folding the viewport mapping into clip space preserves the
// half-width projection while the full framebuffer remains available.
void Renderer::drawField(const Chart& chart, double beat, const RenderOpts& o,
                         FieldEval& E, int vpX, int vpW, float scrollNow,
                         float songTime, double nowSec, float bpm,
                         const Mat4* mvpOverride,
                         const Mat4* piuMvpOverride,
                         const Mat4* engineMvpOverride,
                         const Mat4* gh3MvpOverride) {
    Mods& mods = E.mods;
    float& mx = E.mx; float& my = E.my; float& mz = E.mz;
    const float noteSpeed = E.noteSpeed;
    const float piuT = E.piuT;
    const float moonT = fminf(1.0f, fmaxf(0.0f, mods.moonscraper));
    const float yargT = fminf(1.0f, fmaxf(0.0f, mods.yarg));
    const int engineStyle = moonT >= yargT ? 1 : 2;
    const float engineT = mods.hide == 0.0f ? fmaxf(moonT, yargT) : 0.0f;
    // gh3 crossfades against CH the same way the engine styles do -- the CH
    // field fades in place and the GH3 sprite field draws over it.
    const float gh3T = mods.hide == 0.0f
        ? fminf(1.0f, fmaxf(0.0f, mods.gh3)) : 0.0f;
    // taiko crossfades exactly like gh3 -- another 2D field drawn over a CH
    // highway that fades out underneath it.
    const float taikoT = mods.hide == 0.0f
        ? fminf(1.0f, fmaxf(0.0f, mods.taiko)) : 0.0f;
    const float bmsT = mods.hide == 0.0f
        ? fminf(1.0f, fmaxf(0.0f, mods.bms)) : 0.0f;
    const float originalTint[4] = {
        fieldTint_[0], fieldTint_[1], fieldTint_[2], fieldTint_[3]
    };
    const float fieldSwapT =
        fmaxf(fmaxf(engineT, gh3T), fmaxf(taikoT, bmsT));
    if (fieldSwapT > 0.0f) fieldTint_[3] *= 1.0f - fieldSwapT;
    Mat4& mvpEff = E.mvpEff;
    Mat4 drawMvp = mvpOverride ? *mvpOverride : mvpEff;
    if (!mvpOverride && (vpX != 0 || vpW != W)) {
        Mat4 place{};
        place.m[0] = float(vpW) / float(W);
        place.m[5] = place.m[10] = place.m[15] = 1.0f;
        place.m[12] = float(2 * vpX + vpW) / float(W) - 1.0f;
        drawMvp = mat_mul(place, mvpEff);
    }
    (void)nowSec;
    const bool hasStops = !chart.stops.empty();
    auto ssec = [&](double t) { return hasStops ? chart.scrollSec(t) : t; };
    if (!mvpOverride) glViewport(0, 0, W, H);
    glUseProgram(prog_);
    glUniformMatrix4fv(glGetUniformLocation(prog_, "uMVP"), 1, GL_FALSE, drawMvp.m);
    glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
    glUniform1f(glGetUniformLocation(prog_, "uCurve"),0.0f);
    glUniform2f(glGetUniformLocation(prog_, "uFadeRange"),0.0f,0.0f);
    glUniform3f(glGetUniformLocation(prog_, "uOffset"),
                o.px + mx, o.py + my, o.pz + mz);
    // The glow program shares SCENE_VS, so it needs the same camera and the
    // same rigid movex/movey/movez offset. Set once here; drawLayer only ever
    // switches to it, and only when a note actually glows.
    glUseProgram(glow_);
    glUniformMatrix4fv(glGetUniformLocation(glow_, "uMVP"), 1, GL_FALSE, drawMvp.m);
    glUniform1i(glGetUniformLocation(glow_, "uTex"), 0);
    glUniform3f(glGetUniformLocation(glow_, "uOffset"),
                o.px + mx, o.py + my, o.pz + mz);
    glUseProgram(susGlow_);
    glUniformMatrix4fv(glGetUniformLocation(susGlow_, "uMVP"), 1, GL_FALSE, drawMvp.m);
    glUniform3f(glGetUniformLocation(susGlow_, "uOffset"),
                o.px + mx, o.py + my, o.pz + mz);
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glActiveTexture(GL_TEXTURE0);

    // NotITG `hide`: any nonzero percentage hides the whole playfield. Notes,
    // sustains, frets and board all skip; the actor layers still draw, which is
    // the point -- a section can keep its own visuals while the highway (and
    // the timing it gives away) is gone.
    // PIU replaces the playfield wholesale -- board, frets, notes and all -- so
    // it rides the same switch that `hide` uses to skip every CH layer, and
    // then draws its own field below. `hide` still wins over it.
    // `piu` crossfades rather than switching: below 100% the CH highway is
    // still drawn, pushed down out of frame by the incoming pad (the my offset
    // for that is applied further up, before the matrices). Only a full 100%
    // drops the highway entirely.
    const bool piuMode = piuT > 0.0f && mods.hide == 0.0f && engineT < 1.0f &&
                         gh3T < 1.0f && taikoT < 1.0f && bmsT < 1.0f;
    const bool hidePlayfield = mods.hide != 0.0f || piuT >= 1.0f ||
                               engineT >= 1.0f || gh3T >= 1.0f ||
                               taikoT >= 1.0f || bmsT >= 1.0f;

    // Board layers. --playfield drops all four, leaving only frets and notes.
    // `hideboard` is the --playfield flag as a knob: the board goes, the notes
    // and frets stay. A modchart can therefore drop the highway for a section
    // rather than the whole render having to choose.
    if (!o.playfield && !hidePlayfield && mods.hideboard == 0.0f) {
        // neck -- first because spr_highway_gh6.mat sets m_CustomRenderQueue 0
        ch::buildNeck(v_, scrollNow, noteSpeed);
        drawLayer(texHighway_.id, ch::BLEND_NECK);

        ch::buildBeatLines(v_, chart, beat, scrollNow, noteSpeed, &mods);   // -28000
        drawLayer(texBeat_.id, ch::BLEND_SPRITE);

        ch::buildSidebars(v_);                                      // -27000
        drawLayer(texSide_.id, ch::BLEND_SPRITE);

        ch::buildStrings(v_);                                       // -25000
        drawLayer(texString_.id, ch::BLEND_SPRITE);
    }

    // Frets. Six draw calls in sortingOrder sequence -- NOT batched by texture,
    // because halfCover(-994) sorts after head(-997) despite sharing base's
    // sheet. A lane whose head is above rest jumps to 20000+ and is drawn after
    // the notes instead.
    // --- sustains -----------------------------------------------------------
    // Bodies sort at -1000: after the strings, before every fret quad. Built
    // far -> near because they all share that order, so Unity breaks the tie by
    // camera distance -- same direction as the note loop below.
    //
    // Held is a pure function of the beat. CH integrates
    // (`sustainNote.length -= songTimeDifference`, BaseGuitarPlayer.cs:183),
    // but StartSustain (:300-310) adds `hitNote.time - songTime`, which is 0
    // for a bot that hits on time, so the integration telescopes to
    // `tEnd - songTime` and no history is needed.
    std::vector<ch::Vtx> susBodyV, susBodyGlowV, susGlowV;
    std::vector<ch::Vtx> susOpenV, susOpenBodyGlowV, susOpenGlowV;
    bool laneSustaining[5] = {false, false, false, false, false};
    // ITG draws a hold body twice, exactly like a tap: a diffuse pass whose
    // alpha is the binary GetAlpha cut, and a white-silhouette glow pass
    // (NoteDisplay::DrawHoldBody is called with bGlow false then true). Without
    // the second pass, any stealth >= 50% erases every unheld ribbon while the
    // held ones stay visible through their additive halo -- which is exactly
    // the "sustains only show when held" symptom under a high stealth base.
    const bool susNeedsGlow =
        mods.sudden != 0.0f || mods.hidden != 0.0f || mods.stealth != 0.0f ||
        mods.blink != 0.0f || mods.randomvanish != 0.0f;
    for (int i = int(chart.notes.size()) - 1; i >= 0; --i) {
        const Note& n = chart.notes[i];
        const double tHit = chart.beatToSec(n.beat);
        for (int lane = 0; lane < 5; ++lane) {
            if (!(n.frets & (1 << lane)) || n.sustain[lane] <= 0.0) continue;
            const double tEnd = chart.beatToSec(n.beat + n.sustain[lane]);
            if (songTime >= tEnd) continue;                 // finished
            const bool  held = !o.noBot && songTime >= tHit;
            const float gemZ = float(ssec(tHit) - scrollNow) * noteSpeed;

            const float len = float(held ? (ssec(tEnd) - scrollNow)
                                         : (ssec(tEnd) - ssec(tHit))) * noteSpeed
                            + ch::SUS_LEN_OFFSET;
            // The +0.3 hides the ribbon's blunt near end behind the gem; it
            // means the ribbon jumps 0.3 toward the camera at the hit
            // (GuitarNoteRenderer.cs:497 vs :527). That is CH's behaviour.
            const float zStart = held ? 0.0f : gemZ + ch::SUS_Z_OFFSET;
            const int   frame  = held ? ch::SUS_FRAME_HELD
                               : (n.type == NoteType::Tap ? ch::SUS_FRAME_TAP
                                                          : ch::SUS_FRAME_STRUM);
            const float* tint = held ? ch::SUSTAIN_TINT[lane] : ch::NOTE_TINT[lane];

            ch::buildSustainBody(susBodyV, mods, lane, zStart, len,
                                 ch::SUS_BODY_W * 0.5f, frame, tint,
                                 songTime, float(beat), bpm,
                                 susNeedsGlow ? &susBodyGlowV : nullptr);
            // Unheld sustains have NO glow at all -- GuitarNoteRenderer.cs:449
            // and :493 disable it, and only the held block (:575) turns it back
            // on. So this bucket holds at most five lanes.
            if (held) {
                laneSustaining[lane] = true;
                ch::buildSustainGlow(susGlowV, mods, lane, len,
                                     ch::SUS_GLOW_W * 0.5f, ch::SUSTAIN_TINT[lane],
                                     songTime, float(beat), bpm);
            }
        }
        // Open sustains. A separate pool in CH with its own prefab, sprite
        // sheet and widths -- which is why walking only the five lanes above
        // drew nothing for them at all. Centred on the highway (lane 2's x is
        // 0, and lane 2 is also the mod column an open note uses), Sustains[4]
        // = the open strip's own sprite, no +0.3 unheld z nudge (the open
        // prefab is placed at noteZPosition flat), and NoteColors.OpenSustain
        // whether held or not.
        if (n.open && n.openSustain > 0.0) {
            const double tEnd = chart.beatToSec(n.beat + n.openSustain);
            if (songTime < tEnd) {
                const bool  held = !o.noBot && songTime >= tHit;
                const float gemZ = float(ssec(tHit) - scrollNow) * noteSpeed;
                const float len = float(held ? (ssec(tEnd) - scrollNow)
                                             : (ssec(tEnd) - ssec(tHit))) * noteSpeed
                                // sustainLengthOffset is computed ONCE from
                                // the lane sprite and reused for every
                                // ribbon, opens included.
                                + ch::SUS_LEN_OFFSET;
                const float zStart = held ? 0.0f : gemZ;
                ch::buildSustainBody(susOpenV, mods, 2, zStart, len,
                                     ch::SUS_OPEN_W * 0.5f, 0,
                                     ch::OPEN_SUSTAIN_TINT,
                                     songTime, float(beat), bpm,
                                     susNeedsGlow ? &susOpenBodyGlowV : nullptr,
                                     /*openStrip*/ true);
                if (held)
                    ch::buildSustainGlow(susOpenGlowV, mods, 2, len,
                                         ch::SUS_OPEN_GLOW_W * 0.5f,
                                         ch::OPEN_SUSTAIN_TINT,
                                         songTime, float(beat), bpm);
            }
        }
    }
    if (hidePlayfield) {
        susBodyV.clear(); susBodyGlowV.clear(); susGlowV.clear();
        susOpenV.clear(); susOpenBodyGlowV.clear(); susOpenGlowV.clear();
    }
    // Open ribbons share the lane ribbons' -1000 order; they cannot overlap
    // (an open note is exclusive with fretted ones), so within-layer order
    // between them never matters.
    v_ = susOpenV;      drawLayer(texOpenSustain_.id, ch::BLEND_SPRITE);
    v_ = susOpenBodyGlowV; drawGlowLayer(texOpenSustain_.id);
    v_ = susBodyV;      drawLayer(texSustain_.id, ch::BLEND_SPRITE);   // -1000
    v_ = susBodyGlowV;  drawGlowLayer(texSustain_.id);                  // the bGlow pass

    struct FretBuckets {
        std::vector<ch::Vtx> base, lift, cover, head, headCover, light, half;
        std::vector<ch::Vtx> openHead[3];   // by FretLane::head6
    };
    FretBuckets low, top;
    for (int lane = 0; lane < (hidePlayfield ? 0 : 5); ++lane) {
        float popY = 0.0f;
        bool held = false;
        bool openPop = false;
        if (!o.noBot) {
            const float dt = float(lastHit(lane, songTime));
            const float dtOpen = float(lastOpenHit(songTime));
            // Fret_Animator.cs:34-38 -- Play(isSustainNote) sets isSustaining
            // and snaps to maxHeight, and :70's `if (!isSustaining)` gates the
            // descent, so a fret holding a sustain stays FROZEN at the top for
            // the whole sustain instead of falling back over 0.136s.
            //
            // That also means fretOnTop() is true for the duration, i.e. the
            // fret stack sits in front of the notes throughout -- which is CH's
            // behaviour, not an artifact. And isHeld is what gates the
            // headLight (:110), so the light stays on too.
            const bool sustaining = laneSustaining[lane];
            // An open note raises this fret too, so the pop takes whichever
            // happened more recently. `held` below deliberately does NOT --
            // see buildHitTimes.
            popY = ch::fretPopY(fminf(dt, dtOpen), sustaining, sustaining);
            // The bot must hold the lane to hit the note at all, so the
            // headLight is lit for the duration of the pop.
            held = sustaining || (dt >= 0.0f && dt < ch::POP_T1);
            // The open head shows while THIS pop is the open one and the fret
            // is not otherwise held -- `if (isHeld) openNotePlayed = false`.
            openPop = !held && dtOpen >= 0.0f && dtOpen < ch::POP_T1 &&
                      dtOpen <= dt;
        }
        FretBuckets& B = ch::fretOnTop(popY) ? top : low;
        // The open head is a PIECE OF THIS FRET, so it has to ride every
        // per-lane transform below with the rest of the stack -- the mod
        // displacement, tiny's zoom and column pull, and dark's fade. Left
        // out of this list it stayed full-size and full-bright while the
        // fret it belongs to moved and shrank. Pointing at the bucket even
        // when openPop is false is harmless: nothing was appended, so every
        // loop below spans zero vertices.
        std::vector<ch::Vtx>* bk[8] = {&B.base, &B.lift, &B.cover, &B.head,
                                       &B.headCover, &B.light, &B.half,
                                       &B.openHead[ch::FRETS[lane].head6]};
        size_t n0[8];
        for (int i = 0; i < 8; ++i) n0[i] = bk[i]->size();
        ch::buildFret(B.base, B.lift, B.cover, B.head, B.headCover,
                       B.light, B.half, lane, popY, held, mods.flip,
                       openPop ? &B.openHead[ch::FRETS[lane].head6] : nullptr);
        // The receptor rule, ReceptorArrowRow.cpp:46-54: a receptor is an
        // arrow evaluated at fYOffset = 0. Which mods move frets is a
        // CONSEQUENCE: tornado's term is identically 0 there (cos(acos(b))==b)
        // and ITG's bumpy is sin(0)==0; drunk/flip/invert/beat/tipsy displace.
        // NotClon's bumpyOffset extension makes bumpy nonzero at 0 -- accepted,
        // it is our knob. Guarded so the all-zero case never touches a vertex.
        float wdxPx = GetXPos(mods, lane, 0.0f, songTime, float(beat), bpm);
        // buildFret applies flip on CH's authored 0.2-wide fret ladder so 100%
        // lands on the exact lefty positions. Remove GetXPos's 0.1935-wide
        // note-ladder contribution here; every other ArrowEffects term stays.
        if (mods.flip != 0.0f)
            wdxPx -= (laneXPixels(4 - lane) - laneXPixels(lane)) * mods.flip;
        const float wdx = pxToUnits(wdxPx);
        const float wdy = pxToUnits(GetYPosBump(mods, lane, 0.0f, float(beat), bpm)) * 0.5f;
        const float wdz = ApplyScrollZ(mods,0.0f,lane);
        // tiny, the receptor rule continued: an arrow at fYOffset = 0 zooms by
        // 0.5^tiny like any other (ArrowEffects.cpp:813-818, per-column
        // tiny0..4 included), about the fret's own anchor -- buildFret's x on
        // the flip-lerped fret ladder, pivot y 0. The column pull-together
        // (:561-566) then contracts the fret's offset-from-centre the same way
        // the note path contracts cx, so a shrunk field keeps its gems on its
        // frets. Guarded so the all-zero case never touches a vertex.
        const float fzoom = GetZoom(mods, lane);
        float fx = ch::FRET_X[lane];
        if (mods.flip != 0.0f && lane != 2)
            fx += (ch::FRET_X[4 - lane] - fx) * mods.flip;
        const float pull = mods.tiny != 0.0f
            ? (fx + wdx) * (GetTinyColScale(mods) - 1.0f) : 0.0f;
        if (fzoom != 1.0f || pull != 0.0f ||
            wdx != 0.0f || wdy != 0.0f || wdz != 0.0f)
            for (int i = 0; i < 8; ++i)
                for (size_t j = n0[i]; j < bk[i]->size(); ++j) {
                    ch::Vtx& vtx = (*bk[i])[j];
                    if (fzoom != 1.0f) {
                        vtx.x = fx + (vtx.x - fx) * fzoom;
                        vtx.y *= fzoom;
                    }
                    vtx.x += wdx + pull;
                    vtx.y += wdy;
                    vtx.z += wdz;
                }
        // dark. The only renderer consumer of m_fDark in the whole OITG tree
        // is ReceptorArrowRow::Update (ReceptorArrowRow.cpp:40-43): receptors
        // -- and only receptors -- fade by fBaseAlpha = clamp01(1 - dark).
        // The fret stack is NotClon's receptor. The five BLEND_SPRITE layers
        // scale vertex alpha (SCENE_FS premultiplies, so RGB fades with it);
        // headLight is BLEND_ADD ONE/ONE where alpha does not participate, so
        // its RGB scales by the same factor instead -- which is what fading
        // an additive layer means. Guarded so 0 never touches a vertex.
        if (mods.dark != 0.0f) {
            const float da = fminf(1.0f, fmaxf(0.0f, 1.0f - mods.dark));
            for (int i = 0; i < 8; ++i)
                for (size_t j = n0[i]; j < bk[i]->size(); ++j) {
                    if (bk[i] == &B.light) {
                        (*bk[i])[j].r *= da;
                        (*bk[i])[j].g *= da;
                        (*bk[i])[j].b *= da;
                    } else {
                        (*bk[i])[j].a *= da;
                    }
                }
        }
    }
    // Split so the sustain glow can land between fret base and lift. Unity
    // breaks the -999 tie between fret base and sustain glow by camera
    // distance: the fret transform is at FRET_Z 0.09 and a held sustain root at
    // z 0, so with the camera at -3.98 the fret is 4.07 away and the glow 3.98
    // -- the fret draws first. Two lambdas called back to back cannot change
    // anything on their own.
    auto drawFretBase = [&](FretBuckets& B) {
        v_ = B.base;      drawLayer(texFretB_.id, ch::BLEND_SPRITE);
    };
    auto drawFretRest = [&](FretBuckets& B) {
        v_ = B.lift;      drawLayer(texLift_.id,  ch::BLEND_SPRITE);
        v_ = B.cover;     drawLayer(texFretB_.id, ch::BLEND_SPRITE);
        v_ = B.head;      drawLayer(texFretH_.id, ch::BLEND_SPRITE);
        // Same slot as the head it replaces; no headCover follows it.
        for (int i = 0; i < 3; ++i) {
            v_ = B.openHead[i]; drawLayer(texFretOpen_[i].id, ch::BLEND_SPRITE);
        }
        v_ = B.headCover; drawLayer(texFretH_.id, ch::BLEND_SPRITE);
        v_ = B.light;     drawLayer(texHLight_.id, ch::BLEND_ADD);
        v_ = B.half;      drawLayer(texFretB_.id, ch::BLEND_SPRITE);
    };
    drawFretBase(low);
    v_ = susGlowV;  drawSustainGlow();          // -999, additive, held only
    v_ = susOpenGlowV; drawSustainGlow();       // the open ribbon's, same pass
    drawFretRest(low);

    // Notes, far -> near. Body -> Anim -> Head live in different atlases, so
    // they are drawn per note rather than batched: order within a note matters
    // when notes stack.
    const float NW = 128.0f / ch::PPU_NOTES * ch::GEM_SCALE;
    const float NH =  64.0f / ch::PPU_NOTES * ch::GEM_SCALE;
    const float AH = 128.0f / ch::PPU_NOTES * ch::GEM_SCALE;
    const float OW = 512.0f / ch::PPU_NOTES * ch::GEM_SCALE;
    int animFrame = int(songTime * 20.0f) & 15;   // NoteAnimator: 20fps, 16 frames

    // ITG's mods work in pixels against a 480px screen, so world z is converted
    // into that space and back. This constant sets the on-screen wavelength of
    // every periodic mod -- see AGENTS.md.
    const float Y_PER_UNIT = ARROW_SIZE * 1.6f;

    // tiny: per-note zoom 0.5^tiny plus the column pull-together (SM5
    // ArrowEffects.cpp:813-818 and :561-566). Both constant per frame. cx is
    // exactly SM5's fPixelOffsetFromCenter -- offset from highway centre
    // including mod dx -- so the lateral contraction multiplies it whole.
    const float tinyCol  = GetTinyColScale(mods);

    for (int i = hidePlayfield ? -1 : int(chart.notes.size()) - 1; i >= 0; --i) {
        const Note& n = chart.notes[i];
        const float z0 = float(ssec(chart.beatToSec(n.beat)) - scrollNow) * noteSpeed;
        // Inside a starpower phrase the gem is drawn with CH's star sheets.
        const bool isSP =
            chart.phraseAt(PhraseType::StarPower, n.tick) != nullptr;

        const float y0 = z0*Y_PER_UNIT;
        const float nearCull = o.noBot
                             ? ch::NOTE_CULL_NEAR*(1.0f+mods.drawSizeBack)
                             : 0.0f;
        auto laneState = [&](int lane,float& yOff,float& zDraw,
                             float& alpha,float& glow) {
            yOff = ApplyYMods(mods,lane,y0,float(beat));
            const float z = yOff == y0 ? z0 : yOff/Y_PER_UNIT;
            zDraw = ApplyScrollZ(mods,z,lane);
            if (zDraw > ch::NOTE_CULL_FAR || zDraw < -3.0f || z <= nearCull)
                return false;
            const float fade = fminf(1.0f,fmaxf(0.0f,
                (ch::NOTE_CULL_FAR-zDraw)/ch::NOTE_FADE_LEN));
            alpha = fade*GetAlpha(mods,yOff,songTime);
            glow = fade*GetGlow(mods,yOff,songTime);
            return alpha > 0.0f || glow > 0.0f;
        };

        if (n.open) {
            float yOff, zDraw, alpha, noteGlow;
            if (laneState(2,yOff,zDraw,alpha,noteGlow)) {
            const float rotX = -GetRotationX(mods,yOff);
            const float rotY = GetRotationY(mods,yOff);
            const float rotZ = -GetRotationZ(mods,float(n.beat),float(beat));
            const float noteZoom = GetZoom(mods, 2);
            float dx = pxToUnits(GetXPos(mods, 2, yOff, songTime, float(beat), bpm));
            if (mods.tiny != 0.0f) dx *= tinyCol;
            // open order is Body -> Head -> Anim, unlike standard notes.
            // Inside a starpower phrase CH keeps the same body and head but
            // tints the body its StarPower cyan (0,1,1) and adds the animated
            // SP highlight over the top (SetOpenNoteState).
            ch::quadUpRot(v_, dx, 0.0f, zDraw,
                          -OW*0.5f, -ch::NOTE_PIVOT_Y*NH,
                           OW*0.5f, (1.0f-ch::NOTE_PIVOT_Y)*NH,
                          rotX, rotY, rotZ, noteZoom,
                          0.0f,0.0f,0.2f,1.0f,
                          isSP ? 0.0f : 1.0f, 1.0f, 1.0f, alpha);
            drawLayer(texOpen_.id, ch::BLEND_SPRITE, noteGlow);
            ch::quadUpRot(v_, dx, 0.0f, zDraw,
                          -OW*0.5f, -ch::NOTE_PIVOT_Y*NH,
                           OW*0.5f, (1.0f-ch::NOTE_PIVOT_Y)*NH,
                          rotX, rotY, rotZ, noteZoom,
                          0.2f,0.0f,0.4f,1.0f,
                          ch::NOTE_TINT[5][0], ch::NOTE_TINT[5][1],
                          ch::NOTE_TINT[5][2], alpha);
            drawLayer(texOpen_.id, ch::BLEND_SPRITE, noteGlow);
            // Anim: EVERY open note carries an animated highlight, not just
            // starpower ones (SetOpenNoteState assigns openBodyAnimationSprites
            // unconditionally and only SWAPS in the SP sheet inside the
            // IsStarPower branch). Both sheets are a 4x4 grid of 512x64 frames
            // on the same 16-frame 20fps clock as the note anim. Frame i sits
            // at column i%4; rows run TOP-DOWN in sprite order, and with flipY
            // loading the top PNG row is v=1 -- hence 0.75 - row.
            {
                const int col = animFrame % 4, row = animFrame / 4;
                const float u0 = col * 0.25f, v0 = 0.75f - row * 0.25f;
                ch::quadUpRot(v_, dx, 0.0f, zDraw,
                              -OW*0.5f, -ch::NOTE_PIVOT_Y*NH,
                               OW*0.5f, (1.0f-ch::NOTE_PIVOT_Y)*NH,
                              rotX, rotY, rotZ, noteZoom,
                              u0, v0, u0 + 0.25f, v0 + 0.25f,
                              isSP ? 0.0f : 1.0f, 1.0f, 1.0f, alpha);
                // The two sheets blend differently and the art says so: the SP
                // one carries real alpha (mean 0.08) and composites, while the
                // plain one is fully opaque black with a bright streak (mean
                // alpha 1) -- an additive glow. Alpha-blending that one paints
                // a black bar over the note.
                drawLayer((isSP ? texSpOpen_ : texOpenAnim_).id,
                          isSP ? ch::BLEND_SPRITE : ch::BLEND_ADD, noteGlow);
            }
            // Open_HOPO: a FOURTH layer, on top of the anim, drawn only for a
            // pure HOPO -- frame 3 of the open strip (bodySprites[8]). Without
            // it an open HOPO is indistinguishable from an open strum.
            if (n.openType == NoteType::Hopo) {
                ch::quadUpRot(v_, dx, 0.0f, zDraw,
                              -OW*0.5f, -ch::NOTE_PIVOT_Y*NH,
                               OW*0.5f, (1.0f-ch::NOTE_PIVOT_Y)*NH,
                              rotX, rotY, rotZ, noteZoom,
                              0.6f,0.0f,0.8f,1.0f,
                              1.0f, 1.0f, 1.0f, alpha);
                drawLayer(texOpen_.id, ch::BLEND_SPRITE, noteGlow);
            }
            }
        }

        for (int lane = 0; lane < 5; ++lane) {
            if (!(n.frets & (1 << lane))) continue;
            float yOff, zDraw, alpha, noteGlow;
            if (!laneState(lane,yOff,zDraw,alpha,noteGlow)) continue;
            const float rotX = -GetRotationX(mods,yOff);
            const float rotY = GetRotationY(mods,yOff);
            const float rotZ = -GetRotationZ(mods,float(n.beat),float(beat));
            // dizzyholds: a hold's head does not spin unless asked
            // (ArrowEffects.cpp:906). Identical to rotZ when the lane carries
            // no sustain or dizzy is off, so this costs nothing by default.
            const float rotZL = (n.sustain[lane] > 0.0)
                ? -GetRotationZ(mods, float(n.beat), float(beat), true)
                : rotZ;
            const float noteZoom = GetZoom(mods, lane);
            float px2 = GetXPos(mods, lane, yOff, songTime, float(beat), bpm);
            float bump = GetYPosBump(mods, lane, yOff, float(beat), bpm);
            float cx = ch::noteX(lane) + pxToUnits(px2);
            if (mods.tiny != 0.0f) cx *= tinyCol;   // SM5 :561-566
            float by = pxToUnits(bump) * 0.5f;

            // swaptint. Not an ITG mod: ITG arrows carry a direction glyph so a
            // displaced note stays readable, and a CH gem carries only its lane
            // colour -- so under flip/invert a green gem sits over the orange
            // fret and the swap is close to unreadable. Lerp the gem toward the
            // colour of the lane it has actually landed on, by percent. At 0
            // this is `tint = NOTE_TINT[lane]` exactly, so it is inert.
            const float* tintN = ch::NOTE_TINT[lane];
            const float* tintA = ch::ANIM_TINT[lane];
            float tintNBuf[3], tintABuf[3];
            if (mods.swaptint != 0.0f) {
                int over = int(floorf((cx + 0.387f) / 0.1935f + 0.5f));
                over = over < 0 ? 0 : (over > 4 ? 4 : over);
                if (over != lane) {
                    const float k = mods.swaptint;
                    for (int i = 0; i < 3; ++i) {
                        tintNBuf[i] = ch::NOTE_TINT[lane][i] +
                                      (ch::NOTE_TINT[over][i] - ch::NOTE_TINT[lane][i]) * k;
                        tintABuf[i] = ch::ANIM_TINT[lane][i] +
                                      (ch::ANIM_TINT[over][i] - ch::ANIM_TINT[lane][i]) * k;
                    }
                    tintN = tintNBuf; tintA = tintABuf;
                }
            }

            // Pivot is (cx, by, zDraw); local extents are asymmetric in y
            // because the sprite pivot is 0.16 (0.138 for the anim quad --
            // same world pivot point, its own extents).
            const float hx0 = -NW*0.5f, hx1 = NW*0.5f;
            const float hy0 = -ch::NOTE_PIVOT_Y*NH, hy1 = (1.0f-ch::NOTE_PIVOT_Y)*NH;
            const float ha0 = -ch::ANIM_PIVOT_Y*AH, ha1 = (1.0f-ch::ANIM_PIVOT_Y)*AH;

            const bool isTap  = (n.type == NoteType::Tap);
            const bool isHopo = (n.type == NoteType::Hopo);

            if (isSP) {
                // A starpower-phrase note is the same three-layer composite
                // with CH's star sheets (NoteContainer.SetStandardNoteState):
                // animated star body in the lane colour, the bottom layer at
                // CH's fixed StarPowerAnim tint (0.321, 1, 1) -- it REPLACES
                // the per-lane anim colour, not multiplies it -- and the star
                // cap on top. Unlike plain taps, SP taps keep all three
                // layers; the tap difference is which body sheet animates.
                // Star sheets run on the same 16-frame 20fps clock as the
                // note anim: CH has a separate star clock, but both default
                // to 20fps/16 so the values are identical.
                float au0 = float(animFrame)/16.0f, au1 = float(animFrame+1)/16.0f;
                ch::quadUpRot(v_, cx, by, zDraw, hx0, hy0, hx1, hy1,
                              rotX, rotY, rotZL, noteZoom, au0,0.0f,au1,1.0f,
                              tintN[0], tintN[1], tintN[2], alpha);
                drawLayer((isTap ? texStarBodyTap_ : texStarBody_).id,
                          ch::BLEND_SPRITE, noteGlow);

                ch::quadUpRot(v_, cx, by, zDraw, hx0, hy0, hx1, hy1,
                              rotX, rotY, rotZL, noteZoom, au0,0.0f,au1,1.0f,
                              0.321f, 1.0f, 1.0f, alpha);
                drawLayer(texStarBottom_.id, ch::BLEND_SPRITE, noteGlow);

                // Cap: frame 0 strum/tap, frame 2 HOPO -- the prefab wires
                // bodySprites[4]/[5] to those frames and skips the odd ones.
                float su0 = isHopo ? 0.4f : 0.0f, su1 = isHopo ? 0.6f : 0.2f;
                ch::quadUpRot(v_, cx, by, zDraw, hx0, hy0, hx1, hy1,
                              rotX, rotY, rotZL, noteZoom, su0,0.0f,su1,1.0f,
                              1,1,1, alpha);
                drawLayer(texStarCap_.id, ch::BLEND_SPRITE, noteGlow);
            } else {
            // Body: frame 2 strum/HOPO, frame 3 tap. Tinted per fret.
            float bu0 = isTap ? 0.6f : 0.4f, bu1 = isTap ? 0.8f : 0.6f;
            ch::quadUpRot(v_, cx, by, zDraw, hx0, hy0, hx1, hy1,
                          rotX, rotY, rotZL, noteZoom, bu0,0.0f,bu1,1.0f,
                          tintN[0], tintN[1], tintN[2], alpha);
            drawLayer(texNotes_.id, ch::BLEND_SPRITE, noteGlow);

            if (!isTap) {   // taps have no anim layer
                float au0 = float(animFrame)/16.0f, au1 = float(animFrame+1)/16.0f;
                ch::quadUpRot(v_, cx, by, zDraw, hx0, ha0, hx1, ha1,
                              rotX, rotY, rotZL, noteZoom, au0,0.0f,au1,1.0f,
                              tintA[0], tintA[1], tintA[2], alpha);
                drawLayer(texAnim_.id, ch::BLEND_SPRITE, noteGlow);
            }

            // Head: frame 0 strum/tap (alt_taps defaults false), frame 1 HOPO
            float hu0 = isHopo ? 0.2f : 0.0f, hu1 = isHopo ? 0.4f : 0.2f;
            ch::quadUpRot(v_, cx, by, zDraw, hx0, hy0, hx1, hy1,
                          rotX, rotY, rotZL, noteZoom, hu0,0.0f,hu1,1.0f,
                          1,1,1, alpha);
            drawLayer(texNotes_.id, ch::BLEND_SPRITE, noteGlow);
            }
        }
    }

    drawFretBase(top); drawFretRest(top);   // popped frets, over the notes

    if (piuMode) {
        Mat4 piuDrawMvp{};
        if (piuMvpOverride) {
            piuDrawMvp = *piuMvpOverride;
        } else {
            const float virtW = logicalScreenWidth(W, H);
            const float halfW = virtW * 0.5f;
            Mat4 projection{};
            projection.m[0] = 1.0f / halfW;
            projection.m[5] = -1.0f / 240.0f;
            projection.m[10] = projection.m[15] = 1.0f;
            projection.m[12] = -1.0f;
            projection.m[13] = 1.0f;

            Mat4 placement{};
            placement.m[0] = placement.m[5] = placement.m[10] =
                placement.m[15] = 1.0f;
            placement.m[12] = virtW * (float(vpX) + float(vpW) * 0.5f) /
                              float(W);
            placement.m[13] = 240.0f;
            piuDrawMvp = mat_mul(
                projection,
                mat_mul(placement, piuFieldLocal(mods, beat, halfW)));
        }
        drawPiu(chart, beat, o, mods, songTime, scrollNow, noteSpeed, bpm,
                piuDrawMvp);
    }

    if (engineT > 0.0f) {
        for (int i = 0; i < 4; ++i) fieldTint_[i] = originalTint[i];
        drawEngine(chart, beat, o, mods, songTime, scrollNow, noteSpeed, bpm,
                   engineStyle, engineT, mx, my, mz, vpX, vpW,
                   engineMvpOverride);
    }

    if (gh3T > 0.0f) {
        for (int i = 0; i < 4; ++i) fieldTint_[i] = originalTint[i];
        // GH3's authored space is 1280x720, y down (strike line at y=655,
        // screen centre x=640). Map it across this player's viewport strip,
        // the same shape as the PIU projection above.
        // Square pixels regardless of the strip: the vscreen maps at the
        // FULL framebuffer's scale and x=640 lands on the strip's centre --
        // fe76165's rule that two-player fields keep single-player pixel
        // proportions, the same shape as the PIU projection above. The old
        // vpW/W factor squeezed the whole vscreen into the half strip, which
        // is exactly the 2P stretch the engine styles once had. Reduces to
        // the old matrix verbatim when the strip is the whole screen.
        Mat4 gh3Proj{};
        gh3Proj.m[0] = 2.0f / 1280.0f;
        gh3Proj.m[5] = -2.0f / 720.0f;
        gh3Proj.m[10] = gh3Proj.m[15] = 1.0f;
        gh3Proj.m[12] = 2.0f * (float(vpX) + float(vpW) * 0.5f) / float(W)
                        - 1.0f - 640.0f * (2.0f / 1280.0f);
        gh3Proj.m[13] = 1.0f;
        // Whole-field movex/movey (mx/my, world units -- the offsets the CH
        // field adds through its uOffset uniform) ride the field matrix,
        // converted at the strike line's scale: 1 world unit = 64/0.1935 SM
        // px, x1.6 into the vscreen. World +y is up, the vscreen's is down.
        // movez has no axis here, like the rest of the field's flat-2D
        // exceptions.
        const float w2v = 1.6f / pxToUnits(1.0f);
        Mat4 gh3Move{};
        gh3Move.m[0] = gh3Move.m[5] = gh3Move.m[10] = gh3Move.m[15] = 1.0f;
        gh3Move.m[12] = (o.px + mx) * w2v;
        gh3Move.m[13] = -(o.py + my) * w2v;
        // An actor player source supplies its own projection*model*local (the
        // piuMvp treatment). tilt/wag/mini are no longer a matrix here --
        // they are the real 3D camera applied per vertex in gh3CamVtx.
        const Mat4 gh3DrawMvp =
            mat_mul(gh3MvpOverride ? *gh3MvpOverride : gh3Proj, gh3Move);
        drawGh3(chart, beat, o, mods, songTime, scrollNow, noteSpeed, bpm,
                gh3DrawMvp);
    }

    if (taikoT > 0.0f) {
        for (int i = 0; i < 4; ++i) fieldTint_[i] = originalTint[i];
        // The skin's authored space is 1920x1080, y down. Mapped across this
        // player's viewport strip at the FULL framebuffer's scale, with the
        // lane's own centre x landing on the strip's centre -- fe76165's rule
        // that two-player fields keep single-player pixel proportions, the
        // same shape as the PIU and GH3 projections above. Reduces to the
        // plain 1P matrix when the strip is the whole screen.
        const float TCX = ch::TAIKO_VW * 0.5f;
        Mat4 tProj{};
        tProj.m[0] = 2.0f / ch::TAIKO_VW;
        tProj.m[5] = -2.0f / ch::TAIKO_VH;
        tProj.m[10] = tProj.m[15] = 1.0f;
        tProj.m[12] = 2.0f * (float(vpX) + float(vpW) * 0.5f) / float(W)
                      - 1.0f - TCX * (2.0f / ch::TAIKO_VW);
        tProj.m[13] = 1.0f;
        // Whole-field movex/movey in world units, converted at the strike
        // line's scale the way the GH3 field does it. World +y is up, the
        // vscreen's is down; movez has no axis on a flat field.
        const float w2v = ch::TAIKO_SCALE / pxToUnits(1.0f);
        Mat4 tMove{};
        tMove.m[0] = tMove.m[5] = tMove.m[10] = tMove.m[15] = 1.0f;
        tMove.m[12] = (o.px + mx) * w2v;
        tMove.m[13] = -(o.py + my) * w2v;
        const Mat4 tDrawMvp = mat_mul(tProj, tMove);
        drawTaiko(chart, beat, o, mods, songTime, scrollNow, noteSpeed, bpm,
                  tDrawMvp);
    }

    if (bmsT > 0.0f) {
        for (int i = 0; i < 4; ++i) fieldTint_[i] = originalTint[i];
        // LR2's authored space is 640x480, y down, mapped across this player's
        // viewport strip at the FULL framebuffer's scale -- the same shape as
        // the PIU, GH3 and taiko projections. The lane strip sits at the LEFT
        // of that vscreen in the real game, which would put it off in the
        // margin here, so the field is centred on the strip instead: its own
        // midpoint (BMS_FIELD_X + BMS_FIELD_W/2) lands on the strip's centre.
        const float BCX = ch::BMS_FIELD_X + ch::BMS_FIELD_W * 0.5f;
        // SQUARE PIXELS. LR2's vscreen is 640x480, which is 4:3; mapping 640
        // across the full framebuffer width AND 480 across its full height
        // stretches that 4:3 layout onto a 16:9 frame, and the lane comes out
        // visibly squashed. StepMania's convention -- which drawPiu already
        // uses -- fixes the HEIGHT at 480 and lets the virtual width be
        // whatever the output aspect calls for, so one vscreen pixel is H/480
        // framebuffer pixels on both axes. BMS_VW is therefore the authored
        // 4:3 width, used for placement, never as the projection's x span.
        const float virtW = logicalScreenWidth(W, H);
        Mat4 bProj{};
        bProj.m[0] = 2.0f / virtW;
        bProj.m[5] = -2.0f / ch::BMS_VH;
        bProj.m[10] = bProj.m[15] = 1.0f;
        bProj.m[12] = 2.0f * (float(vpX) + float(vpW) * 0.5f) / float(W)
                      - 1.0f - BCX * (2.0f / virtW);
        bProj.m[13] = 1.0f;
        // Whole-field movex/movey in world units, converted at the judge
        // line's scale the way the gh3 and taiko fields do it. World +y is up,
        // the vscreen's is down; movez has no axis on a flat field.
        const float w2v = ch::BMS_SCALE / pxToUnits(1.0f);
        Mat4 bMove{};
        bMove.m[0] = bMove.m[5] = bMove.m[10] = bMove.m[15] = 1.0f;
        bMove.m[12] = (o.px + mx) * w2v;
        bMove.m[13] = -(o.py + my) * w2v;
        drawBms(chart, beat, o, mods, songTime, scrollNow, noteSpeed, bpm,
                mat_mul(bProj, bMove));
    }

}

void Renderer::drawFrame(const Chart& chart, double beat, const RenderOpts& o,
                         GLuint postTarget) {
    const double nowSec   = chart.beatToSec(beat);
    const float  songTime  = float(nowSec);
    // The scroll axis. Identical to songTime unless the chart carries ncstop
    // markers, in which case it freezes during each stop while the audio (and
    // every time-driven effect: mod phases, fret anims) runs on -- the SM stop
    // look. ssec() is the identity when there are no stops, RETURNING THE SAME
    // DOUBLE, so every subtraction below is bit-identical to the old code and
    // the pinned hashes stay pinned.
    const bool  hasStops  = !chart.stops.empty();
    const float scrollNow = hasStops ? float(chart.scrollSec(nowSec)) : songTime;
    auto ssec = [&](double t) { return hasStops ? chart.scrollSec(t) : t; };
    const float bpm = float(chart.bpmAt(beat));
    actorBeat_ = beat;                     // the actor effect clock
    fieldSec_ = songTime;                  // live Lua PlayerOptions clock
    // Canonical SM5 Lua mutates ModsLevel_Song from its self-queued update
    // command. Step it before evaluating either field so this frame sees the
    // same PlayerOptions values the actor commands just produced.
    if (actors_) {
        actors_->setAutoplay(!o.noBot);
        actors_->pump(*this, songTime);
    }
    const int actorPlayers = actors_ ? actors_->livePlayerCount() : 0;
    FieldEval E = evalField(chart, beat, o, o.doc, 1);
    const bool twoFields = o.doc2 != nullptr || actorPlayers > 1;
    FieldEval E2;
    if (twoFields) E2 = evalField(chart, beat, o, o.doc2 ? o.doc2 : o.doc, 2);
    actorChart_ = &chart;
    actorOpts_ = &o;
    actorFields_[0] = &E;
    actorFields_[1] = twoFields ? &E2 : nullptr;
    actorScrollNow_ = scrollNow;
    actorSongTime_ = songTime;
    actorNowSec_ = nowSec;
    actorBpm_ = bpm;
    Mods& mods = E.mods; PostFx& fx = E.fx;
    float& mx = E.mx; float& my = E.my; float& mz = E.mz;
    float (&bgKnobs)[MAX_BG_UNIFORMS] = E.bgKnobs;
    unsigned& bgDriven = E.bgDriven;
    float& fieldX = E.fieldX; float& fieldY = E.fieldY;
    Mat4& mvpEff = E.mvpEff;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDepthMask(GL_FALSE);

    // #BGCHANGES media (stills / movies / shaders): the true back plane, drawn
    // into fbo_ so post processes it -- NotITG's layout.xml captures background
    // + playfields and runs post.frag on the result. Keys off nowSec, the
    // AUDIO time, never scrollNow: stops freeze the scroll axis, not the film.
    // Null bg_ skips the pass entirely, which is what keeps the pinned hashes
    // pinned (the baselines render REM III, which has no .sm).
    if (bg_) {
        // `field`: the strike line's centre in imageCoord space (y down), for
        // shader layers' corridor masks. Projected through mvpEff -- the SAME
        // tilt/mini/wag transform the columns get, which is why this cannot
        // use mvp_ -- plus the uOffset displacement, at STRIKE_Z = 0
        // (devdocs/spec/background.md section 2.5, corrected for mvpEff).
        const float ox = o.px + mx, oy = o.py + my;
        const float oz = ch::STRIKE_Z + o.pz + mz;
        const float* mm = mvpEff.m;
        const float cx = mm[0]*ox + mm[4]*oy + mm[8]*oz  + mm[12];
        const float cy = mm[1]*ox + mm[5]*oy + mm[9]*oz  + mm[13];
        const float cw = mm[3]*ox + mm[7]*oy + mm[11]*oz + mm[15];
        if (cw != 0.0f) { fieldX = cx/cw*0.5f + 0.5f; fieldY = 0.5f - cy/cw*0.5f; }
        bg_->draw(W, H, nowSec, float(beat), bpm, fieldX, fieldY,
                  bgKnobs, bgDriven);
    }
    // #BGCHANGES actor folders: behind the highway, over the media layer.
    // Media entries and folder entries are one SM layer; NotClon splits them
    // by handler, and actor trees composite over the media plane.
    if (actors_) actors_->drawBackground(*this, songTime);

    // cover: the BrightnessOverlay. It is a child of the Background actor
    // (Background.cpp:225 AddChild(&m_Brightness), added after every layer), so
    // it darkens the whole background plane -- media AND background actor trees
    // -- and nothing in front of it. That is exactly here: after both background
    // passes, before the first highway quad. Skipped entirely at 0, which is
    // what keeps the pinned hashes pinned.
    if (mods.cover != 0.0f) {
        glUseProgram(cover_);
        glBindVertexArray(qvao_);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   // Actor's BLEND_NORMAL
        glUniform1f(glGetUniformLocation(cover_, "uAlpha"), mods.cover);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // A chart that touches Player/NoteField state draws the same centred local
    // sources used by ActorProxy, preserving target visibility and every Player
    // transform. Other charts keep the direct path and pinned output.
    const int fieldW = twoFields ? W / 2 : W;
    const bool drewP1 = actorPlayers > 0 &&
                        actors_->drawPlayer(*this, 1, songTime, beat);
    if (!drewP1)
        drawField(chart, beat, o, E, 0, fieldW,
                  scrollNow, songTime, nowSec, bpm);
    if (twoFields) {
        const bool drewP2 = actorPlayers > 1 &&
                            actors_->drawPlayer(*this, 2, songTime, beat);
        if (!drewP2)
            drawField(chart, beat, o, E2, fieldW, W - fieldW,
                      scrollNow, songTime, nowSec, bpm);
    }
    glViewport(0, 0, W, H);
    // #FGCHANGES actor folders: in front of the playfield. Drawn INSIDE fbo_,
    // so the post chain processes them -- which is what NotITG's layout.xml
    // does (its captured region spans background + playfields, and post.frag
    // runs on the result).
    if (actors_) actors_->drawForeground(*this, songTime);
    actorChart_ = nullptr;
    actorOpts_ = nullptr;
    actorFields_[0] = actorFields_[1] = nullptr;

    // ---- playfield shaders -------------------------------------------------
    // A --fxshader layer runs over the finished frame, so it can warp or fold
    // the playfield itself rather than draw behind it. It cannot read and
    // write one texture, hence the bounce through fxFbo_. Skipped entirely
    // when no scene layer exists, which is what keeps this hash-neutral.
    GLuint sceneTex = colorTex_;
    if (bg_ && bg_->hasScenePass()) {
        glBindFramebuffer(GL_FRAMEBUFFER, fxFbo_);
        glViewport(0, 0, W, H);
        glClear(GL_COLOR_BUFFER_BIT);
        // 1 - fieldY: a scene pass renders through the flipped VS (see
        // background.cpp), so its imageCoord runs the other way in y. Handing
        // it the background layers' fieldY would put the strike line at its
        // mirror image, and every effect that centres on the field -- pinch,
        // corridor masks -- would key off the wrong end of the screen.
        bg_->drawScene(colorTex_, W, H, nowSec, float(beat), bpm,
                       fieldX, 1.0f - fieldY, bgKnobs, bgDriven);
        sceneTex = fxTex_;
    }
    // A --fxchain runs after any --fxshader and owns its own buffers, so it
    // needs no bounce: it returns whichever of them holds the result.
    if (bg_ && bg_->hasChain())
        sceneTex = bg_->drawChain(sceneTex, W, H, nowSec, float(beat), bpm,
                                  fieldX, 1.0f - fieldY, bgKnobs, bgDriven);

    // ---- post ------------------------------------------------------------
    glBindFramebuffer(GL_FRAMEBUFFER, postTarget);
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(post_);
    glBindVertexArray(qvao_);
    glBlendFunc(GL_ONE, GL_ZERO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneTex);
    glUniform1i(glGetUniformLocation(post_, "uTex"), 0);
    glUniform1f(glGetUniformLocation(post_, "uTime"), songTime);
    glUniform1f(glGetUniformLocation(post_, "uAberration"), fx.aberration);
    glUniform1f(glGetUniformLocation(post_, "uGlow"), fx.glow);
    glUniform1f(glGetUniformLocation(post_, "uVignette"), fx.vignette);
    glUniform1f(glGetUniformLocation(post_, "uDesat"), fx.desat);
    glUniform1f(glGetUniformLocation(post_, "uShake"), fx.shake);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace nc
