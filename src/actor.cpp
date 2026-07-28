#include "actor.h"
#include "smbg.h"
#include "smimport.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

namespace nc {

// ---------------------------------------------------------------------------
// Easings. ITG's seven tweens; `sleep` holds and then snaps, which is why it
// also serves as the "reset the pending tween" idiom in a command chain.
// ---------------------------------------------------------------------------
float easeApply(Ease e, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (e) {
        case Ease::Linear:      return t;
        case Ease::Accelerate:  return t * t;
        case Ease::Decelerate:  return 1.0f - (1.0f - t) * (1.0f - t);
        // Overshoots past the target and settles back, as the docs describe.
        case Ease::Spring:      return 1.0f - expf(-6.0f * t) * cosf(12.0f * t);
        case Ease::BounceBegin: { const float u = 1.0f - t;
                                  return 1.0f - fabsf(sinf(u * 3.14159265f * 2.5f)) * u; }
        case Ease::BounceEnd:   return fabsf(sinf(t * 3.14159265f * 2.5f)) * t +
                                       (1.0f - t) * 0.0f + (t == 1.0f ? 1.0f : 0.0f);
        case Ease::Sleep:       return 0.0f;     // hold, then the next seg snaps
        default:                return 1.0f;     // Instant
    }
}

namespace {

std::string trimws(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

// --- a very small XML reader ------------------------------------------------
// Enough for SM actor files: elements, attributes in single or double quotes,
// <children> nesting, comments, and self-closing tags. Attribute VALUES carry
// Lua source with '<' and '>' in it, so the scanner is quote-aware rather than
// angle-bracket-driven.
struct XmlNode {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> kids;
    const std::string* attr(const char* n) const {
        for (const auto& a : attrs) if (a.first == n) return &a.second;
        return nullptr;
    }
};

void xmlUnescape(std::string& s) {
    struct { const char* e; char c; } M[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}
    };
    for (const auto& m : M) {
        const size_t n = strlen(m.e);
        for (size_t p = 0; (p = s.find(m.e, p)) != std::string::npos;)
            s.replace(p, n, 1, m.c), p += 1;
    }
}

bool xmlParse(const std::string& t, size_t& i, XmlNode& out);

void xmlSkip(const std::string& t, size_t& i) {
    for (;;) {
        while (i < t.size() && isspace((unsigned char)t[i])) ++i;
        if (t.compare(i, 4, "<!--") == 0) {
            const size_t e = t.find("-->", i);
            i = (e == std::string::npos) ? t.size() : e + 3;
            continue;
        }
        if (t.compare(i, 2, "<?") == 0) {
            const size_t e = t.find("?>", i);
            i = (e == std::string::npos) ? t.size() : e + 2;
            continue;
        }
        break;
    }
}

bool xmlParse(const std::string& t, size_t& i, XmlNode& out) {
    xmlSkip(t, i);
    if (i >= t.size() || t[i] != '<') return false;
    ++i;
    while (i < t.size() && !isspace((unsigned char)t[i]) && t[i] != '>' && t[i] != '/')
        out.tag += t[i++];
    for (;;) {
        while (i < t.size() && isspace((unsigned char)t[i])) ++i;
        if (i >= t.size()) return false;
        if (t[i] == '/') { i += 2; return true; }          // self-closing
        if (t[i] == '>') { ++i; break; }
        std::string k;
        while (i < t.size() && t[i] != '=' && !isspace((unsigned char)t[i]) &&
               t[i] != '>' && t[i] != '/') k += t[i++];
        while (i < t.size() && isspace((unsigned char)t[i])) ++i;
        if (i < t.size() && t[i] == '=') {
            ++i;
            while (i < t.size() && isspace((unsigned char)t[i])) ++i;
            const char q = (i < t.size() && (t[i] == '"' || t[i] == '\'')) ? t[i++] : 0;
            std::string v;
            if (q) { while (i < t.size() && t[i] != q) v += t[i++]; if (i < t.size()) ++i; }
            else   { while (i < t.size() && !isspace((unsigned char)t[i]) &&
                            t[i] != '>' && t[i] != '/') v += t[i++]; }
            xmlUnescape(v);
            if (!k.empty()) out.attrs.push_back({k, v});
        } else if (!k.empty()) {
            out.attrs.push_back({k, ""});
        }
    }
    // body
    for (;;) {
        xmlSkip(t, i);
        if (i >= t.size()) return true;
        if (t.compare(i, 2, "</") == 0) {
            const size_t e = t.find('>', i);
            i = (e == std::string::npos) ? t.size() : e + 1;
            return true;
        }
        if (t[i] == '<') {
            XmlNode kid;
            const size_t save = i;
            if (!xmlParse(t, i, kid)) { i = save; return true; }
            if (kid.tag == "children") {
                for (auto& g : kid.kids) out.kids.push_back(std::move(g));
            } else {
                out.kids.push_back(std::move(kid));
            }
            continue;
        }
        ++i;                                               // stray text
    }
}

// --- command-chain tokenising -----------------------------------------------
// A chain is `verb,a,b;verb,a;...`. Splitting must not cut inside a
// `%function(self) ... end` chunk, which is free-form Lua containing both ';'
// and ','.
struct Cmd { std::string verb; std::vector<std::string> args; std::string lua; };

bool isLuaCommand(const std::string& src) {
    const std::string s = trimws(src);
    if (s.empty() || s[0] != '%') return false;
    size_t p = 1;
    if (s.compare(p, 8, "function") != 0) return false;
    p += 8;
    while (p < s.size() && isspace((unsigned char)s[p])) ++p;
    return p < s.size() && s[p] == '(';
}

std::vector<Cmd> parseChain(const std::string& src) {
    std::vector<Cmd> out;
    const std::string s = trimws(src);
    if (s.empty()) return out;
    // Whole-attribute Lua chunk.
    if (isLuaCommand(s)) {
        Cmd c; c.verb = "%lua"; c.lua = s;
        out.push_back(c);
        return out;
    }
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ';' || isspace((unsigned char)s[i]))) ++i;
        if (i >= s.size()) break;
        std::string piece;
        while (i < s.size() && s[i] != ';') piece += s[i++];
        piece = trimws(piece);
        if (piece.empty()) continue;
        Cmd c;
        size_t p = 0;
        while (p < piece.size() && piece[p] != ',') c.verb += piece[p++];
        c.verb = lower(trimws(c.verb));
        while (p < piece.size()) {
            ++p;
            std::string a;
            while (p < piece.size() && piece[p] != ',') a += piece[p++];
            c.args.push_back(trimws(a));
        }
        out.push_back(c);
    }
    return out;
}

// SM's virtual resolution. Actor coordinates are in these units; the layer
// projects them onto whatever the render target actually is.
const float SCREEN_W = 640.0f, SCREEN_H = 480.0f;

float argf(const Cmd& c, size_t i, float d = 0.0f) {
    if (i >= c.args.size()) return d;
    const std::string& a = c.args[i];
    if (a == "SCREEN_WIDTH")    return SCREEN_W;
    if (a == "SCREEN_HEIGHT")   return SCREEN_H;
    if (a == "SCREEN_CENTER_X") return SCREEN_W * 0.5f;
    if (a == "SCREEN_CENTER_Y") return SCREEN_H * 0.5f;
    // SCREEN_CENTER_X-233 and friends: one trailing +/- term.
    for (const char* base : {"SCREEN_CENTER_X", "SCREEN_CENTER_Y",
                             "SCREEN_WIDTH", "SCREEN_HEIGHT"}) {
        const size_t n = strlen(base);
        if (a.compare(0, n, base) == 0 && a.size() > n &&
            (a[n] == '+' || a[n] == '-')) {
            float v = (base[7] == 'C' || base[8] == 'E')
                    ? ((base[14] == 'X') ? SCREEN_W * 0.5f : SCREEN_H * 0.5f)
                    : ((strcmp(base, "SCREEN_WIDTH") == 0) ? SCREEN_W : SCREEN_H);
            if (strcmp(base, "SCREEN_CENTER_X") == 0) v = SCREEN_W * 0.5f;
            if (strcmp(base, "SCREEN_CENTER_Y") == 0) v = SCREEN_H * 0.5f;
            if (strcmp(base, "SCREEN_WIDTH")    == 0) v = SCREEN_W;
            if (strcmp(base, "SCREEN_HEIGHT")   == 0) v = SCREEN_H;
            return v + float(atof(a.c_str() + n));
        }
    }
    return float(atof(a.c_str()));
}

}  // namespace

Actor::~Actor() {
    if (aftFbo) glDeleteFramebuffers(1, &aftFbo);
    if (aftTex) glDeleteTextures(1, &aftTex);
}

const ActorCommand* Actor::findCommandEntry(const std::string& name) const {
    for (auto it = commands.rbegin(); it != commands.rend(); ++it)
        if (it->name == name) return &*it;
    return nullptr;
}

ActorCommand* Actor::findCommandEntry(const std::string& name) {
    for (auto it = commands.rbegin(); it != commands.rend(); ++it)
        if (it->name == name) return &*it;
    return nullptr;
}

const std::string* Actor::findCommand(const std::string& name) const {
    const ActorCommand* c = findCommandEntry(name);
    return c ? &c->text : nullptr;
}

