#include "modfile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nc {

namespace {

struct ModInfo { const char* name; float def; };

// Order must match the ModId enum. NO explicit bound: with MODS[MOD_COUNT] a
// missing row is silently zero-filled at the end, which makes the static_assert
// below compare MOD_COUNT to itself and pass. That is exactly how the `piu` row
// went missing -- every name from MOD_PIU on was off by one, so a .ncmod's
// `glow` drove aberration and `aberration` drove movez.
const ModInfo MODS[] = {
    {"tornado",    0.0f},
    {"drunk",      0.0f},
    {"flip",       0.0f},
    {"invert",     0.0f},
    {"beat",       0.0f},
    {"bumpy",      0.0f},
    {"bumpyspeed", 0.0f},
    {"tipsy",      0.0f},
    {"tipsyspeed", 0.0f},
    {"boomerang",  0.0f},
    {"expand",     0.0f},
    {"sudden",     0.0f},
    {"hidden",     0.0f},
    {"stealth",    0.0f},
    // stealth's partners
    {"blink",        0.0f},
    {"randomvanish", 0.0f},
    {"wave",       0.0f},
    {"boost",      0.0f},
    {"brake",      0.0f},
    // The one knob whose neutral is not zero: 1.0 == 1x.
    {"scrollspeed",1.0f},
    {"drawsizeback", 0.0f},
    // the rotation family + per-note zoom
    {"dizzy",      0.0f},
    {"confusion",  0.0f},
    {"roll",       0.0f},
    {"twirl",      0.0f},
    {"tiny",       0.0f},
    {"dark",       0.0f},
    // hallway = -tilt, distant = +tilt, overhead = 0; the importer maps the
    // three names onto this single signed knob, as PlayerOptions does.
    {"tilt",       0.0f},
    {"mini",       0.0f},
    {"wag",        0.0f},
    {"reverse",    0.0f},
    {"centered",   0.0f},
    {"swaptint",   0.0f},
    {"hide",       0.0f},
    {"hideboard",  0.0f},
    {"piu",        0.0f},
    {"moonscraper",0.0f},
    {"yarg",       0.0f},
    {"suddenoffset", 0.0f},
    {"hiddenoffset", 0.0f},
    {"split",      0.0f},
    {"cross",      0.0f},
    {"alternate",  0.0f},
    {"blind",      0.0f},
    {"drunkperiod",   0.0f},
    {"bumpyperiod",   0.0f},
    {"waveperiod",    0.0f},
    {"tornadoperiod", 0.0f},
    {"confusionoffset",  0.0f},
    {"confusionxoffset", 0.0f},
    {"confusionyoffset", 0.0f},
    {"zigzag",        0.0f},
    {"zigzagperiod",  0.0f},
    {"sawtooth",      0.0f},
    {"sawtoothperiod",0.0f},
    {"square",        0.0f},
    {"squareperiod",  0.0f},
    {"digital",       0.0f},
    {"digitalperiod", 0.0f},
    {"digitalsteps",  0.0f},
    {"tandrunk",      0.0f},
    {"cosec",         0.0f},
    {"beatoffset",    0.0f},
    {"beatmult",      0.0f},
    {"beaty",         0.0f},
    {"beatyoffset",   0.0f},
    {"beatymult",     0.0f},
    {"beatz",         0.0f},
    {"beatzoffset",   0.0f},
    {"beatzmult",     0.0f},
    {"movex0",        0.0f},
    {"movex1",        0.0f},
    {"movex2",        0.0f},
    {"movex3",        0.0f},
    {"movex4",        0.0f},
    {"movey0",        0.0f},
    {"movey1",        0.0f},
    {"movey2",        0.0f},
    {"movey3",        0.0f},
    {"movey4",        0.0f},
    {"movez0",        0.0f},
    {"movez1",        0.0f},
    {"movez2",        0.0f},
    {"movez3",        0.0f},
    {"movez4",        0.0f},
    {"tiny0",         0.0f},
    {"tiny1",         0.0f},
    {"tiny2",         0.0f},
    {"tiny3",         0.0f},
    {"tiny4",         0.0f},
    {"zigzagz",       0.0f},
    {"zigzagzperiod", 0.0f},
    {"sawtoothz",     0.0f},
    {"sawtoothzperiod", 0.0f},
    {"digitalz",      0.0f},
    {"digitalzperiod",0.0f},
    {"digitalzoffset",0.0f},
    {"digitalzsteps", 0.0f},
    {"dizzyholds",    0.0f},
    {"movex",      0.0f},
    {"movey",      0.0f},
    {"movez",      0.0f},
    // An empty document renders CLEAN. The post chain used to default to
    // aberration 5% / glow 55% / vignette 35%, which is a look, not a neutral
    // starting point -- it fogs the whole frame before the author has asked
    // for anything. Post is opt-in like every other knob now.
    // (src/modchart.h's PostFx struct still carries those numbers; that is the
    // hardcoded R.E.M. III chart's own baseline, not a default, and changing
    // it would alter an authored modchart.)
    {"aberration", 0.0f},
    {"glow",       0.0f},
    {"vignette",   0.0f},
    {"desat",      0.0f},
    {"shake",      0.0f},
    {"cover",      0.0f},
};
static_assert(sizeof(MODS) / sizeof(MODS[0]) == MOD_COUNT,
              "MODS[] must stay in ModId order and cover every knob");

// OITG's fapproach: move val toward other by at most toMove.
inline void fapproach(float& val, float other, float toMove) {
    if (val == other) return;
    float delta = other - val;
    float sign = delta > 0 ? 1.0f : -1.0f;
    float move = sign * toMove;
    if (fabsf(move) > fabsf(delta)) val = other; else val += move;
}

}  // namespace

