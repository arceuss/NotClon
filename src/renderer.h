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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace nc {

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

// flipY=true puts PNG row 0 at v=1, which is Unity's convention and what every
// highway/note texture wants. Actor sprites are top-left origin and must load
// with flipY=false -- devdocs/spec/background.md section 2.3 called this out, and
// getting it wrong renders the whole actor tree upside down.
Tex gl_loadTex(const std::string& path, bool repeat, bool flipY = true);

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
    bool  playfield = false;   // drop neck/sidebars/strings/beatlines
    bool  noPost    = false;
    bool  noMods    = false;
    bool  noBot     = false;
    float px = 0, py = 0, pz = 0;   // rigid-body playfield offset (CLI baseline)

    // Where the mods come from. Null falls back to the hardcoded R.E.M. III
    // modchart in modchart.h, which is what notclon.exe still does by default.
    const ModDoc* doc = nullptr;
    // Player 2's modchart. Non-null = draw TWO playfields, CH's own 2P
    // layout: identical camera, viewport shifted half a screen each way.
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
    };
    FieldEval evalField(const Chart& chart, double beat, const RenderOpts& o,
                        const ModDoc* doc, int plr);
    void drawField(const Chart& chart, double beat, const RenderOpts& o,
                   FieldEval& E, int vpX, float scrollNow,
                   float songTime, double nowSec, float bpm);

    // Requires a current GL context. assetDir must end with a separator.
    bool init(int w, int h, const std::string& assetDir);
    void resize(int w, int h);

    // Draw one frame of `chart` at `beat` into `postTarget` (0 = backbuffer).
    void drawFrame(const Chart& chart, double beat, const RenderOpts& o,
                   GLuint postTarget);

    // --- actor layer -------------------------------------------------------
    // Actors are 2D, in SM's virtual 640x480 space, drawn with their OWN
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

    Mat4 mvp_{};
    GLuint prog_ = 0, post_ = 0, glow_ = 0, susGlow_ = 0, actor_ = 0, cover_ = 0, piu_ = 0;
    GLuint avao_ = 0, avbo_ = 0;
    double actorBeat_ = 0.0;
    double fieldSec_ = 0.0;   // when evalField reads the Plr() field proxies
    ActorLayer* actors_ = nullptr;
    Background* bg_ = nullptr;
    GLuint vao_ = 0, vbo_ = 0, qvao_ = 0, qvbo_ = 0;
    GLuint fbo_ = 0, colorTex_ = 0, postFbo_ = 0, postTex_ = 0;
    // Depth attachment on fbo_ ONLY, for the actor layer's z-mask
    // (blend,noeffect + zwrite writes it; ztest reads it). The highway never
    // enables depth testing -- every CH layer is ZWrite Off and the painter
    // order is load-bearing -- so this is inert for everything but actors.
    GLuint depthRb_ = 0;
    // One more colour target, for --fxshader: a playfield shader cannot read
    // and write the same texture, so the scene is shaded colorTex_ -> fxTex_
    // and the post chain then reads fxTex_ instead. Allocated always, bound
    // only when a scene layer exists.
    GLuint fxFbo_ = 0, fxTex_ = 0;
    GLint locPremul_ = -1;

    Tex texHighway_, texSide_, texString_, texBeat_;
    Tex texNotes_, texAnim_, texOpen_, texSustain_;
    // PIU mode: three arts each, indexed by ch::PIU_ART (DownLeft/UpLeft/Center).
    Tex texPiuTap_[3], texPiuRecep_[3], texPiuHoldBody_[3], texPiuHoldCap_[3];
    Tex texPiuFlash_;
    Tex texFretB_, texFretH_, texLift_, texHLight_;

    std::vector<ch::Vtx> v_;
    std::vector<double>  hitTimes_[5];

    // glow > 0 re-draws the same quad as a flat-white silhouette on top, which
    // is ITG's second sprite pass (see NOTE_GLOW_FS). 0 skips it entirely.
    void drawLayer(GLuint tex, int blend, float glow = 0.0f);
    void drawSustainGlow();
    void drawGlowLayer(GLuint tex);
    void drawPiuLayer(GLuint tex, int blend);
    // The pump playfield -- a wholly separate field, not a reskin. See the
    // comment on the definition.
    void drawPiu(const Chart& chart, double beat, const RenderOpts& o,
                 const Mods& mods, float songTime, float scrollNow,
                 float noteSpeed, float bpm);
    void buildCamera();
};

}  // namespace nc