// ---------------------------------------------------------------------------
// Scheduling a chain onto an actor's timeline.
//
// The tween state machine, per the docs: a tween verb (linear/accelerate/...)
// arms a duration that applies to every property command that FOLLOWS it,
// until a `sleep` resets it. So the chain is walked with a cursor `t` in
// absolute seconds and an armed (dur, ease); each property command emits one
// Seg from the current value to the new one and, if armed, advances t.
// ---------------------------------------------------------------------------
namespace {

struct Sched {
    Actor* a;
    double t;            // absolute seconds cursor
    float  dur = 0;      // armed tween duration
    Ease   ease = Ease::Instant;
    ActorState cur;      // running value, so `from` is right
};

void emit(Sched& S, int prop, int n, const float* to) {
    Seg s{};
    s.prop = prop; s.n = n; s.ease = (S.dur > 0.0f) ? S.ease : Ease::Instant;
    s.t0 = float(S.t); s.t1 = float(S.t + S.dur);
    float* from = nullptr;
    switch (prop) {
        case PROP_X: from = &S.cur.x; break;
        case PROP_Y: from = &S.cur.y; break;
        case PROP_Z: from = &S.cur.z; break;
        case PROP_ZOOMX: from = &S.cur.zoomX; break;
        case PROP_ZOOMY: from = &S.cur.zoomY; break;
        case PROP_ZOOMZ: from = &S.cur.zoomZ; break;
        case PROP_ROTX: from = &S.cur.rotX; break;
        case PROP_ROTY: from = &S.cur.rotY; break;
        case PROP_ROTZ: from = &S.cur.rotZ; break;
        case PROP_DIFFUSEALPHA: from = &S.cur.a; break;
        case PROP_GLOWALPHA: from = &S.cur.glowA; break;
        default: break;
    }
    if (prop == PROP_DIFFUSE) {
        s.from[0] = S.cur.r; s.from[1] = S.cur.g; s.from[2] = S.cur.b; s.from[3] = S.cur.a;
        for (int i = 0; i < 4; ++i) s.to[i] = to[i];
        S.cur.r = to[0]; S.cur.g = to[1]; S.cur.b = to[2]; S.cur.a = to[3];
    } else if (prop == PROP_SIZE) {
        s.from[0] = S.cur.sizeX; s.from[1] = S.cur.sizeY;
        s.to[0] = to[0]; s.to[1] = to[1];
        S.cur.sizeX = to[0]; S.cur.sizeY = to[1];
    } else if (from) {
        s.from[0] = *from; s.to[0] = to[0];
        *from = to[0];
    } else {                                     // flags: instantaneous
        s.from[0] = to[0]; s.to[0] = to[0];
        switch (prop) {
            case PROP_HIDDEN: S.cur.hidden = to[0] != 0; break;
            case PROP_HALIGN: S.cur.horizAlign = int(to[0]); break;
            case PROP_VALIGN: S.cur.vertAlign  = int(to[0]); break;
            case PROP_BLEND:  S.cur.blend      = int(to[0]); break;
            case PROP_ZWRITE: S.cur.zWrite     = to[0] != 0; break;
            case PROP_ZTEST:  S.cur.zTest      = to[0] != 0; break;
            case PROP_CLEARZ: S.cur.clearZ     = to[0] != 0; break;
            default: break;
        }
    }
    S.a->segs.push_back(s);
    if (S.dur > 0.0f) { S.t += S.dur; S.dur = 0.0f; S.ease = Ease::Instant; }
}

void emit1(Sched& S, int prop, float v) { emit(S, prop, 1, &v); }
ActorState evalActor(const Actor& a, double sec);
void settleTweens(Sched& S, bool finish) {
    bool touched[PROP_COUNT] = {};
    double end = S.t;
    for (const Seg& s : S.a->segs) {
        if (s.t1 > S.t) {
            touched[s.prop] = true;
            end = std::max(end, double(s.t1));
        }
    }
    if (end == S.t) return;
    const ActorState target = evalActor(*S.a, finish ? end : S.t);
    S.a->segs.erase(std::remove_if(S.a->segs.begin(), S.a->segs.end(),
        [&](const Seg& s) { return s.t1 > float(S.t); }), S.a->segs.end());
    S.cur = target;
    S.dur = 0;
    S.ease = Ease::Instant;
    for (int prop = 0; prop < PROP_COUNT; ++prop) {
        if (!touched[prop]) continue;
        switch (prop) {
            case PROP_X: emit1(S, prop, target.x); break;
            case PROP_Y: emit1(S, prop, target.y); break;
            case PROP_Z: emit1(S, prop, target.z); break;
            case PROP_ZOOMX: emit1(S, prop, target.zoomX); break;
            case PROP_ZOOMY: emit1(S, prop, target.zoomY); break;
            case PROP_ZOOMZ: emit1(S, prop, target.zoomZ); break;
            case PROP_ROTX: emit1(S, prop, target.rotX); break;
            case PROP_ROTY: emit1(S, prop, target.rotY); break;
            case PROP_ROTZ: emit1(S, prop, target.rotZ); break;
            case PROP_DIFFUSE: {
                const float v[4] = {target.r, target.g, target.b, target.a};
                emit(S, prop, 4, v);
            } break;
            case PROP_DIFFUSEALPHA: emit1(S, prop, target.a); break;
            case PROP_GLOWALPHA: emit1(S, prop, target.glowA); break;
            case PROP_HIDDEN: emit1(S, prop, target.hidden ? 1.0f : 0.0f); break;
            case PROP_SIZE: {
                const float v[2] = {target.sizeX, target.sizeY};
                emit(S, prop, 2, v);
            } break;
            case PROP_HALIGN: emit1(S, prop, float(target.horizAlign)); break;
            case PROP_VALIGN: emit1(S, prop, float(target.vertAlign)); break;
            case PROP_BLEND: emit1(S, prop, float(target.blend)); break;
            case PROP_ZWRITE: emit1(S, prop, target.zWrite ? 1.0f : 0.0f); break;
            case PROP_ZTEST: emit1(S, prop, target.zTest ? 1.0f : 0.0f); break;
            case PROP_CLEARZ: emit1(S, prop, target.clearZ ? 1.0f : 0.0f); break;
            default: break;
        }
    }
}


void scheduleChain(Actor& a, const std::string& text, double atSec,
                   ActorState startState, int depth, LuaHost* lua);

void runCmds(Sched& S, const std::vector<Cmd>& cmds, int depth, LuaHost* lua) {
    for (const Cmd& c : cmds) {
        const std::string& v = c.verb;
        // --- tween arming ---
        if      (v == "linear")      { S.dur = argf(c, 0); S.ease = Ease::Linear; }
        else if (v == "accelerate")  { S.dur = argf(c, 0); S.ease = Ease::Accelerate; }
        else if (v == "decelerate")  { S.dur = argf(c, 0); S.ease = Ease::Decelerate; }
        else if (v == "spring")      { S.dur = argf(c, 0); S.ease = Ease::Spring; }
        else if (v == "bouncebegin") { S.dur = argf(c, 0); S.ease = Ease::BounceBegin; }
        else if (v == "bounceend")   { S.dur = argf(c, 0); S.ease = Ease::BounceEnd; }
        else if (v == "sleep")       { S.t += argf(c, 0); S.dur = 0; S.ease = Ease::Instant; }
        else if (v == "finishtweening") { S.dur = 0; S.ease = Ease::Instant; }
        // --- position / size ---
        else if (v == "x")     emit1(S, PROP_X, argf(c, 0));
        else if (v == "y")     emit1(S, PROP_Y, argf(c, 0));
        else if (v == "z")     emit1(S, PROP_Z, argf(c, 0));
        else if (v == "addx")  emit1(S, PROP_X, S.cur.x + argf(c, 0));
        else if (v == "addy")  emit1(S, PROP_Y, S.cur.y + argf(c, 0));
        else if (v == "addz")  emit1(S, PROP_Z, S.cur.z + argf(c, 0));
        else if (v == "zoom")  { const float z = argf(c, 0, 1);
                                 emit1(S, PROP_ZOOMX, z); emit1(S, PROP_ZOOMY, z); }
        else if (v == "zoomx") emit1(S, PROP_ZOOMX, argf(c, 0, 1));
        else if (v == "zoomy") emit1(S, PROP_ZOOMY, argf(c, 0, 1));
        else if (v == "zoomto") { const float sz[2] = {argf(c, 0), argf(c, 1)};
                                  emit(S, PROP_SIZE, 2, sz); }
        else if (v == "zoomtowidth")  { const float sz[2] = {argf(c, 0), S.cur.sizeY};
                                        emit(S, PROP_SIZE, 2, sz); }
        else if (v == "zoomtoheight") { const float sz[2] = {S.cur.sizeX, argf(c, 0)};
                                        emit(S, PROP_SIZE, 2, sz); }
        else if (v == "rotationx") emit1(S, PROP_ROTX, argf(c, 0));
        else if (v == "rotationy") emit1(S, PROP_ROTY, argf(c, 0));
        else if (v == "rotationz") emit1(S, PROP_ROTZ, argf(c, 0));
        // --- colour ---
        else if (v == "diffuse") { const float d[4] = {argf(c,0,1), argf(c,1,1),
                                                       argf(c,2,1), argf(c,3,1)};
                                   emit(S, PROP_DIFFUSE, 4, d); }
        else if (v == "diffusealpha") emit1(S, PROP_DIFFUSEALPHA, argf(c, 0, 1));
        else if (v == "glow")         emit1(S, PROP_GLOWALPHA, argf(c, 3, 1));
        // --- flags ---
        else if (v == "hidden")    emit1(S, PROP_HIDDEN, argf(c, 0, 1));
        else if (v == "horizalign") {
            const std::string s = c.args.empty() ? "center" : lower(c.args[0]);
            emit1(S, PROP_HALIGN, s == "left" ? -1.0f : (s == "right" ? 1.0f : 0.0f));
        } else if (v == "vertalign") {
            const std::string s = c.args.empty() ? "middle" : lower(c.args[0]);
            emit1(S, PROP_VALIGN, s == "top" ? -1.0f : (s == "bottom" ? 1.0f : 0.0f));
        } else if (v == "blend") {
            const std::string s = c.args.empty() ? "normal" : lower(c.args[0]);
            emit1(S, PROP_BLEND, s == "add" ? 1.0f : (s == "noeffect" ? 2.0f : 0.0f));
        }
        else if (v == "zwrite")      emit1(S, PROP_ZWRITE, argf(c, 0, 1));
        else if (v == "ztest")       emit1(S, PROP_ZTEST,  argf(c, 0, 1));
        else if (v == "clearzbuffer")emit1(S, PROP_CLEARZ, argf(c, 0, 1));
        // --- effects ---
        else if (v == "bob")     S.a->effect.kind = Effect::Bob;
        else if (v == "bounce")  S.a->effect.kind = Effect::Bounce;
        else if (v == "spin")    S.a->effect.kind = Effect::Spin;
        else if (v == "wag")     S.a->effect.kind = Effect::Wag;
        else if (v == "pulse")   S.a->effect.kind = Effect::Pulse;
        else if (v == "vibrate") S.a->effect.kind = Effect::Vibrate;
        else if (v == "stopeffect") S.a->effect.kind = Effect::None;
        else if (v == "effectmagnitude") { S.a->effect.magX = argf(c, 0);
                                           S.a->effect.magY = argf(c, 1);
                                           S.a->effect.magZ = argf(c, 2); }
        else if (v == "effectperiod") S.a->effect.period = argf(c, 0, 1);
        else if (v == "effectdelay")  S.a->effect.delay  = argf(c, 0);
        else if (v == "effectclock")  S.a->effect.beatClock =
                                          (!c.args.empty() && (lower(c.args[0]) == "bgm" ||
                                                               lower(c.args[0]) == "beat"));
        else if (v == "diffuseshift") S.a->effect.kind = Effect::DiffuseShift;
        else if (v == "diffuseblink") S.a->effect.kind = Effect::DiffuseBlink;
        else if (v == "effectcolor1" || v == "effectcolor2") {
            float* dst = (v == "effectcolor1") ? S.a->effect.c1 : S.a->effect.c2;
            for (int i = 0; i < 4; ++i) dst[i] = argf(c, size_t(i), 1.0f);
        }
        // --- control ---
        else if (v == "queuecommand" || v == "playcommand") {
            if (!c.args.empty() && depth < 8) {
                // Static resolution: with no Lua in the chain a queued command
                // is just more chain at the cursor, so it folds in at load.
                if (const std::string* t = S.a->findCommand(c.args[0] + "Command")) {
                    Sched sub = S;
                    runCmds(sub, parseChain(*t), depth + 1, lua);
                    S.t = sub.t; S.cur = sub.cur; S.dur = sub.dur; S.ease = sub.ease;
                }
            }
        }
        else if (v == "%lua") { /* handled by the Lua host; see ActorTree::load */ }
        // anything else: absorbed
    }
}

void scheduleChain(Actor& a, const std::string& text, double atSec,
                   ActorState startState, int depth, LuaHost* lua) {
    Sched S; S.a = &a; S.t = atSec; S.cur = startState;
    runCmds(S, parseChain(text), depth, lua);
}

// Resolve an actor's state at `sec` by replaying its timeline.
ActorState evalActor(const Actor& a, double sec) {
    ActorState st = a.base;
    const float t = float(sec);
    for (const Seg& s : a.segs) {
        if (t < s.t0) break;                         // timeline is time-ordered
        float f = 1.0f;
        if (s.t1 > s.t0) {
            if (t >= s.t1) f = 1.0f;
            else f = easeApply(s.ease, (t - s.t0) / (s.t1 - s.t0));
        }
        auto L = [&](int i) { return s.from[i] + (s.to[i] - s.from[i]) * f; };
        switch (s.prop) {
            case PROP_X: st.x = L(0); break;
            case PROP_Y: st.y = L(0); break;
            case PROP_Z: st.z = L(0); break;
            case PROP_ZOOMX: st.zoomX = L(0); break;
            case PROP_ZOOMY: st.zoomY = L(0); break;
            case PROP_ZOOMZ: st.zoomZ = L(0); break;
            case PROP_ROTX: st.rotX = L(0); break;
            case PROP_ROTY: st.rotY = L(0); break;
            case PROP_ROTZ: st.rotZ = L(0); break;
            case PROP_DIFFUSEALPHA: st.a = L(0); break;
            case PROP_GLOWALPHA: st.glowA = L(0); break;
            case PROP_DIFFUSE: st.r = L(0); st.g = L(1); st.b = L(2); st.a = L(3); break;
            case PROP_SIZE: st.sizeX = L(0); st.sizeY = L(1); break;
            case PROP_HIDDEN: st.hidden = s.to[0] != 0; break;
            case PROP_HALIGN: st.horizAlign = int(s.to[0]); break;
            case PROP_VALIGN: st.vertAlign = int(s.to[0]); break;
            case PROP_BLEND:  st.blend = int(s.to[0]); break;
            case PROP_ZWRITE: st.zWrite = s.to[0] != 0; break;
            case PROP_ZTEST:  st.zTest = s.to[0] != 0; break;
            case PROP_CLEARZ: st.clearZ = s.to[0] != 0; break;
            default: break;
        }
    }
    return st;
}

// Actor effects, evaluated as pure functions of the clock.
void applyEffect(const Actor& a, ActorState& st, double sec, double beat) {
    const Effect& e = a.effect;
    if (e.kind == Effect::None) return;
    const double clk = (e.beatClock ? beat : sec) - e.delay;
    const float p = (e.period > 0.0001f) ? e.period : 1.0f;
    const float ph = float(clk) / p;
    const float s = sinf(ph * 2.0f * 3.14159265f);
    switch (e.kind) {
        case Effect::Bob:    st.x += e.magX * s; st.y += e.magY * s; st.z += e.magZ * s; break;
        // bounce is a rectified sine: it never goes past the rest position.
        case Effect::Bounce: { const float b = fabsf(s);
                               st.x += e.magX * b; st.y += e.magY * b; st.z += e.magZ * b; } break;
        case Effect::Spin:   st.rotX += e.magX * float(clk); st.rotY += e.magY * float(clk);
                             st.rotZ += e.magZ * float(clk); break;
        case Effect::Wag:    st.rotX += e.magX * s; st.rotY += e.magY * s;
                             st.rotZ += e.magZ * s; break;
        case Effect::Pulse:  { const float z = 1.0f + e.magX * s;
                               st.zoomX *= z; st.zoomY *= z; } break;
        case Effect::Vibrate: {
            // Deterministic in `sec`, or a seek re-renders differently: the
            // classic fract(sin(k*x)) hash, evaluated per frame quantum.
            const float q = floorf(float(sec) * 60.0f);
            const float h1 = sinf(q * 12.9898f) * 43758.547f;
            const float h2 = sinf(q * 78.2330f) * 43758.547f;
            st.x += e.magX * ((h1 - floorf(h1)) * 2.0f - 1.0f);
            st.y += e.magY * ((h2 - floorf(h2)) * 2.0f - 1.0f);
        } break;
        case Effect::DiffuseShift: {
            // A cosine crossfade between the two effect colors, multiplied
            // into the actor's own diffuse (SM5 Actor::UpdateInternal).
            const float t = (cosf(ph * 2.0f * 3.14159265f) + 1.0f) * 0.5f;
            const float r = e.c1[0] * t + e.c2[0] * (1.0f - t);
            const float g = e.c1[1] * t + e.c2[1] * (1.0f - t);
            const float b = e.c1[2] * t + e.c2[2] * (1.0f - t);
            const float al = e.c1[3] * t + e.c2[3] * (1.0f - t);
            st.r *= r; st.g *= g; st.b *= b; st.a *= al;
        } break;
        case Effect::DiffuseBlink: {
            // Hard toggle each half period -- this is a strobe, not a fade.
            const float fr = ph - floorf(ph);
            const float* c = fr < 0.5f ? e.c1 : e.c2;
            st.r *= c[0]; st.g *= c[1]; st.b *= c[2]; st.a *= c[3];
        } break;
        default: break;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// ActorTree
// ---------------------------------------------------------------------------
namespace {


// --- .sprite -----------------------------------------------------------------
// SM's animated-sprite descriptor. An ini:
//
//     [Sprite]
//     Texture=walk 2x1.png
//     Frame0000=0
//     Delay0000=0.5
//
// `Texture` resolves against the .sprite's own folder. The sheet grid comes
// from the texture's FILENAME -- a trailing "<cols>x<rows>" -- which is the
// same convention assets/pump uses. Without this a .sprite reaches gl_loadTex,
// which cannot decode an ini, and the actor draws as the fallback white quad.
static void parseSpriteFile(const std::string& path, Actor& a) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    std::string txt;
    { char b[8192]; size_t n;
      while ((n = fread(b, 1, sizeof b, f)) > 0) txt.append(b, n); }
    fclose(f);

    const size_t sl = path.find_last_of("/\\");
    const std::string dir = sl == std::string::npos ? std::string(".")
                                                    : path.substr(0, sl);
    std::string texName;
    std::map<int, int>   frames;      // ordinal -> frame index, kept sorted
    std::map<int, float> delays;

    size_t i = 0;
    while (i < txt.size()) {
        size_t e = txt.find('\n', i);
        if (e == std::string::npos) e = txt.size();
        const std::string line = trimws(txt.substr(i, e - i));
        i = e + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trimws(line.substr(0, eq));
        const std::string v = trimws(line.substr(eq + 1));
        if (lower(k) == "texture") texName = v;
        else if (k.size() > 5 && lower(k).compare(0, 5, "frame") == 0)
            frames[atoi(k.c_str() + 5)] = atoi(v.c_str());
        else if (k.size() > 5 && lower(k).compare(0, 5, "delay") == 0)
            delays[atoi(k.c_str() + 5)] = float(atof(v.c_str()));
    }
    if (texName.empty()) return;

    a.file = dir + "/" + texName;
    a.texLoaded = false;

    // Grid from the filename: "... 2x1.png" -> 2 cols, 1 row. Anything that
    // does not end in that pattern is a single-frame image.
    { std::string stem = texName;
      const size_t d = stem.find_last_of('.');
      if (d != std::string::npos) stem = stem.substr(0, d);
      const size_t sp = stem.find_last_of(' ');
      if (sp != std::string::npos) {
          const std::string g = stem.substr(sp + 1);
          const size_t x = g.find('x');
          if (x != std::string::npos && x > 0 && x + 1 < g.size()) {
              const int c = atoi(g.c_str());
              const int r = atoi(g.c_str() + x + 1);
              if (c > 0 && r > 0) { a.sheetCols = c; a.sheetRows = r; }
          }
      } }

    for (const auto& kv : frames) {
        auto d = delays.find(kv.first);
        // A frame with no delay would be shown for zero seconds, i.e. never;
        // treat a missing one as a sensible default rather than dropping it.
        const float sec = d == delays.end() ? 0.1f : d->second;
        if (sec <= 0.0f) continue;
        a.spriteFrames.push_back(kv.second);
        a.spriteDelays.push_back(sec);
        a.spriteTotal += sec;
    }
}

// Which cell of the sheet is showing at `sec`, as UVs. A pure function of the
// time -- see the note on Actor::spriteFrames.
static void spriteUV(const Actor& a, double sec,
                     float& u0, float& v0, float& u1, float& v1) {
    const int cols = a.sheetCols > 0 ? a.sheetCols : 1;
    const int rows = a.sheetRows > 0 ? a.sheetRows : 1;
    int idx = a.spriteState;
    if (a.spriteAnimate && !a.spriteFrames.empty() && a.spriteTotal > 0.0f) {
        double t = fmod(sec, double(a.spriteTotal));
        if (t < 0) t += a.spriteTotal;
        size_t k = 0;
        for (; k + 1 < a.spriteFrames.size(); ++k) {
            if (t < a.spriteDelays[k]) break;
            t -= a.spriteDelays[k];
        }
        idx = a.spriteFrames[k];
    }
    const int cells = cols * rows;
    idx = cells > 0 ? ((idx % cells) + cells) % cells : 0;
    const int col = idx % cols;
    const int row = idx / cols;
    const float cw = 1.0f / float(cols);
    const float ch = 1.0f / float(rows);
    u0 = col * cw; u1 = u0 + cw;
    v0 = row * ch; v1 = v0 + ch;
}

void buildActor(const XmlNode& n, Actor& a, const std::string& dir,
                double startSec, std::vector<std::string>* warn) {
    a.type = n.tag;
    // SM's loader keys off Type= and the TAG NAME IS NOISE -- charts use
    // arbitrary spellings freely, some as draw-order tricks, some as in-jokes.
    // Keying the Type= read off a whitelist of known tags left every unlisted
    // tag with its tag name as its type, and drawActor treats any
    // non-ActorFrame type as a sprite -- so a container element with an
    // unlisted tag rendered as an untextured 64x64 quad: a white square
    // sitting exactly where its children draw.
    if (const std::string* t = n.attr("Type")) a.type = *t;
    else if (a.type != "ActorFrame" && a.type != "quad" && a.type != "Quad" &&
             a.type != "Sprite" && a.type != "CODE")
        a.type = "Sprite";               // Layer/AutoActor/anything else
    if (const std::string* nm = n.attr("Name")) a.name = *nm;
    // Text= means File= is a font, not an image. Recorded before the File=
    // branch so the font name never reaches the texture loader.
    if (n.attr("Text")) a.isText = true;
    if (const std::string* f = n.attr("File")) {
        if (a.isText) {
            if (warn) warn->push_back("text actor not rendered (no font support): "
                                      + *f);
            for (const auto& at : n.attrs) {
                const std::string& k = at.first;
                if (k.size() > 7 && k.compare(k.size() - 7, 7, "Command") == 0)
                    a.commands.push_back({k, at.second});
            }
            for (const auto& kid : n.kids) {
                auto c = std::make_unique<Actor>();
                buildActor(kid, *c, dir, startSec, warn);
                a.children.push_back(std::move(c));
            }
            return;
        }
        std::string p = *f;
        // A DIRECTORY is a nested actor tree: SM loads <folder>/default.xml.
        // Resolved BEFORE the .xml/.sprite branches so the rewritten path goes
        // through the nested-tree path rather than the texture loader.
        {
            const std::string asDir =
                (p.size() > 1 && (p[0] == '/' || p[1] == ':')) ? p : dir + "/" + p;
            std::error_code ec;
            if (std::filesystem::is_directory(asDir, ec)) {
                for (const char* nm : {"/default.xml", "/Default.xml"}) {
                    FILE* fp = fopen((asDir + nm).c_str(), "rb");
                    if (fp) { fclose(fp); p += nm; break; }
                }
            }
        }
        // A File= naming another .xml is a NESTED ACTOR TREE, not an image --
        // SM's loader recurses into it and splices its children in here. It
        // is how a chart factors shared per-frame code out into its own file,
        // and feeding the path to the texture loader instead kills the whole
        // song on "cannot load texture .../<name>.xml".
        if (p.size() > 7 && p.compare(p.size() - 7, 7, ".sprite") == 0) {
            const std::string sp = (p.size() > 1 && (p[0] == '/' || p[1] == ':'))
                                 ? p : dir + "/" + p;
            parseSpriteFile(sp, a);
            if (a.file.empty() && warn) warn->push_back("cannot read " + sp);
            // parseSpriteFile has already set a.file to the SHEET it names.
            // Falling through would overwrite that with the .sprite path
            // itself, which the texture loader cannot decode -- so finish the
            // node here rather than letting the generic assignment run.
            for (const auto& at : n.attrs) {
                const std::string& k = at.first;
                if (k.size() > 7 && k.compare(k.size() - 7, 7, "Command") == 0)
                    a.commands.push_back({k, at.second});
            }
            for (const auto& kid : n.kids) {
                auto c = std::make_unique<Actor>();
                buildActor(kid, *c, dir, startSec, warn);
                a.children.push_back(std::move(c));
            }
            return;
        }
        else if (p.size() > 4 && p.compare(p.size() - 4, 4, ".xml") == 0) {
            const std::string sub = (p.size() > 1 && (p[0] == '/' || p[1] == ':'))
                                  ? p : dir + "/" + p;
            FILE* sf = fopen(sub.c_str(), "rb");
            if (sf) {
                std::string txt;
                { char b[65536]; size_t r;
                  while ((r = fread(b, 1, sizeof b, sf)) > 0) txt.append(b, r); }
                fclose(sf);
                size_t si = 0;
                XmlNode sroot;
                if (xmlParse(txt, si, sroot)) {
                    // Resolve the nested file's own paths against ITS folder.
                    const size_t sl = sub.find_last_of("/\\");
                    const std::string sdir = sl == std::string::npos ? dir
                                                                     : sub.substr(0, sl);
                    for (const auto& kid : sroot.kids) {
                        auto c = std::make_unique<Actor>();
                        buildActor(kid, *c, sdir, startSec, warn);
                        a.children.push_back(std::move(c));
                    }
                } else if (warn) {
                    warn->push_back("cannot parse " + sub);
                }
            } else if (warn) {
                warn->push_back("missing " + sub);
            }
            // Fall through to the child loop; no texture for this node.
            for (const auto& at : n.attrs) {
                const std::string& k = at.first;
                if (k.size() > 7 && k.compare(k.size() - 7, 7, "Command") == 0)
                    a.commands.push_back({k, at.second});
            }
            for (const auto& kid : n.kids) {
                auto c = std::make_unique<Actor>();
                buildActor(kid, *c, dir, startSec, warn);
                a.children.push_back(std::move(c));
            }
            return;
        }
        // File="mask" and File="platform" are extensionless; SM resolves by
        // trying the known image extensions. The dot check must look at the
        // BASENAME only: "../effects/platform" contains dots in the "..", and
        // testing the whole path skipped probing -- which is how the taiko2
        // platform rendered as an untextured white square.
        const size_t slash = p.find_last_of("/\\");
        const std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
        if (base.find('.') == std::string::npos) {
            bool found = false;
            for (const char* ext : {".png", ".jpg", ".jpeg", ".gif", ".bmp"}) {
                FILE* fp = fopen((dir + "/" + p + ext).c_str(), "rb");
                if (fp) { fclose(fp); p += ext; found = true; break; }
            }
            // Still nothing: SM matches a texture name against a file whose
            // stem STARTS with it, which is how a sheet keeps its dimensions
            // in the filename -- "walk" resolves to "walk 2x1.png". Scan the
            // folder rather than guessing the grid.
            if (!found) {
                const size_t sl2 = p.find_last_of("/\\");
                const std::string sub = sl2 == std::string::npos
                                      ? dir : dir + "/" + p.substr(0, sl2);
                const std::string stem = sl2 == std::string::npos
                                       ? p : p.substr(sl2 + 1);
                std::error_code ec;
                for (const auto& de : std::filesystem::directory_iterator(sub, ec)) {
                    const std::string fn = de.path().filename().string();
                    if (fn.size() <= stem.size()) continue;
                    if (fn.compare(0, stem.size(), stem) != 0) continue;
                    if (fn[stem.size()] != ' ') continue;   // "laugh2" is not "laugh"
                    const std::string ext = de.path().extension().string();
                    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" &&
                        ext != ".gif" && ext != ".bmp") continue;
                    p = (sl2 == std::string::npos) ? fn : p.substr(0, sl2 + 1) + fn;
                    found = true;
                    break;
                }
            }
        }
        a.file = (p.size() > 1 && (p[0] == '/' || p[1] == ':')) ? p : dir + "/" + p;
        // A texture that does not open leaves tex.id 0, and an untextured
        // quad draws SOLID WHITE. That is indistinguishable from an actor the
        // chart meant to be white, so say so here rather than leaving a white
        // rectangle on screen with no explanation.
        if (warn) {
            FILE* probe = fopen(a.file.c_str(), "rb");
            if (probe) fclose(probe);
            else warn->push_back("missing texture " + a.file +
                                 " (will draw as a white quad)");
        }
    }
    for (const auto& at : n.attrs) {
        const std::string& k = at.first;
        if (k.size() > 7 && k.compare(k.size() - 7, 7, "Command") == 0)
            a.commands.push_back({k, at.second});
    }
    for (const auto& kid : n.kids) {
        auto c = std::make_unique<Actor>();
        buildActor(kid, *c, dir, startSec, warn);
        a.children.push_back(std::move(c));
    }
}

void compileActorLua(Actor& a, LuaHost& lua, std::map<std::string, int>& cache,
                     std::vector<std::string>* warn) {
    for (ActorCommand& c : a.commands) {
        if (!isLuaCommand(c.text)) continue;
        const auto found = cache.find(c.text);
        if (found != cache.end()) {
            c.luaRef = found->second;
            continue;
        }
        std::string err;
        const std::string where = (a.name.empty() ? a.type : a.name) + "." + c.name;
        c.luaRef = lua.compileChunk(c.text, where, err);
        if (c.luaRef >= 0) cache.emplace(c.text, c.luaRef);
        else if (warn) warn->push_back(where + ": " + err);
    }
    for (auto& child : a.children) compileActorLua(*child, lua, cache, warn);
}

void scheduleActor(Actor& a, double startSec, LuaHost& lua,
                   std::vector<std::string>* warn) {
    // InitCommand runs first and instantly; OnCommand starts from that state.
    ActorState st;
    a.base = st;
    a.segs.clear();
    if (const ActorCommand* ini = a.findCommandEntry("InitCommand")) {
        if (ini->luaRef >= 0) {
            std::string err;
            const std::string where = (a.name.empty() ? a.type : a.name) + ".InitCommand";
            if (!lua.callChunk(ini->luaRef, a, startSec, where, err) && warn)
                warn->push_back(where + ": " + err);
            double endSec = startSec;
            for (const Seg& s : a.segs) endSec = std::max(endSec, double(s.t1));
            st = evalActor(a, endSec);
        } else if (!isLuaCommand(ini->text)) {
            Sched S; S.a = &a; S.t = startSec; S.cur = st;
            runCmds(S, parseChain(ini->text), 0, &lua);
            st = S.cur;
        }
    }
    a.base = st;
    a.segs.clear();
    a.onBase = st;      // OnCommand starts from the post-Init state
    for (auto& c : a.children) scheduleActor(*c, startSec, lua, warn);
}

// SM runs EVERY actor's InitCommand before ANY actor's OnCommand -- that is
// the whole point of having two phases. Interleaving them per actor means an
// OnCommand can run before a later sibling's InitCommand has published the
// global it depends on, which is exactly how a chart that does
// `<aft>.InitCommand: my_aft = self` and reads `my_aft` from an earlier
// sibling's OnCommand ends up indexing nil.
void scheduleActorOn(Actor& a, double startSec, LuaHost& lua,
                     std::vector<std::string>* warn) {
    if (const ActorCommand* on = a.findCommandEntry("OnCommand")) {
        if (on->luaRef >= 0) {
            std::string err;
            const std::string where = (a.name.empty() ? a.type : a.name) + ".OnCommand";
            if (!lua.callChunk(on->luaRef, a, startSec, where, err) && warn)
                warn->push_back(where + ": " + err);
        } else if (!isLuaCommand(on->text)) {
            scheduleChain(a, on->text, startSec, a.onBase, 0, &lua);
        }
    }
    for (auto& c : a.children) scheduleActorOn(*c, startSec, lua, warn);
}

void dispatchActorCommand(Actor& a, const std::string& cmd, double sec,
                          LuaHost& lua) {
    if (const ActorCommand* c = a.findCommandEntry(cmd)) {
        const ActorState st = evalActor(a, sec);
        // Preserve commands scheduled at this exact instant; the newly
        // appended segments override only properties this message touches.
        a.segs.erase(std::remove_if(a.segs.begin(), a.segs.end(),
            [&](const Seg& s) { return s.t0 > float(sec); }), a.segs.end());
        if (c->luaRef >= 0) {
            std::string err;
            const std::string where = (a.name.empty() ? a.type : a.name) + "." + cmd;
            if (!lua.callChunk(c->luaRef, a, sec, where, err))
                lua.note(where + ": " + err);
        } else if (!isLuaCommand(c->text)) {
            scheduleChain(a, c->text, sec, st, 0, &lua);
        }
    }
    for (auto& child : a.children) dispatchActorCommand(*child, cmd, sec, lua);
}


}  // namespace

ActorTree::ActorTree() = default;
ActorTree::~ActorTree() = default;
const std::vector<std::string>& ActorTree::log() const {
    static const std::vector<std::string> empty;
    return lua_ ? lua_->log() : empty;
}
void ActorTree::setChart(const Chart* chart) {
    chart_ = chart;
    if (lua_) lua_->setChart(chart);
}



bool ActorTree::load(const std::string& dir, double startSec, std::string& err) {
    dir_ = dir;
    startSec_ = startSec;
    root_.reset();
    namedTex_.clear();
    gameCmdReported_ = false;
    lua_ = std::make_unique<LuaHost>();
    if (!lua_->open(err)) return false;
    lua_->setTree(this);   // so an AFT can publish its texture by name
    // Before ANY chunk runs: the AFT sizes itself in InitCommand.
    lua_->setDisplaySize(dispW_, dispH_);
    lua_->setChart(chart_);
    const size_t slash = dir.find_last_of("/\\");
    lua_->setSongDir(slash == std::string::npos ? "." : dir.substr(0, slash));
    lua_->setBeat(0.0, startSec);

    // SM looks for default.xml; Windows is case-insensitive but the repo may
    // not be, and Saitama2000 ships both spellings across its folders.
    std::string path;
    for (const char* nm : {"/default.xml", "/Default.xml"}) {
        FILE* f = fopen((dir + nm).c_str(), "rb");
        if (f) { fclose(f); path = dir + nm; break; }
    }
    if (path.empty()) { err = "no default.xml in " + dir; return false; }

    FILE* f = fopen(path.c_str(), "rb");
    std::string txt;
    { char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) txt.append(b, n); }
    fclose(f);