// The bg.<name> registry, MOD_BG_BASE-relative. Full "bg.x" spellings so
// modName can hand back exactly what the file said. Process-wide on purpose:
// the shader loader and the .ncmod parser must agree on slot numbering, and
// both binaries hold exactly one of each.
static std::vector<std::string> g_bgNames;

int modBgSlot(const std::string& fullName) {
    for (size_t i = 0; i < g_bgNames.size(); ++i)
        if (g_bgNames[i] == fullName) return MOD_BG_BASE + int(i);
    if (int(g_bgNames.size()) >= MAX_BG_UNIFORMS) return -1;
    g_bgNames.push_back(fullName);
    return MOD_BG_BASE + int(g_bgNames.size()) - 1;
}

// How many bg./fx. slots have actually been registered by a loaded shader.
// The editor needs this to offer them: a knob nothing can select is a knob
// that does not exist as far as the UI is concerned.
int modBgCount() { return int(g_bgNames.size()); }

const char* modName(int id) {
    if (id >= MOD_BG_BASE && id < MOD_BG_BASE + int(g_bgNames.size()))
        return g_bgNames[id - MOD_BG_BASE].c_str();
    return (id >= 0 && id < MOD_COUNT) ? MODS[id].name : "?";
}

float modDefault(int id) {
    // bg slots default to 0 -- but note an undriven slot is never handed to
    // the shader at all (ModDoc::bgUsedMask), so this 0 only feeds the eval
    // arrays, not a uniform.
    return (id >= 0 && id < MOD_COUNT) ? MODS[id].def : 0.0f;
}

int modFromName(const std::string& name) {
    for (int i = 0; i < MOD_COUNT; ++i)
        if (name == MODS[i].name) return i;
    if (name == "beatx")       return MOD_BEAT;
    if (name == "beatxoffset") return MOD_BEATOFFSET;
    if (name == "beatxmult")   return MOD_BEATMULT;
    // Shader knobs: bg.<name> for a background layer, fx.<name> for a
    // playfield layer. Auto-registered on first sight from either side (the
    // shader loader or a .ncmod parse) so a modchart carrying them round-trips
    // even where no shader is loaded.
    if (name.size() > 3 && (name.compare(0, 3, "bg.") == 0 ||
                            name.compare(0, 3, "fx.") == 0))
        return modBgSlot(name);
    return -1;
}

