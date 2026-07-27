// Implementation of the shared render core. See renderer.h for why it exists.

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

#include "renderer.h"
#include "actor.h"
#include "background.h"

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
uniform mat4 uMVP;
// Rigid-body offset for the whole playfield. With --playfield the board is
// gone, so nothing anchors the frets to the bottom of the frame and the
// assembly can be placed anywhere. A modchart will drive this later.
uniform vec3 uOffset;
void main() {
    vUV = aUV; vCol = aCol;
    gl_Position = uMVP * vec4(aPos + uOffset, 1.0);
}
)";

const char* SCENE_FS = R"(#version 330
in vec2 vUV;
in vec4 vCol;
out vec4 oCol;
uniform sampler2D uTex;
uniform float uPremul;   // 1 for Unity sprite blending (One, 1-SrcAlpha)
void main() {
    vec4 c = texture(uTex, vUV) * vCol;
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
out vec2 vUV; out vec4 vCol;
uniform float uDepth;
void main() {
    vUV = aUV; vCol = aCol;
    gl_Position = vec4(aPos.x / 320.0 - 1.0, 1.0 - aPos.y / 240.0, uDepth, 1.0);
}
)";

const char* ACTOR_FS = R"(#version 330
in vec2 vUV; in vec4 vCol;
out vec4 oCol;
uniform sampler2D uTex;
uniform float uHasTex;
void main() {
    vec4 c = vCol;
    if (uHasTex > 0.5) c *= texture(uTex, vUV);
    // SM's fixed-function alpha test: glAlphaFunc(GL_GREATER, 0.01)
    // (RageDisplay_OGL.cpp:1881). It applies to MASK draws too -- that is what
    // lets a mostly-transparent mask image stamp depth only where it has ink.
    // Saitama's mask.png is a frame-shaped border at alpha 1-63/255; the
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

// The PIU playfield's program. Straight ortho over StepMania's virtual
// 640x480 with y DOWN from the top-left -- the exact space ArrowEffects was
// written in. Shares SCENE_VS's attribute layout so it can reuse vao_/vbo_ and
// the same ch::Vtx batching; only the projection differs. aPos.z is unused.
const char* PIU_VS = R"(#version 330
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
out vec2 vUV; out vec4 vCol;
// The virtual screen. Height is always 480; WIDTH follows the output aspect,
// which is how StepMania does widescreen -- a fixed 640x480 would stretch a
// square panel into a rectangle on any 16:9 render.
uniform vec2 uVirt;
void main() {
    vUV = aUV; vCol = aCol;
    gl_Position = vec4(aPos.x / (uVirt.x * 0.5) - 1.0,
                       1.0 - aPos.y / (uVirt.y * 0.5), 0.0, 1.0);
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
Tex gl_loadTex(const std::string& path, bool repeat, bool flipY) {
    Tex t;
    int n = 0;
    stbi_set_flip_vertically_on_load(flipY ? 1 : 0);
    unsigned char* d = stbi_load(path.c_str(), &t.w, &t.h, &n, 4);
    if (!d) { fprintf(stderr, "cannot load texture %s\n", path.c_str()); exit(1); }
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    // Project colour space is Gamma: plain RGBA8, blend in gamma space, no sRGB.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    stbi_image_free(d);
    return t;
}

// ---------------------------------------------------------------------------
void Renderer::buildCamera() {
    float eye[3] = {ch::CAM_X, ch::CAM_Y, ch::CAM_Z};
    float pitch = ch::CAM_PITCH_DEG * 3.14159265f / 180.0f;
    float fwd[3] = {0.0f, -sinf(pitch), cosf(pitch)};
    float at[3]  = {eye[0] + fwd[0]*10, eye[1] + fwd[1]*10, eye[2] + fwd[2]*10};
    mvp_ = mat_mul(mat_perspective(ch::CAM_FOV, float(W)/float(H),
                                   ch::CAM_NEAR, ch::CAM_FAR),
                   mat_lookAt(eye, at));
    // Unity is left-handed; this right-handed lookAt puts +x on the left.
    // Negating clip-space x once lets every Unity coordinate be used verbatim
    // instead of sprinkling sign flips through the geometry.
    mvp_.m[0] = -mvp_.m[0]; mvp_.m[4] = -mvp_.m[4];
    mvp_.m[8] = -mvp_.m[8]; mvp_.m[12] = -mvp_.m[12];
}

void Renderer::makeFbos() {
    auto mk = [&](GLuint& f, GLuint& t) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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
    mk(fbo_, colorTex_);
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
    mk(postFbo_, postTex_);
    mk(fxFbo_, fxTex_);            // --fxshader's intermediate; see renderer.h
}

void Renderer::destroyFbos() {
    if (depthRb_) { glDeleteRenderbuffers(1, &depthRb_); depthRb_ = 0; }
    if (fbo_)     { glDeleteFramebuffers(1, &fbo_);     fbo_ = 0; }
    if (postFbo_) { glDeleteFramebuffers(1, &postFbo_); postFbo_ = 0; }
    if (fxFbo_)   { glDeleteFramebuffers(1, &fxFbo_);   fxFbo_ = 0; }
    if (fxTex_)   { glDeleteTextures(1, &fxTex_);       fxTex_ = 0; }
    if (colorTex_) { glDeleteTextures(1, &colorTex_); colorTex_ = 0; }
    if (postTex_)  { glDeleteTextures(1, &postTex_);  postTex_ = 0; }
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
    texAnim_    = gl_loadTex(A + "notes/spr_note_anim_strip16.png", false);
    texOpen_    = gl_loadTex(A + "notes/spr_open_notes_strip5.png", false);
    texFretB_   = gl_loadTex(A + "frets/spr_newtargets_bottom_strip12.png", false);
    texFretH_   = gl_loadTex(A + "frets/spr_newtargets_head_strip6.png", false);
    texLift_    = gl_loadTex(A + "frets/spr_targets_lift.png", false);
    texHLight_  = gl_loadTex(A + "frets/Head_Lights.png", false);

    prog_ = gl_program(SCENE_VS, SCENE_FS, "scene");
    post_ = gl_program(POST_VS, POST_FS, "post");
    glow_ = gl_program(SCENE_VS, NOTE_GLOW_FS, "noteGlow");
    susGlow_ = gl_program(SCENE_VS, SUSTAIN_GLOW_FS, "sustainGlow");
    actor_   = gl_program(ACTOR_VS, ACTOR_FS, "actor");
    piu_     = gl_program(PIU_VS, PIU_FS, "piu");
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
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(2*sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(4*sizeof(float)));
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);

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

void Renderer::resize(int w, int h) {
    if (w == W && h == H) return;
    W = w; H = h;
    destroyFbos();
    makeFbos();
    buildCamera();
}

void Renderer::buildHitTimes(const Chart& chart) {
    for (int i = 0; i < 5; ++i) hitTimes_[i].clear();
    for (const auto& n : chart.notes) {
        double t = chart.beatToSec(n.beat);
        for (int lane = 0; lane < 5; ++lane)
            if (n.frets & (1 << lane)) hitTimes_[lane].push_back(t);
    }
}

double Renderer::lastHit(int lane, double now) const {
    const auto& v = hitTimes_[lane];
    size_t lo = 0, hi = v.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (v[m] <= now) lo = m + 1; else hi = m; }
    double best = (lo > 0) ? v[lo - 1] : -1e9;
    return now - best;
}

// A bucket of pre-tinted silhouette quads through the note-glow program:
// the texture's alpha with the vertex colour's RGB, premultiplied. Used for
// the hold-body glow pass, whose alpha varies per ROW (GetGlow is a function
// of yOffset), so it cannot go through drawLayer's uniform-alpha repaint.
void Renderer::drawGlowLayer(GLuint tex) {
    if (v_.empty()) return;
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
    glUseProgram(piu_);
    glBlendFunc(blend == ch::BLEND_ADD ? GL_ONE : GL_ONE,
                blend == ch::BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(piu_, "uTex"), 0);
    glUniform2f(glGetUniformLocation(piu_, "uVirt"),
                480.0f * float(W) / float(H), 480.0f);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                 v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
    v_.clear();
    glUseProgram(prog_);
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
                       float noteSpeed, float bpm) {
    // The receptor row: SCREEN_CENTER_Y + GRAY_ARROWS_Y_STANDARD, which
    // OpenITG's theme puts at -125 (metrics.ini:2942) -- above centre, with
    // notes rising into it. UPSCROLL, because StepMania spends the `reverse`
    // mod ON downscroll; if the baseline were already downscroll, reverse would
    // have nothing left to mean.
    // StepMania's widescreen convention: the virtual screen is always 480 tall
    // and as wide as the output aspect calls for. Hard-coding 640 would squash
    // every panel horizontally on a 16:9 render.
    const float VIRT_H = 480.0f;
    const float VIRT_W = 480.0f * float(W) / float(H);
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
    float revShift = nc::scale(mods.reverse, 0.0f, 1.0f, revBase, -revBase);
    revShift = nc::scale(mods.centered, 0.0f, 1.0f, revShift, 0.0f);
    // scale=+1 upscroll, -1 reversed (:621). Re-origined by -revBase so that
    // reverse 0 leaves the pad exactly at PAD_Y; `centered` then lands it on
    // SCREEN_CENTER_Y, 96 + 144, on its own.
    const float revScale = nc::scale(mods.reverse, 0.0f, 1.0f, 1.0f, -1.0f);
    const float padTarget = PAD_Y + (revShift - revBase);
    const float RECEPTOR_Y = padTarget - (1.0f - piuT) * (padTarget + PANEL);
    // Screen y for a note at arrow-space offset `yPx`.
    auto padY = [&](float yPx) { return RECEPTOR_Y + yPx * revScale * PSCALE; };
    // Under reverse the pump skin flips the ribbon art top-to-bottom
    // (pump/defaultsm5 metrics.ini:20, FlipHoldBodyWhenReverse=1).
    const bool flipHold = mods.reverse > 0.5f;

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
        const float ry = padY(0.0f);
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
        const float yOff = ApplyYMods(mods, 0, yRaw, float(beat));

        // ArrowEffects rotations. NO sign flip here: the CH path negates rotX
        // and rotZ because its world is Y-UP, and this path is already in ITG's
        // Y-DOWN screen space, which is what those formulas were written for.
        const float rotZ = GetRotationZ(mods, float(n.beat), float(beat));
        const float rotX = GetRotationX(mods, yOff);
        const float rotY = GetRotationY(mods, yOff);
        // roll/twirl tip the quad out of the screen plane; with no perspective
        // to project through, they read as foreshortening. Same approximation
        // the actor layer makes for rotationx/y.
        const float fx = fabsf(cosf(rotY * 3.14159265f / 180.0f));
        const float fy = fabsf(cosf(rotX * 3.14159265f / 180.0f));

        const float baseAlpha = GetAlpha(mods, yOff, songTime);
        const float baseGlow  = GetGlow(mods, yOff, songTime);
        // This early-out is keyed on the HEAD's offset, so it can only speak
        // for a tap. A hold's body runs GetAlpha per segment and is visible at
        // offsets the head is not -- under sudden, most of the ribbon.
        if (!hasSus && baseAlpha <= 0.0f && baseGlow <= 0.0f) continue;

        for (int lane = 0; lane < 5; ++lane) {
            if (!(n.frets & (1 << lane))) continue;
            const int  art = ch::PIU_ART[lane];
            const bool mir = ch::PIU_MIRROR[lane];

            const float sy = padY(yOff);
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
            const float zoomB = 1.0f + GetYPosBump(mods, lane, yOff) / 512.0f;
            const float hw = HALF * noteZoom * zoomB * fx;
            const float hh = HALF * noteZoom * zoomB * fy;

            // ---- the hold body, drawn BEFORE the head so the head caps it ---
            if (n.sustain[lane] > 0.0) {
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
                            const float my = ApplyYMods(mods, 0, ym, float(beat));
                            const float sy0 = padY(ApplyYMods(mods, 0, y0, float(beat)));
                            const float sy1 = padY(ApplyYMods(mods, 0, y1, float(beat)));
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
                            quad2(vBody[art], mx, 0.5f * (top + bot),
                                  hw, 0.5f * (bot - top) + 0.5f, 0.0f,
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
                        const float myEnd = ApplyYMods(mods, 0, yEndRaw, float(beat));
                        const float syT = padY(myEnd);
                        // Away from the receptor, which flips with the scroll.
                        const float syF = syT + nc::ARROW_SIZE * PSCALE * noteZoom * revScale;
                        const float top = syT < syF ? syT : syF;
                        const float bot = syT < syF ? syF : syT;
                        if (bot >= -PANEL && top <= VIRT_H + PANEL) {
                            const float mx = CENTER_X + (nc::laneXPixels(lane) +
                                             GetXPos(mods, lane, myEnd, songTime,
                                                     float(beat), bpm)) * PSCALE;
                            const float ha = GetAlpha(mods, myEnd, songTime);
                            ch::piuSheetUV(6, 1, holdFrame, mir, u0, vb, u1, vt);
                            quad2(vCap[art], mx, 0.5f * (top + bot),
                                  hw, 0.5f * (bot - top), 0.0f,
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

void Renderer::drawActorQuad(float cx, float cy, float w, float h, float rotZDeg,
                             float r, float g, float b, float a,
                             GLuint tex, int blend, bool zWrite, bool zTest,
                             bool clearZ) {
    // blend,noeffect writes depth with NO colour, so it must still draw at
    // alpha 0 -- that is the whole point of a mask actor.
    if (a <= 0.0f && blend != 2) return;
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float th = rotZDeg * 3.14159265f / 180.0f;
    const float c = cosf(th), s2 = sinf(th);
    auto P = [&](float dx, float dy, float u, float v, float* o) {
        o[0] = cx + dx * c - dy * s2;
        o[1] = cy + dx * s2 + dy * c;
        o[2] = u; o[3] = v;
        o[4] = r; o[5] = g; o[6] = b; o[7] = a;
    };
    float q[6][8];
    P(-hw, -hh, 0, 0, q[0]); P(hw, -hh, 1, 0, q[1]); P(hw, hh, 1, 1, q[2]);
    P(-hw, -hh, 0, 0, q[3]); P(hw,  hh, 1, 1, q[4]); P(-hw, hh, 0, 1, q[5]);

    glUseProgram(actor_);
    glBindVertexArray(avao_);
    glBindBuffer(GL_ARRAY_BUFFER, avbo_);
    // blend: 0 normal, 1 add, 2 noeffect (colour write off -- used with zwrite
    // to lay a depth mask, which is exactly what Saitama's taiko mask does).
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
    glUniform1f(glGetUniformLocation(actor_, "uDepth"), blend == 2 ? -0.1f : 0.1f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(actor_, "uTex"), 0);
    glUniform1f(glGetUniformLocation(actor_, "uHasTex"), tex ? 1.0f : 0.0f);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (blend == 2) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    // Hand the scene path back exactly what it expects.
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
}

// The sustain glow: its own program, additive, no texture. Mirrors drawLayer
// but cannot share it -- the fragment shader is different and there is nothing
// to bind.
void Renderer::drawSustainGlow() {
    if (v_.empty()) return;
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
        for (ch::Vtx& vt : v_) { vt.r = vt.g = vt.b = 1.0f; vt.a = glow; }
        glUseProgram(glow_);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v_.size() * sizeof(ch::Vtx)),
                     v_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(v_.size()));
        glUseProgram(prog_);
    }
    v_.clear();
}

// ---------------------------------------------------------------------------
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
    Mods mods; PostFx fx;
    float mx = 0, my = 0, mz = 0;
    // bg.<name> knob values for shader background layers. The trailing evalAt
    // argument defaults to null, so the editor's call sites are untouched; the
    // mask says which slots the document actually drives -- undriven uniforms
    // are never set, so a shader's own defaults survive (modfile.h).
    float bgKnobs[MAX_BG_UNIFORMS] = {};
    // The strike line in screen UV, for shaders that want to protect or
    // target the playfield. Filled by the background block below and reused
    // by the --fxshader pass; the 0.5,0.5 default is screen centre.
    float fieldX = 0.5f, fieldY = 0.5f;
    unsigned bgDriven = 0;
    if (o.doc) {
        o.doc->evalAt(chart, beat * chart.resolution, mods, fx, mx, my, mz,
                      bg_ ? bgKnobs : nullptr);
        bgDriven = o.doc->bgUsedMask();
    }
    else       modchartAt(beat, mods, fx);
    if (o.noMods) { mods = Mods{}; mx = my = mz = 0; bgDriven = 0; }

    // `piu` is a transition, not a switch. The pad slides in from the top of
    // the virtual screen and SHOVES the CH highway out of the bottom -- both
    // driven by the same piuT, so 40% is 40% of the way through the swap. This
    // has to happen HERE, before mvp_/mvpEff are built from my, not next to the
    // hidePlayfield test further down: down there my has already been baked
    // into the matrices and the subtraction is a no-op.
    const float piuT = mods.piu < 0.0f ? 0.0f : (mods.piu > 1.0f ? 1.0f : mods.piu);
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
    const float noteSpeed = o.noteSpeed * mods.scrollspeed;

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
    // wag is the Actor effect from Saitama's lua layer: rotZ of the field,
    // wag% * 21 degrees on a 2-beat bgm-clock sine.
    Mat4 mvpEff = mvp_;
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
            mvpEff = mat_mul(mvp_, F);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

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

    glUseProgram(prog_);
    glUniformMatrix4fv(glGetUniformLocation(prog_, "uMVP"), 1, GL_FALSE, mvpEff.m);
    glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
    glUniform3f(glGetUniformLocation(prog_, "uOffset"),
                o.px + mx, o.py + my, o.pz + mz);
    // The glow program shares SCENE_VS, so it needs the same camera and the
    // same rigid movex/movey/movez offset. Set once here; drawLayer only ever
    // switches to it, and only when a note actually glows.
    glUseProgram(glow_);
    glUniformMatrix4fv(glGetUniformLocation(glow_, "uMVP"), 1, GL_FALSE, mvpEff.m);
    glUniform1i(glGetUniformLocation(glow_, "uTex"), 0);
    glUniform3f(glGetUniformLocation(glow_, "uOffset"),
                o.px + mx, o.py + my, o.pz + mz);
    glUseProgram(susGlow_);
    glUniformMatrix4fv(glGetUniformLocation(susGlow_, "uMVP"), 1, GL_FALSE, mvpEff.m);
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
    const bool piuMode = piuT > 0.0f && mods.hide == 0.0f;
    const bool hidePlayfield = mods.hide != 0.0f || piuT >= 1.0f;

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
    }
    if (hidePlayfield) { susBodyV.clear(); susBodyGlowV.clear(); susGlowV.clear(); }
    v_ = susBodyV;      drawLayer(texSustain_.id, ch::BLEND_SPRITE);   // -1000
    v_ = susBodyGlowV;  drawGlowLayer(texSustain_.id);                  // the bGlow pass

    struct FretBuckets {
        std::vector<ch::Vtx> base, lift, cover, head, headCover, light, half;
    };
    FretBuckets low, top;
    for (int lane = 0; lane < (hidePlayfield ? 0 : 5); ++lane) {
        float popY = 0.0f;
        bool held = false;
        if (!o.noBot) {
            const float dt = float(lastHit(lane, songTime));
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
            popY = ch::fretPopY(dt, sustaining, sustaining);
            // The bot must hold the lane to hit the note at all, so the
            // headLight is lit for the duration of the pop.
            held = sustaining || (dt >= 0.0f && dt < ch::POP_T1);
        }
        FretBuckets& B = ch::fretOnTop(popY) ? top : low;
        std::vector<ch::Vtx>* bk[7] = {&B.base, &B.lift, &B.cover, &B.head,
                                       &B.headCover, &B.light, &B.half};
        size_t n0[7];
        for (int i = 0; i < 7; ++i) n0[i] = bk[i]->size();
        ch::buildFret(B.base, B.lift, B.cover, B.head, B.headCover,
                      B.light, B.half, lane, popY, held);
        // The receptor rule, ReceptorArrowRow.cpp:46-54: a receptor is an
        // arrow evaluated at fYOffset = 0. Which mods move frets is a
        // CONSEQUENCE: tornado's term is identically 0 there (cos(acos(b))==b)
        // and ITG's bumpy is sin(0)==0; drunk/flip/invert/beat/tipsy displace.
        // NotClon's bumpyOffset extension makes bumpy nonzero at 0 -- accepted,
        // it is our knob. Guarded so the all-zero case never touches a vertex.
        const float wdx = pxToUnits(GetXPos(mods, lane, 0.0f, songTime,
                                            float(beat), bpm));
        const float wdy = pxToUnits(GetYPosBump(mods, lane, 0.0f)) * 0.5f;
        const float wdz = ApplyScrollZ(mods, 0.0f);
        if (wdx != 0.0f || wdy != 0.0f || wdz != 0.0f)
            for (int i = 0; i < 7; ++i)
                for (size_t j = n0[i]; j < bk[i]->size(); ++j) {
                    (*bk[i])[j].x += wdx;
                    (*bk[i])[j].y += wdy;
                    (*bk[i])[j].z += wdz;
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
            for (int i = 0; i < 7; ++i)
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
        v_ = B.headCover; drawLayer(texFretH_.id, ch::BLEND_SPRITE);
        v_ = B.light;     drawLayer(texHLight_.id, ch::BLEND_ADD);
        v_ = B.half;      drawLayer(texFretB_.id, ch::BLEND_SPRITE);
    };
    drawFretBase(low);
    v_ = susGlowV;  drawSustainGlow();          // -999, additive, held only
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
    const float noteZoom = GetZoom(mods);
    const float tinyCol  = GetTinyColScale(mods);

    for (int i = hidePlayfield ? -1 : int(chart.notes.size()) - 1; i >= 0; --i) {
        const Note& n = chart.notes[i];
        const float z0 = float(ssec(chart.beatToSec(n.beat)) - scrollNow) * noteSpeed;

        // In OpenITG GetYOffset *is* the note's position, so boomerang and
        // expand move it. This used to compute yOff and never feed it back,
        // which left both mods able only to perturb the phase of drunk/tornado
        // -- they did not shift a single note. The equality guard keeps the
        // no-y-mod path bit-identical: z0*K/K is not exactly z0 in float.
        const float y0   = z0 * Y_PER_UNIT;
        const float yOff = ApplyYMods(mods, 0, y0, float(beat));
        const float z    = (yOff == y0) ? z0 : yOff / Y_PER_UNIT;

        // With the bot playing, a note is consumed at the strike line; without
        // it, CH lets the note travel to its own cull plane. Culled on the
        // modified z, as ITG does, so boomerang can pull a note back into view.
        float nearCull = o.noBot ? ch::NOTE_CULL_NEAR : 0.0f;
        // Consumption stays in the raw scroll domain (time-anchored); the far
        // cull and the behind-the-eye clamp act on the DRAWN z, which only
        // differs under reverse/centered.
        const float zDraw = ApplyScrollZ(mods, z);
        if (zDraw > ch::NOTE_CULL_FAR || zDraw < -3.0f || z <= nearCull) continue;
        float alpha = fminf(1.0f, fmaxf(0.0f,
                            (ch::NOTE_CULL_FAR - zDraw) / ch::NOTE_FADE_LEN));
        // sudden / hidden / stealth. GetAlpha is a hard binary cut and GetGlow
        // is the white silhouette that fills in around it -- they are one
        // effect and drawing only the first renders stealth >= 50% as nothing.
        // See the comment above GetAlpha in mods.h.
        const float noteAlpha = alpha * GetAlpha(mods, yOff, songTime);
        const float noteGlow  = alpha * GetGlow(mods, yOff, songTime);
        alpha = noteAlpha;
        if (noteAlpha <= 0.0f && noteGlow <= 0.0f) continue;

        // Per-note rotation (dizzy/confusion/roll/twirl). NoteDisplay.cpp:
        // 1020-1043: GetRotationX/Y take fYOffset -- the post-accel-mods value,
        // i.e. yOff -- and GetRotationZ takes the note's beat. All three come
        // back in ITG's screen-Y-down degrees; NotClon's world is Y-up, so
        // conjugation by diag(1,-1,1) negates rotX and rotZ and keeps rotY.
        // Notes only, not sustain ribbons (ITG's hold body takes none of them;
        // SM5 even gates hold heads behind DIZZY_HOLD_HEADS). The glow pass
        // rotates for free: drawLayer redraws the same v_.
        //
        // Documented deviation: SM5 rotates the RECEPTORS by confusion too
        // (ReceptorGetRotationZ is named for it). NotClon's fret stack is six
        // axis-aligned quads with no rotated emitter, so frets do not spin.
        const float rotX = -GetRotationX(mods, yOff);
        const float rotY =  GetRotationY(mods, yOff);
        const float rotZ = -GetRotationZ(mods, float(n.beat), float(beat));

        if (n.open) {
            float dx = pxToUnits(GetXPos(mods, 2, yOff, songTime, float(beat), bpm));
            if (mods.tiny != 0.0f) dx *= tinyCol;
            // open order is Body -> Head -> Anim, unlike standard notes
            ch::quadUpRot(v_, dx, 0.0f, zDraw,
                          -OW*0.5f, -ch::NOTE_PIVOT_Y*NH,
                           OW*0.5f, (1.0f-ch::NOTE_PIVOT_Y)*NH,
                          rotX, rotY, rotZ, noteZoom,
                          0.0f,0.0f,0.2f,1.0f, 1,1,1, alpha);
            drawLayer(texOpen_.id, ch::BLEND_SPRITE, noteGlow);
            ch::quadUpRot(v_, dx, 0.0f, zDraw,
                          -OW*0.5f, -ch::NOTE_PIVOT_Y*NH,
                           OW*0.5f, (1.0f-ch::NOTE_PIVOT_Y)*NH,
                          rotX, rotY, rotZ, noteZoom,
                          0.2f,0.0f,0.4f,1.0f,
                          ch::NOTE_TINT[5][0], ch::NOTE_TINT[5][1],
                          ch::NOTE_TINT[5][2], alpha);
            drawLayer(texOpen_.id, ch::BLEND_SPRITE, noteGlow);
            continue;
        }

        for (int lane = 0; lane < 5; ++lane) {
            if (!(n.frets & (1 << lane))) continue;
            float px2 = GetXPos(mods, lane, yOff, songTime, float(beat), bpm);
            float bump = GetYPosBump(mods, lane, yOff);
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

            // Body: frame 2 strum/HOPO, frame 3 tap. Tinted per fret.
            float bu0 = isTap ? 0.6f : 0.4f, bu1 = isTap ? 0.8f : 0.6f;
            ch::quadUpRot(v_, cx, by, zDraw, hx0, hy0, hx1, hy1,
                          rotX, rotY, rotZ, noteZoom, bu0,0.0f,bu1,1.0f,
                          tintN[0], tintN[1], tintN[2], alpha);
            drawLayer(texNotes_.id, ch::BLEND_SPRITE, noteGlow);

            if (!isTap) {   // taps have no anim layer
                float au0 = float(animFrame)/16.0f, au1 = float(animFrame+1)/16.0f;
                ch::quadUpRot(v_, cx, by, zDraw, hx0, ha0, hx1, ha1,
                              rotX, rotY, rotZ, noteZoom, au0,0.0f,au1,1.0f,
                              tintA[0], tintA[1], tintA[2], alpha);
                drawLayer(texAnim_.id, ch::BLEND_SPRITE, noteGlow);
            }

            // Head: frame 0 strum/tap (alt_taps defaults false), frame 1 HOPO
            float hu0 = isHopo ? 0.2f : 0.0f, hu1 = isHopo ? 0.4f : 0.2f;
            ch::quadUpRot(v_, cx, by, zDraw, hx0, hy0, hx1, hy1,
                          rotX, rotY, rotZ, noteZoom, hu0,0.0f,hu1,1.0f,
                          1,1,1, alpha);
            drawLayer(texNotes_.id, ch::BLEND_SPRITE, noteGlow);
        }
    }

    drawFretBase(top); drawFretRest(top);   // popped frets, over the notes

    if (piuMode)
        drawPiu(chart, beat, o, mods, songTime, scrollNow, noteSpeed, bpm);

    // #FGCHANGES actor folders: in front of the playfield. Drawn INSIDE fbo_,
    // so the post chain processes them -- which is what NotITG's layout.xml
    // does (its captured region spans background + playfields, and post.frag
    // runs on the result).
    if (actors_) actors_->drawForeground(*this, songTime);

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
        bg_->drawScene(colorTex_, W, H, nowSec, float(beat), bpm,
                       fieldX, fieldY, bgKnobs, bgDriven);
        sceneTex = fxTex_;
    }
    // A --fxchain runs after any --fxshader and owns its own buffers, so it
    // needs no bounce: it returns whichever of them holds the result.
    if (bg_ && bg_->hasChain())
        sceneTex = bg_->drawChain(sceneTex, W, H, nowSec, float(beat), bpm,
                                  fieldX, fieldY, bgKnobs, bgDriven);

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
