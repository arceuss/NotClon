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

#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace nc {
class LuaHost;

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
    enum Kind { None, Bob, Bounce, Spin, Wag, Pulse, Vibrate } kind = None;
    float magX = 0, magY = 0, magZ = 0;
    float period = 1.0f;
    float delay = 0.0f;
    bool  beatClock = false;                // effectclock,'bgm'
};
struct ActorCommand {
    std::string name;
    std::string text;
    int luaRef = -1;                         // registry ref for %function bodies
};


struct Actor {
    std::string type;                       // ActorFrame | Sprite | Quad
    std::string name;
    std::string file;                       // texture path, resolved
    Tex         tex;
    bool        texLoaded = false;
    bool        textureFiltering = true;
    bool        textureFilterDirty = false;

    ActorState  base;                       // after InitCommand
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
    void broadcast(const std::string& msg, double sec);

    Actor* root() { return root_.get(); }
    const std::vector<std::string>& log() const;

private:
    std::unique_ptr<Actor> root_;
    std::string dir_;
    double startSec_ = 0.0;
    std::unique_ptr<LuaHost> lua_;
    void dispatchPending(double sec);
};

// --- the Lua host ----------------------------------------------------------
// One lua_State per actor tree. Globals therefore persist across sibling
// actors, while separate scheduled trees cannot cross-talk. The binding
// surface is deliberately the subset exercised by the actor files.
class LuaHost {
public:
    bool  open(std::string& err);
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

private:
    struct CallState;
    CallState* call_ = nullptr;
    void pushActor(Actor& actor);
    bool invokeChunk(int ref, Actor& actor, const std::string& where,
                     std::string& err);
    static int actorIndex(lua_State* L);
    static int actorCall(lua_State* L);

    lua_State* L_ = nullptr;
    double beat_ = 0, sec_ = 0;
    std::string songDir_;
    std::vector<std::string> pending_;
    std::vector<std::string> log_;
};

// --- the scheduler ---------------------------------------------------------
// FG/BGCHANGES resolved to seconds, each pointing at an ActorTree. This is the
// object drawFrame asks "what is on screen at this second".
class ActorLayer {
public:
    struct Entry { double startSec; std::string dir; bool foreground; };

    // Reads the .sm beside the chart (if any) for #FGCHANGES/#BGCHANGES, or
    // takes explicit folders. Trees are loaded once, up front.
    bool loadFromSm(const std::string& smPath, const std::string& songDir,
                    std::string& err);
    void addFolder(const std::string& songDir, const std::string& sub,
                   double startSec, bool foreground);

    bool empty() const { return trees_.empty(); }
    void drawBackground(Renderer& R, double sec);
    void drawForeground(Renderer& R, double sec);
    const std::vector<std::string>& log() const { return log_; }

private:
    struct Slot { Entry e; std::unique_ptr<ActorTree> tree; };
    std::vector<Slot> trees_;
    std::vector<std::string> log_;
};

}  // namespace nc
