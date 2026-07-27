#include "background.h"

#include "modfile.h"

#include <cctype>
#include <cmath>
#include <cstring>

namespace nc {

// ---------------------------------------------------------------------------
// Shaders. imageCoord is (0,0) at the TOP-LEFT of the output image -- NotITG's
// sprite convention, which is what lets a NotITG .frag port with no coordinate
// edits (devdocs/spec/background.md section 2.3). The FBO is GL-convention
// (y=0 bottom) and both write paths flip rows, so FBO top == output top and
// the VS emits y = 0.5 - aPos.y*0.5. Video rows arrive top-down and upload to
// v=0 first, so sampling at imageCoord is upright with no vflip; stills load
// with flipY=false for the same reason.
// ---------------------------------------------------------------------------
static const char* BG_VS_330 = R"(#version 330
layout(location=0) in vec2 aPos;
out vec2 imageCoord;
out vec2 textureCoord;
void main() {
    imageCoord = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
    textureCoord = imageCoord;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// A #version 120 FS declaring `varying vec2 imageCoord` cannot link against a
// 330 VS declaring `out`, so the loader pairs by version. 120 links because
// both binaries hold compatibility-profile contexts (editor/main.cpp:99,
// src/main.cpp:353's legacy wglCreateContext).
static const char* BG_VS_120 = R"(#version 120
attribute vec2 aPos;
varying vec2 imageCoord;
varying vec2 textureCoord;
void main() {
    imageCoord = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
    textureCoord = imageCoord;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* BG_BLIT_FS = R"(#version 330
in vec2 imageCoord;
out vec4 oCol;
uniform sampler2D sampler0;
uniform float uAlpha;
void main() {
    vec4 c = texture(sampler0, imageCoord);
    oCol = vec4(c.rgb, c.a * uAlpha);
}
)";

// ---------------------------------------------------------------------------
// Non-fatal program build. gl_program/gl_compile exit(1) on failure, which is
// right for built-in shaders and wrong for a user's .frag -- a typo must log
// and degrade, not kill an encode.
// ---------------------------------------------------------------------------
static GLuint bgCompile(GLenum type, const char* src, std::string& log) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[4096]; glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        log += buf;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint bgProgram(const char* vs, const char* fs, bool bindAPos,
                        std::string& log) {
    GLuint v = bgCompile(GL_VERTEX_SHADER, vs, log);
    if (!v) return 0;
    GLuint f = bgCompile(GL_FRAGMENT_SHADER, fs, log);
    if (!f) { glDeleteShader(v); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    // The 120 path has no layout qualifier; do not rely on attribute 0
    // aliasing gl_Vertex.
    if (bindAPos) glBindAttribLocation(p, 0, "aPos");
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[4096]; glGetProgramInfoLog(p, sizeof buf, nullptr, buf);
        log += buf;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ---------------------------------------------------------------------------
void Background::ensureGL() {
    if (vao_) return;
    // Own fullscreen triangle rather than borrowing the renderer's qvao_:
    // Background is caller-owned and must not reach into Renderer's privates.
    const float quad[6] = {-1, -1, 3, -1, -1, 3};
    glGenVertexArrays(1, &vao_); glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    blit_ = gl_program(BG_VS_330, BG_BLIT_FS, "bgBlit");
    blitLocAlpha_ = glGetUniformLocation(blit_, "uAlpha");
    glUseProgram(blit_);
    glUniform1i(glGetUniformLocation(blit_, "sampler0"), 0);

    // sampler0 for a pure-shader layer: 1x1 opaque white, matching NotITG's
    // Texture="white" sprite (layout.xml:12).
    const unsigned char px[4] = {255, 255, 255, 255};
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

// ---------------------------------------------------------------------------
// SM5's own extension table, ActorUtil.cpp:535-559.
static bool isBitmapExt(const std::string& e) {
    return e == "bmp" || e == "gif" || e == "jpeg" || e == "jpg" || e == "png";
}
static bool isMovieExt(const std::string& e) {
    return e == "avi" || e == "f4v" || e == "flv" || e == "mkv" || e == "mp4" ||
           e == "mpeg" || e == "mpg" || e == "mov" || e == "ogv" ||
           e == "webm" || e == "wmv";
}

static std::string lowerExtOf(const std::string& file) {
    const size_t sl = file.find_last_of("/\\");
    const std::string base = sl == std::string::npos ? file : file.substr(sl + 1);
    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = base.substr(dot + 1);
    for (char& c : e) c = char(tolower((unsigned char)c));
    return e;
}

bool Background::loadFromSm(const std::string& smPath, const std::string& songDir,
                            float videoScale) {
    videoScale_ = videoScale;
    FILE* f = fopen(smPath.c_str(), "rb");
    if (!f) { log_.push_back("cannot open " + smPath); return false; }
    std::string raw;
    { char b[65536]; size_t n;
      while ((n = fread(b, 1, sizeof b, f)) > 0) raw.append(b, n); }
    fclose(f);

    const std::string s = SmTiming::stripComments(raw);
    Layer L;
    L.list.timing.parse(s);

    // #ANIMATIONS is the pre-3.9 spelling of #BGCHANGES; both are layer 1
    // (NotesLoaderSM.cpp:369). #BGCHANGES2 has no test case -- warn and skip.
    if (s.find("#BGCHANGES2:") != std::string::npos)
        log_.push_back("#BGCHANGES2 present -- layer 2 not supported, skipped");
    for (const char* tag : {"BGCHANGES", "ANIMATIONS"}) {
        std::string body;
        if (!SmTiming::tagValue(s, tag, body)) continue;
        // Comma split ignores empty tokens (RageUtil.h:321 bIgnoreEmpty).
        size_t a = 0;
        while (a <= body.size()) {
            const size_t b = body.find(',', a);
            const std::string ent = body.substr(
                a, b == std::string::npos ? std::string::npos : b - a);
            a = (b == std::string::npos) ? body.size() + 1 : b + 1;
            if (ent.find_first_not_of(" \t\r\n") == std::string::npos) continue;

            SmBgChange c;
            if (!smBgDecode(ent, c)) continue;
            if (c.file1.empty() || c.file1[0] == '-') continue;  // -nosongbg- etc
            const std::string ext = lowerExtOf(c.file1);
            if (ext.empty()) continue;   // a folder = an actor tree, the
                                         // ActorLayer's job, not ours
            if (!isBitmapExt(ext) && !isMovieExt(ext)) {
                log_.push_back("#" + std::string(tag) + ": '" + c.file1 +
                               "' (." + ext + ") is not a supported still or "
                               "movie -- skipped");
                continue;
            }
            c.media = isMovieExt(ext) ? BgMedia::Video : BgMedia::Still;
            c.path1 = songDir + "/" + c.file1;
            FILE* probe = fopen(c.path1.c_str(), "rb");
            if (!probe) {
                log_.push_back("#" + std::string(tag) + ": '" + c.file1 +
                               "' not found beside the chart -- skipped");
                continue;
            }
            fclose(probe);
            c.startSec = L.list.timing.beatToSec(c.startBeat);
            if (!c.effect.empty() && c.effect != "StretchNormal" &&
                c.effect != "StretchNoLoop" && c.effect != "StretchRewind")
                log_.push_back("effect '" + c.effect + "' on '" + c.file1 +
                               "' draws as a plain stretch for now");
            if (!c.transition.empty() && c.transition != "CrossFade")
                log_.push_back("transition '" + c.transition + "' on '" +
                               c.file1 + "' degrades to a hard cut");
            L.list.changes.push_back(c);
        }
    }
    if (L.list.changes.empty()) return true;   // nothing scheduled: no layer

    // std::sort in SM is not stable; stable_sort cannot disagree with it on a
    // well-formed file and is deterministic on a malformed one
    // (BackgroundUtil.cpp:73-76, devdocs/spec/background.md section 1.5).
    std::stable_sort(L.list.changes.begin(), L.list.changes.end(),
                     [](const SmBgChange& x, const SmBgChange& y) {
                         return x.startBeat < y.startBeat;
                     });

    ensureGL();
    L.stills.resize(L.list.changes.size());
    L.videoIdx.assign(L.list.changes.size(), -1);
    std::vector<std::string> vpaths;
    for (size_t i = 0; i < L.list.changes.size(); ++i) {
        SmBgChange& c = L.list.changes[i];
        if (c.media == BgMedia::Still) {
            // Stills are top-left origin like actor sprites: flipY=false.
            L.stills[i] = gl_loadTex(c.path1, /*repeat*/ false, /*flipY*/ false);
        } else {
            int vi = -1;
            for (size_t j = 0; j < vpaths.size(); ++j)
                if (vpaths[j] == c.path1) { vi = int(j); break; }
            if (vi < 0) {
                auto vs = std::make_unique<VideoStream>();
                if (!vs->open(c.path1, videoScale_)) {
                    log_.push_back("cannot open movie '" + c.file1 + "' -- skipped");
                    continue;
                }
                char m[256];
                snprintf(m, sizeof m, "movie %s: decoding %dx%d @ %.3f fps, %.2fs",
                         c.file1.c_str(), vs->texW(), vs->texH(),
                         vs->fps(), vs->duration());
                log_.push_back(m);
                vi = int(videos_.size());
                videos_.push_back(std::move(vs));
                vpaths.push_back(c.path1);
            }
            L.videoIdx[i] = vi;
        }
        char m[256];
        snprintf(m, sizeof m, "%.3f -> %.3fs  %s%s%s", c.startBeat, c.startSec,
                 c.file1.c_str(), c.effect.empty() ? "" : " ",
                 c.effect.c_str());
        log_.push_back(m);
    }
    layers_.push_back(std::move(L));
    return true;
}

// ---------------------------------------------------------------------------
// A knob's uniform must be set at its DECLARED type: glUniform1f into an
// `int` uniform is silently ignored by GL, leaving it at 0 -- which for a
// shader's `blockSize` is a divide by zero and a black frame.
static void setKnob(GLint loc, unsigned type, float v) {
    if (loc < 0) return;
    if (type == GL_INT || type == GL_BOOL) glUniform1i(loc, int(v));
    else                                   glUniform1f(loc, v);
}

bool Background::buildShader(Layer& L) {
    const std::string& fragPath = L.path;
    FILE* f = fopen(fragPath.c_str(), "rb");
    if (!f) { log_.push_back("cannot open " + fragPath); return false; }
    std::string src;
    { char b[65536]; size_t n;
      while ((n = fread(b, 1, sizeof b, f)) > 0) src.append(b, n); }
    fclose(f);

    // Pair the FS with a matching VS by its #version: 120 gets the varying VS,
    // 150+ is treated as 330. Anything else is an error naming what was found.
    // No #version line at all means 120 -- that is what NotITG assumes, and
    // real ported shaders rely on it (datamosh's blockmatch and findbest both
    // open straight onto a uniform declaration). Erroring here rejected files
    // that are perfectly valid GLSL 120.
    int ver = 120;
    { const size_t p = src.find("#version");
      if (p != std::string::npos) ver = atoi(src.c_str() + p + 8); }
    const char* vs = nullptr;
    if (ver == 120) vs = BG_VS_120;
    else if (ver >= 150) vs = BG_VS_330;
    else {
        char m[128];
        snprintf(m, sizeof m, "unsupported '#version %d' (want 120 or 150+)", ver);
        log_.push_back(fragPath + ": " + m);
        return false;
    }

    ensureGL();
    std::string clog;
    GLuint prog = bgProgram(vs, src.c_str(), ver == 120, clog);
    if (!prog) {
        log_.push_back("shader " + fragPath + " failed to compile:\n" + clog);
        return false;   // L untouched: a reload keeps the last good program
    }

    if (L.prog) glDeleteProgram(L.prog);
    L.knobs.clear();
    L.prog = prog;
    L.locTime    = glGetUniformLocation(prog, "time");
    L.locBeat    = glGetUniformLocation(prog, "beat");
    L.locBpm     = glGetUniformLocation(prog, "bpm");
    L.locRes     = glGetUniformLocation(prog, "resolution");
    L.locRes2    = glGetUniformLocation(prog, "res");
    L.locTexSize = glGetUniformLocation(prog, "textureSize");
    L.locImgSize = glGetUniformLocation(prog, "imageSize");
    L.locField   = glGetUniformLocation(prog, "field");
    L.locSampler = glGetUniformLocation(prog, "sampler0");
    glUseProgram(prog);
    if (L.locSampler >= 0) glUniform1i(L.locSampler, 0);

    // Every other float uniform is a bg.<name> knob a .ncmod can drive.
    // Undriven knobs keep the shader's own defaults -- which may be black
    // (dream.frag's bright defaults to 0); the status line says so.
    static const char* BUILTIN[] = {"time", "beat", "bpm", "resolution", "res",
                                    "textureSize", "imageSize", "field",
                                    "sampler0"};
    GLint nu = 0;
    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &nu);
    L.samplers.clear();
    std::string names;
    for (GLint i = 0; i < nu; ++i) {
        char nm[256]; GLsizei len = 0; GLint sz = 0; GLenum ty = 0;
        glGetActiveUniform(prog, GLuint(i), sizeof nm, &len, &sz, &ty, nm);
        if (strchr(nm, '[')) continue;
        bool builtin = false;
        for (const char* b : BUILTIN) if (!strcmp(nm, b)) { builtin = true; break; }
        if (builtin) continue;
        // Any OTHER sampler2D is a chain input, bound by name to a buffer.
        if (ty == GL_SAMPLER_2D) {
            L.samplers.push_back({nm, glGetUniformLocation(prog, nm)});
            continue;
        }
        // int and bool are as driveable as float -- blockSize is an int, and
        // setting it with glUniform1f leaves it at 0.
        if (ty != GL_FLOAT && ty != GL_INT && ty != GL_BOOL) continue;
        const char* pre = L.scenePass ? "fx." : "bg.";
        const int slot = modBgSlot(std::string(pre) + nm);
        if (slot < 0) {
            log_.push_back(std::string("knob table full -- '") + pre + nm +
                           "' cannot be driven");
            continue;
        }
        L.knobs.push_back({glGetUniformLocation(prog, nm), slot - MOD_BG_BASE,
                           unsigned(ty)});
        if (!names.empty()) names += " ";
        names += pre;
        names += nm;
    }
    log_.push_back("shader " + fragPath +
                   (names.empty() ? ": no knobs"
                                  : ": knobs " + names +
                                    " (undriven knobs keep the shader's own "
                                    "defaults, which may render black)"));
    return true;
}

// Last write time of `path`, or a zeroed FILETIME if it cannot be stat'ed --
// which compares unequal to any real stamp, so a file that reappears reloads.
static FILETIME fileMtime(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d))
        return FILETIME{};
    return d.ftLastWriteTime;
}

bool Background::addSceneShader(const std::string& fragPath) {
    Layer L;
    L.shader = true;
    L.scenePass = true;
    L.path = fragPath;
    if (!buildShader(L)) return false;
    L.mtime = fileMtime(fragPath);
    layers_.push_back(std::move(L));
    return true;
}

bool Background::hasScenePass() const {
    for (const Layer& L : layers_) if (L.scenePass && L.prog) return true;
    return false;
}

// Every playfield layer, in the order added, each reading the rendered frame
// as sampler0 and replacing it. Blending is OFF: the pass IS the frame now,
// not something composited over it.
void Background::drawScene(GLuint sceneTex, int W, int H, double songTime,
                           float beat, float bpm, float fieldX, float fieldY,
                           const float* bgKnobs, unsigned bgDriven) {
    ensureGL();
    for (Layer& L : layers_) {
        if (!L.scenePass || !L.prog) continue;
        glUseProgram(L.prog);
        if (L.locTime >= 0) glUniform1f(L.locTime, float(songTime));
        if (L.locBeat >= 0) glUniform1f(L.locBeat, beat);
        if (L.locBpm  >= 0) glUniform1f(L.locBpm, bpm);
        if (L.locRes  >= 0) glUniform2f(L.locRes, float(W), float(H));
        if (L.locRes2 >= 0) glUniform2f(L.locRes2, float(W), float(H));
        if (L.locTexSize >= 0) glUniform2f(L.locTexSize, float(W), float(H));
        if (L.locImgSize >= 0) glUniform2f(L.locImgSize, float(W), float(H));
        if (L.locField >= 0) glUniform2f(L.locField, fieldX, fieldY);
        if (L.locSampler >= 0) glUniform1i(L.locSampler, 0);
        for (const Knob& k : L.knobs)
            if (k.loc >= 0 && bgKnobs && (bgDriven & (1u << k.slot)))
                setKnob(k.loc, k.type, bgKnobs[k.slot]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneTex);
        glDisable(GL_BLEND);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_BLEND);
    }
}

// ---------------------------------------------------------------------------
// Multi-pass chains (--fxchain). See the contract on Background::loadChain in
// background.h -- especially the part about feedback and seeking.
// ---------------------------------------------------------------------------

Background::Buf& Background::ensureBuf(const std::string& name, int W, int H) {
    Buf& b = bufs_[name];
    if (b.tex[0] && b.w == W && b.h == H) return b;
    for (int i = 0; i < 2; ++i) {
        if (b.fbo[i]) { glDeleteFramebuffers(1, &b.fbo[i]); b.fbo[i] = 0; }
        if (b.tex[i]) { glDeleteTextures(1, &b.tex[i]);     b.tex[i] = 0; }
    }
    b.w = W; b.h = H; b.cur = 0;
    for (int i = 0; i < 2; ++i) {
        glGenTextures(1, &b.tex[i]);
        glBindTexture(GL_TEXTURE_2D, b.tex[i]);
        // CLAMP, not repeat: a block-matching pass reads far outside its own
        // texel, and wrapping would fold the far edge of the frame into the
        // search. NEAREST because these buffers hold packed data, not
        // pictures -- interpolating a packed vector is nonsense.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &b.fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, b.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, b.tex[i], 0);
        // Black, not garbage: this IS the first frame a feedback pass reads.
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    return b;
}

GLuint Background::bufRead(const std::string& src, GLuint sceneTex) {
    if (src == "@frame") return sceneTex;
    auto it = bufs_.find(src);
    return it == bufs_.end() ? white_ : it->second.tex[it->second.cur];
}

bool Background::loadChain(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { log_.push_back("cannot open chain " + path); return false; }
    std::string txt;
    { char b[65536]; size_t n;
      while ((n = fread(b, 1, sizeof b, f)) > 0) txt.append(b, n); }
    fclose(f);

    // Shader paths resolve against the CHAIN FILE's folder, not the cwd, so a
    // chain and its .frag files move together as one directory.
    const size_t sl = path.find_last_of("/\\");
    const std::string base = sl == std::string::npos ? std::string(".")
                                                     : path.substr(0, sl);
    auto trim = [](std::string t) {
        const size_t a = t.find_first_not_of(" \t\r");
        if (a == std::string::npos) return std::string();
        return t.substr(a, t.find_last_not_of(" \t\r") - a + 1);
    };

    chain_.clear();
    chainOut_.clear();
    std::vector<std::string> written;      // buffers written SO FAR, in order
    size_t lineNo = 0;
    for (size_t i = 0; i < txt.size();) {
        size_t e = txt.find('\n', i);
        if (e == std::string::npos) e = txt.size();
        std::string line = txt.substr(i, e - i);
        i = e + 1;
        ++lineNo;
        const size_t h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (!trim(line).empty()) {
                char m[128];
                snprintf(m, sizeof m, "chain line %u: expected '<name> = ...'",
                         unsigned(lineNo));
                log_.push_back(path + ": " + m);
                chain_.clear();
                return false;
            }
            continue;
        }
        const std::string lhs = trim(line.substr(0, eq));
        const std::string rhs = line.substr(eq + 1);
        if (lhs.empty()) continue;

        std::vector<std::string> tok;
        { size_t j = 0;
          while (j < rhs.size()) {
              while (j < rhs.size() && isspace((unsigned char)rhs[j])) ++j;
              std::string t;
              while (j < rhs.size() && !isspace((unsigned char)rhs[j])) t += rhs[j++];
              if (!t.empty()) tok.push_back(t);
          } }
        if (tok.empty()) continue;

        if (lhs == "out") { chainOut_ = tok[0]; continue; }

        Pass P;
        P.out = lhs;
        if (tok[0] == "@copy") {
            if (tok.size() < 2) {
                log_.push_back(path + ": @copy needs a source");
                chain_.clear(); return false;
            }
            P.layer = SIZE_MAX;
            P.copySrc = tok[1];
        } else {
            Layer L;
            L.shader = true;
            L.scenePass = true;             // knobs register as fx.<name>
            L.path = (tok[0].size() > 1 && (tok[0][0] == '/' || tok[0][1] == ':'))
                   ? tok[0] : base + "/" + tok[0];
            if (!buildShader(L)) { chain_.clear(); return false; }
            L.mtime = fileMtime(L.path);
            P.layer = layers_.size();
            layers_.push_back(std::move(L));
            for (size_t t = 1; t < tok.size(); ++t) {
                const size_t a = tok[t].find('=');
                if (a == std::string::npos) continue;
                P.binds.push_back({tok[t].substr(0, a), tok[t].substr(a + 1)});
            }
        }
        // A read of a buffer not yet written THIS frame is a FEEDBACK edge --
        // it sees the previous frame. Detected here so the editor can say so
        // rather than silently showing a picture the encode will not produce.
        auto isFeedback = [&](const std::string& src) {
            if (src == "@frame") return false;
            for (const std::string& w : written) if (w == src) return false;
            return true;
        };
        if (P.layer == SIZE_MAX) {
            if (isFeedback(P.copySrc)) chainFeedback_ = true;
        } else {
            for (const auto& b : P.binds)
                if (isFeedback(b.second)) chainFeedback_ = true;
        }

        written.push_back(P.out);
        chain_.push_back(std::move(P));
    }

    if (chain_.empty()) { log_.push_back(path + ": no passes"); return false; }
    if (chainOut_.empty()) chainOut_ = chain_.back().out;
    char m[512];
    snprintf(m, sizeof m, "chain %s: %u passes -> '%s'%s", path.c_str(),
             unsigned(chain_.size()), chainOut_.c_str(),
             chainFeedback_ ? "  [FEEDBACK: exact in an encode, approximate in "
                              "the editor until it settles after a seek]" : "");
    log_.push_back(m);
    return true;
}

GLuint Background::drawChain(GLuint sceneTex, int W, int H, double songTime,
                             float beat, float bpm, float fieldX, float fieldY,
                             const float* bgKnobs, unsigned bgDriven) {
    if (chain_.empty()) return sceneTex;
    ensureGL();
    for (const Pass& P : chain_) {
        Buf& out = ensureBuf(P.out, W, H);
        const int dst = 1 - out.cur;
        glBindFramebuffer(GL_FRAMEBUFFER, out.fbo[dst]);
        glViewport(0, 0, W, H);
        glDisable(GL_BLEND);

        if (P.layer == SIZE_MAX) {
            glUseProgram(blit_);
            if (blitLocAlpha_ >= 0) glUniform1f(blitLocAlpha_, 1.0f);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, bufRead(P.copySrc, sceneTex));
        } else {
            Layer& L = layers_[P.layer];
            if (!L.prog) { glEnable(GL_BLEND); continue; }
            glUseProgram(L.prog);
            if (L.locTime >= 0) glUniform1f(L.locTime, float(songTime));
            if (L.locBeat >= 0) glUniform1f(L.locBeat, beat);
            if (L.locBpm  >= 0) glUniform1f(L.locBpm, bpm);
            if (L.locRes  >= 0) glUniform2f(L.locRes, float(W), float(H));
            if (L.locRes2 >= 0) glUniform2f(L.locRes2, float(W), float(H));
            if (L.locTexSize >= 0) glUniform2f(L.locTexSize, float(W), float(H));
            if (L.locImgSize >= 0) glUniform2f(L.locImgSize, float(W), float(H));
            if (L.locField >= 0) glUniform2f(L.locField, fieldX, fieldY);
            for (const Knob& k : L.knobs)
                if (k.loc >= 0 && bgKnobs && (bgDriven & (1u << k.slot)))
                    setKnob(k.loc, k.type, bgKnobs[k.slot]);

            // Unit 0 is sampler0; every named sampler gets a unit after it.
            std::string s0 = "@frame";
            for (const auto& b : P.binds) if (b.first == "sampler0") s0 = b.second;
            if (L.locSampler >= 0) glUniform1i(L.locSampler, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, bufRead(s0, sceneTex));
            int unit = 1;
            for (const auto& sm : L.samplers) {
                std::string src;
                for (const auto& b : P.binds) if (b.first == sm.first) src = b.second;
                if (src.empty()) continue;          // unbound: leave it on 0
                glUniform1i(sm.second, unit);
                glActiveTexture(GLenum(GL_TEXTURE0 + unit));
                glBindTexture(GL_TEXTURE_2D, bufRead(src, sceneTex));
                ++unit;
            }
            glActiveTexture(GL_TEXTURE0);
        }
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_BLEND);
        out.cur = dst;                    // published; later reads see it
    }
    auto it = bufs_.find(chainOut_);
    return it == bufs_.end() ? sceneTex : it->second.tex[it->second.cur];
}

