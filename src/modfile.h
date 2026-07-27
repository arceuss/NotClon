// The modchart document: a list of "at tick T, approach P% of <mod> at rate R,
// optionally for L ticks only".
//
// This is OpenITG's model, not a new one. In OITG a modchart is a sequence of
// attacks whose payload is a PlayerOptions string:
//
//     *<approach> <percent> <modname>
//
// and PlayerOptions::Approach() then moves every knob toward its target by
// fDeltaSeconds * speed each frame. `*-1` is the idiom for "snap instantly".
// One entry here is exactly one such token, pinned to a tick instead of a
// second because .chart is tick-based.
//
// An OITG attack is *scoped*, not a keyframe. GameState.cpp:656-657 holds one
// active while `fStartSecond < t < fStartSecond + fSecsRemaining`, and
// RebuildPlayerOptionsFromActiveAttacks (GameState.cpp:1350) rebuilds the knob
// set from m_StoredPlayerOptions and re-applies only the attacks currently on.
// So **a value reverts by itself when its window closes** -- nothing has to
// schedule it back. `ModEntry::len` is that window. Length 0 means "no window",
// i.e. the plain keyframe the format started with, which is still the default.
//
// One consequence worth stating because it is easy to get wrong: the approach
// *rate* reverts with the target. PlayerOptions::Approach reads
// `other.m_Speed<knob>` -- the speed belongs to whatever is being approached --
// so when an interval expires the knob eases back at the underlying entry's
// rate, not at the expiring one's.
//
// The one thing OITG gets for free and we do not: it only ever plays forward,
// so its state is whatever the last frame approached to. NotClon seeks. So
// rebuild() does a prefix scan over the entries once and stores the resolved
// state at every keyframe; evalAt() binary-searches that and approaches for the
// remaining delta. Same numbers, but O(log n) from a cold seek.
#pragma once

#include "chart.h"
#include "mods.h"
#include "modchart.h"

#include <string>
#include <vector>

namespace nc {

// Every knob the renderer actually consumes -- an editor that offers a
// slider which does nothing is worse than one that does not offer it.
enum ModId {
    MOD_TORNADO, MOD_DRUNK, MOD_FLIP, MOD_INVERT, MOD_BEAT,
    MOD_BUMPY, MOD_BUMPYSPEED, MOD_TIPSY, MOD_TIPSYSPEED,
    MOD_BOOMERANG, MOD_EXPAND, MOD_SUDDEN, MOD_HIDDEN, MOD_STEALTH,
    // stealth's partners (ArrowEffects.cpp:470-481)
    MOD_BLINK, MOD_RANDOMVANISH,
    MOD_WAVE, MOD_BOOST,
    MOD_BRAKE,           // boost's partner accel (ArrowEffects.cpp:73-82)
    MOD_SCROLLSPEED,
    // the rotation family (quadUpRot) + tiny's per-note zoom
    MOD_DIZZY, MOD_CONFUSION, MOD_ROLL, MOD_TWIRL, MOD_TINY,
    // receptor fade (ReceptorArrowRow.cpp:40-43); dims the fret stack only
    MOD_DARK,
    MOD_TILT, MOD_MINI, MOD_WAG, MOD_REVERSE, MOD_CENTERED,
    // NOT an ITG mod -- a readability affordance, and the only knob in this
    // enum that is not a port of something. See the comment in renderer.cpp.
    MOD_SWAPTINT,
    // NotITG's `hide` tween-as-mod: "Equivalent to self:hidden(1) ... Any
    // non-zero percentage will enable it." Hides the WHOLE playfield -- board,
    // frets, notes, sustains -- which is what a section wants when the notes
    // are already stealthed and a visible highway would still give the timing
    // away.
    MOD_HIDE,
    // The board only: neck, sidebars, strings, beat lines. Notes and frets
    // stay. This is the --playfield flag as a knob.
    MOD_HIDEBOARD,
    // PIU display mode: gems render as Pump It Up panels (SM5 pump/default
    // noteskin, assets/pump/) instead of CH note sprites. Any nonzero percent
    // switches the art. Sections meant for pump should be CHARTED AS TAP
    // NOTES: pump/default charts hold heads as taps too (NoteSkin.lua:18-19),
    // and sustains keep their CH ribbons under this mode.
    MOD_PIU,
    MOD_MOVEX, MOD_MOVEY, MOD_MOVEZ,
    MOD_ABERRATION, MOD_GLOW, MOD_VIGNETTE, MOD_DESAT, MOD_SHAKE,
    // ITG m_fCover: a black quad over the BACKGROUND, under the notefield, at
    // alpha == cover. BrightnessOverlay::SetActualBrightness
    // (Background.cpp:958-984) is its one consumer in the whole OITG tree. It
    // was a stub until the #BGCHANGES background layer landed, because before
    // that there was nothing for it to draw over; the derivation of the alpha
    // is at COVER_FS in renderer.cpp. (Saitama2000's `-50% cover` is the
    // brightness-1.5 theme-lighting trick AGENTS.md flags as analogue-less.)
    MOD_COVER,
    MOD_COUNT,
    // --- STUBS -------------------------------------------------------------
    // A stub knob parses, stores, saves and shows a value, and drawFrame
    // ignores it. The range exists so a .sm #MODS: list can be imported without
    // silently losing tokens -- an import that drops a third of a modfile on
    // the floor is far worse than one that records it and says the knob does
    // nothing yet. modIsStub() is true for exactly [MOD_STUB_FIRST, MOD_COUNT)
    // and the editor labels them, so a knob left here after being wired for
    // real is a lie. **The range is currently EMPTY** -- cover was the last
    // stub and is wired. Put new stubs immediately before MOD_COUNT and move
    // MOD_STUB_FIRST back to the first of them.
    MOD_STUB_FIRST = MOD_COUNT
};

// --- bg.<name> shader-background knobs --------------------------------------
// A shader .frag's uniforms register as knobs past the fixed enum: slot ids
// MOD_BG_BASE..MOD_SLOTS-1, spelled `bg.<uniform>` for a background layer and
// `fx.<uniform>` for a playfield layer. Both share this one pool -- they are
// the same kind of thing, and the prefix keeps two shaders' `speed` uniforms
// from colliding. They are
// auto-registered on first sight -- by the shader loader OR by a .ncmod parse
// -- so a modchart carrying bg. entries round-trips even where no shader is
// loaded (the editor, --nobg) instead of dropping them as unknown mods. The
// percent column keeps the uniform /100 rule: `bg.rings` is written 400 for
// rings = 4.0 (devdocs/spec/background.md section 6.4, Option A).
static const int MAX_BG_UNIFORMS = 32;
static const int MOD_BG_BASE     = MOD_COUNT;
static const int MOD_SLOTS       = MOD_COUNT + MAX_BG_UNIFORMS;
// ModDoc::bgUsed_ is one `unsigned` bit per slot, shifted in rebuild() and
// read back in Background::draw. Widening the table past the mask makes
// `1u << (mod - MOD_BG_BASE)` undefined behaviour with no diagnostic.
static_assert(MAX_BG_UNIFORMS <= 32, "bgUsed_ is a 32-bit mask");
int modBgSlot(const std::string& fullName);   // "bg.x" -> slot id, -1 if full

const char* modName(int id);
int         modFromName(const std::string& name);   // -1 if unknown
float       modDefault(int id);
// True for a knob that is stored and displayed but not read by drawFrame.
inline bool modIsStub(int id) { return id >= MOD_STUB_FIRST && id < MOD_COUNT; }

struct ModEntry {
    int   tick     = 0;
    int   mod      = MOD_DRUNK;
    float percent  = 0.0f;    // 1.0 == 100%, as OITG stores it
    float approach = 1.0f;    // full swing per second; <= 0 snaps

