// The actor layer: StepMania/OpenITG XML actor trees, their command/tween
// engine, and the Lua that drives them.
//
// This is what `#FGCHANGES:0.000=lua=1.000=0=0=1=====;` loads -- the folder
// named in the change holds `default.xml` plus its own images. NotClon keeps
// the same convention, so a song folder is:
//
//     charts/<Song>/
//         notes.chart          the chart
//         <name>.ncmod         the modchart
//         <audio>.ogg          named by [Song] MusicStream
//         lua/default.xml      an actor tree, plus any images it references
//         effects/default.xml  another, with effects/*.png beside it
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
// runs. Per-frame UpdateCommand scripts remain unsupported rather than making
// seeking depend on which frames happened to render first.
#pragma once

#include "chart.h"
#include "renderer.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace nc {
class LuaHost;
class ActorTree;

// --- easings ---------------------------------------------------------------
// The seven ITG tweens. `sleep` is one of them: it holds the old value for the
// duration and then snaps, which is why it doubles as "reset the tween state".
enum class Ease { Instant, Linear, Accelerate, Decelerate, Spring, BounceBegin, BounceEnd, Sleep };
float easeApply(Ease e, float t);           // t in [0,1] -> eased fraction

// --- the animatable state of one actor -------------------------------------
struct ActorState {
    float x = 0, y = 0, z = 0;
    float zoomX = 1, zoomY = 1, zoomZ = 1;
    float rotX = 0, rotY = 0, rotZ = 0;     // degrees
    float r = 1, g = 1, b = 1, a = 1;
    float glowA = 0;
    bool  hidden = false;
    // Sizing overrides for a textured actor, in virtual 640x480 SM pixels.
    float sizeX = -1, sizeY = -1;           // <0 = use the texture's own size
    int   horizAlign = 0;                   // -1 left, 0 centre, +1 right
    int   vertAlign = 0;
    int   blend = 0;                        // 0 normal, 1 add, 2 noeffect
    bool  zWrite = false, zTest = false, clearZ = false;
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
    PROP_X, PROP_Y, PROP_Z, PROP_ZOOMX, PROP_ZOOMY, PROP_ZOOMZ,
    PROP_ROTX, PROP_ROTY, PROP_ROTZ, PROP_DIFFUSE, PROP_DIFFUSEALPHA,
    PROP_GLOWALPHA, PROP_HIDDEN, PROP_SIZE, PROP_HALIGN, PROP_VALIGN,
    PROP_BLEND, PROP_ZWRITE, PROP_ZTEST, PROP_CLEARZ, PROP_COUNT
};

// An Actor effect (bob/bounce/spin/wag/pulse/vibrate). Pure function of the
// effect clock, so it survives seeking for free.
struct Effect {
    enum Kind { None, Bob, Bounce, Spin, Wag, Pulse, Vibrate,
                DiffuseShift, DiffuseBlink } kind = None;
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
    std::string file;                       // texture path, resolved
    Tex         tex;
    bool        texLoaded = false;
    // A BitmapText: File= names a FONT and Text= is its string. NotClon has no
    // font rendering, so such an actor draws nothing -- an untextured quad
    // would be a white box sitting where the text belongs, which is worse than
    // the text being absent.
    bool        isText = false;

    // --- ActorFrameTexture ---------------------------------------------------
    // A render target that captures whatever has already been drawn. SM's AFT
    // does this by rendering its preceding siblings into its own texture;
    // NotClon copies the live framebuffer at the point the AFT is reached,
    // which is the same content for a tree that draws in order.
    //
    // Create() allocates; SetTextureName() publishes it under a name other
    // actors can reference by Texture= or SetTexture(). The Enable* flags are
    // accepted and mostly recorded rather than honoured -- see aftCreate.
    bool        isAft = false;
    std::string aftName;
    int         aftW = 0, aftH = 0;
    bool        aftAlpha = true, aftDepth = false, aftFloat = false;
    bool        aftPreserve = true;
    GLuint      aftTex = 0, aftFbo = 0;

    // --- .sprite animation ---------------------------------------------------
    // SM's animated-sprite descriptor: an ini naming a sheet plus a Frame/Delay
    // list. The sheet's grid comes from its FILENAME ("walk 2x1.png"),
    // the same convention the pump noteskin uses.
    //
    // The frame index is a PURE FUNCTION OF TIME -- total the delays and take
    // the remainder. Not a counter: drawFrame() is called at an arbitrary beat
    // with no history, so a counter would make a seek show a different frame
    // than the encode did.
    std::vector<int>   spriteFrames;   // frame index into the sheet, in order
    std::vector<float> spriteDelays;   // seconds each entry is held
    float              spriteTotal = 0.0f;
    int                sheetCols = 1, sheetRows = 1;
    bool        textureFiltering = true;
    int         spriteState = 0;
    bool        spriteAnimate = true;
    float       baseZoomX = 1.0f, baseZoomY = 1.0f;
    bool        textureFilterDirty = false;

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

// A loaded folder: <dir>/default.xml plus whatever it references.
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
    void update(double sec, double beat, int maxSteps = 20000);
    void setChart(const Chart* chart);

