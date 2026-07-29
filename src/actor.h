// The actor layer: StepMania/OpenITG XML actor trees, their command/tween
// engine, and the Lua that drives them.
//
// This is what `#FGCHANGES:0.000=lua=1.000=0=0=1=====;` loads -- the folder
// named in the change holds `default.lua` (or legacy `default.xml`) plus its
// own images. NotClon keeps the same convention, so a song folder is:
//
//     charts/<Song>/
//         notes.chart          the chart
//         <name>.ncmod         the modchart
//         <audio>.ogg          named by [Song] MusicStream
//         lua/default.lua      an actor tree, plus any images it references
//         effects/default.xml  a legacy tree, with effects/*.png beside it
//
// SCOPE, stated plainly. Six of Saitama2000's seven actor files contain zero
// Lua -- they are pure command lists -- so the command/tween engine carries
// most of the weight and the Lua host only has to serve `%function(self) ...
// end` attributes. The engine below is complete for the ITG command set those
// files use; the Lua binding surface is deliberately the subset Saitama needs
// (see LuaHost::open), because a binding nobody calls is a binding nobody has
// tested.
//
// THE SEEK CONSTRAINT. drawFrame() is called at an arbitrary beat with no
// history, so an actor cannot be a live object that ticks. Every actor here
// stores its command chain as a *timeline*: each command resolves to an
// absolute (startSec, endSec, from, to, easing) segment when its chain is
// scheduled, and evaluating an actor at time T is a search, not an
// integration. Lua setters capture into the same timeline when their command
// runs. Self-queued UpdateCommand loops are replayed deterministically; a
// backwards seek rebuilds the tree and replays it from its load time.
#pragma once

#include "chart.h"
#include "renderer.h"
#include "smbg.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace nc {
class LuaHost;
class ActorTree;
class ActorLayer;

struct PlayerModSnapshot {
    float target[MOD_COUNT] = {};
    float speed[MOD_COUNT] = {};
};

struct PlayerModChange {
    double sec = 0.0;
    int player = 1;
    int mod = MOD_DRUNK;
    float target = 0.0f;
    float speed = 1.0f;
};

struct LuaMessageParams {
    const char* player = nullptr;
    const char* tapNoteScore = nullptr;
};

// --- easings ---------------------------------------------------------------
// The seven ITG tweens. `sleep` is one of them: it holds the old value for the
// duration and then snaps, which is why it doubles as "reset the tween state".
enum class Ease {
    Instant, Linear, Accelerate, Decelerate, Spring, BounceBegin, BounceEnd,
    Sleep, InCubic, OutCubic, InOutCubic, InOutQuad, InOutSine,
    InCirc, OutCirc, InOutCirc, InExpo, OutExpo, InOutExpo,
    InQuart, OutQuart, InQuint, InOutQuint,
    PositiveSine6, NegativeSine6, Spring2
};
float easeApply(Ease e, float t);           // t in [0,1] -> eased fraction

// --- the animatable state of one actor -------------------------------------
struct ActorState {
    float x = 0, y = 0, z = 0, aux = 0;
    float zoomX = 1, zoomY = 1, zoomZ = 1;
    float rotX = 0, rotY = 0, rotZ = 0;     // degrees
    float r = 1, g = 1, b = 1, a = 1;
    float glowR = 1, glowG = 1, glowB = 1, glowA = 0;
    bool  hidden = false;
    // Sizing overrides for a textured actor, in virtual 640x480 SM pixels.
    float sizeX = -1, sizeY = -1;           // <0 = use the texture's own size
    float horizAlign = 0;                   // -1 left, 0 centre, +1 right
    float vertAlign = 0;                    // intermediate values are valid
    float skewX = 0;                        // x += skewX * y, SM's skewx
    float cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    int   blend = 0;                        // 0 normal, 1 add, 2 noeffect
    bool  zWrite = false, zTest = false, clearZ = false;
    // Camera state inherited from the nearest ActorFrame/WrapperState whose
    // FOV was set. -1 means the normal screen-space projection.
    float projFov = -1, vanishX = 0, vanishY = 0;
};