// ---------------------------------------------------------------------------
void ModDoc::rebuild(const Chart& c) {
    keys_.clear();
    bgUsed_ = 0;
    // The player filter: an entry attacking the other player simply does not
    // exist for this doc's timeline. Two fields = two docs over one entry
    // list, differing only in forPlayer.
    auto takes = [this](const ModEntry& e) {
        return e.enabled && (e.pn == 0 || forPlayer == 0 || e.pn == forPlayer);
    };
    for (const ModEntry& e : entries)
        if (takes(e) && e.mod >= MOD_BG_BASE && e.mod < MOD_SLOTS)
            bgUsed_ |= 1u << (e.mod - MOD_BG_BASE);
    std::stable_sort(entries.begin(), entries.end(),
                     [](const ModEntry& a, const ModEntry& b) { return a.tick < b.tick; });
    if (entries.empty()) return;

    // The knob set can change at an entry's tick and again when a scoped entry
    // expires, so both are keyframes. Collect and dedupe the event ticks up
    // front; with no scoped entries this is exactly the old list of entry
    // ticks and everything below reduces to the old prefix scan.
    std::vector<int> ev;
    ev.reserve(entries.size() * 2);
    for (const ModEntry& e : entries) {
        if (!takes(e)) continue;
        ev.push_back(e.tick);
        if (e.len > 0) ev.push_back(e.tick + e.len);
    }
    if (ev.empty()) return;
    std::sort(ev.begin(), ev.end());
    ev.erase(std::unique(ev.begin(), ev.end()), ev.end());

    // `base` is what the knob reverts to: the persistent (len 0) entries,
    // prefix-scanned as before. `live` is the stack of scoped entries covering
    // this tick, in start order -- the top wins, which is
    // RebuildPlayerOptionsFromActiveAttacks re-applying each active attack's
    // mod string over the stored options in launch order (GameState.cpp:1356).
    // MOD_SLOTS, not MOD_COUNT: the bg slots ride the same machinery. The
    // extra slots hold zeros unless a bg. entry drives them, and a zero
    // approaching a zero is a no-op, so slots MOD_TORNADO..MOD_SHAKE come out
    // bit-identical to the un-widened code (the --nomods control triple is the
    // proof). MODS[] itself stays MOD_COUNT-sized; slots past it default 0.
    float val[MOD_SLOTS], tgt[MOD_SLOTS], spd[MOD_SLOTS];
    float base[MOD_SLOTS], baseSpd[MOD_SLOTS];
    for (int i = 0; i < MOD_SLOTS; ++i) {
        val[i] = tgt[i] = base[i] = modDefault(i);
        spd[i] = baseSpd[i] = 1.0f;
    }
    std::vector<size_t> live[MOD_SLOTS];
    double prevSec = c.tickToSec(0);

    size_t i = 0;   // next entry to start; entries are sorted by tick
    for (const int tick : ev) {
        const double sec = c.tickToSec(tick);
        const float dt = float(sec - prevSec);

        // Approach under the target and rate that were in force since the
        // previous event.
        for (int k = 0; k < MOD_SLOTS; ++k) fapproach(val[k], tgt[k], dt * spd[k]);

        // Expire first, so back-to-back intervals hand over cleanly: the range
        // is half-open, and an entry starting exactly where another ends must
        // win. This is the whole point of the .sm alternation idiom, where a
        // 0.0625s pulse is immediately followed by its opposite.
        for (int k = 0; k < MOD_SLOTS; ++k) {
            std::vector<size_t>& L = live[k];
            for (size_t j = 0; j < L.size();) {
                const ModEntry& e = entries[L[j]];
                if (e.tick + e.len <= tick) L.erase(L.begin() + j); else ++j;
            }
        }

        // Every entry sharing this tick lands at once, so the order two
        // simultaneous entries were typed in does not change the result unless
        // they touch the same knob (in which case the later one wins, same as
        // OITG parsing a mod string left to right). `<=` rather than `==`
        // because a muted entry contributes no event tick, and the cursor
        // still has to walk past it.
        while (i < entries.size() && entries[i].tick <= tick) {
            const ModEntry& e = entries[i];
            if (!takes(e)) { ++i; continue; }
            if (e.len > 0) {
                live[e.mod].push_back(i);
            } else {
                base[e.mod]    = e.percent;
                baseSpd[e.mod] = e.approach;
            }
            ++i;
        }

        for (int k = 0; k < MOD_SLOTS; ++k) {
            if (live[k].empty()) {
                tgt[k] = base[k];    spd[k] = baseSpd[k];
            } else {
                const ModEntry& e = entries[live[k].back()];
                tgt[k] = e.percent;  spd[k] = e.approach;
            }
            // *-1 snaps. Applied to the resolved target rather than only to an
            // arriving entry, so a knob whose interval expires back onto a
            // snapping base snaps back instead of holding a stale value. With
            // no scoped entries this is a no-op: val already equals tgt.
            if (spd[k] <= 0.0f) val[k] = tgt[k];
        }

        Key k;
        k.tick = tick;
        k.sec  = sec;
        memcpy(k.val, val, sizeof val);
        memcpy(k.tgt, tgt, sizeof tgt);
        memcpy(k.spd, spd, sizeof spd);
        keys_.push_back(k);
        prevSec = sec;
    }
}