    // Textures published by an ActorFrameTexture's SetTextureName, so a later
    // Texture="<name>" or SetTexture() resolves to the live render target.
    // id + pixel size: the display sprite's natural size must be the
    // AFT's real allocation or the chart's virt/real basezoom math is
    // scaling the wrong number.
    struct NamedTex { GLuint id = 0; int w = 0, h = 0; };
    std::map<std::string, NamedTex>& namedTextures() { return namedTex_; }
    // See ActorLayer::drainLuaMods. One tree, one lua_State, one `mods` table.
    int drainLuaMods(ModDoc& doc, ModDoc* doc2, int resolution);

private:
    std::unique_ptr<Actor> root_;
    std::string dir_;
    double startSec_ = 0.0;
    std::unique_ptr<LuaHost> lua_;
    const Chart* chart_ = nullptr;
    std::map<std::string, NamedTex> namedTex_;
    int perPlayerDropped_ = 0;
    // Commands waiting to fire, earliest first.
    struct Pending { double t; Actor* a; std::string cmd; };
    std::vector<Pending> pending_;
    double luaClock_ = -1.0;      // how far the pump has been stepped
    bool   pumpOverrun_ = false;  // hit maxSteps; reported once
    bool   gameCmdReported_ = false;
    double pumpBeat_ = 0.0;
    int dispW_ = 1920, dispH_ = 1080;
    int    pumpRan_ = 0;
    bool   pumpEverRan_ = false;
    void   runPending(double sec, int maxSteps);
public:
    // Called by queuecommand. Kept ordered by time.
    void   enqueue(double t, Actor& a, const std::string& cmd);
private:
    void dispatchPending(double sec);
};

// --- the Lua host ----------------------------------------------------------
// One lua_State per actor tree. Globals therefore persist across sibling
// actors, while separate scheduled trees cannot cross-talk. The binding
// surface is deliberately the subset exercised by the actor files.
class LuaHost {
public:
    bool  open(std::string& err);
    // The tree that owns the named-texture registry an AFT publishes into.
    void  setTree(ActorTree* t) { tree_ = t; }
    const Chart* chart() const { return chart_; }
    void  setChart(const Chart* chart) { chart_ = chart; }
    // `sleep` inside a command body does not tween -- it delays whatever that
    // body queues next. Accumulated per chunk call and read by queuecommand.
    double pendingSleep = 0.0;
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
                    std::string& err);
    void  setBeat(double beat, double sec) { beat_ = beat; sec_ = sec; }
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
    void  setDisplaySize(int w, int h) { dispW_ = w; dispH_ = h; }
    int   displayW() const { return dispW_; }
    int   displayH() const { return dispH_; }
    // Per-frame ApplyGameCommand calls: accepted but not applied, counted
    // so the gap is reported rather than silent.
    void  noteGameCommand() { ++gameCmds_; }
    int   gameCommands() const { return gameCmds_; }

private:
    struct CallState;
    CallState* call_ = nullptr;
    void pushActor(Actor& actor);
    // Lua stack index -> the Actor it wraps, or null if it is not one.
    Actor* toActor(lua_State* L, int idx);
    // Allocate an AFT's texture+FBO and publish it under its name. Idempotent:
    // a chart calls Create() once, but a re-entered InitCommand must not leak.
    void aftCreate(Actor& a);
    bool invokeChunk(int ref, Actor& actor, const std::string& where,
                     std::string& err);
    static int actorIndex(lua_State* L);
    static int actorCall(lua_State* L);

    lua_State* L_ = nullptr;
    ActorTree* tree_ = nullptr;   // owns the named-texture registry
    const Chart* chart_ = nullptr;
    double beat_ = 0, sec_ = 0;
    std::string songDir_;
    std::vector<std::string> pending_;
    std::vector<std::string> log_;
    int gameCmds_ = 0;
    int dispW_ = 1920, dispH_ = 1080;
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
    int drainLuaMods(ModDoc& doc, ModDoc* doc2, int resolution);

    // Step every tree's command loop to `sec`. See ActorTree::update.
    void pump(Renderer& R, double sec);
    void drawBackground(Renderer& R, double sec);
    void drawForeground(Renderer& R, double sec);
    const std::vector<std::string>& log() const { return log_; }

private:
    struct Slot { Entry e; std::unique_ptr<ActorTree> tree; };
    std::vector<Slot> trees_;
    int dispW_ = 1920, dispH_ = 1080;
    const Chart* chart_ = nullptr;
    std::vector<std::string> log_;
};

}  // namespace nc