    size_t i = 0;
    XmlNode root;
    if (!xmlParse(txt, i, root)) { err = "cannot parse " + path; return false; }

    root_ = std::make_unique<Actor>();
    std::vector<std::string> warn;
    buildActor(root, *root_, dir, startSec, &warn);
    std::map<std::string, int> cache;
    compileActorLua(*root_, *lua_, cache, &warn);
    scheduleActor(*root_, startSec, *lua_, &warn);
    // Second pass: see the note on scheduleActorOn.
    scheduleActorOn(*root_, startSec, *lua_, &warn);
    dispatchPending(startSec);
    for (const std::string& w : warn) lua_->note(w);
    return true;
}


// --- the Lua mod table -> ModDoc --------------------------------------------
// See the contract on ActorLayer::drainLuaMods.
int ActorTree::drainLuaMods(ModDoc& doc, ModDoc* doc2, int resolution) {
    if (!lua_ || !lua_->L()) return 0;
    lua_State* L = lua_->L();
    int added = 0;
    ModStringStats st;

    // gat splits its list across `mods` and `mods2`; both are the same shape.
    for (const char* tableName : {"mods", "mods2"}) {
        lua_getglobal(L, tableName);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

        // No ipairs: the reader in a NotITG modchart uses pairs(), so a table
        // with a hole still dispatches every row, and stopping at the first
        // gap would silently drop the tail.
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

            double beat = 0, span = 0;
            std::string modstr, kind = "len";
            lua_rawgeti(L, -1, 1); beat = lua_tonumber(L, -1);      lua_pop(L, 1);
            lua_rawgeti(L, -1, 2); span = lua_tonumber(L, -1);      lua_pop(L, 1);
            lua_rawgeti(L, -1, 3);
            if (lua_isstring(L, -1)) modstr = lua_tostring(L, -1);  lua_pop(L, 1);
            lua_rawgeti(L, -1, 4);
            if (lua_isstring(L, -1)) kind = lua_tostring(L, -1);    lua_pop(L, 1);
            // Row 5 is the player number. 0/absent drives both fields; 1 or 2
            // drives that player's own -- these charts mirror values between
            // the two, so folding them into one field would have P1's and
            // P2's rows fighting over the same knob.
            int pn = 0;
            lua_rawgeti(L, -1, 5);
            if (!lua_isnil(L, -1)) pn = int(lua_tonumber(L, -1));
            lua_pop(L, 1);

            lua_pop(L, 1);   // the row; key stays for lua_next

            if (modstr.empty() || kind == "error") continue;
            if (pn != 0 && !doc2) { ++perPlayerDropped_; continue; }

            const int tick = int(beat * resolution + 0.5);
            int len = 0;
            if (kind == "end") {
                // The second field is an END BEAT here, not a duration.
                const int endTick = int(span * resolution + 0.5);
                len = endTick - tick;
            } else {
                len = int(span * resolution + 0.5);
            }
            if (len < 0) len = 0;
            // A window that rounds away to nothing would never be live; one
            // tick is the smallest thing that still fires.
            if (len == 0 && span > 0.0) len = 1;

            // pn 0 -> both docs; pn 1 -> player 1's; pn 2 -> player 2's.
            if (doc2 && pn != 1) {
                const int b2 = int(doc2->entries.size());
                addModString(*doc2, modstr, tick, len, st);
                if (pn == 2) { added += int(doc2->entries.size()) - b2; continue; }
                st.entries -= int(doc2->entries.size()) - b2;   // counted once
            }
            const int before = int(doc.entries.size());
            addModString(doc, modstr, tick, len, st);
            added += int(doc.entries.size()) - before;
        }
        lua_pop(L, 1);   // the table
    }

    for (const std::string& u : st.unknown)
        lua_->note("mod token not implemented: " + u);
    if (perPlayerDropped_) {
        char m[128];
        snprintf(m, sizeof m, "%d per-player mod rows dropped -- one playfield",
                 perPlayerDropped_);
        lua_->note(m);
        perPlayerDropped_ = 0;
    }
    return added;
}

