// The background layer: #BGCHANGES stills/movies from a .sm beside the chart,
// plus optional GLSL .frag shader layers with the NotITG uniform conventions.
// Specced in devdocs/spec/background.md; drawn as the true back plane of fbo_
// so the post chain processes it, exactly like NotITG's layout.xml captures
// background + playfields before post.frag runs.
//
// Caller-owned, exactly like ActorLayer: background.h needs Tex (renderer.h),
// so Renderer holding one by value would be the include cycle actor.h already
// hit. The caller constructs it after Renderer::init (texture/shader loads
// need GL) and hands over a pointer; a null pointer skips the pass entirely,
// which is what keeps the pinned hashes pinned (REM III has no .sm).
//
// THE SEEK CONSTRAINT. Everything is a pure function of the frame's audio
// time: the active change is a binary search over start seconds, a movie's
// position is the closed form (t - startSec) * rate (mod duration when
// looping), and the decoder is a cache, not state (video.h).
#pragma once

#include "renderer.h"
#include "smbg.h"
#include "video.h"

#include <memory>
#include <string>
#include <vector>

namespace nc {

class Background {
public:
    // #BGCHANGES media entries from the .sm, resolved against songDir. Folder
    // entries (actor trees) are the ActorLayer's job and are skipped silently
    // here. videoScale is --bgscale: movies decode at that fraction of native.
    bool loadFromSm(const std::string& smPath, const std::string& songDir,
                    float videoScale);

    // A GLSL fragment-shader layer (--bgshader), drawn over any media layer in
    // the order added. Compile failure logs and returns false; nothing is
    // added. Uniforms beyond the built-in set register as bg.<name> knobs a
    // .ncmod can drive.
    bool addShader(const std::string& fragPath);

    // A PLAYFIELD shader (--fxshader). Same file format and uniform contract
    // as a background layer, but instead of drawing behind everything it runs
    // over the already-rendered frame with sampler0 bound to it -- so it can
    // warp, tint or fold the playfield itself. Its uniforms register as
    // fx.<name> knobs. Runs before the built-in post chain.
    bool addSceneShader(const std::string& fragPath);
    bool hasScenePass() const;

    // Run every scene layer over `sceneTex`. The caller has bound the target
    // FBO; this leaves program/VAO/texture bindings clobbered exactly as
    // draw() does.
    void drawScene(GLuint sceneTex, int W, int H, double songTime, float beat,
                   float bpm, float fieldX, float fieldY,
                   const float* bgKnobs, unsigned bgDriven);

    // Re-read every shader layer whose file has been written since it was
    // loaded, and rebuild it in place. Returns true if any layer was retried.
    // A failed rebuild KEEPS the old program -- an editor mid-edit sees the
    // last shader that compiled, plus the error in log(), not a black frame.
    //
    // The editor calls this once a frame; the encoder never does. That is not
    // a second draw path (AGENTS.md "two binaries, one renderer") -- it
    // changes what is *loaded*, and a shader must not change under an encode.
    bool reloadIfChanged();

    bool empty() const { return layers_.empty(); }
    size_t layerCount() const { return layers_.size(); }
    // One line per layer for the editor's panel: "shader <path>" or
    // "media: N changes".
    std::string layerDesc(size_t i) const;
    const std::vector<std::string>& log() const { return log_; }
    void clearLog() { log_.clear(); }   // log_ only ever grows; a reloading
                                        // editor accumulates every retry
    double benchMs() const;   // media decode+upload time, for --bench

    // Draw every layer into the currently bound FBO. The first quad drawn
    // replaces the clear (GL_ONE, GL_ZERO); everything after alpha-blends.
    // Clobbers program / VAO / texture binding and the blend function --
    // drawFrame re-establishes all of them right after -- but leaves depth
    // test off, depth writes off and blending enabled, the init state.
    //
    // songTime is AUDIO time (nowSec), never the scroll axis: stops freeze the
    // scroll, not the film. bgKnobs is MAX_BG_UNIFORMS floats (may be null);
    // bgDriven is a bitmask of which of those slots the modchart actually
    // drives -- an undriven uniform is never set, so the shader's own default
    // applies (devdocs/spec/background.md section 6.4).
    void draw(int W, int H, double songTime, float beat, float bpm,
              float fieldX, float fieldY,
              const float* bgKnobs, unsigned bgDriven);

private:
    struct Knob { GLint loc; int slot; };   // shader uniform -> bg knob slot
    struct Layer {
        bool shader = false;
        bool scenePass = false;       // --fxshader: runs OVER the frame

        // -- media layer --
        SmBgList list;
        std::vector<Tex> stills;      // per change; id 0 = not a still
        std::vector<int> videoIdx;    // per change; index into videos_, or -1
        // -- shader layer --
        std::string path;
        FILETIME mtime = {};          // last write seen; hot-reload trigger
        GLuint prog = 0;
        GLint locTime = -1, locBeat = -1, locBpm = -1, locRes = -1,
              locRes2 = -1, locTexSize = -1, locImgSize = -1, locField = -1,
              locSampler = -1;
        std::vector<Knob> knobs;
    };

    void ensureGL();                  // fullscreen VAO + blit program + white 1x1
    // Compile L.path into L, replacing prog/locs/knobs only on success.
    bool buildShader(Layer& L);
    void blit(GLuint tex, float alpha, bool opaque);

    std::vector<Layer> layers_;
    std::vector<std::unique_ptr<VideoStream>> videos_;
    float videoScale_ = 0.5f;
    GLuint vao_ = 0, vbo_ = 0, blit_ = 0, white_ = 0;
    GLint blitLocAlpha_ = -1;
    std::vector<std::string> log_;
};

}  // namespace nc
