// The shared render core.
//
// Owns the GL context's resources and knows how to draw exactly one frame of a
// chart at a given beat into a given framebuffer. Nothing in here knows about
// ffmpeg, windows, input, or UI.
//
// Both binaries link this:
//   notclon.exe         resolves into an FBO and reads it back for encoding
//   notclon-editor.exe  resolves into the window backbuffer and swaps
//
// That is the whole reason this file exists: the editor must show *exactly*
// what the encoder will produce, so there can only be one implementation of a
// frame. If you add a layer, add it here, not in a caller.
#pragma once

#include "gl.h"
#include "chart.h"
#include "mods.h"
#include "highway.h"
#include "render.h"
#include "modchart.h"
#include "modfile.h"
#include "engine_mesh.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace nc {

// SM5 ScreenDimensions.cpp:31-42. A 640x480 theme widens with the display,
// then rounds the logical width down to an even integer after ceilf (854 at
// 16:9). Actor Lua constants and actor projection must use this same value.
inline float logicalScreenWidth(int pixelW, int pixelH) {
    const float scaled = pixelH > 0
                       ? fmaxf(640.0f, 480.0f * float(pixelW) / float(pixelH))
                       : 640.0f;
    int width = int(ceilf(scaled));
    width -= width % 2;
    return float(width);
}

// Forward-declared, not held by value: actor.h includes THIS header (it needs
// Tex and Renderer), so owning an ActorLayer here would be a cycle. The caller
// owns it and hands over a pointer.
class ActorLayer;
// Same pattern, same reason: background.h needs Tex.
class Background;

// ---------------------------------------------------------------------------
// Small matrix helpers
// ---------------------------------------------------------------------------
struct Mat4 { float m[16]; };

inline Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rw = 0; rw < 4; ++rw) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rw] * b.m[c * 4 + k];
            r.m[c * 4 + rw] = s;
        }
    return r;
}

inline Mat4 mat_perspective(float fovyDeg, float aspect, float zn, float zf) {
    float t = 1.0f / tanf(fovyDeg * 3.14159265f / 360.0f);
    Mat4 r{};
    r.m[0] = t / aspect; r.m[5] = t;
    r.m[10] = (zf + zn) / (zn - zf); r.m[11] = -1.0f;
    r.m[14] = 2.0f * zf * zn / (zn - zf);
    return r;
}

inline void vec_norm3(float* v) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

inline Mat4 mat_lookAt(const float* eye, const float* at) {
    float f[3] = {at[0]-eye[0], at[1]-eye[1], at[2]-eye[2]}; vec_norm3(f);
    float up[3] = {0, 1, 0};
    float s[3] = {f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0]};
    vec_norm3(s);
    float u[3] = {s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0]};
    Mat4 r{};
    r.m[0]=s[0]; r.m[4]=s[1]; r.m[8]=s[2];
    r.m[1]=u[0]; r.m[5]=u[1]; r.m[9]=u[2];
    r.m[2]=-f[0]; r.m[6]=-f[1]; r.m[10]=-f[2];
    r.m[12] = -(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
    r.m[13] = -(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    r.m[14] =  (f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    r.m[15] = 1.0f;
    return r;
}

// ---------------------------------------------------------------------------
// GL helpers
// ---------------------------------------------------------------------------
inline GLuint gl_compile(GLenum type, const char* src, const char* label) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetShaderInfoLog(s, sizeof log, nullptr, log);
        fprintf(stderr, "[%s] shader failed:\n%s\n", label, log);
        exit(1);
    }
    return s;
}

inline GLuint gl_program(const char* vs, const char* fs, const char* label) {
    GLuint p = glCreateProgram();
    GLuint v = gl_compile(GL_VERTEX_SHADER, vs, label);
    GLuint f = gl_compile(GL_FRAGMENT_SHADER, fs, label);
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetProgramInfoLog(p, sizeof log, nullptr, log);
        fprintf(stderr, "[%s] link failed:\n%s\n", label, log);
        exit(1);
    }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