void ActorTree::broadcast(const std::string& msg, double sec) {
    if (!root_ || !lua_) return;
    lua_->setBeat(lua_->beat(), sec);
    dispatchActorCommand(*root_, msg + "MessageCommand", sec, *lua_);
    dispatchPending(sec);
}

void ActorTree::dispatchPending(double sec) {
    if (!root_ || !lua_) return;
    for (int pass = 0; pass < 64; ++pass) {
        auto& pending = lua_->pendingBroadcasts();
        if (pending.empty()) return;
        std::vector<std::string> batch;
        batch.swap(pending);
        for (const std::string& msg : batch)
            dispatchActorCommand(*root_, msg + "MessageCommand", sec, *lua_);
    }
    lua_->pendingBroadcasts().clear();
    lua_->note("MESSAGEMAN broadcast recursion limit reached");
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
namespace {

// The tree's AFT registry, for the duration of one draw walk. A parameter
// would have to thread through every recursive call to be read by one leaf
// case; ActorTree::draw sets this immediately before walking and there is one
// draw at a time, so the narrower plumbing is not worth the noise.
static std::map<std::string, ActorTree::NamedTex>* g_namedTex = nullptr;

void drawActor(Renderer& R, Actor& a, double sec, double beat,
               const ActorState& parent) {
    ActorState st = evalActor(a, sec);
    applyEffect(a, st, sec, beat);
    if (st.hidden) return;

    // Compose with the parent frame: ActorFrame children are relative, and a
    // rotated frame rotates its children's OFFSETS too -- effects2 does
    // `rotationz,20` on the whole taiko assembly at 9.6 s, and without this
    // the children would stay put while their sprites spun in place.
    ActorState w = st;
    {
        const float th = parent.rotZ * 3.14159265f / 180.0f;
        const float c = cosf(th), sn = sinf(th);
        const float lx = st.x * parent.zoomX, ly = st.y * parent.zoomY;
        w.x = parent.x + lx * c - ly * sn;
        w.y = parent.y + lx * sn + ly * c;
    }
    w.zoomX = parent.zoomX * st.zoomX * a.baseZoomX;
    w.zoomY = parent.zoomY * st.zoomY * a.baseZoomY;
    w.rotZ = parent.rotZ + st.rotZ;
    w.rotX = parent.rotX + st.rotX;
    w.rotY = parent.rotY + st.rotY;
    w.a = parent.a * st.a;

    // An ActorFrameTexture draws nothing; it CAPTURES. SM renders the AFT's
    // preceding siblings into its texture, and for a tree drawn in order that
    // is the framebuffer as it stands right here -- so a copy from the read
    // buffer is the same content without a second render pass.
    if (a.isAft) {
        if (a.aftTex) {
            glBindTexture(GL_TEXTURE_2D, a.aftTex);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, a.aftW, a.aftH);
        }
        for (auto& c : a.children) drawActor(R, *c, sec, beat, w);
        return;
    }

    if (a.type != "ActorFrame" && !a.isText) {
        if (!a.texLoaded && !a.file.empty() && a.file[0] == '@') {
            // Published by an AFT's SetTextureName. Resolved every frame until
            // it exists, because the AFT that creates it may be built after
            // the sprite that references it.
            if (g_namedTex) {
                auto it = g_namedTex->find(a.file.substr(1));
                if (it != g_namedTex->end()) {
                    a.tex.id = it->second.id;
                    // Natural size = the AFT's real allocation, so the
                    // chart's basezoom(virt/real) scales it back to
                    // exactly virtual-screen size.
                    a.tex.w = it->second.w;
                    a.tex.h = it->second.h;
                    a.sheetCols = a.sheetRows = 1;
                    a.texLoaded = true;
                }
            }
        }
        else if (!a.texLoaded && !a.file.empty()) {
            FILE* f = fopen(a.file.c_str(), "rb");
            if (f) { fclose(f); a.tex = gl_loadTex(a.file, false, /*flipY=*/false); }
            a.texLoaded = true;
        }
        // A Sprite whose texture never resolved must draw NOTHING. The
        // untextured white quad is Quad's contract, not a fallback: a missing
        // file or an AFT name that has not been published yet would otherwise
        // paint a white rectangle exactly where the intended art belongs (the
        // load-time warning already says which file). Quad and CODE still
        // draw untextured -- that is what they are for.
        if (!a.tex.id && a.type != "Quad" && a.type != "quad" &&
            a.type != "CODE") {
            for (auto& c : a.children) drawActor(R, *c, sec, beat, w);
            return;
        }
        if (a.tex.id && a.textureFilterDirty) {
            glBindTexture(GL_TEXTURE_2D, a.tex.id);
            const GLint filter = a.textureFiltering ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            a.textureFilterDirty = false;
        }
        // Natural size is ONE CELL, not the whole sheet: an unzoomed .sprite
        // whose sheet is 2x1 would otherwise draw at double width.
        const int cols = a.sheetCols > 0 ? a.sheetCols : 1;
        const int rows = a.sheetRows > 0 ? a.sheetRows : 1;
        // An untextured actor with no explicit size is 1x1, as SM's Quad is.
        // 64 here was a guess, and it is exactly the size of the stray white
        // square a bare function-holder Def.Quad painted at the origin -- a
        // chart idiom that relies on the default being invisible.
        float sw = (w.sizeX > 0) ? w.sizeX
                                 : float(a.tex.w ? a.tex.w / cols : 1);
        float sh = (w.sizeY > 0) ? w.sizeY
                                 : float(a.tex.h ? a.tex.h / rows : 1);
        sw *= w.zoomX; sh *= w.zoomY;
        float ox = 0, oy = 0;
        if (st.horizAlign < 0) ox = sw * 0.5f;
        else if (st.horizAlign > 0) ox = -sw * 0.5f;
        if (st.vertAlign < 0) oy = sh * 0.5f;
        else if (st.vertAlign > 0) oy = -sh * 0.5f;
        // rotX/rotY as foreshortening: cos-scale the axis the rotation tips
        // away. An approximation -- SM projects through VanishX/VanishY and a
        // real FOV -- but it reads correctly at the ~20 degree tilts these
        // files use, where the difference from true perspective is a few px.
        const float fx = cosf(w.rotY * 3.14159265f / 180.0f);
        const float fy = cosf(w.rotX * 3.14159265f / 180.0f);
        float su0, sv0, su1, sv1;
        spriteUV(a, sec, su0, sv0, su1, sv1);
        R.drawActorQuad(w.x + ox, w.y + oy, sw * fabsf(fx), sh * fabsf(fy), w.rotZ,
                        w.r, w.g, w.b, w.a,
                        a.tex.id, st.blend, st.zWrite, st.zTest, st.clearZ,
                        su0, sv0, su1, sv1);
    }
    for (auto& c : a.children) drawActor(R, *c, sec, beat, w);
}

}  // namespace