void ModDoc::evalAt(const Chart& c, double tick, float* out) const {
    for (int i = 0; i < MOD_SLOTS; ++i) out[i] = modDefault(i);
    if (keys_.empty()) return;
    if (tick < keys_.front().tick) return;

    size_t lo = 0, hi = keys_.size();
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (keys_[m].tick <= tick) lo = m + 1; else hi = m;
    }
    const Key& k = keys_[lo - 1];
    const float dt = float(c.tickToSec(tick) - k.sec);
    for (int i = 0; i < MOD_SLOTS; ++i) {
        out[i] = k.val[i];
        if (k.spd[i] <= 0.0f) out[i] = k.tgt[i];
        else fapproach(out[i], k.tgt[i], dt * k.spd[i]);
    }
}

void ModDoc::evalAt(const Chart& c, double tick, Mods& m, PostFx& fx,
                    float& mx, float& my, float& mz, float* bgOut) const {
    float v[MOD_SLOTS];
    evalAt(c, tick, v);
    if (bgOut) memcpy(bgOut, v + MOD_BG_BASE, MAX_BG_UNIFORMS * sizeof(float));
    modValuesToState(v, float(tick / double(c.resolution)), m, fx, mx, my, mz);
}

void modValuesToState(const float* v, float beat, Mods& m, PostFx& fx,
                      float& mx, float& my, float& mz) {
    m = Mods{};
    m.tornado   = v[MOD_TORNADO];
    m.drunk     = v[MOD_DRUNK];
    m.flip      = v[MOD_FLIP];
    m.invert    = v[MOD_INVERT];
    m.beat      = v[MOD_BEAT];
    m.bumpy     = v[MOD_BUMPY];
    m.tipsy     = v[MOD_TIPSY];
    m.boomerang = v[MOD_BOOMERANG];
    m.expand    = v[MOD_EXPAND];
    m.sudden    = v[MOD_SUDDEN];
    m.hidden    = v[MOD_HIDDEN];
    m.stealth   = v[MOD_STEALTH];
    m.blink        = v[MOD_BLINK];
    m.randomvanish = v[MOD_RANDOMVANISH];
    m.wave      = v[MOD_WAVE];
    m.boost     = v[MOD_BOOST];
    m.brake     = v[MOD_BRAKE];
    m.scrollspeed = v[MOD_SCROLLSPEED];
    m.drawSizeBack = v[MOD_DRAWSIZEBACK];
    m.dizzy     = v[MOD_DIZZY];
    m.confusion = v[MOD_CONFUSION];
    m.roll      = v[MOD_ROLL];
    m.twirl     = v[MOD_TWIRL];
    m.tiny      = v[MOD_TINY];
    m.dark      = v[MOD_DARK];
    m.tilt      = v[MOD_TILT];
    m.mini      = v[MOD_MINI];
    m.wag       = v[MOD_WAG];
    m.reverse   = v[MOD_REVERSE];
    m.centered  = v[MOD_CENTERED];
    m.swaptint  = v[MOD_SWAPTINT];
    m.hide      = v[MOD_HIDE];
    m.hideboard = v[MOD_HIDEBOARD];
    m.piu       = v[MOD_PIU];
    m.moonscraper = v[MOD_MOONSCRAPER];
    m.yarg      = v[MOD_YARG];
    m.suddenOffset = v[MOD_SUDDENOFFSET];
    m.hiddenOffset = v[MOD_HIDDENOFFSET];
    m.split     = v[MOD_SPLIT];
    m.drunkPeriod    = v[MOD_DRUNKPERIOD];
    m.bumpyPeriod    = v[MOD_BUMPYPERIOD];
    m.wavePeriod     = v[MOD_WAVEPERIOD];
    m.tornadoPeriod  = v[MOD_TORNADOPERIOD];
    m.confusionOffset  = v[MOD_CONFUSIONOFFSET];
    m.confusionXOffset = v[MOD_CONFUSIONXOFFSET];
    m.confusionYOffset = v[MOD_CONFUSIONYOFFSET];
    m.zigzag    = v[MOD_ZIGZAG];    m.zigzagPeriod   = v[MOD_ZIGZAGPERIOD];
    m.sawtooth  = v[MOD_SAWTOOTH];  m.sawtoothPeriod = v[MOD_SAWTOOTHPERIOD];
    m.square    = v[MOD_SQUARE];    m.squarePeriod   = v[MOD_SQUAREPERIOD];
    m.digital   = v[MOD_DIGITAL];   m.digitalPeriod  = v[MOD_DIGITALPERIOD];
    m.digitalSteps = v[MOD_DIGITALSTEPS];
    m.tandrunk  = v[MOD_TANDRUNK];
    m.cosecant  = v[MOD_COSEC];
    m.beatOffset = v[MOD_BEATOFFSET];  m.beatMult  = v[MOD_BEATMULT];
    m.beaty = v[MOD_BEATY]; m.beatyOffset = v[MOD_BEATYOFFSET]; m.beatyMult = v[MOD_BEATYMULT];
    m.beatz = v[MOD_BEATZ]; m.beatzOffset = v[MOD_BEATZOFFSET]; m.beatzMult = v[MOD_BEATZMULT];
    static_assert(MOD_MOVEX4 - MOD_MOVEX0 == NUM_LANES - 1,
                  "movex knobs must be contiguous, one per lane");
    static_assert(MOD_MOVEY4 - MOD_MOVEY0 == NUM_LANES - 1,
                  "movey knobs must be contiguous, one per lane");
    static_assert(MOD_MOVEZ4 - MOD_MOVEZ0 == NUM_LANES - 1,
                  "movez knobs must be contiguous, one per lane");
    static_assert(MOD_TINY4 - MOD_TINY0 == NUM_LANES - 1,
                  "tiny knobs must be contiguous, one per lane");
    for (int c = 0; c < NUM_LANES; ++c) {
        m.movexCol[c] = v[MOD_MOVEX0 + c];
        m.moveyCol[c] = v[MOD_MOVEY0 + c];
        m.movezCol[c] = v[MOD_MOVEZ0 + c];
        m.tinyCol[c] = v[MOD_TINY0 + c];
    }
    m.zigzagZ = v[MOD_ZIGZAGZ]; m.zigzagZPeriod = v[MOD_ZIGZAGZPERIOD];
    m.sawtoothZ = v[MOD_SAWTOOTHZ]; m.sawtoothZPeriod = v[MOD_SAWTOOTHZPERIOD];
    m.digitalZ = v[MOD_DIGITALZ]; m.digitalZPeriod = v[MOD_DIGITALZPERIOD];
    m.digitalZOffset = v[MOD_DIGITALZOFFSET]; m.digitalZSteps = v[MOD_DIGITALZSTEPS];
    m.dizzyHolds = v[MOD_DIZZYHOLDS];
    m.cross     = v[MOD_CROSS];
    m.alternate = v[MOD_ALTERNATE];
    m.cover     = v[MOD_COVER];
    // The two *speed knobs are phase rates, not amplitudes: they say how fast
    // the pattern crawls along the highway, so they are integrated against the
    // beat rather than handed to the formula directly.
    m.bumpyOffset = beat * v[MOD_BUMPYSPEED];
    m.tipsyOffset = beat * v[MOD_TIPSYSPEED];

    fx.aberration = v[MOD_ABERRATION];
    fx.glow       = v[MOD_GLOW];
    fx.vignette   = v[MOD_VIGNETTE];
    fx.desat      = v[MOD_DESAT];
    fx.shake      = v[MOD_SHAKE];

    mx = v[MOD_MOVEX]; my = v[MOD_MOVEY]; mz = v[MOD_MOVEZ];
}