struct Tex { GLuint id = 0; int w = 0, h = 0; };
struct ActorShaderBinding {
    std::string name;
    int components = 0;                        // 1/2/4 float, 0 = sampler2D
    bool integer = false;
    float value[4] = {};
    GLuint texture = 0;
};
struct ActorPolygonVertex {
    float x = 0, y = 0, z = 0, u = 0, v = 0;
};

// flipY=true puts PNG row 0 at v=1, which is Unity's convention and what every
// highway/note texture wants. Actor sprites are top-left origin and must load
// with flipY=false -- devdocs/spec/background.md section 2.3 called this out, and
// getting it wrong renders the whole actor tree upside down.
Tex gl_loadTex(const std::string& path, bool repeat, bool flipY = true,
               bool srgb = false, bool mipmaps = false);

// Where the executable lives -- assets ship beside it (a post-build step
// copies assets/ into the build dir), so nothing assumes a fixed install path.
inline std::string nc_exeDir() {
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    const size_t sl = p.find_last_of("\\/");
    return sl == std::string::npos ? std::string(".") : p.substr(0, sl);
}

// The ffmpeg to run. A bundled ffmpeg.exe beside the executable wins over
// whatever is on PATH, which is the whole point of shipping one: a release
// should work on a machine that has never heard of ffmpeg, and it should keep
// working when the user later installs a different version. Falls back to the
// bare name so a source build with ffmpeg on PATH behaves as it always did.
//
// Returned as an 8.3 SHORT PATH, unquoted, and that is not a micro-optimisation.
// _popen runs the string through `cmd /c`, and cmd has a rule that when the
// line begins with a quote AND contains another quote later, it strips the
// wrong pair -- so a quoted ffmpeg path followed by a quoted output file
// silently fails to launch. The encode then reports success having written
// nothing, because the failure is invisible from this side. A short path has
// no spaces, so it needs no quotes and the rule never triggers.
//
// If 8.3 generation is disabled on the volume, GetShortPathName hands back the
// long path; quote it then and accept the risk, which is strictly better than
// an unquoted path with spaces.
inline std::string nc_ffmpeg() {
    const std::string local = nc_exeDir() + "/ffmpeg.exe";
    FILE* f = fopen(local.c_str(), "rb");
    if (!f) return "ffmpeg";
    fclose(f);
    char shortBuf[MAX_PATH];
    const DWORD n = GetShortPathNameA(local.c_str(), shortBuf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::string sp(shortBuf, n);
        if (sp.find(' ') == std::string::npos) return sp;
    }
    return "\"" + local + "\"";
}

// Asset dir resolution: an explicit --assets wins; otherwise <exe>/assets/,
// then ./assets/ relative to the working directory. Probed by a texture every
// build must have, so a wrong guess fails here with a clear message instead of
// deep inside init.
inline std::string nc_findAssets(const std::string& overridePath) {
    auto ok = [](const std::string& d) {
        FILE* f = fopen((d + "highway/spr_highway_gh6.png").c_str(), "rb");
        if (f) { fclose(f); return true; }
        return false;
    };
    if (!overridePath.empty()) {
        std::string d = overridePath;
        if (d.back() != '/' && d.back() != '\\') d += '/';
        return d;
    }
    const std::string tries[] = { nc_exeDir() + "/assets/", "assets/" };
    for (const std::string& d : tries)
        if (ok(d)) return d;
    fprintf(stderr, "cannot find assets/ -- tried \"%s\" and \"./assets/\"; "
                    "pass --assets <dir>\n", tries[0].c_str());
    return tries[0];
}

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
extern const char* SCENE_VS;
extern const char* SCENE_FS;
extern const char* NOTE_GLOW_FS;
extern const char* SUSTAIN_GLOW_FS;
extern const char* PIU_VS;
extern const char* PIU_FS;
extern const char* POST_VS;
extern const char* POST_FS;

// ---------------------------------------------------------------------------
// Per-frame knobs. Everything a caller can vary without touching the core.
// ---------------------------------------------------------------------------
struct RenderOpts {
    float noteSpeed = ch::NOTE_SPEED_DEFAULT;
    double audioDuration = 0.0; // longest audio stem, in seconds; 0 if unavailable
    bool  playfield = false;   // drop neck/sidebars/strings/beatlines
    bool  noPost    = false;
    bool  noMods    = false;
    bool  noBot     = false;
    float px = 0, py = 0, pz = 0;   // rigid-body playfield offset (CLI baseline)