void ActorTree::enqueue(double t, Actor& a, const std::string& cmd) {
    // Runaway guard: a body that queues itself with no sleep would append
    // forever inside one pump step. 4096 is far above any real chart's
    // in-flight count and well below anything that would stall a frame.
    if (pending_.size() > 4096) return;
    Pending p{t, &a, cmd};
    auto it = std::upper_bound(pending_.begin(), pending_.end(), p,
        [](const Pending& x, const Pending& y) { return x.t < y.t; });
    pending_.insert(it, p);
}

void ActorTree::runPending(double sec, int maxSteps) {
    if (!lua_) return;
    int steps = 0;
    const size_t queued = pending_.size();
    while (!pending_.empty() && pending_.front().t <= sec) {
        if (++steps > maxSteps) {
            if (!pumpOverrun_) {
                pumpOverrun_ = true;
                lua_->note("command pump hit its step cap; the chart is "
                           "behind where it should be");
            }
            break;
        }
        const Pending p = pending_.front();
        pending_.erase(pending_.begin());
        if (!p.a) continue;
        const ActorCommand* c = p.a->findCommandEntry(p.cmd + "Command");
        if (!c) continue;
        // The body runs AS OF ITS SCHEDULED TIME, not the frame time: a chart
        // that steps 0.02s at a time must see each of those instants, or its
        // beat-gated branches are skipped wholesale.
        const double eventBeat = chart_ ? chart_->secToBeat(p.t) : pumpBeat_;
        lua_->setBeat(eventBeat, p.t);
        lua_->chunkStart   = p.t;
        lua_->pendingSleep = 0.0;
        if (c->luaRef >= 0) {
            std::string err;
            const std::string where =
                (p.a->name.empty() ? p.a->type : p.a->name) + "." + c->name;
            if (!lua_->callChunk(c->luaRef, *p.a, p.t, where, err))
                lua_->note(where + ": " + err);
        } else if (!isLuaCommand(c->text)) {
            scheduleChain(*p.a, c->text, p.t, p.a->onBase, 0, lua_.get());
        }
        dispatchPending(p.t);   // MESSAGEMAN:Broadcast from inside the body
    }
    if (steps > pumpRan_) pumpRan_ = steps;
    if (queued && !pumpEverRan_) {
        pumpEverRan_ = true;
        char m[96];
        snprintf(m, sizeof m, "command pump started (%d queued)", int(queued));
        lua_->note(m);
    }
}