// One scheduled property change, resolved to absolute seconds.
struct Seg {
    int   prop;                             // PROP_* below
    float t0, t1;                           // absolute seconds
    Ease  ease;
    float from[4], to[4];
    int   n;                                // components used
};

enum {
    PROP_X, PROP_Y, PROP_Z, PROP_AUX, PROP_ZOOMX, PROP_ZOOMY, PROP_ZOOMZ,
    PROP_ROTX, PROP_ROTY, PROP_ROTZ, PROP_DIFFUSE, PROP_DIFFUSEALPHA,
    PROP_GLOWALPHA, PROP_HIDDEN, PROP_SIZE, PROP_HALIGN, PROP_VALIGN,
    PROP_BLEND, PROP_ZWRITE, PROP_ZTEST, PROP_CLEARZ, PROP_SKEWX,
    PROP_CROPLEFT, PROP_CROPRIGHT, PROP_CROPTOP, PROP_CROPBOTTOM, PROP_COUNT
};

// An Actor effect (bob/bounce/spin/wag/pulse/vibrate). Pure function of the
// effect clock, so it survives seeking for free.
struct Effect {
    enum Kind { None, Bob, Bounce, Spin, Wag, Pulse, Vibrate,
                DiffuseShift, DiffuseBlink, GlowShift } kind = None;
    float magX = 0, magY = 0, magZ = 0;
    float period = 1.0f;
    float delay = 0.0f;
    bool  beatClock = false;                // effectclock,'bgm'
    // effectcolor1/effectcolor2, for the diffuse effects. They MULTIPLY the
    // actor's own diffuse (SM5 Actor::UpdateInternal), which is what lets a
    // chart park a glow at diffusealpha 0 with the effect armed and fade the
    // whole thing in later -- replacing the diffuse would pin it visible.
    float c1[4] = {1, 1, 1, 1};
    float c2[4] = {1, 1, 1, 1};
};
struct ActorCommand {
    std::string name;
    std::string text;
    int luaRef = -1;                         // registry ref for %function bodies
};


struct Actor {
    ~Actor();
    std::string type;                       // ActorFrame | Sprite | Quad
    std::string name;
    std::string var;                        // legacy XML Var= Lua global
    std::string file;                       // texture path, resolved
    Tex         tex;
    bool        texLoaded = false;
    bool        isText = false;
    std::string font;
    std::string text;

    // --- ActorFrameTexture ---------------------------------------------------
    // SM5 binds this target and draws its children into it. NotITG XML instead
    // snapshots everything already drawn when the AFT sibling is reached.
    //
    // Create() allocates; SetTextureName() publishes it under a name other
    // actors can reference by Texture= or SetTexture().
    bool        isAft = false;
    bool        aftCapturePrevious = false;
    std::string aftName;
    int         aftW = 0, aftH = 0;
    bool        aftAlpha = false, aftDepth = false, aftFloat = false;
    bool        aftPreserve = false;
    GLuint      aftTex = 0, aftFbo = 0, aftDepthRb = 0;

    // --- .sprite animation ---------------------------------------------------
    // SM's animated-sprite descriptor: an ini naming a sheet plus a Frame/Delay
    // list. The sheet's grid comes from its FILENAME ("walk 2x1.png"),
    // the same convention the pump noteskin uses.
    //
    // Frame selection is a pure function of time plus the recorded SetState
    // keys. SetState resets SM's per-sprite animation clock; keeping that reset
    // as an absolute key makes a cold seek match a forward encode.
    struct SpriteStateKey { double sec; int state; };
    std::vector<int>   spriteFrames;   // frame index into the sheet, in order
    std::vector<float> spriteDelays;   // seconds each entry is held
    std::vector<SpriteStateKey> spriteStateKeys;
    float              spriteTotal = 0.0f;
    int                sheetCols = 1, sheetRows = 1;
    bool        textureFiltering = true;
    bool        textureWrapping = false;
    bool        textureFromTarget = false;
    Actor*      textureTarget = nullptr;       // SetTexture(aft:GetTexture())
    int         spriteState = 0;
    bool        spriteAnimate = true;
    float       baseZoomX = 1.0f, baseZoomY = 1.0f;
    bool        textureFilterDirty = false;
    bool        customTexRect = false;
    float       texRect[4] = {0, 0, 1, 1};
    float       texCoordVelX = 0, texCoordVelY = 0;
    float       texCoordBaseX = 0, texCoordBaseY = 0;
    double      texCoordVelocityStart = 0;
    float       fadeLeft = 0, fadeRight = 0, fadeTop = 0, fadeBottom = 0;
    float       fov = -1, vanishX = 0, vanishY = 0;
    bool        vanishSet = false;
    float       farDist = 1000.0f;
    bool        drawByZPosition = false;