    // Where the mods come from. Null falls back to the hardcoded R.E.M. III
    // modchart in modchart.h, which is what notclon.exe still does by default.
    const ModDoc* doc = nullptr;
    // Player 2's modchart. Non-null = draw TWO playfields in the visible
    // left/right halves of CH's offset camera rects, each with half-screen
    // aspect so the complete highway remains on-screen.
    const ModDoc* doc2 = nullptr;
};

// ---------------------------------------------------------------------------
class Renderer {
public:
    int W = 1920, H = 1080;

    // One player's evaluated mod state + the matrices built from it.
    struct FieldEval {
        Mods mods; PostFx fx;
        float mx = 0, my = 0, mz = 0;
        float bgKnobs[MAX_BG_UNIFORMS] = {};
        unsigned bgDriven = 0;
        float fieldX = 0.5f, fieldY = 0.5f;
        float noteSpeed = 10.0f;
        float piuT = 0.0f;
        Mat4 mvpEff{};
        Mat4 viewEff{};
    };
    FieldEval evalField(const Chart& chart, double beat, const RenderOpts& o,
                        const ModDoc* doc, int plr);
    void drawField(const Chart& chart, double beat, const RenderOpts& o,
                   FieldEval& E, int vpX, int vpW, float scrollNow,
                   float songTime, double nowSec, float bpm,
                   const Mat4* mvpOverride = nullptr,
                   const Mat4* piuMvpOverride = nullptr,
                   const Mat4* engineMvpOverride = nullptr);

    // Requires a current GL context. assetDir must end with a separator.
    bool init(int w, int h, const std::string& assetDir);
    void resize(int w, int h);

    // Draw one frame of `chart` at `beat` into `postTarget` (0 = backbuffer).
    void drawFrame(const Chart& chart, double beat, const RenderOpts& o,
                   GLuint postTarget);

    // --- actor layer -------------------------------------------------------
    // Actors are 2D, in SM's 480-high aspect-correct space, drawn with their OWN
    // program and VBO. Deliberately not routed through drawLayer/SCENE_FS:
    // that path is what the pinned hashes certify, and an actor has different
    // blending, no premultiply and an ortho projection.
    // u0..v1 select a sub-rectangle of the texture, for a .sprite's sheet.
    // Defaulted to the whole image so every existing caller is unchanged.
    void drawActorQuad(float cx, float cy, float w, float h, float rotZDeg,
                       float r, float g, float b, float a,
                       GLuint tex, int blend, bool zWrite, bool zTest, bool clearZ,
                       float u0 = 0.0f, float v0 = 0.0f,
                       float u1 = 1.0f, float v1 = 1.0f);
    void drawActorQuad3D(float cx, float cy, float cz,
                         float localX, float localY, float w, float h,
                         float rotXDeg, float rotYDeg, float rotZDeg, float skewX,
                         float fovDeg, float vanishX, float vanishY,
                         float fadeLeft, float fadeRight,
                         float fadeTop, float fadeBottom,
                         float cropLeft, float cropRight,
                         float cropTop, float cropBottom,
                         float r, float g, float b, float a,
                         GLuint tex, int blend, bool zWrite, bool zTest, bool clearZ,
                         float u0 = 0.0f, float v0 = 0.0f,
                         float u1 = 1.0f, float v1 = 1.0f,
                          GLuint customProgram = 0,
                          const std::vector<ActorShaderBinding>* customUniforms = nullptr,
                          int imageW = 0, int imageH = 0,
                          int imageBackingW = 0, int imageBackingH = 0,
                          bool textureGlow = false,
                          const std::vector<ActorPolygonVertex>* polygon = nullptr,
                          bool polygonTriangles = true, int cullMode = 0,
                          float polygonZoomZ = 1.0f);
    void drawActorText(const std::string& text,
                       float cx, float cy, float cz, float zoomX, float zoomY,
                       float rotXDeg, float rotYDeg, float rotZDeg, float skewX,
                       float fovDeg, float vanishX, float vanishY,
                       float r, float g, float b, float a,
                       int blend, bool zWrite, bool zTest, bool clearZ);
    void drawActorPlayerSource(int pn, float x, float y,
                               float z, float zoomX, float zoomY, float zoomZ,
                               float rotXDeg, float rotYDeg, float rotZDeg,
                               float skewX, float fovDeg,
                               float vanishX, float vanishY,
                               float r, float g, float b, float a);
    bool actorTargetYInverted() const { return actorTargetYInverted_; }
    void setActorTargetYInverted(bool inverted) {
        actorTargetYInverted_ = inverted;
    }
    void setActorTargetSize(int w, int h) {
        actorTargetW_ = w; actorTargetH_ = h;
    }
    int actorTargetWidth() const { return actorTargetW_; }
    int actorTargetHeight() const { return actorTargetH_; }
    const Tex& actorMissingTexture() const { return actorMissing_; }
    // Null = no actor folders, and the passes are skipped entirely -- which is
    // what keeps this hash-neutral.
    void setActorLayer(ActorLayer* a) { actors_ = a; }
    // Null = no #BGCHANGES media / shader layers, and the pass is skipped
    // entirely -- which is what keeps this hash-neutral (REM III has no .sm).
    void setBackground(Background* b) { bg_ = b; }
    // The beat the actor clock reads for effectclock,'bgm'. Set per frame.
    double actorBeat() const { return actorBeat_; }