void ActorTree::setDisplaySize(int w, int h) {
    dispW_ = w; dispH_ = h;
    if (lua_) lua_->setDisplaySize(w, h);
}

void ActorTree::update(double sec, double beat, int maxSteps) {
    if (!root_ || !lua_) return;
    if (sec < startSec_) return;

    // First call: arm the pump from whatever Init/On queued.
    if (luaClock_ < 0.0) luaClock_ = startSec_;

    // Backwards seek. The loop's cursors only move forward, so there is
    // nothing to rewind -- the tree is rebuilt and replayed. Deterministic and
    // identical to what an encode produced, but it costs a replay, which is
    // why the editor feels a scrub backwards and an encode never does.
    if (sec < luaClock_ - 1e-6) {
        std::string err;
        const std::string d = dir_;
        const double st = startSec_;
        pending_.clear();
        pumpOverrun_ = false;
        luaClock_ = -1.0;
        if (!load(d, st, err)) return;
        luaClock_ = startSec_;
    }

    pumpBeat_ = beat;
    runPending(sec, maxSteps);
    if (!gameCmdReported_ && lua_->gameCommands() > 0) {
        gameCmdReported_ = true;
        lua_->note("GAMESTATE:ApplyGameCommand is accepted but not applied");
    }
    luaClock_ = sec;
}

void ActorTree::draw(Renderer& R, double sec) {
    if (!root_ || sec < startSec_) return;
    if (lua_) lua_->setBeat(R.actorBeat(), sec);
    ActorState id;
    g_namedTex = &namedTex_;
    drawActor(R, *root_, sec, R.actorBeat(), id);
    g_namedTex = nullptr;
}

// ---------------------------------------------------------------------------
// LuaHost -- vendored Lua 5.1.5, opened with the shim globals charts expect.
// ---------------------------------------------------------------------------
namespace {
int lua_absorb(lua_State* L) {                 // returns itself, so chains work
    lua_pushvalue(L, 1);
    return 1;
}
struct LuaActorRef {
    Actor* actor;
    LuaHost* host;
};

enum ActorMethod {
    AM_X, AM_Y, AM_Z, AM_ZOOM, AM_ZOOMTO, AM_ZOOMX, AM_ZOOMY, AM_ZOOMZ,
    AM_VISIBLE, AM_HIDDEN, AM_DIFFUSEALPHA, AM_DIFFUSE,
    AM_ROTX, AM_ROTY, AM_ROTZ,
    AM_LINEAR, AM_ACCEL, AM_DECEL, AM_SPRING, AM_BOUNCEBEGIN, AM_BOUNCEEND,
    AM_SLEEP, AM_FINISH, AM_STOP, AM_FILTER, AM_PLAY, AM_QUEUE,
    AM_QUEUEMESSAGE, AM_GETCHILD,
    AM_GETX, AM_GETY, AM_GETZ, AM_GETZOOM, AM_GETROT,
    AM_SETSTATE, AM_ANIMATE,
    AM_BOB, AM_BOUNCE, AM_WAG, AM_VIBRATE, AM_STOPEFFECT,
    AM_EFFECTMAG, AM_EFFECTPERIOD, AM_EFFECTCLOCK,
    // ActorFrameTexture
    AM_AFT_NAME, AM_AFT_W, AM_AFT_H, AM_AFT_CREATE, AM_AFT_GETTEX,
    AM_AFT_DEPTH, AM_AFT_ALPHA, AM_AFT_FLOAT, AM_AFT_PRESERVE,
    // plain actor methods that were reaching the unsupported path
    AM_SETTEXTURE, AM_BLEND, AM_BASEZOOMX, AM_BASEZOOMY,
    AM_ADDX, AM_ADDY, AM_ADDZ
};

struct ActorMethodDef { const char* name; ActorMethod method; };
const ActorMethodDef ACTOR_METHODS[] = {
    {"x", AM_X}, {"y", AM_Y}, {"z", AM_Z},
    {"zoom", AM_ZOOM}, {"zoomto", AM_ZOOMTO},
    {"zoomx", AM_ZOOMX}, {"zoomy", AM_ZOOMY}, {"zoomz", AM_ZOOMZ},
    {"visible", AM_VISIBLE}, {"hidden", AM_HIDDEN},
    {"diffusealpha", AM_DIFFUSEALPHA}, {"diffuse", AM_DIFFUSE},
    {"rotationx", AM_ROTX}, {"rotationy", AM_ROTY}, {"rotationz", AM_ROTZ},
    {"linear", AM_LINEAR}, {"accelerate", AM_ACCEL},
    {"decelerate", AM_DECEL}, {"spring", AM_SPRING},
    {"bouncebegin", AM_BOUNCEBEGIN}, {"bounceend", AM_BOUNCEEND},
    {"sleep", AM_SLEEP}, {"finishtweening", AM_FINISH},
    {"stoptweening", AM_STOP}, {"SetTextureFiltering", AM_FILTER},
    {"playcommand", AM_PLAY}, {"queuecommand", AM_QUEUE},
    {"queuemessage", AM_QUEUEMESSAGE}, {"GetChild", AM_GETCHILD},
    {"GetX", AM_GETX}, {"GetY", AM_GETY}, {"GetZ", AM_GETZ},
    {"GetZoom", AM_GETZOOM}, {"getrotation", AM_GETROT},
    {"setstate", AM_SETSTATE}, {"animate", AM_ANIMATE},
    {"bob", AM_BOB}, {"bounce", AM_BOUNCE}, {"wag", AM_WAG},
    {"vibrate", AM_VIBRATE}, {"stopeffect", AM_STOPEFFECT},
    {"effectmagnitude", AM_EFFECTMAG}, {"effectperiod", AM_EFFECTPERIOD},
    {"effectclock", AM_EFFECTCLOCK},
    {"SetTextureName", AM_AFT_NAME}, {"SetWidth", AM_AFT_W},
    {"SetHeight", AM_AFT_H}, {"Create", AM_AFT_CREATE},
    {"GetTexture", AM_AFT_GETTEX},
    {"EnableDepthBuffer", AM_AFT_DEPTH}, {"EnableAlphaBuffer", AM_AFT_ALPHA},
    {"EnableFloat", AM_AFT_FLOAT}, {"EnablePreserveTexture", AM_AFT_PRESERVE},
    {"SetTexture", AM_SETTEXTURE}, {"blend", AM_BLEND},
    {"basezoomx", AM_BASEZOOMX}, {"basezoomy", AM_BASEZOOMY},
    {"addx", AM_ADDX}, {"addy", AM_ADDY}, {"addz", AM_ADDZ},
};


// The natives the boot chunk's shims close over. Each
// carries its LuaHost as an upvalue rather than a global, so two hosts in one
// process cannot cross-talk.
LuaHost* hostOf(lua_State* L) {
    return static_cast<LuaHost*>(lua_touserdata(L, lua_upvalueindex(1)));
}
int lua_nc_sec(lua_State* L) {
    LuaHost* h = hostOf(L);
    lua_pushnumber(L, h ? h->sec() : 0.0);
    return 1;
}
int lua_nc_beat(lua_State* L) {
    LuaHost* h = hostOf(L);
    lua_pushnumber(L, h ? h->beat() : 0.0);
    return 1;
}
int lua_nc_songdir(lua_State* L) {
    LuaHost* h = hostOf(L);
    lua_pushstring(L, h ? h->songDir().c_str() : "");
    return 1;
}
int lua_nc_broadcast(lua_State* L) {
    LuaHost* h = hostOf(L);
    const char* m = lua_tostring(L, 1);
    if (h && m) h->queueBroadcast(m);
    return 0;
}
// Counts what a per-frame ApplyGameCommand asked for, so the gap is visible
// rather than silent. Reported once per run by the tree.
int lua_nc_gamecmd(lua_State* L) {
    LuaHost* h = hostOf(L);
    if (h) h->noteGameCommand();
    return 0;
}
int lua_nc_dispw(lua_State* L) {
    LuaHost* h = hostOf(L);
    lua_pushnumber(L, h ? h->displayW() : 1920);
    return 1;
}
int lua_nc_disph(lua_State* L) {
    LuaHost* h = hostOf(L);
    lua_pushnumber(L, h ? h->displayH() : 1080);
    return 1;
}
int lua_nc_trace(lua_State* L) {
    LuaHost* h = hostOf(L);
    const char* s = lua_tostring(L, 1);
    if (h && s) h->note(s);
    return 0;
}
}  // namespace
struct LuaHost::CallState {
    std::map<Actor*, Sched> actors;
    int depth = 0;
};

void LuaHost::pushActor(Actor& actor) {
    LuaActorRef* ref = static_cast<LuaActorRef*>(lua_newuserdata(L_, sizeof(LuaActorRef)));
    ref->actor = &actor;
    ref->host = this;
    luaL_getmetatable(L_, "nc.Actor");
    lua_setmetatable(L_, -2);
}


Actor* LuaHost::toActor(lua_State* L, int idx) {
    if (!lua_isuserdata(L, idx)) return nullptr;
    void* ud = lua_touserdata(L, idx);
    if (!ud) return nullptr;
    // luaL_checkudata would throw on a foreign userdata; this is a query, and
    // a chart passing something else should get null rather than an error.
    if (!lua_getmetatable(L, idx)) return nullptr;
    luaL_getmetatable(L, "nc.Actor");
    const bool ours = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return ours ? static_cast<LuaActorRef*>(ud)->actor : nullptr;
}

void LuaHost::aftCreate(Actor& a) {
    if (a.aftTex) return;                       // Create() called twice
    if (a.aftW <= 0) a.aftW = 512;
    if (a.aftH <= 0) a.aftH = 512;

    glGenTextures(1, &a.aftTex);
    glBindTexture(GL_TEXTURE_2D, a.aftTex);
    // EnableFloat asks for a float target. It exists so an accumulating
    // feedback chain does not band, and RGBA8 is what everything else here
    // uses; honouring it costs nothing where the driver has the format.
    const GLint fmt = a.aftFloat ? GL_RGBA16F : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, a.aftW, a.aftH, 0, GL_RGBA,
                 a.aftFloat ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &a.aftFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, a.aftFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, a.aftTex, 0);
    // Cleared once, not per frame: EnablePreserveTexture(true) means the
    // contents survive between frames, which is what a feedback effect reads.
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (tree_ && !a.aftName.empty())
        tree_->namedTextures()[a.aftName] = {a.aftTex, a.aftW, a.aftH};
    note("ActorFrameTexture '" + a.aftName + "' created");
}

int LuaHost::actorIndex(lua_State* L) {
    LuaActorRef* ref = static_cast<LuaActorRef*>(luaL_checkudata(L, 1, "nc.Actor"));
    const char* name = luaL_checkstring(L, 2);
    for (const ActorMethodDef& def : ACTOR_METHODS) {
        if (strcmp(def.name, name) != 0) continue;
        lua_pushinteger(L, int(def.method));
        lua_pushcclosure(L, &LuaHost::actorCall, 1);
        return 1;
    }
    if (ref && ref->host && name)
        ref->host->note(std::string("unsupported Actor method: ") + name);
    lua_pushcfunction(L, lua_absorb);
    return 1;
}