    // NotITG actor-local GLSL. The program is compiled lazily after a GL
    // context exists; Lua uniforms are retained because OnCommand normally
    // sets them long before the actor first draws.
    struct ShaderUniform {
        std::string name;
        int components = 0;                    // 1/2/4 float, 0 = texture
        float value[4] = {};
        Actor* texture = nullptr;
    };
    std::string shaderVert;
    std::string shaderFrag;
    GLuint      shaderProgram = 0;
    bool        shaderTried = false;
    std::vector<ShaderUniform> shaderUniforms;

    using PolygonVertex = ActorPolygonVertex;
    std::vector<PolygonVertex> polygonVertices;
    bool        polygonTriangles = true;
    int         cullMode = 0;                  // 0 none, 1 back, 2 front

    // ActorProxy and ActorFrame wrapper state are real typed objects in SM5.
    // They are kept outside children: a proxy target is not owned by the
    // proxy, and a wrapper transforms its owner rather than drawing beside it.
    Actor*      proxyTarget = nullptr;
    std::unique_ptr<Actor> wrapper;
    int         playerField = 0;             // renderer-owned NoteField source

    ActorState  base;                       // after InitCommand
    ActorState  onBase;                     // state OnCommand starts from
    Effect      effect;
    std::vector<Seg> segs;                  // resolved timeline
    std::vector<std::unique_ptr<Actor>> children;

    // Raw command text by name: "On", "Init", "Update", "WagMessage", ...
    std::vector<ActorCommand> commands;

    const ActorCommand* findCommandEntry(const std::string& name) const;
    ActorCommand* findCommandEntry(const std::string& name);
    const std::string* findCommand(const std::string& name) const;
};

// A loaded folder: <dir>/default.lua (SM5) or the legacy default.xml fallback.
class ActorTree {
public:
    ActorTree();
    ~ActorTree();

    // `dir` is the folder named by the FG/BGCHANGES entry, resolved against the
    // song folder. `startSec` is when the change activates -- all timelines are
    // absolute from there.
    bool load(const std::string& dir, double startSec, std::string& err);
    bool ok() const { return root_ != nullptr; }
    const std::string& dir() const { return dir_; }
    double startSec() const { return startSec_; }

    // Resolve every actor's state at `sec` and draw. Needs the renderer for the
    // quad emitter and the virtual-resolution projection.
    void draw(Renderer& R, double sec);

    // Broadcast a message to this tree (MESSAGEMAN:Broadcast). Schedules the
    // matching <Name>MessageCommand on every actor that declares one.
    // Lua message bodies may have non-idempotent side effects. Runtime callers
    // must invoke this once per event edge, never once per rendered frame.
    void broadcast(const std::string& msg, double sec);

    Actor* root() { return root_.get(); }
    const std::vector<std::string>& log() const;
    // --- the per-frame command pump ------------------------------------------
    // A NotITG-lineage modchart runs its whole engine from one self-requeueing
    // command:  UpdateCommand does the work, then `self:sleep(t)
    // self:queuecommand('Update')` re-arms it. Nothing else drives the chart --
    // mods, actor positions and render targets all come out of that loop -- so
    // without a pump the tree loads, Init/On fire once, and the song is static.
    //
    // SEEKING. The loop is stateful (its cursors only move forward), so it is
    // NOT a pure function of the beat like the rest of NotClon. Stepping
    // forward is exact and is what an encode does. Seeking BACKWARDS cannot be
    // stepped, so the tree is rebuilt and replayed from the start; that is
    // deterministic and matches the encode, at the cost of the replay.
    void setDisplaySize(int w, int h);   // store + forward to the Lua env
    void update(double sec, double beat);
    void setChart(const Chart* chart);
    void setSmTiming(const SmTiming* timing) {
        haveSmTiming_ = timing != nullptr;
        if (timing) smTiming_ = *timing;
    }