    GLuint sceneFbo() const { return fbo_; }
    GLuint postFbo()  const { return postFbo_; }
    GLuint postTex()  const { return postTex_; }   // for ImGui::Image in the editor

    // Autoplay: per-lane time since that lane was last struck. Built once from
    // the chart so the editor can scrub without rebuilding it.
    void buildHitTimes(const Chart& chart);

private:
    void makeFbos();
    void destroyFbos();
    double lastHit(int lane, double now) const;

    Mat4 view_{}, mvp_{}, mvp2_{}, piuMvp_{}, gh3Mvp_{};
    GLuint prog_ = 0, post_ = 0, glow_ = 0, susGlow_ = 0, actor_ = 0;
    GLuint cover_ = 0, piu_ = 0, gh3Sprite_ = 0, gh3Whammy_ = 0, engine_ = 0;
    GLuint engineGlow_ = 0, yargEffect_ = 0;
    GLuint moonOccluder_ = 0, moonBlur_ = 0;
    GLuint yargBloomPrefilter_ = 0, yargBloomDownH_ = 0;
    GLuint yargBloomDownV_ = 0, yargBloomUp_ = 0;
    GLuint yargNormalProg_ = 0, yargAoEstimate_ = 0;
    GLuint yargAoBlur_ = 0, yargAoFinal_ = 0;
    GLuint yargSustain_ = 0, yargBeatline_ = 0;
    GLuint yargMaskMesh_ = 0;
    float yargGroove_ = 0.0f;
    int hitLightCount_ = 0;
    float hitLightPos_[15] = {};
    float hitLightColor_[15] = {};
    GLuint linearCompose_ = 0;
    bool engineUseAo_ = true;
    GLuint avao_ = 0, avbo_ = 0;
    bool actorTargetYInverted_ = false;
    int actorTargetW_ = 0, actorTargetH_ = 0;
    double actorBeat_ = 0.0;
    double fieldSec_ = 0.0;   // when evalField reads live Lua PlayerOptions
    const Chart* actorChart_ = nullptr;
    const RenderOpts* actorOpts_ = nullptr;
    FieldEval* actorFields_[2] = {};
    float actorScrollNow_ = 0.0f, actorSongTime_ = 0.0f, actorBpm_ = 0.0f;
    double actorNowSec_ = 0.0;
    ActorLayer* actors_ = nullptr;
    Background* bg_ = nullptr;
    GLuint vao_ = 0, vbo_ = 0, qvao_ = 0, qvbo_ = 0;
    GLuint fbo_ = 0, colorTex_ = 0, postFbo_ = 0, postTex_ = 0;
    // Depth attachment on fbo_ ONLY, for the actor layer's z-mask
    // (blend,noeffect + zwrite writes it; ztest reads it). The CH highway never
    // enables depth testing -- its painter order is load-bearing. Engine-style
    // 3D fields use their own isolated colour/depth targets below.
    GLuint depthRb_ = 0;
    // One more colour target, for --fxshader: a playfield shader cannot read
    // and write the same texture, so the scene is shaded colorTex_ -> fxTex_
    // and the post chain then reads fxTex_ instead. Allocated always, bound
    // only when a scene layer exists.
    GLuint fxFbo_ = 0, fxTex_ = 0;
    GLuint moonSceneFbo_ = 0, moonSceneTex_ = 0;
    GLuint moonSceneMsaaFbo_ = 0, moonSceneMsaaColor_ = 0, moonSceneDepth_ = 0;
    int moonSceneW_ = 0, moonSceneH_ = 0;
    GLuint moonGlowFbo_ = 0, moonGlowTex_ = 0, moonGlowDepth_ = 0;
    GLuint moonBlurFbo_[2] = {}, moonBlurTex_[2] = {};
    int moonGlowW_ = 0, moonGlowH_ = 0;
    GLuint yargFbo_ = 0, yargTex_ = 0, yargDepth_ = 0;
    GLuint yargMsaaFbo_ = 0, yargMsaaColor_ = 0, yargMsaaDepth_ = 0;
    GLuint yargMaskFbo_ = 0, yargMaskTex_ = 0;
    GLuint yargNormalFbo_ = 0, yargNormalTex_ = 0, yargAoDepth_ = 0;
    GLuint yargAoFbo_[4] = {}, yargAoTex_[4] = {};
    GLuint yargBloomDownFbo_[6] = {}, yargBloomDownTex_[6] = {};
    GLuint yargBloomUpFbo_[6] = {}, yargBloomUpTex_[6] = {};
    int yargW_ = 0, yargH_ = 0;
    int yargBloomMips_ = 1;
    GLint locPremul_ = -1;