// ---------------------------------------------------------------------------
void ModDoc::add(const Chart& c, int tick, int mod, float percent, float approach,
                 int len, int pn) {
    ModEntry e;
    e.tick = tick; e.mod = mod; e.percent = percent; e.approach = approach;
    e.len = len; e.pn = pn;
    entries.push_back(e);
    rebuild(c);
}

void ModDoc::set(const Chart& c, size_t i, const ModEntry& e) {
    if (i < entries.size()) { entries[i] = e; rebuild(c); }
}

void ModDoc::erase(const Chart& c, size_t i) {
    if (i < entries.size()) { entries.erase(entries.begin() + i); rebuild(c); }
}

void ModDoc::clear(const Chart& c) { entries.clear(); rebuild(c); }

// ---------------------------------------------------------------------------
// Text format. One entry per line, in the same word order as an OITG mod
// string so it reads the way a charter already thinks:
//
//     <tick>  *<approach>  <percent>  <modname>  [len=<ticks>]
//
// `len=` is optional and is only written when nonzero, so a file with no
// scoped entries round-trips exactly as it did before intervals existed. It is
// spelled out rather than positional because a bare fifth number reads as
// nothing in particular, and because it matches the LEN= a .sm #MODS: line
// already uses.
//
// Comments start with '#'. The header lines are informational only -- the tick
// is authoritative, so the file stays correct if the chart's tempo map moves.
// ---------------------------------------------------------------------------
// Relative entries resolve against the .ncmod's own folder; absolute ones are
// left alone. "X:\..." and "/..." are the two absolute spellings that matter
// here, matching how the actor and shader loaders already test a path.
std::vector<std::string> ModDoc::shaderPaths(const std::vector<std::string>& rel,
                                             const std::string& dir) const {
    std::vector<std::string> out;
    for (const std::string& p : rel) {
        if (p.empty()) continue;
        const bool abs = p.size() > 1 && (p[0] == '/' || p[0] == '\\' || p[1] == ':');
        out.push_back(abs || dir.empty() ? p : dir + "/" + p);
    }
    return out;
}