    // Textures published by an ActorFrameTexture's SetTextureName, so a later
    // Texture="<name>" or SetTexture() resolves to the live render target.
    // id + pixel size: the display sprite's natural size must be the
    // AFT's real allocation or the chart's virt/real basezoom math is
    // scaling the wrong number.
    struct NamedTex { GLuint id = 0; int w = 0, h = 0; };
    std::map<std::string, NamedTex>& namedTextures() { return namedTex_; }
    // See ActorLayer::drainLuaMods. One tree, one lua_State, one `mods` table.
    int drainLuaMods(ModDoc& doc, int resolution);
    // Lua-driven SM5 and legacy NotITG trees both drive the live
    // ModsLevel_Song PlayerOptions. Returns false until the tree requests the
    // player; a drained legacy table remains only a seek/layout fallback.
    bool playerMods(int pn, float beat, Mods& mods, PostFx& fx,
                    float& mx, float& my, float& mz) const;
    bool playerModSnapshot(int pn, PlayerModSnapshot& out) const;
    void collectPlayerModChanges(std::vector<PlayerModChange>& out) const;
    bool playerState(int pn, double sec, double beat, ActorState& out) const;
    bool drawPlayer(Renderer& R, int pn, double sec, double beat);
    int livePlayerCount() const;
    bool canonicalSource() const { return canonicalSource_; }
    // Renderer-owned Player actors outside the FG/BG tree. NoteField is a
    // distinct child so ActorProxy targeting Player retains Player placement,
    // while targeting GetChild("NoteField") does not invent that parent.
    Actor& plrProxy(int i) { return plrProxy_[i & 1]; }
    Actor& topScreen() { return screen_; }
    Actor* screenActor(const std::string& name);
    void setOwner(ActorLayer* owner) { owner_ = owner; }
    void collectActorGlobals(std::map<std::string, Actor*>& out);
    void installActorGlobals(const std::map<std::string, Actor*>& values);

private:
    std::unique_ptr<Actor> root_;
    Actor screen_;
    Actor plrProxy_[2];
    std::string dir_;
    double startSec_ = 0.0;
    std::unique_ptr<LuaHost> lua_;
    ActorLayer* owner_ = nullptr;
    const Chart* chart_ = nullptr;
    SmTiming smTiming_;
    bool haveSmTiming_ = false;
    std::map<std::string, NamedTex> namedTex_;
    int perPlayerDropped_ = 0;
    // Commands waiting to fire, earliest first.
    struct Pending { double t; Actor* a; std::string cmd; };
    std::vector<Pending> pending_;
    double luaClock_ = -1.0;      // how far the pump has been stepped
    bool   pumpStalled_ = false;  // too many commands at one timestamp
    bool   gameCmdReported_ = false;
    double pumpBeat_ = 0.0;
    int dispW_ = 1920, dispH_ = 1080;
    int    pumpRan_ = 0;
    bool   pumpEverRan_ = false;
    bool   canonicalSource_ = false;
    void   runPending(double sec);
    void   broadcastLocal(const std::string& msg, double sec,
                          const LuaMessageParams* params = nullptr);
    bool   ownsActor(const Actor* actor) const;
    friend class ActorLayer;
public:
    // Called by queuecommand. Kept ordered by time.
    void   enqueue(double t, Actor& a, const std::string& cmd);
private:
    void dispatchPending(double sec);
};