bool Background::addShader(const std::string& fragPath) {
    Layer L;
    L.shader = true;
    L.path = fragPath;
    if (!buildShader(L)) return false;
    L.mtime = fileMtime(fragPath);
    layers_.push_back(std::move(L));
    return true;
}

bool Background::reloadIfChanged() {
    bool any = false;
    for (Layer& L : layers_) {
        if (!L.shader) continue;
        const FILETIME m = fileMtime(L.path);
        if (m.dwLowDateTime == L.mtime.dwLowDateTime &&
            m.dwHighDateTime == L.mtime.dwHighDateTime) continue;
        // Stamp before rebuilding, so a shader that keeps failing is retried
        // once per edit rather than once per frame.
        L.mtime = m;
        buildShader(L);
        any = true;
    }
    return any;
}

// ---------------------------------------------------------------------------
std::string Background::layerDesc(size_t i) const {
    if (i >= layers_.size()) return "";
    const Layer& L = layers_[i];
    if (L.shader) return "shader " + L.path;
    char m[64];
    snprintf(m, sizeof m, "media: %d changes", int(L.list.changes.size()));
    return m;
}

double Background::benchMs() const {
    double t = 0;
    for (const auto& v : videos_) t += v->benchMs();
    return t;
}

void Background::blit(GLuint tex, float alpha, bool opaque) {
    glUseProgram(blit_);
    glUniform1f(blitLocAlpha_, alpha);
    // Layer 0 defines the frame and replaces the clear; overlays use SM's
    // default BLEND_NORMAL (devdocs/spec/background.md section 3.1).
    if (opaque) glBlendFunc(GL_ONE, GL_ZERO);
    else        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void Background::draw(int W, int H, double songTime, float beat, float bpm,
                      float fieldX, float fieldY,
                      const float* bgKnobs, unsigned bgDriven) {
    if (layers_.empty()) return;
    glActiveTexture(GL_TEXTURE0);
    bool first = true;

    // Texture for change `i` of a media layer, with the movie advanced to the
    // closed-form position: local = (t - startSec) * rate, looped or clamped
    // (Background.cpp:811-814's fDeltaTime, SetUpdateRate at :790). Deliberate
    // divergence, documented in the spec: SM pauses an off-focus movie, so its
    // position integrates on-screen time; ours is the offset from the change's
    // own start. They agree whenever a file appears in the list once.
    auto mediaTex = [&](Layer& L, int i) -> GLuint {
        const SmBgChange& c = L.list.changes[i];
        if (c.media == BgMedia::Still) return L.stills[i].id;
        if (L.videoIdx[i] < 0) return 0;
        VideoStream& v = *videos_[L.videoIdx[i]];
        double local = (songTime - c.startSec) * c.rate;
        const double dur = v.duration();
        if (c.loops()) {
            local = fmod(local, dur);
            if (local < 0) local += dur;
        } else {
            const double last = dur - 1.0 / v.fps();   // hold the last frame
            local = local < 0 ? 0 : (local > last ? last : local);
        }
        v.ensureFrame(int(floor(local * v.fps())));
        return v.hasFrame() ? v.tex() : 0;
    };

    for (Layer& L : layers_) {
        if (L.scenePass) continue;      // runs later, over the finished frame
        if (!L.shader) {
            const int i = L.list.indexAt(songTime);
            if (i < 0) continue;               // before the first change: the
                                               // black clear stands, as SM's
                                               // pre-first-segment does
            const GLuint tex = mediaTex(L, i);
            if (tex) { blit(tex, 1.0f, first); first = false; }

            // CrossFade: 1.0s linear on the OLD background only, drawn OVER
            // the new one, scaled by the PREVIOUS change's rate
            // (CrossFade.xml; Background.cpp:806, :887-894). Equal defs mean
            // no fade at all -- both resolve to the same actor (:783-786).
            if (i > 0 && L.list.changes[i].transition == "CrossFade" &&
                (L.list.changes[i].path1 != L.list.changes[i - 1].path1 ||
                 L.list.changes[i].effect != L.list.changes[i - 1].effect)) {
                const double fadeLen =
                    1.0 / std::max(L.list.changes[i - 1].rate, 1e-6);
                const double fadeLeft =
                    fadeLen - (songTime - L.list.changes[i].startSec);
                if (fadeLeft > 0.0) {
                    const GLuint old = mediaTex(L, i - 1);
                    if (old) blit(old, float(fadeLeft / fadeLen), false);
                }
            }
        } else {
            glUseProgram(L.prog);
            if (L.locTime >= 0)    glUniform1f(L.locTime, float(songTime));
            if (L.locBeat >= 0)    glUniform1f(L.locBeat, beat);
            if (L.locBpm >= 0)     glUniform1f(L.locBpm, bpm);
            if (L.locRes >= 0)     glUniform2f(L.locRes, float(W), float(H));
            if (L.locRes2 >= 0)    glUniform2f(L.locRes2, float(W), float(H));
            if (L.locTexSize >= 0) glUniform2f(L.locTexSize, float(W), float(H));
            if (L.locImgSize >= 0) glUniform2f(L.locImgSize, float(W), float(H));
            if (L.locField >= 0)   glUniform2f(L.locField, fieldX, fieldY);
            for (const Knob& k : L.knobs)
                if (bgKnobs && (bgDriven >> k.slot) & 1u)
                    setKnob(k.loc, k.type, bgKnobs[k.slot]);
            if (first) glBlendFunc(GL_ONE, GL_ZERO);
            else       glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBindTexture(GL_TEXTURE_2D, white_);
            glBindVertexArray(vao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            first = false;
        }
    }
}

}  // namespace nc