    Tex texHighway_, texSide_, texString_, texBeat_;
    Tex texNotes_, texAnim_, texOpen_, texSustain_;
    // Starpower phrase art, CH's own sheets: the star cap strip (frame 0 =
    // strum/tap, frame 2 = HOPO -- the prefab skips the odd frames), the two
    // 16-frame animated star bodies (plain and tap), the 16-frame bottom
    // layer CH tints (0.321, 1, 1), and the 4x4 open-note highlight grid.
    Tex texStarCap_, texStarBody_, texStarBodyTap_, texStarBottom_, texSpOpen_;
    // PIU mode: three arts each, indexed by ch::PIU_ART (DownLeft/UpLeft/Center).
    Tex texPiuTap_[3], texPiuRecep_[3], texPiuHoldBody_[3], texPiuHoldCap_[3];
    Tex texPiuFlash_;
    // GH3 board art (assets/engine/gh3_*, see gh3.SOURCE.txt). The highway
    // surface itself is a flat black fill -- GH3's fretboard art is venue
    // geometry, not in global.pak, and black is the chosen stand-in.
    Tex texGh3Fretbar_[3];              // small, medium, large
    Tex texGh3String_, texGh3Sidebar_;
    // Nowbar pieces, lane order green..orange. down is the held-state head
    // art; the lit heads are loaded but unused until the IDB dig pins their
    // trigger down (script-side never touches them).
    Tex texGh3NowbarMid_[5], texGh3NowbarLip_[5], texGh3NowbarHead_[5];
    Tex texGh3NowbarDown_[5], texGh3NowbarHeadLit_[5];
    Tex texGh3NowbarNeck_;
    // Note art. Every frame is 128x64: gem/hammer single, star and
    // star-hammer 4x4 spin flipbooks, tap 2x4 flipbooks (GH3+ battle-gem
    // slots). Opens are 512x64 (the SP-phrase opens 4x4 sheets of it).
    Tex texGh3Gem_[5], texGh3GemHammer_[5], texGh3Star_[5], texGh3StarHammer_[5];
    Tex texGh3Tap_[5], texGh3TapSp_[5];
    Tex texGh3Open_, texGh3OpenHopo_, texGh3OpenSp_, texGh3OpenHopoSp_;
    // Whammy tails: 32x32 tiles, repeat-wrapped; open tails 128x32.
    Tex texGh3Whammy_[5], texGh3WhammySp_, texGh3WhammyDead_;
    Tex texGh3OpenSus_, texGh3OpenSusDead_;
    Tex texWhite_;                      // 1x1 white, untextured fills
    Tex texFretB_, texFretH_, texLift_, texHLight_;
    struct EngineGpu {
        GLuint vao = 0, vbo = 0;
        GLsizei count = 0;
    };
    EngineGpu moonNote_, moonOpen_, moonSp_;
    EngineGpu yargNormal_, yargHopo_, yargTap_, yargOpen_, yargFret_;
    EngineGpu yargTrack_, yargTrackTrim_;
    Tex texMoonHighway_, texMoonRail_, texMoonBeat_, texMoonBeatWeak_;
    Tex texMoonMeasure_, texMoonIndicator_, texMoonStrike_;
    Tex texMoonSustainFretted_, texMoonSustainOpen_, texMoonSpTail_;
    Tex texYargNote_, texYargNoteShine_, texYargNoteShader_;
    Tex texYargOpenNote_, texYargOpenHopo_;
    Tex texYargFret_, texYargFretShine_;
    Tex texYargTrackFade_, texYargTrackSmall_, texYargTrackSide_;
    Tex texYargTrackTrim_;
    Tex texYargSoloTrack_, texYargSoloRail_, texYargSoloTransitionTrack_;
    Tex texYargSoloTransitionRailLeft_, texYargSoloTransitionRailRight_;
    Tex texYargSpTrim_;
    Tex texYargBeatline_, texYargSustain_, texYargSustainSecondary_;
    Tex texYargOpenSustain_;
    Tex texYargFretHitFlash_, texYargFretHitRing_;
    Tex texYargGrain_;
    Tex actorFont_, actorMissing_;
    int actorFontWidth_[256] = {};
    int actorFontAdvance_[256] = {};