// --- the Lua host ----------------------------------------------------------
// One lua_State per actor tree. Globals persist across sibling actors;
// ActorLayer mirrors actor-valued globals and message broadcasts between the
// states to reproduce SM's one screen-wide Lua environment. The binding
// surface is deliberately the subset exercised by the actor files.
class LuaHost {
public:
    bool  open(std::string& err);
    // The tree that owns the named-texture registry an AFT publishes into.
    void  setTree(ActorTree* t) { tree_ = t; }
    ActorTree* treePtr() const { return tree_; }
    // Push an actor userdata; the binding surface for natives like Plr.
    void pushActor(Actor& actor);
    const Chart* chart() const { return chart_; }
    void  setChart(const Chart* chart) { chart_ = chart; }
    // `sleep` inside a command body does not tween -- it delays whatever that
    // body queues next. Accumulated per chunk call and read by queuecommand.
    double chunkStart   = 0.0;
    ~LuaHost() { close(); }
    void  close();
    lua_State* L() const { return L_; }
    // Compile `%function(self) ... end` body; returns a registry ref or -1.
    int   compileChunk(const std::string& src, const std::string& where,
                       std::string& err);
    // Execute one compiled body with `self` bound to `actor`. Setter calls
    // capture into the actor's deterministic segment timeline at `sec`.
    bool  callChunk(int ref, Actor& actor, double sec, const std::string& where,
                    std::string& err,
                    const LuaMessageParams* params = nullptr);
    void  setBeat(double beat, double sec);
    double beat() const { return beat_; }
    double sec()  const { return sec_; }
    void  setSongDir(const std::string& d) { songDir_ = d; }
    const std::string& songDir() const { return songDir_; }
    // MESSAGEMAN:Broadcast queues here; the owning ActorTree drains it after
    // the current Lua body returns, so dispatch never re-enters the VM.
    void  queueBroadcast(const std::string& m) { pending_.push_back(m); }
    std::vector<std::string>& pendingBroadcasts() { return pending_; }
    const std::vector<std::string>& log() const { return log_; }
    void  note(const std::string& s);
    // The REAL output size in pixels, for DISPLAY:GetDisplayWidth/Height.
    // Charts size their render targets with it and then scale the display
    // sprite by SCREEN_WIDTH/DisplayWidth -- handing back the virtual size
    // made that ratio 1 and the AFT a corner crop of the framebuffer.
    void  setDisplaySize(int w, int h);
    int   displayW() const { return dispW_; }
    int   displayH() const { return dispH_; }
    // Stock SM5 ModsLevel_Song state. The current value approaches the target
    // using the target's speed, exactly like PlayerOptions::Approach().
    void  pushPlayerOptions(int pn);
    void  applyGameCommand(const std::string& command, int player);
    void  launchAttack(double startSec, double lengthSec,
                       const std::string& mods, int player);
    float noteZ(int pn, int col, float yOffset) const;
    void  setLegacyXml(bool legacy);
    bool  playerMods(int pn, float beat, Mods& mods, PostFx& fx,
                     float& mx, float& my, float& mz) const;
    bool  playerModSnapshot(int pn, PlayerModSnapshot& out) const;
    const std::vector<PlayerModChange>& playerModChanges() const {
        return poChanges_;
    }
    int   livePlayerCount() const { return requestedPlayers_; }
    void  collectActorGlobals(std::map<std::string, Actor*>& out);
    void  setActorGlobal(const std::string& name, Actor& actor);

private:
    struct CallState;
    CallState* call_ = nullptr;

    // Lua stack index -> the Actor it wraps, or null if it is not one.
    Actor* toActor(lua_State* L, int idx);
    // Allocate an AFT's texture+FBO and publish it under its name. Idempotent:
    // a chart calls Create() once, but a re-entered InitCommand must not leak.
    void aftCreate(Actor& a);
    bool invokeChunk(int ref, Actor& actor, const std::string& where,
                     std::string& err,
                     const LuaMessageParams* params = nullptr);
    static int actorIndex(lua_State* L);
    static int actorCall(lua_State* L);
    static int playerOptionsIndex(lua_State* L);
    static int playerOptionsCall(lua_State* L);
    void  applyModString(int pn, const std::string& mods);
    void  advancePlayerOptions(double sec);
    void  rebuildPlayerOptionsFromAttacks(int pn, double sec);
    void  recordPlayerModChange(int pn, int id);