    // How long this entry stays in force, in ticks. 0 (the default) means
    // "until something else changes the knob" -- the original keyframe
    // behaviour. Anything > 0 makes it an OITG attack: live over the half-open
    // range [tick, tick + len), after which the knob reverts to whatever the
    // entry underneath it says. Ticks rather than seconds for the same reason
    // `tick` is: the entry keeps meaning what it meant if the tempo map moves.
    int   len      = 0;

    // Muting an entry is an authoring affordance, not part of the OITG model:
    // it is how you A/B a change without losing the values you tuned. To turn
    // a mod off *in the chart* you add another entry at 0% -- that is what
    // `*-1 0 drunk` means and what OITG itself does.
    bool  enabled  = true;
};

class ModDoc {
public:
    std::vector<ModEntry> entries;   // kept sorted by tick, then insertion order

    // The song folder this modchart was authored against, so reopening the
    // document reopens the chart with it. Written as a `#chart <path>` line,
    // which is a comment to anything that does not know about it. Empty if the
    // file did not name one.
    std::string chartDir;

    // Shaders this modchart drives, written as `#bgshader` / `#fxshader` /
    // `#fxchain` lines. Same reasoning as chartDir: a .ncmod that names
    // `fx.wobble` is meaningless without the shader declaring `wobble`, so the
    // document has to carry the pointer or reopening it loses the effect and
    // leaves a knob driving nothing.
    //
    // RELATIVE PATHS RESOLVE AGAINST THE .ncmod'S OWN FOLDER, so a modchart
    // and its shaders/ move together as one directory. Absolute paths are
    // taken as-is. Resolution is the caller's job -- ModDoc does no file I/O
    // beyond its own -- so use shaderPaths() rather than reading these raw.
    std::vector<std::string> bgShaders, fxShaders;
    std::string fxChain;

    // The above, made absolute against `dir` (the folder the .ncmod was loaded
    // from). Empty entries are skipped.
    std::vector<std::string> shaderPaths(const std::vector<std::string>& rel,
                                         const std::string& dir) const;

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    // All four mutators re-run rebuild(), so the document is always evaluable.
    void add(const Chart& c, int tick, int mod, float percent, float approach,
             int len = 0);
    void set(const Chart& c, size_t i, const ModEntry& e);
    void erase(const Chart& c, size_t i);
    void clear(const Chart& c);

    void rebuild(const Chart& c);

    // Resolve the whole knob set at an arbitrary tick.
    void evalAt(const Chart& c, double tick, float* out) const;   // out[MOD_SLOTS]

    // Convenience: the same, unpacked into what drawFrame wants. bgOut, when
    // non-null, receives the MAX_BG_UNIFORMS bg.<name> slot values.
    void evalAt(const Chart& c, double tick, Mods& m, PostFx& fx,
                float& mx, float& my, float& mz, float* bgOut = nullptr) const;

    // Which bg slots this document actually drives (bit i = slot MOD_BG_BASE+i).
    // An undriven shader uniform is never set at all, so the shader's own
    // default applies -- evalAt's zeros must not overwrite it.
    unsigned bgUsedMask() const { return bgUsed_; }

private:
    // One snapshot per distinct tick at which the knob set can change -- every
    // entry's tick, plus every interval's expiry, which is just as much a
    // keyframe as its start.
    struct Key {
        int   tick;
        float val[MOD_SLOTS];   // resolved value on arrival at `tick`
        float tgt[MOD_SLOTS];   // targets in force from `tick` onward
        float spd[MOD_SLOTS];
        double sec;
    };
    std::vector<Key> keys_;
    unsigned bgUsed_ = 0;
};

}  // namespace nc