int LuaHost::actorCall(lua_State* L) {
    LuaActorRef* ref = static_cast<LuaActorRef*>(luaL_checkudata(L, 1, "nc.Actor"));
    LuaHost* host = ref ? ref->host : nullptr;
    Actor* actor = ref ? ref->actor : nullptr;
    if (!host || !actor || !host->call_) {
        lua_pushvalue(L, 1);
        return 1;
    }
    const ActorMethod method = ActorMethod(lua_tointeger(L, lua_upvalueindex(1)));
    if (method == AM_GETCHILD) {
        const char* name = luaL_checkstring(L, 2);
        for (auto& child : actor->children) {
            if (child->name == name) {
                host->pushActor(*child);
                return 1;
            }
        }
        lua_getglobal(L, "ABSORB");
        lua_call(L, 0, 1);
        return 1;
    }

    // The AFT methods run OUTSIDE the tween scheduler: they configure and
    // allocate a render target rather than animating a property, so they must
    // work during InitCommand, before any Sched exists for this actor.
    switch (method) {
        case AM_AFT_NAME: {
            actor->isAft = true;
            const char* nm = lua_tostring(L, 2);
            if (nm) actor->aftName = nm;
            lua_pushvalue(L, 1);
            return 1;
        }
        case AM_AFT_W: actor->isAft = true;
                       actor->aftW = int(lua_tonumber(L, 2));
                       lua_pushvalue(L, 1); return 1;
        case AM_AFT_H: actor->isAft = true;
                       actor->aftH = int(lua_tonumber(L, 2));
                       lua_pushvalue(L, 1); return 1;
        case AM_AFT_DEPTH:    actor->aftDepth    = lua_toboolean(L, 2) != 0;
                              lua_pushvalue(L, 1); return 1;
        case AM_AFT_ALPHA:    actor->aftAlpha    = lua_toboolean(L, 2) != 0;
                              lua_pushvalue(L, 1); return 1;
        case AM_AFT_FLOAT:    actor->aftFloat    = lua_toboolean(L, 2) != 0;
                              lua_pushvalue(L, 1); return 1;
        case AM_AFT_PRESERVE: actor->aftPreserve = lua_toboolean(L, 2) != 0;
                              lua_pushvalue(L, 1); return 1;
        case AM_AFT_CREATE:
            actor->isAft = true;
            host->aftCreate(*actor);
            lua_pushvalue(L, 1);
            return 1;
        case AM_AFT_GETTEX:
            // SM hands back a texture object. Ours is the actor itself: every
            // consumer either passes it straight to SetTexture or reads its
            // name, and both work off the actor. Returning a distinct userdata
            // would be a second type with one member.
            lua_pushvalue(L, 1);
            return 1;
        case AM_SETTEXTURE: {
            // SetTexture(aft) or SetTexture("name").
            if (lua_isstring(L, 2)) {
                const char* nm = lua_tostring(L, 2);
                if (nm) actor->file = std::string("@") + nm;
            } else {
                Actor* src = host->toActor(L, 2);
                if (src && !src->aftName.empty())
                    actor->file = "@" + src->aftName;
            }
            actor->texLoaded = false;
            lua_pushvalue(L, 1);
            return 1;
        }
        case AM_BASEZOOMX:
            actor->baseZoomX = float(luaL_optnumber(L, 2, 1.0));
            lua_pushvalue(L, 1);
            return 1;
        case AM_BASEZOOMY:
            actor->baseZoomY = float(luaL_optnumber(L, 2, 1.0));
            lua_pushvalue(L, 1);
            return 1;
        case AM_SETSTATE:
            actor->spriteState = int(luaL_optinteger(L, 2, 0));
            lua_pushvalue(L, 1);
            return 1;
        case AM_ANIMATE:
            actor->spriteAnimate = lua_toboolean(L, 2) != 0;
            lua_pushvalue(L, 1);
            return 1;
        default: break;
    }

    auto found = host->call_->actors.find(actor);
    if (found == host->call_->actors.end()) {
        Sched initial{};
        initial.a = actor;
        initial.t = host->sec_;
        initial.cur = evalActor(*actor, host->sec_);
        found = host->call_->actors.emplace(actor, initial).first;
    }
    Sched& S = found->second;
    auto number = [&](int i, float d = 0.0f) {
        return float(luaL_optnumber(L, i, d));
    };
    auto boolean = [&](int i, bool d) {
        if (lua_gettop(L) < i || lua_isnil(L, i)) return d;
        return lua_isnumber(L, i) ? lua_tonumber(L, i) != 0.0
                                  : lua_toboolean(L, i) != 0;
    };

    switch (method) {
        case AM_X: emit1(S, PROP_X, number(2)); break;
        case AM_Y: emit1(S, PROP_Y, number(2)); break;
        case AM_Z: emit1(S, PROP_Z, number(2)); break;
        case AM_ZOOM: {
            const float z = number(2, 1.0f);
            emit1(S, PROP_ZOOMX, z);
            emit1(S, PROP_ZOOMY, z);
        } break;
        case AM_ZOOMTO: {
            const float size[2] = {number(2), number(3)};
            emit(S, PROP_SIZE, 2, size);
        } break;
        case AM_ZOOMX: emit1(S, PROP_ZOOMX, number(2, 1.0f)); break;
        case AM_ZOOMY: emit1(S, PROP_ZOOMY, number(2, 1.0f)); break;
        case AM_ZOOMZ: emit1(S, PROP_ZOOMZ, number(2, 1.0f)); break;
        case AM_ADDX: emit1(S, PROP_X, S.cur.x + number(2)); break;
        case AM_ADDY: emit1(S, PROP_Y, S.cur.y + number(2)); break;
        case AM_ADDZ: emit1(S, PROP_Z, S.cur.z + number(2)); break;
        case AM_VISIBLE: emit1(S, PROP_HIDDEN, boolean(2, true) ? 0.0f : 1.0f); break;
        case AM_HIDDEN: emit1(S, PROP_HIDDEN, boolean(2, true) ? 1.0f : 0.0f); break;
        case AM_DIFFUSEALPHA: emit1(S, PROP_DIFFUSEALPHA, number(2, 1.0f)); break;
        case AM_DIFFUSE: {
            float colour[4] = {1, 1, 1, 1};
            if (lua_istable(L, 2)) {
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, 2, i + 1);
                    if (lua_isnumber(L, -1)) colour[i] = float(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                for (int i = 0; i < 4; ++i) colour[i] = number(i + 2, 1.0f);
            }
            emit(S, PROP_DIFFUSE, 4, colour);
        } break;
        case AM_ROTX: emit1(S, PROP_ROTX, number(2)); break;
        case AM_ROTY: emit1(S, PROP_ROTY, number(2)); break;
        case AM_ROTZ: emit1(S, PROP_ROTZ, number(2)); break;
        case AM_LINEAR: S.dur = number(2); S.ease = Ease::Linear; break;
        case AM_ACCEL: S.dur = number(2); S.ease = Ease::Accelerate; break;
        case AM_DECEL: S.dur = number(2); S.ease = Ease::Decelerate; break;
        case AM_SPRING: S.dur = number(2); S.ease = Ease::Spring; break;
        case AM_BOUNCEBEGIN: S.dur = number(2); S.ease = Ease::BounceBegin; break;
        case AM_BOUNCEEND: S.dur = number(2); S.ease = Ease::BounceEnd; break;
        case AM_SLEEP:
            // Two effects, both real: it advances the tween cursor, and it
            // delays whatever this body queues afterwards.
            S.t += number(2);
            S.dur = 0;
            S.ease = Ease::Instant;
            host->pendingSleep += number(2);
            break;
        case AM_FINISH:
            settleTweens(S, true);
            break;
        case AM_STOP:
            settleTweens(S, false);
            break;
        case AM_GETX: lua_pushnumber(L, S.cur.x); return 1;
        case AM_GETY: lua_pushnumber(L, S.cur.y); return 1;
        case AM_GETZ: lua_pushnumber(L, S.cur.z); return 1;
        case AM_GETZOOM: lua_pushnumber(L, S.cur.zoomX); return 1;
        case AM_GETROT:
            lua_pushnumber(L, S.cur.rotX);
            lua_pushnumber(L, S.cur.rotY);
            lua_pushnumber(L, S.cur.rotZ);
            return 3;
        case AM_BLEND: {
            const char* value = lua_tostring(L, 2);
            const std::string b = value ? lower(value) : "normal";
            emit1(S, PROP_BLEND, b == "add" ? 1.0f :
                                (b == "noeffect" ? 2.0f : 0.0f));
        } break;
        case AM_BOB: actor->effect.kind = Effect::Bob; break;
        case AM_BOUNCE: actor->effect.kind = Effect::Bounce; break;
        case AM_WAG: actor->effect.kind = Effect::Wag; break;
        case AM_VIBRATE: actor->effect.kind = Effect::Vibrate; break;
        case AM_STOPEFFECT: actor->effect.kind = Effect::None; break;
        case AM_EFFECTMAG:
            actor->effect.magX = number(2);
            actor->effect.magY = number(3);
            actor->effect.magZ = number(4);
            break;
        case AM_EFFECTPERIOD: actor->effect.period = number(2, 1.0f); break;
        case AM_EFFECTCLOCK: {
            const char* value = lua_tostring(L, 2);
            actor->effect.beatClock = value && lower(value) == "bgm";
        } break;
        case AM_FILTER: {
            const bool filter = boolean(2, true);
            if (actor->textureFiltering != filter) {
                actor->textureFiltering = filter;
                actor->textureFilterDirty = true;
            }
        } break;
        case AM_QUEUEMESSAGE: {
            const char* name = luaL_checkstring(L, 2);
            host->queueBroadcast(name);
        } break;
        case AM_PLAY:
        case AM_QUEUE: {
            const char* name = luaL_checkstring(L, 2);
            // Scheduled, not called: a body that ends `sleep(t)
            // queuecommand('Update')` is arming itself for later, and running
            // it inline turns the chart's main loop into 8 recursive frames at
            // load time that then stop forever.
            if (host->tree_) {
                const double when = host->chunkStart + host->pendingSleep;
                if (method == AM_PLAY) {
                    // SM's ActorFrame::PlayCommand runs on self AND every
                    // descendant; queuecommand queues on self only. The
                    // stacked-sprite animation idiom depends on the recursion:
                    // playcommand('Attack') on the character frame is what
                    // reaches the AttackCommand on each grandchild sheet, and
                    // without it no pose ever switches.
                    struct W { static void go(ActorTree* t, double tm,
                                              Actor& x, const char* n) {
                        if (x.findCommandEntry(std::string(n) + "Command"))
                            t->enqueue(tm, x, n);
                        for (auto& c : x.children) go(t, tm, *c, n);
                    } };
                    W::go(host->tree_, when, *actor, name);
                } else {
                    host->tree_->enqueue(when, *actor, name);
                }
                lua_pushvalue(L, 1);
                return 1;
            }
            if (host->call_->depth < 8) {
                ++host->call_->depth;
                if (const ActorCommand* command =
                        actor->findCommandEntry(std::string(name) + "Command")) {
                    if (command->luaRef >= 0) {
                        std::string err;
                        if (!host->invokeChunk(command->luaRef, *actor, command->name, err))
                            host->note(command->name + ": " + err);
                    } else if (!isLuaCommand(command->text)) {
                        runCmds(S, parseChain(command->text), host->call_->depth, host);
                    }
                }
                --host->call_->depth;
            }
        } break;
        default: break;
    }
    lua_pushvalue(L, 1);
    return 1;
}