bool ModDoc::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    entries.clear();
    keys_.clear();
    chartDir.clear();

    char line[512];
    int lineNo = 0;
    while (fgets(line, sizeof line, f)) {
        ++lineNo;
        std::string s = nc_trim(line);
        if (s.empty()) continue;
        // A muted entry is written as `#! ...` so that anything else reading
        // the file just sees a comment.
        bool enabled = true;
        if (s.compare(0, 2, "#!") == 0) { enabled = false; s = nc_trim(s.substr(2)); }
        else if (s.compare(0, 7, "#chart ") == 0) { chartDir = nc_trim(s.substr(7)); continue; }
        // Shader pointers. Spelled as comments so any other reader skips them,
        // exactly like #chart.
        else if (s.compare(0, 10, "#bgshader ") == 0) {
            bgShaders.push_back(nc_trim(s.substr(10))); continue;
        }
        else if (s.compare(0, 10, "#fxshader ") == 0) {
            fxShaders.push_back(nc_trim(s.substr(10))); continue;
        }
        else if (s.compare(0, 9, "#fxchain ") == 0) {
            fxChain = nc_trim(s.substr(9)); continue;
        }
        else if (s.compare(0, 9, "#players ") == 0) {
            players = atoi(s.c_str() + 9) >= 2 ? 2 : 1;
            continue;
        }
        else if (s[0] == '#') continue;

        int tick = 0;
        char star[64] = {0}, name[128] = {0}, lenTok[64] = {0}, pnTok[64] = {0};
        float percent = 0.0f;
        const int n = sscanf(s.c_str(), "%d %63s %f %127s %63s %63s",
                             &tick, star, &percent, name, lenTok, pnTok);
        if (n < 4) {
            fprintf(stderr, "%s:%d: cannot parse '%s'\n", path.c_str(), lineNo, s.c_str());
            continue;
        }
        const int id = modFromName(name);
        if (id < 0) {
            fprintf(stderr, "%s:%d: unknown mod '%s'\n", path.c_str(), lineNo, name);
            continue;
        }
        int len = 0, pn = 0;
        // len= and pn= are both optional and order-free.
        for (const char* tok : {lenTok, pnTok}) {
            if (!tok[0]) continue;
            if      (strncmp(tok, "len=", 4) == 0) len = atoi(tok + 4);
            else if (strncmp(tok, "pn=", 3) == 0)  pn  = atoi(tok + 3);
            else fprintf(stderr, "%s:%d: ignoring trailing '%s'"
                         " (expected len=<ticks> or pn=<player>)%c",
                         path.c_str(), lineNo, tok, 10);
        }
        if (pn < 0 || pn > 2) pn = 0;
        (void)n;
        const float approach = (star[0] == '*') ? float(atof(star + 1)) : float(atof(star));
        ModEntry e;
        e.tick = tick; e.mod = id; e.percent = percent / 100.0f;
        e.approach = approach; e.len = len < 0 ? 0 : len; e.enabled = enabled;
        e.pn = pn;
        entries.push_back(e);
    }
    fclose(f);
    return true;
}