    struct PlayerAttack {
        double startSec = 0.0;
        double endSec = 0.0;
        std::string mods;
        int player = 0;             // 0 = both, otherwise legacy 1/2
    };

    lua_State* L_ = nullptr;
    ActorTree* tree_ = nullptr;   // owns the named-texture registry
    const Chart* chart_ = nullptr;
    double beat_ = 0, sec_ = 0;
    std::string songDir_;
    std::vector<std::string> pending_;
    std::vector<std::string> log_;
    int dispW_ = 1920, dispH_ = 1080;
    float poCurrent_[2][MOD_COUNT] = {};
    float poTarget_[2][MOD_COUNT] = {};
    float poSpeed_[2][MOD_COUNT] = {};
    double poClock_ = -1.0;
    int requestedPlayers_ = 0;
    bool legacyColumnNames_ = false;
    std::vector<PlayerModChange> poChanges_;
    std::vector<PlayerAttack> attacks_;
};

// --- the scheduler ---------------------------------------------------------
// FG/BGCHANGES resolved to seconds, each pointing at an ActorTree. This is the
// object drawFrame asks "what is on screen at this second".
class ActorLayer {
public:
    struct Entry { double startSec; std::string dir; bool foreground; };

    // Reads the .sm beside the chart (if any) for #FGCHANGES/#BGCHANGES, or
    // takes explicit folders. Trees are loaded once, up front.
    // Must be called BEFORE loading: an AFT sizes itself in InitCommand,
    // which runs at load, long before the first pump.
    void setDisplaySize(int w, int h) { dispW_ = w; dispH_ = h; }
    bool loadFromSm(const std::string& smPath, const std::string& songDir,
                    std::string& err);
    void addFolder(const std::string& songDir, const std::string& sub,
                   double startSec, bool foreground);
    void setChart(const Chart* chart);

    bool empty() const { return trees_.empty(); }

    // Drain the Lua globals `mods` / `mods2` into `doc`.
    //
    // A NotITG-lineage modchart keeps its whole mod list in a Lua table and
    // walks it every frame, calling PlayerOptions::FromString on whatever is
    // live at the current beat. That is the same scoped-attack model ModDoc
    // already implements with `len`, and the same `*<approach> <percent>
    // <name>` grammar #MODS uses -- so the table converts rather than needing
    // a second evaluator.
    //
    // Each row is {beat, len_or_end, modstring, 'len'|'end', pn?}. Returns how
    // many entries were added. Call after loading and before ModDoc::rebuild.
    int drainLuaMods(ModDoc& doc, int resolution);

    // Step every tree's command loop to `sec`. See ActorTree::update.
    // Draw the live synthetic Player through its locally-centred field source.
    // True means a live Player existed, including when its own state hid it.
    bool drawPlayer(Renderer& R, int pn, double sec, double beat);
    bool playerMods(int pn, double sec, float beat, Mods& mods, PostFx& fx,
                    float& mx, float& my, float& mz) const;
    bool playerModSnapshot(int pn, double sec, PlayerModSnapshot& out) const;
    void collectPlayerModChanges(std::vector<PlayerModChange>& out) const;
    int livePlayerCount() const;
    void broadcast(const std::string& msg, double sec);
    void setAutoplay(bool enabled) { autoplay_ = enabled; }
    void pump(Renderer& R, double sec);
    void pump(Renderer& R, double sec, double beat);
    void drawBackground(Renderer& R, double sec);
    void drawForeground(Renderer& R, double sec);
    const std::vector<std::string>& log() const { return log_; }

private:
    struct Slot { Entry e; std::unique_ptr<ActorTree> tree; };
    std::vector<Slot> trees_;
    int dispW_ = 1920, dispH_ = 1080;
    const Chart* chart_ = nullptr;
    SmTiming smTiming_;
    bool haveSmTiming_ = false;
    bool autoplay_ = true;
    size_t judgmentCursor_ = 0;
    double judgmentClock_ = -1.0;
    std::vector<std::string> log_;
    void syncActorGlobals();
    void broadcastJudgment(double sec, int player);
    friend class ActorTree;
};

}  // namespace nc