bool LuaHost::invokeChunk(int ref, Actor& actor, const std::string& where,
                          std::string& err) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        err = where + ": invalid Lua registry reference";
        return false;
    }
    pushActor(actor);
    if (lua_pcall(L_, 1, 0, 0) != 0) {
        err = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "runtime error";
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaHost::callChunk(int ref, Actor& actor, double sec,
                        const std::string& where, std::string& err) {
    if (!L_ || ref < 0) return false;
    if (call_) return invokeChunk(ref, actor, where, err);
    CallState state;
    call_ = &state;
    sec_ = sec;
    const bool ok = invokeChunk(ref, actor, where, err);
    call_ = nullptr;
    return ok;
}


void LuaHost::note(const std::string& s) {
    for (const auto& e : log_) if (e == s) return;
    log_.push_back(s);
}

bool LuaHost::open(std::string& err) {
    L_ = luaL_newstate();
    if (!L_) { err = "luaL_newstate failed"; return false; }
    luaL_openlibs(L_);
    luaL_newmetatable(L_, "nc.Actor");
    lua_pushcfunction(L_, &LuaHost::actorIndex);
    lua_setfield(L_, -2, "__index");
    lua_pop(L_, 1);

    // Register the natives FIRST: the boot chunk defines GAMESTATE and
    // MESSAGEMAN as closures over them, and while Lua resolves globals at call
    // time, a missing native here is the difference between a working modfile
    // and one that dies on its first GAMESTATE:GetSongBeat().
    struct Reg { const char* name; lua_CFunction fn; };
    const Reg natives[] = {
        {"__nc_beat", lua_nc_beat},
        {"__nc_sec", lua_nc_sec},
        {"__nc_songdir", lua_nc_songdir},
        {"__nc_broadcast", lua_nc_broadcast},
        {"__nc_gamecmd", lua_nc_gamecmd},
        {"__nc_dispw", lua_nc_dispw},
        {"__nc_disph", lua_nc_disph},
        {"__nc_trace", lua_nc_trace},
    };
    for (const Reg& r : natives) {
        lua_pushlightuserdata(L_, this);
        lua_pushcclosure(L_, r.fn, 1);
        lua_setglobal(L_, r.name);
    }
    // An absorbing metatable: any unknown index yields a function that returns
    // its receiver. That makes `SCREENMAN:GetTopScreen():GetChild('x'):hidden(1)`
    // a no-op chain instead of a crash, which is the right failure mode for a
    // modfile that reaches for theme UI NotClon does not have.
    const char* boot =
        // table.getn/table.setn. Stock Lua 5.1.5 ships with LUA_COMPAT_GETN
        // OFF (third_party/lua/luaconf.h:322), so neither exists -- but every
        // OITG-era modfile uses them, including Saitama2000's own
        // lua/default.xml (three calls). Without these, the first script that
        // runs dies on a nil `table.getn`. Guarded so a future Lua that has
        // them keeps its own.
        "if not table.getn then table.getn = function(t) return #t end end\n"
        // NOT `if not table.setn`: 5.1.5 DEFINES setn and its body is
        // `error("'setn' is obsolete")`, so the guard never fires and every
        // caller still dies. Overwrite it unconditionally with the no-op that
        // matches what setn did back when tables carried a stored size.
        "SCREEN_WIDTH = 640\n"
        "SCREEN_HEIGHT = 480\n"
        "SCREEN_CENTER_X = 320\n"
        "SCREEN_CENTER_Y = 240\n"
        "table.setn = function(t, n) end\n"
        "local function absorb()\n"
        "  local t = {}\n"
        "  setmetatable(t, {__index = function(_, _) return function(s, ...) return s end end})\n"
        "  return t\n"
        "end\n"
        "ABSORB = absorb\n"
        "SCREENMAN = absorb()\n"
        "PREFSMAN = { GetPreference = function() return '' end,\n"
        "             SetPreference = function() end }\n"
        "DISPLAY = { GetVendor = function() return '' end,\n"
        "            GetDisplayWidth = function() return __nc_dispw() end,\n"
        "            GetDisplayHeight = function() return __nc_disph() end }\n"
        "Trace = function(v) __nc_trace(tostring(v)) end\n"
        "MESSAGEMAN = { Broadcast = function(self, m) __nc_broadcast(m) end }\n"
        "local playerStats = {\n"
        "  GetActualDancePoints = function() return 0 end,\n"
        "  GetPossibleDancePoints = function() return 1 end,\n"
        "}\n"
        "local stageStats = {\n"
        "  GetPlayerStageStats = function() return playerStats end,\n"
        "}\n"
        "local song = {\n"
        "  GetSongDir = function() return __nc_songdir() end,\n"
        "  GetDisplayFullTitle = function() return '' end,\n"
        "}\n"
        "local songPosition = { GetMusicSeconds = function() return __nc_sec() end }\n"
        "PLAYER_1 = 1\n"
        "PLAYER_2 = 2\n"
        "STATSMAN = { GetCurStageStats = function() return stageStats end }\n"
        "GAMESTATE = {\n"
        "  GetSongBeat = function() return __nc_beat() end,\n"
        "  GetSongBeatVisible = function() return __nc_beat() end,\n"
        "  GetCurMusicSeconds = function() return __nc_sec() end,\n"
        "  GetSongPosition = function() return songPosition end,\n"
        "  GetCurrentSong = function() return song end,\n"
        "  GetPlayerStageStats = function() return playerStats end,\n"
        "  GetCurStageStats = function() return stageStats end,\n"
        "  GetCurrentSteps = function() return absorb() end,\n"
        "  IsPlayerEnabled = function() return true end,\n"
        "  ApplyGameCommand = function(self, command, player) __nc_gamecmd(command, player) end,\n"
        "  SetShaderFlag = function() end,\n"
        "  SetShaderFlagNum = function() end,\n"
        "}\n";
    if (luaL_dostring(L_, boot) != 0) {
        err = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "boot failed";
        lua_close(L_); L_ = nullptr; return false;
    }
    return true;
}

void LuaHost::close() { if (L_) { lua_close(L_); L_ = nullptr; } }

int LuaHost::compileChunk(const std::string& src, const std::string& where,
                          std::string& err) {
    if (!L_) return -1;
    // `%function(self) ... end` -> `return function(self) ... end`
    std::string body = trimws(src);
    if (body.compare(0, 1, "%") == 0) body = body.substr(1);
    const std::string chunk = "return " + body;
    if (luaL_loadbuffer(L_, chunk.c_str(), chunk.size(), where.c_str()) != 0 ||
        lua_pcall(L_, 0, 1, 0) != 0) {
        err = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "compile failed";
        lua_pop(L_, 1);
        return -1;
    }
    return luaL_ref(L_, LUA_REGISTRYINDEX);
}

// ---------------------------------------------------------------------------
// ActorLayer
// ---------------------------------------------------------------------------
namespace {

std::vector<std::string> splitc(const std::string& s, char d) {
    std::vector<std::string> o; size_t a = 0;
    for (;;) { size_t b = s.find(d, a);
        o.push_back(s.substr(a, b == std::string::npos ? b : b - a));
        if (b == std::string::npos) break; a = b + 1; }
    return o;
}

}  // namespace

void ActorLayer::addFolder(const std::string& songDir, const std::string& sub,
                           double startSec, bool foreground) {
    Slot s;
    s.e.startSec = startSec; s.e.dir = songDir + "/" + sub; s.e.foreground = foreground;
    s.tree = std::make_unique<ActorTree>();
    s.tree->setChart(chart_);
    s.tree->setDisplaySize(dispW_, dispH_);
    std::string err;
    if (!s.tree->load(s.e.dir, startSec, err)) { log_.push_back(err); return; }
    for (const std::string& entry : s.tree->log()) log_.push_back(entry);
    trees_.push_back(std::move(s));
}

void ActorLayer::setChart(const Chart* chart) {
    chart_ = chart;
    for (auto& s : trees_) s.tree->setChart(chart);
}

bool ActorLayer::loadFromSm(const std::string& smPath, const std::string& songDir,
                            std::string& err) {
    FILE* f = fopen(smPath.c_str(), "rb");
    if (!f) { err = "cannot open " + smPath; return false; }
    std::string raw;
    { char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) raw.append(b, n); }
    fclose(f);

    // The .sm's own timing, so a change's beat resolves to the same audio
    // seconds the renderer uses. SmTiming ports GetElapsedTimeFromBeat with
    // #STOPS -- the hand-rolled beatToSec that used to live here ignored them,
    // so Saitama2000's FGCHANGES after its beat-224 0.15s stop fired 0.15s
    // early.
    nc::SmTiming timing;
    timing.parse(raw);
    auto beatToSec = [&](double beat) { return timing.beatToSec(beat); };
    auto tagOf = [&](const char* tag, std::string& out) {
        return SmTiming::tagValue(raw, tag, out);
    };

    for (const char* tag : {"BGCHANGES", "FGCHANGES"}) {
        std::string body;
        if (!tagOf(tag, body)) continue;
        const bool fg = (tag[0] == 'F');
        for (const std::string& ent : splitc(body, ',')) {
            const std::string t = trimws(ent);
            if (t.empty()) continue;
            const std::vector<std::string> fld = splitc(t, '=');
            if (fld.size() < 2) continue;
            const std::string file = trimws(fld[1]);
            if (file.empty() || file[0] == '-') continue;      // -nosongbg- etc
            // A folder (no extension) is an actor tree; a file is a still or a
            // movie. In #BGCHANGES those are the Background layer's job (it
            // reads the same .sm), so they are not logged here; #FGCHANGES
            // media has no handler and keeps the warning.
            if (file.find('.') != std::string::npos) {
                if (fg)
                    log_.push_back(std::string(tag) + ": '" + file +
                                   "' is a media file, not an actor folder -- "
                                   "skipped (foreground media is not supported)");
                continue;
            }
            addFolder(songDir, file, beatToSec(atof(trimws(fld[0]).c_str())), fg);
        }
    }
    return true;
}


int ActorLayer::drainLuaMods(ModDoc& doc, ModDoc* doc2, int resolution) {
    int n = 0;
    for (Slot& sl : trees_) {
        if (!sl.tree) continue;
        // The tree's log was merged into ours when it loaded, so anything the
        // drain notes -- unimplemented tokens above all -- would be stranded.
        // Take only what is new. A mod the chart asks for and we silently do
        // not have is the single most misleading thing this can do.
        const size_t before = sl.tree->log().size();
        n += sl.tree->drainLuaMods(doc, doc2, resolution);
        for (size_t i = before; i < sl.tree->log().size(); ++i)
            log_.push_back(sl.tree->log()[i]);
    }
    if (n) {
        char m[128];
        snprintf(m, sizeof m, "%d mod entries from Lua", n);
        log_.push_back(m);
    }
    return n;
}

void ActorLayer::pump(Renderer& R, double sec) {
    // Every tree, not just the ones about to be drawn: the loop that drives a
    // chart lives in one tree and moves actors in all of them. Called from
    // both draw paths because a chart may be foreground-only or
    // background-only; ActorTree::update is idempotent within a frame (its
    // clock only moves forward), so the second call is free.
    for (auto& s : trees_)
        if (s.tree->ok() && sec >= s.e.startSec) {
            // Take whatever the step logged. The tree's log was merged into
            // ours when it LOADED, so anything a command body reports at draw
            // time -- a Lua error above all -- would otherwise be stranded
            // where nothing prints it, and the chart would just silently stop
            // animating. That is how this loop's first failure went unseen.
            const size_t before = s.tree->log().size();
            s.tree->setDisplaySize(R.W, R.H);
            s.tree->update(sec, R.actorBeat());
            for (size_t i = before; i < s.tree->log().size(); ++i)
                log_.push_back(s.tree->log()[i]);
        }
}

void ActorLayer::drawBackground(Renderer& R, double sec) {
    pump(R, sec);
    for (auto& s : trees_)
        if (!s.e.foreground && s.tree->ok() && sec >= s.e.startSec)
            s.tree->draw(R, sec);
}

void ActorLayer::drawForeground(Renderer& R, double sec) {
    pump(R, sec);
    for (auto& s : trees_)
        if (s.e.foreground && s.tree->ok() && sec >= s.e.startSec)
            s.tree->draw(R, sec);
}

}  // namespace nc