bool ModDoc::save(const std::string& path) const {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    fprintf(f, "# NotClon modchart%c", 10);
    // Reopening the modchart reopens its chart. Spelled as a comment so any
    // other reader just skips it.
    if (!chartDir.empty()) fprintf(f, "#chart %s%c", chartDir.c_str(), 10);
    for (const auto& p : bgShaders) fprintf(f, "#bgshader %s%c", p.c_str(), 10);
    for (const auto& p : fxShaders) fprintf(f, "#fxshader %s%c", p.c_str(), 10);
    if (!fxChain.empty()) fprintf(f, "#fxchain %s%c", fxChain.c_str(), 10);
    if (players == 2) fprintf(f, "#players 2%c", 10);
    fprintf(f, "# tick   *approach   percent   mod%c%c", 10, 10);
    // The "#! " prefix is folded into the tick field's width so muted lines
    // stay in the same columns as live ones.
    for (const auto& e : entries) {
        fprintf(f, "%s%-*d *%-8g %-9g %s", e.enabled ? "" : "#! ",
                e.enabled ? 8 : 5, e.tick, e.approach, e.percent * 100.0f,
                modName(e.mod));
        if (e.len > 0) fprintf(f, "   len=%d", e.len);
        if (e.pn != 0) fprintf(f, "   pn=%d", e.pn);
        fprintf(f, "%c", 10);
    }
    fclose(f);
    return true;
}

}  // namespace nc