    std::vector<ch::Vtx> v_;
    float fieldTint_[4] = {1, 1, 1, 1};
    std::vector<double>  hitTimes_[5];

    // glow > 0 re-draws the same quad as a flat-white silhouette on top, which
    // is ITG's second sprite pass (see NOTE_GLOW_FS). 0 skips it entirely.
    void drawLayer(GLuint tex, int blend, float glow = 0.0f);
    void applyFieldTint();
    void drawSustainGlow();
    void drawGlowLayer(GLuint tex);
    void drawPiuLayer(GLuint tex, int blend);
    void drawGh3Layer(GLuint tex, int blend, bool fade = false);
    void drawGh3Whammy(GLuint tex, bool glow);
    // The pump playfield -- a wholly separate field, not a reskin. See the
    // comment on the definition.
    void drawPiu(const Chart& chart, double beat, const RenderOpts& o,
                 const Mods& mods, float songTime, float scrollNow,
                 float noteSpeed, float bpm, const Mat4& mvp);
    // The GH3 highway -- a 2D sprite field like drawPiu, not a drawEngine
    // style. See the comment on the definition.
    void drawGh3(const Chart& chart, double beat, const RenderOpts& o,
                 const Mods& mods, float songTime, float scrollNow,
                 float noteSpeed, float bpm, const Mat4& mvp);
    void loadEngineMesh(EngineGpu& gpu, const std::string& path);
    void drawEngineMesh(const EngineGpu& gpu, const Mat4& camera,
                        const Mat4& model, int kind, const float* color,
                        float alpha, GLuint texture, GLuint texture2 = 0,
                        GLuint texture3 = 0, float scroll = 0.0f,
                        int materialFilter = -1, float materialState = 0.0f,
                        const float* random = nullptr,
                        float shaderTime = 0.0f);
    void drawEngine(const Chart& chart, double beat, const RenderOpts& o,
                    const Mods& mods, float songTime, float scrollNow,
                    float noteSpeed, float bpm, int style, float alpha,
                    float mx, float my, float mz, int vpX, int vpW,
                    const Mat4* mvpOverride);
    void buildCamera();
};

}  // namespace nc
