// NotClon -- offline Clone Hero chart renderer.
//
// This file is only the offline driver: parse argv, create a hidden window +
// WGL context, ask the shared core (renderer.h) for one frame at a time, and
// pipe raw RGB straight into ffmpeg. Nothing is screen-recorded and nothing
// runs in real time, so a 4K 60fps render is just a slower loop, not a
// dropped-frame problem.
//
// Everything about what a frame *looks* like lives in renderer.cpp, so
// notclon-editor.exe shows exactly what this writes to disk.

#include "renderer.h"
#include "smimport.h"
#include "actor.h"
#include "background.h"
#include "stems.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static double audioDuration(const std::string& path) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "ffprobe -v error -show_entries format=duration "
             "-of default=noprint_wrappers=1:nokey=1 \"%s\" 2>NUL",
             path.c_str());
    FILE* pipe = _popen(cmd, "r");
    if (!pipe) return 0.0;
    char line[128] = {};
    const double duration = fgets(line, sizeof line, pipe) ? atof(line) : 0.0;
    const int status = _pclose(pipe);
    return status == 0 && std::isfinite(duration) && duration > 0.0
         ? duration : 0.0;
}

static void usage() {
    printf(
"NotClon -- offline Clone Hero chart renderer.\n"
"\n"
"  notclon --dir <song folder> [options]\n"
"\n"
"A song folder holds notes.chart plus its audio, named by CH's stem convention\n"
"(guitar/bass/rhythm/vocals*/drums*/keys/song/crowd .{ogg,mp3,wav}); every stem\n"
"found is mixed, exactly as CH layers them. MusicStream is a fallback only.\n"
"\n"
"Input\n"
"  --dir <path>          song folder, or the notes.chart inside it (required)\n"
"  --mods <f.ncmod>      modchart to render; omitted uses the hardcoded one\n"
"  --nomods              ignore the modchart entirely\n"
"  --randmods            generate a random modchart (motion mods only, at\n"
"                        -100%..100%, re-rolled at each chart section)\n"
"  --randengine          generate a random ENGINE VISUAL modchart: one of\n"
"                        CH / piu / moonscraper / yarg / gh3 / taiko / bms\n"
"                        per chart section, always 100%, never two at once\n"
"  --randseed <n>        seed for --randmods/--randengine; same seed = same\n"
"                        modchart\n"
"  --randcount <n>       how many mods at once (default 3)\n"
"  --actor <sub[@beat]>  load <dir>/<sub>/default.lua (or legacy XML) as a\n"
"                        foreground tree at <beat> (default 0). Repeatable.\n"
"                        With none\n"
"                        given, a .sm beside the chart supplies #FG/#BGCHANGES.\n"
"  --noactors            skip the actor layer entirely\n"
"  --bgshader <f.frag>   add a GLSL shader background layer (OpenITG uniform\n"
"                        conventions; uniforms register as bg.<name> knobs a\n"
"                        .ncmod can drive). Repeatable; drawn over .sm media.\n"
"  --fxshader <f.frag>   add a GLSL PLAYFIELD shader: it runs over the\n"
"                        rendered frame, so it can warp or fold the\n"
"                        playfield itself rather than draw behind it.\n"
"                        Uniforms register as fx.<name> knobs. Repeatable.\n"
"  --fxchain <f.ncfx>    run a MULTI-PASS chain over the frame: named\n"
"                        buffers, one shader per pass, and buffers that\n"
"                        persist between frames -- so a pass can read its\n"
"                        own previous output (feedback).\n"
"  --nobg                skip the background layer (a .sm beside the chart\n"
"                        supplies #BGCHANGES stills/movies automatically)\n"
"  --bgscale <n>         movie decode scale, 0..1 (default 0.5 -- the board\n"
"                        covers the centre and post blurs the rest)\n"
"\n"
"Output\n"
"  --out <file.mp4>      output file (default out.mp4)\n"
"  --from/--to <beat>    beat range (default: whole chart)\n"
"  --w/--h/--fps <n>     size and frame rate (default 1920 1080 60)\n"
"  --enc x264|nvenc|hevc|av1    encoder (default x264)\n"
"  --preview <beat>      render one frame to preview.png and exit\n"
"  --bench               encode to null, print a per-stage timing breakdown\n"
"\n"
"Look\n"
"  --speed <n>           note speed, world units/sec (CH default 10)\n"
"  --playfield           frets and notes only, no board\n"
"  --nopost              disable the post chain\n"
"  --nobot               no autoplay; notes fly past the strike line\n"
"  --px/--py/--pz <n>    rigid offset of the whole playfield\n"
"\n"
"Conversion (runs headless, then exits)\n"
"  --import-sm <f.sm>    StepMania -> notes.chart + <name>.ncmod, written\n"
"                        into --dir. Needs --dir.\n"
"  --sm-diff <n>         which #NOTES block (default: hardest dance-single)\n"
"  --export-modchart <f.ncmod>\n"
"                        sample the hardcoded modchart.h into a .ncmod.\n"
"                        Needs --dir for the tempo map.\n"
"  --export-step <beats> sampling grid for the above (default 4)\n"
"  --dump-mods <f.ncmod> run the adjacent or --actor Lua/XML modfile and\n"
"                        dump every live song PlayerOptions change, rounded\n"
"                        to the nearest chart tick. Needs --dir.\n"
"\n"
"Examples\n"
"  notclon --dir \"charts/REM III\" --from 232 --to 264 --enc av1 --out clip.mp4\n"
"  notclon --dir \"charts/REM III\" --preview 240.05 --w 640 --h 360\n"
"  notclon --import-sm \"charts/My Song/song.sm\" --dir \"charts/My Song\"\n");
}

// --randmods -- a procedurally generated modchart.
//
// OpenITG has a `random` mod token (PlayerOptions.cpp:360 dispatching to :455
// ChooseRandomModifiers) and this is its descendant, with three deliberate
// deviations, each for a reason:
//
//   * OITG rolls ONCE, at the start, and every knob it picks lands on exactly
//     1.0. That is a single static combination for a whole song. This re-rolls
//     at every section so a render actually goes somewhere.
//   * Values span -100%..+100% rather than a flat 1.0. Every knob in the pool
//     is signed and the negative half is half the interesting shapes -- a
//     -60% tilt rears the neck up where +60% lays it down.
//   * The pool is curated rather than "any accel or any effect" (see below).
//
// Deterministic: same seed, same modchart, on any machine. xorshift32 rather
// than rand() precisely because rand() differs across CRTs, and this project's
// premise is that a render is reproducible.
static nc::ModDoc randomModchart(const nc::Chart& chart, unsigned seed,
                                 int perRoll) {
    unsigned s = seed ? seed : 0x9E3779B9u;
    auto next = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    auto uni  = [&]() { return float(next() & 0xFFFFFF) / float(0xFFFFFF); };

    // Playfield mods -- motion, geometry AND appearance. Only four things are
    // excluded, each because a roll of it produces nothing worth watching
    // rather than because it is "too much":
    //   piu                     not a mod, and a surprise
    //   aberration/glow/vignette/desat/shake
    //                           the post chain, not the playfield
    //   movex/movey/movez       rigid placement of the whole assembly
    //   scrollspeed             its neutral is 1.0, not 0, so a -100..100 roll
    //                           is meaningless and 0 would freeze the chart
    //   hide                    renders literally nothing for a whole section
    //   cover                   a stub; does nothing yet
    //   swaptint                a readability aid, not a look
    static const int POOL[] = {
        nc::MOD_TORNADO, nc::MOD_DRUNK, nc::MOD_FLIP, nc::MOD_INVERT,
        nc::MOD_BEAT, nc::MOD_BUMPY, nc::MOD_TIPSY, nc::MOD_BOOMERANG,
        nc::MOD_EXPAND, nc::MOD_WAVE, nc::MOD_BOOST, nc::MOD_BRAKE,
        nc::MOD_DIZZY, nc::MOD_CONFUSION, nc::MOD_ROLL, nc::MOD_TWIRL,
        nc::MOD_TINY, nc::MOD_MINI, nc::MOD_TILT, nc::MOD_WAG,
        nc::MOD_REVERSE, nc::MOD_CENTERED,
        // The appearance family. ITG's own random rolls these too
        // (ChooseRandomModifiers picks hidden/sudden at :469-473).
        nc::MOD_STEALTH, nc::MOD_HIDDEN, nc::MOD_SUDDEN, nc::MOD_BLINK,
        nc::MOD_DARK, nc::MOD_RANDOMVANISH,
    };
    const int NPOOL = int(sizeof(POOL) / sizeof(POOL[0]));

    // Appearance knobs are rolled 0..+100% rather than -100..+100%. Their
    // formulas only ever SUBTRACT visibility -- ArrowEffects.cpp:468-469 is
    // `fVisibleAdjust -= stealth`, and sudden/hidden clamp their term into
    // [-1, 0] -- so a negative percent adds visibility to something already
    // fully visible and clamps back to 1. Rolling them signed would silently
    // waste half the draws on a no-op.
    auto positiveOnly = [](int id) {
        return id == nc::MOD_STEALTH || id == nc::MOD_HIDDEN ||
               id == nc::MOD_SUDDEN  || id == nc::MOD_BLINK  ||
               id == nc::MOD_DARK    || id == nc::MOD_RANDOMVANISH;
    };

    // Roll on the chart's own section markers -- they are where the music
    // changes, so the mods change with it. A chart with no sections falls back
    // to a fixed 16-beat grid.
    std::vector<double> rolls;
    for (const auto& sec : chart.sections) rolls.push_back(sec.beat);
    if (rolls.size() < 2) {
        rolls.clear();
        const double last = chart.notes.empty() ? 64.0 : chart.notes.back().beat;
        for (double b = 0.0; b < last; b += 16.0) rolls.push_back(b);
    }

    nc::ModDoc doc;
    std::vector<int> prev;
    for (double b : rolls) {
        const int tick = int(b * chart.resolution + 0.5);
        std::vector<int> cur;
        for (int k = 0; k < perRoll; ++k) {
            const int id = POOL[next() % unsigned(NPOOL)];
            if (std::find(cur.begin(), cur.end(), id) == cur.end()) cur.push_back(id);
        }
        // Retire anything the previous roll turned on that this one does not
        // want. Without this the knobs accumulate into undifferentiated mush.
        for (int id : prev)
            if (std::find(cur.begin(), cur.end(), id) == cur.end())
                doc.entries.push_back(nc::ModEntry{tick, id, 0.0f, 0.6f, 0, 0, true});
        for (int id : cur) {
            const float v = positiveOnly(id) ? uni()      //    0% .. +100%
                                             : uni() * 2.0f - 1.0f;  // -100..+100
            const float ap = 0.35f + uni();               // eased, not snapped
            doc.entries.push_back(nc::ModEntry{tick, id, v, ap, 0, 0, true});
        }
        prev = cur;
    }
    doc.rebuild(chart);
    return doc;
}

// --randengine -- one engine visual per section, always at 100%.
//
// A different animal from --randmods and deliberately so. That one rolls the
// MOTION knobs, which are continuous, signed and stack: a section can hold six
// of them at partial strength and the interesting part is the combination.
// The engine visuals are none of those things. They are mutually exclusive
// whole-field swaps -- piu, moonscraper, yarg, gh3, taiko each replace the
// playfield outright -- so there is nothing to combine and nothing a partial
// value buys. 50% taiko is a crossfade, which is a real feature when a modchart
// asks for it and just a smeared half-swap when a sampler rolls it by accident.
// So: exactly one at a time, snapped, at 100%, changing on section boundaries.
//
// CH IS IN THE POOL. "No engine mod" is the stock field, which is as much a
// member of the set as the other five -- a sampler that cannot come home to CH
// is not sampling the whole set.
//
// Immediate repeats are re-rolled. Every section boundary is meant to be a
// visible change; a roll that lands on what is already showing spends a whole
// section saying nothing.
//
// Deterministic, same xorshift32 as randomModchart and for the same reason:
// same seed, same sequence, on any machine.
static nc::ModDoc randomEngineModchart(const nc::Chart& chart, unsigned seed) {
    unsigned s = seed ? seed : 0x9E3779B9u;
    auto next = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };

    static const int POOL[] = {
        -1,                       // CH -- the stock field, no knob at all
        nc::MOD_PIU, nc::MOD_MOONSCRAPER, nc::MOD_YARG,
        nc::MOD_GH3, nc::MOD_TAIKO, nc::MOD_BMS,
    };
    const int NPOOL = int(sizeof(POOL) / sizeof(POOL[0]));

    // Section markers, exactly as --randmods does it -- they are where the
    // music changes, so the field changes with it.
    std::vector<double> rolls;
    for (const auto& sec : chart.sections) rolls.push_back(sec.beat);
    if (rolls.size() < 2) {
        rolls.clear();
        const double last = chart.notes.empty() ? 64.0 : chart.notes.back().beat;
        for (double b = 0.0; b < last; b += 16.0) rolls.push_back(b);
    }

    nc::ModDoc doc;
    int prev = -2;                // -2 = nothing rolled yet, -1 = CH is showing
    for (double b : rolls) {
        const int tick = int(b * chart.resolution + 0.5);
        int id;
        do { id = POOL[next() % unsigned(NPOOL)]; } while (id == prev);
        // Retire the outgoing field before raising the incoming one. Both land
        // on the same tick; without the retire they would both be at 100% and
        // draw over each other.
        //
        // Snapped, not approached. Tried it both ways: at the default rate the
        // swap takes a second, which straddles the boundary instead of landing
        // on it and draws both fields over each other the whole way across --
        // two dense fields overlapping is muddy, not a crossfade. A hard cut on
        // the section boundary is the look.
        if (prev >= 0)
            doc.entries.push_back(nc::ModEntry{tick, prev, 0.0f, -1.0f, 0, 0, true});
        if (id >= 0)
            doc.entries.push_back(nc::ModEntry{tick, id, 1.0f, -1.0f, 0, 0, true});
        prev = id;
    }
    doc.rebuild(chart);
    return doc;
}

// Sample modchart.h into a .ncmod.
//
// This is lossy and the loss is structural, not sloppiness: modchart.h is C++,
// so it uses smoothstep ramps (ncRamp) and continuous expressions
// (0.04*sinf(t*0.06)) that a list of linear approaches cannot reproduce
// exactly. The conversion is therefore a piecewise-linear sample.
//
// The one non-obvious trick is that each sample's approach RATE is chosen so
// the knob lands exactly on the next sample's value at the next sample time --
// |dv|/dt, since fapproach moves linearly and clamps on arrival. That makes the
// output piecewise-linear through the true curve rather than a staircase, which
// is what a snap-sampled (*-1) export would give. At the default 4-beat grid
// the slowest thing being sampled is sin(t*0.05), period 126 beats, so it gets
// 31 samples per cycle.
//
// Section boundaries are forced sample points and are DISCONTINUITIES -- the
// blocks at 200 and 464 do `m = nc::Mods{}`. A jump is emitted as a snap at the
// boundary, with the following ramp displaced one tick later so the snap is not
// overwritten at the same tick (a later entry on the same knob at the same tick
// wins, and would take the rate with it).
static int exportModchart(const nc::Chart& chart, const std::string& chartDir,
                          const std::string& path, double stepBeats) {
    // Every beat modchart.h branches on, so no jump is straddled by a segment.
    const double MARKS[] = {0,8,24,40,56,72,104,136,152,168,200,232,248,
                            296,335,404,464,496,526,560,624,650,658};
    const double lastBeat = chart.notes.empty() ? 660.0 : chart.notes.back().beat + 8.0;

    std::vector<double> grid;
    for (double b = 0.0; b <= lastBeat; b += stepBeats) grid.push_back(b);
    for (double m : MARKS) if (m <= lastBeat) grid.push_back(m);
    std::sort(grid.begin(), grid.end());
    grid.erase(std::unique(grid.begin(), grid.end()), grid.end());

    // modchart.h's knobs, in the order they read best in the file.
    struct Knob { int id; float nc::Mods::*mod; float PostFx::*fx; };
    const Knob KNOBS[] = {
        {nc::MOD_DRUNK,      &nc::Mods::drunk,     nullptr},
        {nc::MOD_TORNADO,    &nc::Mods::tornado,   nullptr},
        {nc::MOD_BEAT,       &nc::Mods::beat,      nullptr},
        {nc::MOD_BUMPY,      &nc::Mods::bumpy,     nullptr},
        {nc::MOD_TIPSY,      &nc::Mods::tipsy,     nullptr},
        {nc::MOD_FLIP,       &nc::Mods::flip,      nullptr},
        {nc::MOD_INVERT,     &nc::Mods::invert,    nullptr},
        {nc::MOD_DIZZY,      &nc::Mods::dizzy,     nullptr},
        {nc::MOD_CONFUSION,  &nc::Mods::confusion, nullptr},
        // The two *speed knobs are derived, not read: modfile.cpp computes
        // bumpyOffset = beat * bumpyspeed, and modchart.h sets bumpyOffset =
        // t * k, so k = offset/beat recovers the knob EXACTLY. They are
        // piecewise constant, so they come out as clean snaps.
        {nc::MOD_BUMPYSPEED, nullptr, nullptr},
        {nc::MOD_TIPSYSPEED, nullptr, nullptr},
        {nc::MOD_ABERRATION, nullptr, &PostFx::aberration},
        {nc::MOD_GLOW,       nullptr, &PostFx::glow},
        {nc::MOD_VIGNETTE,   nullptr, &PostFx::vignette},
        {nc::MOD_DESAT,      nullptr, &PostFx::desat},
        {nc::MOD_SHAKE,      nullptr, &PostFx::shake},
    };
    const int NK = int(sizeof(KNOBS) / sizeof(KNOBS[0]));

    auto sample = [&](double beat, float* v) {
        nc::Mods m; PostFx fx;
        modchartAt(beat, m, fx);
        for (int k = 0; k < NK; ++k) {
            if      (KNOBS[k].mod) v[k] = m.*(KNOBS[k].mod);
            else if (KNOBS[k].fx)  v[k] = fx.*(KNOBS[k].fx);
            else if (KNOBS[k].id == nc::MOD_BUMPYSPEED)
                v[k] = beat != 0.0 ? float(m.bumpyOffset / beat) : 0.0f;
            else
                v[k] = beat != 0.0 ? float(m.tipsyOffset / beat) : 0.0f;
        }
    };

    const float EPS = 1e-4f;
    nc::ModDoc doc;
    { std::error_code ec;
      const auto abs = std::filesystem::absolute(chartDir, ec);
      doc.chartDir = ec ? chartDir : abs.generic_string(); }

    int jumps = 0;
    std::vector<float> cur(NK), nxt(NK), pre(NK);
    sample(grid[0], cur.data());
    for (int k = 0; k < NK; ++k)
        if (fabsf(cur[k] - nc::modDefault(KNOBS[k].id)) > EPS)
            doc.entries.push_back(nc::ModEntry{0, KNOBS[k].id, cur[k], -1.0f, 0, 0, true});

    for (size_t i = 0; i + 1 < grid.size(); ++i) {
        const double b0 = grid[i], b1 = grid[i + 1];
        const int t0 = int(b0 * chart.resolution + 0.5);
        const double dt = chart.beatToSec(b1) - chart.beatToSec(b0);
        if (dt <= 0.0) continue;

        sample(b0, cur.data());
        sample(b1, nxt.data());
        sample(b1 - 1e-6, pre.data());     // value arriving at b1 from the left

        for (int k = 0; k < NK; ++k) {
            const bool jump = fabsf(nxt[k] - pre[k]) > EPS;
            if (fabsf(pre[k] - cur[k]) > EPS)      // ramp across the segment
                doc.entries.push_back(nc::ModEntry{
                    t0 + 1, KNOBS[k].id, pre[k],
                    float(fabsf(pre[k] - cur[k]) / dt), 0, 0, true});
            if (jump) {                            // then snap at the boundary
                doc.entries.push_back(nc::ModEntry{
                    int(b1 * chart.resolution + 0.5), KNOBS[k].id, nxt[k],
                    -1.0f, 0, 0, true});
                ++jumps;
            }
        }
    }

    doc.rebuild(chart);
    if (!doc.save(path)) return 1;
    printf("exported %s: %zu entries over %zu samples (step %g beats, %d jumps)\n",
           path.c_str(), doc.entries.size(), grid.size(), stepBeats, jumps);
    return 0;
}

static int dumpActorMods(const nc::Chart& chart, const std::string& chartDir,
                         const std::string& path, nc::ActorLayer& actors,
                         nc::Renderer& renderer) {
    auto setChartDir = [&](nc::ModDoc& doc) {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(chartDir, ec);
        doc.chartDir = ec ? chartDir : abs.generic_string();
    };

    nc::ModDoc legacy;
    setChartDir(legacy);
    const int legacyEntries = actors.drainLuaMods(legacy, chart.resolution);

    double lastBeat = chart.notes.empty() ? 16.0 : chart.notes.back().beat + 8.0;
    for (const std::string& stem : nc::findAudioStems(chartDir, chart.musicStream)) {
        const double duration = audioDuration(stem);
        if (duration > 0.0) lastBeat = std::max(lastBeat, chart.secToBeat(duration));
    }
    actors.pump(renderer, chart.beatToSec(lastBeat), lastBeat);
    std::vector<nc::PlayerModChange> changes;
    actors.collectPlayerModChanges(changes);
    const int players = actors.livePlayerCount();

    nc::ModDoc doc;
    if (players > 0) {
        setChartDir(doc);
        doc.players = players;
        int positions[2][nc::MOD_COUNT];
        std::fill(&positions[0][0], &positions[0][0] + 2 * nc::MOD_COUNT, -1);
        int activeTick = 0;
        bool haveTick = false;
        for (const nc::PlayerModChange& change : changes) {
            const int tick = int(std::llround(
                chart.secToBeat(change.sec) * chart.resolution));
            if (!haveTick || tick != activeTick) {
                std::fill(&positions[0][0],
                          &positions[0][0] + 2 * nc::MOD_COUNT, -1);
                activeTick = tick;
                haveTick = true;
            }
            if (change.player < 1 || change.player > 2 ||
                change.mod < 0 || change.mod >= nc::MOD_COUNT)
                continue;
            int& position = positions[change.player - 1][change.mod];
            if (position >= 0) {
                doc.entries[position].percent = change.target;
                doc.entries[position].approach = change.speed;
            } else {
                position = int(doc.entries.size());
                doc.entries.push_back(nc::ModEntry{
                    tick, change.mod, change.target, change.speed, 0,
                    players == 2 ? change.player : 0, true});
            }
        }
    } else {
        doc = legacy;
    }
    doc.rebuild(chart);
    if (!doc.save(path)) return 1;

    printf("dumped %s: %zu entries from %s through beat %.3f%c",
           path.c_str(), doc.entries.size(), players > 0 ? "live PlayerOptions"
                                                         : "Lua mod tables",
           lastBeat, 10);
    if (players > 0 && legacyEntries > 0)
        printf("  live PlayerOptions took precedence over %d static table entries%c",
               legacyEntries, 10);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 0; }

    std::string chartDir;
    std::string out = "out.mp4";
    std::string modPath;
    int W = 1920, H = 1080, fps = 60;
    double beatA = 0.0, beatB = -1.0;
    bool fromGiven = false, toGiven = false;
    bool previewOnly = false, bench = false;
    std::string enc_name = "x264";
    double previewBeat = 240.0;
    nc::RenderOpts opt;
    bool builtinMods = false;
    bool randMods = false;
    bool randEngine = false;
    unsigned randSeed = 0;
    int randCount = 3;
    bool noActors = false;
    bool noBg = false;
    double bgScale = 0.5;
    std::string assetDir;
    std::vector<std::string> actorArgs;
    std::vector<std::string> bgShaderArgs;
    std::vector<std::string> fxShaderArgs;
    std::string fxChainArg;
    double syncMs = 0.0;
    bool noBf = false;
    std::string smPath, exportPath, dumpPath;
    double exportStep = 4.0;
    nc::SmImportOpts smOpts;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if      (a == "--dir")     chartDir = next();
        else if (a == "--out")     out = next();
        else if (a == "--mods")    modPath = next();
        else if (a == "--builtin-mods") builtinMods = true;
        else if (a == "--randmods")  randMods = true;
        else if (a == "--randengine") randEngine = true;
        else if (a == "--randseed")  randSeed = unsigned(strtoul(next().c_str(), nullptr, 10));
        else if (a == "--randcount") randCount = atoi(next().c_str());
        else if (a == "--actor")  actorArgs.push_back(next());
        else if (a == "--noactors") noActors = true;
        else if (a == "--nobg")    noBg = true;
        else if (a == "--bgscale") bgScale = atof(next().c_str());
        else if (a == "--bgshader") bgShaderArgs.push_back(next());
        else if (a == "--fxshader") fxShaderArgs.push_back(next());
        else if (a == "--fxchain")  fxChainArg = next();
        else if (a == "--assets") assetDir = next();
        else if (a == "--sync")   syncMs = atof(next().c_str());
        else if (a == "--nobf")   noBf = true;
        else if (a == "--w")       W = atoi(next().c_str());
        else if (a == "--h")       H = atoi(next().c_str());
        else if (a == "--fps")     fps = atoi(next().c_str());
        else if (a == "--from")  { beatA = atof(next().c_str()); fromGiven = true; }
        else if (a == "--to")    { beatB = atof(next().c_str()); toGiven = true; }
        else if (a == "--speed")   opt.noteSpeed = float(atof(next().c_str()));
        else if (a == "--nopost")  opt.noPost = true;
        else if (a == "--nomods")  opt.noMods = true;
        else if (a == "--nobot")   opt.noBot = true;
        else if (a == "--bench")   bench = true;
        else if (a == "--playfield") opt.playfield = true;
        else if (a == "--px")      opt.px = float(atof(next().c_str()));
        else if (a == "--py")      opt.py = float(atof(next().c_str()));
        else if (a == "--pz")      opt.pz = float(atof(next().c_str()));
        else if (a == "--enc")     enc_name = next();
        else if (a == "--preview") { previewOnly = true; previewBeat = atof(next().c_str()); }
        else if (a == "--import-sm") smPath = next();
        else if (a == "--sm-diff")   smOpts.diffIndex = atoi(next().c_str());
        else if (a == "--sm-strum")  smOpts.allStrum = true;
        else if (a == "--export-modchart") exportPath = next();
        else if (a == "--export-step")     exportStep = atof(next().c_str());
        else if (a == "--dump-mods")       dumpPath = next();
    }

    // Import mode: convert and exit. No GL context is created, so this works
    // headless and does not touch the render path at all.
    if (!smPath.empty()) {
        if (chartDir.empty()) {
            fprintf(stderr, "--import-sm needs --dir <output folder>%c", 10);
            return 1;
        }
        const nc::SmImportReport r = nc::importSm(smPath, chartDir, smOpts);
        if (!r.ok) { fprintf(stderr, "import failed: %s%c", r.error.c_str(), 10); return 1; }
        printf("%s - %s   [%s, %d blocks]%c", r.artist.c_str(), r.title.c_str(),
               r.chosenDiff.c_str(), r.diffCount, 10);
        printf("  notes %d (holds %d)   strum %d / hopo %d / tap %d%c",
               r.notes, r.holds, r.strum, r.hopo, r.tap, 10);
        printf("  bpm points %d   stops %d   last note %.1fs%c",
               r.bpmPoints, r.stops, r.lastSec, 10);
        printf("  #MODS lines %d -> %d entries (%d scoped)%c",
               r.modLines, r.modEntries, r.modIntervals, 10);
        if (r.copiedAssets > 0)
            printf("  copied %d referenced asset file%s into --dir%c",
                   r.copiedAssets, r.copiedAssets == 1 ? "" : "s", 10);
        // An empty .ncmod is a legitimate result -- plenty of charts keep no
        // native mods at all -- but "0 -> 0 entries" buried in a status block
        // reads as a failed import. Say what happened and where the modchart
        // actually is, rather than leaving an empty file and no explanation.
        if (r.modEntries == 0) {
            printf("  ! no #MODS or #ATTACKS in this .sm, so the .ncmod is "
                   "empty -- that is the file, not the importer%c", 10);
            if (!r.fgActor.empty())
                printf("    its modchart is the actor tree in '%s' "
                       "(#FGCHANGES); render it with --actor %s%c",
                       r.fgActor.c_str(), r.fgActor.c_str(), 10);
            else
                printf("    nothing in #FGCHANGES either -- check for a Lua "
                       "folder beside the .sm%c", 10);
        }
        if (!r.stubbed.empty()) {
            printf("  imported but NOT rendered:");
            for (const auto& s : r.stubbed) printf(" %s", s.c_str());
            printf("%c", 10);
        }
        for (const auto& w : r.warnings) printf("  ! %s%c", w.c_str(), 10);
        return 0;
    }

    if (!exportPath.empty()) {
        if (chartDir.empty()) {
            fprintf(stderr, "--export-modchart needs --dir for the tempo map%c", 10);
            return 1;
        }
        nc::Chart c;
        if (!c.load(chartDir + "/notes.chart")) return 1;
        return exportModchart(c, chartDir, exportPath, exportStep);
    }

    if (chartDir.empty()) {
        fprintf(stderr, "--dir is required. Run with no arguments for help.%c", 10);
        return 1;
    }
    // --dir may name the .chart itself rather than the folder holding it --
    // dragging a chart onto the exe, or tab-completing to the file, is the
    // obvious thing to try. Everything downstream (audio stems, .ncmod
    // discovery, actor/background folders) is folder-relative, so normalise
    // here and the rest of main never knows the difference.
    {
        const size_t dot = chartDir.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = chartDir.substr(dot);
            for (char& c : ext) if (c >= 'A' && c <= 'Z') c += 32;
            if (ext == ".chart") {
                const size_t sl = chartDir.find_last_of("/\\");
                chartDir = (sl == std::string::npos) ? std::string(".")
                                                     : chartDir.substr(0, sl);
            }
        }
    }
    nc::Chart chart;
    if (!chart.load(chartDir + "/notes.chart")) return 1;
    printf("%s - %s [%s]\n", chart.artist.c_str(), chart.name.c_str(), chart.charter.c_str());
    printf("%zu notes, %zu sections, res %d, offset %.3f, noteSpeed %.1f\n",
           chart.notes.size(), chart.sections.size(), chart.resolution,
           chart.offset, opt.noteSpeed);

    // Which modchart, explicitly. This used to be "--mods or else the hardcoded
    // R.E.M. III one", which silently applied REM III's modchart to any other
    // song you pointed at -- and once a chart folder can carry its own .ncmod,
    // ignoring that file is the wrong default too.
    //
    //   --mods <file>    that file
    //   --builtin-mods   modchart.h, whatever the folder holds
    //   neither          the folder's own .ncmod if there is exactly one,
    //                    otherwise no modchart at all
    //
    // The active source is always printed, because the failure mode here is
    // silent: you get a picture either way and it is simply the wrong one.
    nc::ModDoc doc;
    nc::ModDoc doc2;                 // player 2, when a chart drives one
    if (!dumpPath.empty()) {
        opt.noMods = true;
        printf("modchart: actor dump%c", 10);
    } else if (randMods) {
        if (randCount < 1) randCount = 1;
        doc = randomModchart(chart, randSeed, randCount);
        opt.doc = &doc;
        printf("modchart: RANDOM, seed %u, %d at a time (%zu entries) -- "
               "re-run with --randseed %u for this exact roll%c",
               randSeed, randCount, doc.entries.size(), randSeed, 10);
    } else if (randEngine) {
        doc = randomEngineModchart(chart, randSeed);
        opt.doc = &doc;
        printf("modchart: RANDOM ENGINE, seed %u (%zu entries) -- "
               "re-run with --randseed %u for this exact roll%c",
               randSeed, doc.entries.size(), randSeed, 10);
        // The roll is the whole point of the flag, so it is printed rather
        // than left to be inferred from a render. Only the entries that turn
        // something ON name a field; a section with none of them is CH.
        for (const auto& sec : chart.sections) {
            const int tick = int(sec.beat * chart.resolution + 0.5);
            const char* which = "ch";
            for (const auto& e : doc.entries) {
                if (e.tick != tick || e.percent <= 0.0f) continue;
                switch (e.mod) {
                    case nc::MOD_PIU:         which = "piu";         break;
                    case nc::MOD_MOONSCRAPER: which = "moonscraper"; break;
                    case nc::MOD_YARG:        which = "yarg";        break;
                    case nc::MOD_GH3:         which = "gh3";         break;
                    case nc::MOD_TAIKO:       which = "taiko";       break;
                    case nc::MOD_BMS:         which = "bms";         break;
                    default: break;
                }
            }
            printf("  %-24s %s%c", sec.name.c_str(), which, 10);
        }
    } else if (modPath.empty() && !builtinMods) {
        std::error_code ec;
        std::string found;
        int n = 0;
        for (const auto& e : std::filesystem::directory_iterator(chartDir, ec)) {
            if (e.path().extension() == ".ncmod") { found = e.path().generic_string(); ++n; }
        }
        if (n == 1) modPath = found;
        else if (n > 1)
            printf("%d .ncmod files in %s -- pass --mods to pick one; "
                   "rendering without a modchart%c", n, chartDir.c_str(), 10);
    }
    if (!dumpPath.empty()) {
        /* actor runtime is the source */
    } else if (randMods || randEngine) {
        /* already built */
    } else if (!modPath.empty()) {
        if (!doc.load(modPath)) return 1;
        doc.rebuild(chart);
        opt.doc = &doc;
        printf("modchart: %s (%zu entries)%c", modPath.c_str(), doc.entries.size(), 10);
    } else if (builtinMods) {
        printf("modchart: built-in modchart.h (R.E.M. III)%c", 10);
    } else {
        opt.noMods = true;
        printf("modchart: none found in %s%c", chartDir.c_str(), 10);
    }

    // ---- hidden window + WGL context -------------------------------------
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "NotClonGL";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "NotClon", WS_OVERLAPPEDWINDOW,
                                0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(dc, &pfd);
    SetPixelFormat(dc, pf, &pfd);
    HGLRC rc = wglCreateContext(dc);
    wglMakeCurrent(dc, rc);
    if (!nc_load_gl()) return 1;
    printf("GL: %s | %s\n", (const char*)glGetString(GL_RENDERER),
                            (const char*)glGetString(GL_VERSION));

    nc::Renderer R;
    if (!R.init(W, H, nc::nc_findAssets(assetDir))) return 1;
    R.buildHitTimes(chart);

    // The .sm this chart was imported from, if it is still there. Discovered
    // once; it feeds two consumers -- the actor layer (#FG/#BGCHANGES folder
    // entries) and the background layer (#BGCHANGES media entries).
    std::string smBeside;
    {
        std::error_code ec2;
        for (const auto& e : std::filesystem::directory_iterator(chartDir, ec2))
            if (e.path().extension() == ".sm") { smBeside = e.path().generic_string(); break; }
    }

    // Actor folders. A song folder keeps the StepMania layout: the chart, the
    // .ncmod and the audio at the root, and one subfolder per actor tree with
    // its own images. If a .sm sits beside the chart its #FGCHANGES/#BGCHANGES
    // schedule them; --actor adds one by hand at a beat.
    nc::ActorLayer actors;
    actors.setChart(&chart);
    // Before loading: AFTs size themselves from DISPLAY at InitCommand time.
    actors.setDisplaySize(W, H);
    if (!noActors) {
        std::string aerr;
        for (const auto& av : actorArgs) {
            const size_t at = av.find('@');
            const std::string sub = (at == std::string::npos) ? av : av.substr(0, at);
            const double b = (at == std::string::npos) ? 0.0 : atof(av.c_str() + at + 1);
            actors.addFolder(chartDir, sub, chart.beatToSec(b), true);
        }
        if (actorArgs.empty() && !smBeside.empty())
            actors.loadFromSm(smBeside, chartDir, aerr);
        // A NotITG-lineage chart keeps its modchart in a Lua table rather than
        // in #MODS, so the actor tree has to be loaded before the modchart is
        // complete. Draining is a no-op for a tree with no `mods` global,
        // which is every chart that predates this.
        if (!actors.empty() && !opt.noMods) {
            const int n = actors.drainLuaMods(doc, chart.resolution);
            if (n > 0) {
                doc.rebuild(chart);
                opt.doc = &doc;
                opt.noMods = false;
            }
        }
        // Two playfields whenever the document says so -- from a `#players 2`
        // header or from a drained modfile that drove player 2. One entry
        // list, two docs differing only in the player filter.
        if (actors.livePlayerCount() >= 2) {
            doc2 = doc;                 // pointer is the two-field layout signal
            opt.doc2 = &doc2;           // live Lua PlayerOptions override its values
            printf("modchart: two live SM5 playfields%c", 10);
        }
        else if (opt.doc == &doc && doc.players == 2) {
            doc.forPlayer = 1;
            doc.rebuild(chart);
            doc2 = doc;
            doc2.forPlayer = 2;
            doc2.rebuild(chart);
            opt.doc2 = &doc2;
            printf("modchart: two playfields%c", 10);
        }
        for (const auto& m : actors.log()) printf("actor: %s%c", m.c_str(), 10);
        if (!actors.empty()) R.setActorLayer(&actors);
    }

    if (!dumpPath.empty()) {
        if (noActors || actors.empty()) {
            fprintf(stderr, "--dump-mods found no actor modfile; keep actors enabled "
                            "and use an adjacent .sm or --actor <folder>%c", 10);
            return 1;
        }
        const size_t oldLogSize = actors.log().size();
        const int result = dumpActorMods(chart, chartDir, dumpPath, actors, R);
        for (size_t i = oldLogSize; i < actors.log().size(); ++i)
            printf("actor: %s%c", actors.log()[i].c_str(), 10);
        return result;
    }

    const std::vector<std::string> stems =
        nc::findAudioStems(chartDir, chart.musicStream);
    std::string unreadableStem;
    for (const std::string& stem : stems) {
        const double duration = audioDuration(stem);
        if (duration <= 0.0) {
            if (unreadableStem.empty()) unreadableStem = stem;
        } else {
            opt.audioDuration = std::max(opt.audioDuration, duration);
        }
    }

    // Background layer: #BGCHANGES stills/movies from the same .sm, auto-on
    // like the actor layer (--nobg opts out), plus any --bgshader layers over
    // them. No .sm and no --bgshader means no Background is ever constructed
    // and drawFrame's pass is a skipped null check -- the pinned REM III
    // baselines stay byte-identical by construction.
    nc::Background bg;
    if (!noBg) {
        if (!smBeside.empty()) bg.loadFromSm(smBeside, chartDir, float(bgScale));
        for (const auto& fp : bgShaderArgs) bg.addShader(fp);
        for (const auto& fp : fxShaderArgs) bg.addSceneShader(fp);
        // Shaders the .ncmod names itself, resolved against ITS folder. A
        // document that drives fx.<name> is meaningless without the shader
        // that declares <name>, so carrying the pointer is what makes the
        // modchart openable on its own -- the same reason #chart exists.
        // Command-line flags are added first and these stack on top, so a
        // --fxshader can still be layered over a document's own.
        { const size_t sl = modPath.find_last_of("/\\");
          const std::string mdir = sl == std::string::npos ? std::string()
                                                           : modPath.substr(0, sl);
          // A `#background` still/movie the document names, resolved against
          // the .ncmod's folder ONLY. Added before the shader layers so they
          // draw over it; after any .sm media, so the more specific source
          // covers the generic one.
          if (!doc.background.empty()) {
              const auto b = doc.shaderPaths({doc.background}, mdir);
              if (!b.empty()) bg.loadMedia(b[0], float(bgScale));
          }
          for (const auto& fp : doc.shaderPaths(doc.bgShaders, mdir))
              bg.addShader(fp, true);
          for (const auto& fp : doc.shaderPaths(doc.fxShaders, mdir))
              bg.addSceneShader(fp, true);
          if (!doc.fxChain.empty()) {
              const auto c = doc.shaderPaths({doc.fxChain}, mdir);
              if (!c.empty()) bg.loadChain(c[0], true);
          } }
        if (!fxChainArg.empty()) bg.loadChain(fxChainArg);
        for (const auto& m : bg.log()) printf("bg: %s%c", m.c_str(), 10);
        if (!bg.empty() || bg.hasChain()) R.setBackground(&bg);
    }

    std::vector<unsigned char> pixels(size_t(W) * H * 3);
    // glReadPixels returns rows bottom-up; rawvideo and PPM both want top-down.
    // Flipping via memcpy into a staging buffer costs ~0.3 ms, whereas the
    // old row-by-row fwrite cost 28 ms/frame in pipe syscalls.
    std::vector<unsigned char> flipped(pixels.size());
    const size_t rowBytes = size_t(W) * 3;
    auto flipRows = [&]() {
        for (int y = 0; y < H; ++y)
            memcpy(&flipped[size_t(y) * rowBytes],
                   &pixels[size_t(H - 1 - y) * rowBytes], rowBytes);
    };

    FILE* enc = nullptr;
    int frameCount = 1;
    double t0 = 0.0, t1 = 0.0;
    if (!previewOnly) {
        // Default the start to where the AUDIO starts, not to beat 0. With a
        // negative #OFFSET beat 0 is some way into the song (for -2.733, beat 0
        // is at 2.733s), so starting at beat 0 lops that much off the front.
        // min() so a positive-offset chart, where audio 0 is already past beat
        // 0, is unaffected -- R.E.M. III stays at exactly 0.
        if (!fromGiven) beatA = std::min(0.0, chart.secToBeat(0.0));
        t0 = chart.beatToSec(beatA);
        if (toGiven) {
            t1 = chart.beatToSec(beatB);
        } else {
            if (!unreadableStem.empty()) {
                fprintf(stderr, "cannot read audio duration: %s\n",
                        unreadableStem.c_str());
                return 1;
            }
            if (!stems.empty()) {
                t1 = opt.audioDuration;
                beatB = chart.secToBeat(t1);
            } else {
                beatB = chart.notes.empty() ? 16.0 : chart.notes.back().beat + 8.0;
                t1 = chart.beatToSec(beatB);
            }
        }
        if (t1 <= t0) {
            fprintf(stderr, "render range ends before it starts\n");
            return 1;
        }
        frameCount = std::max(1, int(std::ceil((t1 - t0) * fps)));
        // NVENC hands the encode to the GPU's dedicated hardware block, which
        // sits completely idle while we render.
        std::string vcodec;
        if      (enc_name == "nvenc" || enc_name == "h264")
            vcodec = "-c:v h264_nvenc -preset p5 -tune hq -rc vbr -cq 19 -b:v 0 -pix_fmt yuv420p";
        else if (enc_name == "hevc")
            vcodec = "-c:v hevc_nvenc -preset p5 -tune hq -rc vbr -cq 21 -b:v 0 -pix_fmt yuv420p";
        else if (enc_name == "av1")
            vcodec = "-c:v av1_nvenc -preset p5 -tune hq -rc vbr -cq 25 -b:v 0 -pix_fmt yuv420p";
        else
            vcodec = "-c:v libx264 -preset medium -crf 16 -pix_fmt yuv420p";
        if (noBf) vcodec += " -bf 0";

        // The audio is whatever the chart names, not necessarily song.ogg -- an
        // imported .sm keeps its own filename in [Song] MusicStream. Getting
        // this wrong is silent and total: ffmpeg cannot open the input, exits,
        // and every frame we write goes into a broken pipe, so the run reports
        // success and produces no file.
        if (stems.empty())
            printf("no audio found in %s -- encoding video only\n", chartDir.c_str());
        else if (stems.size() > 1)
            printf("mixing %zu audio stems\n", stems.size());

        char cmd[2048];
        if (bench)
            snprintf(cmd, sizeof cmd,
                     "%s -y -hide_banner -loglevel error "
                     "-f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i - %s -f null -",
                     nc::nc_ffmpeg().c_str(), W, H, fps, vcodec.c_str());
        else if (stems.empty())
            snprintf(cmd, sizeof cmd,
                     "%s -y -hide_banner -loglevel warning "
                     "-f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i - %s "
                     "-t %.6f \"%s\"",
                     nc::nc_ffmpeg().c_str(), W, H, fps, vcodec.c_str(),
                     t1 - t0, out.c_str());
        else {
            // Video is input 0; each stem is its own -ss'd input, mixed at
            // unity gain the way CH layers them (amix normalize=0).
            std::string c;
            char head[256];
            snprintf(head, sizeof head,
                     "%s -y -hide_banner -loglevel warning "
                     "-f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i - ",
                     nc::nc_ffmpeg().c_str(), W, H, fps);
            c = head;
            for (const auto& sp : stems) {
                char in[600];
                if (t0 > 0.0)
                    snprintf(in, sizeof in, "-ss %.6f -i \"%s\" ", t0, sp.c_str());
                else if (t0 < 0.0)
                    snprintf(in, sizeof in, "-itsoffset %.6f -i \"%s\" ", -t0, sp.c_str());
                else
                    snprintf(in, sizeof in, "-i \"%s\" ", sp.c_str());
                c += in;
            }
            if (stems.size() > 1) {
                std::string fl = "-filter_complex \"";
                for (size_t i = 0; i < stems.size(); ++i)
                    fl += "[" + std::to_string(i + 1) + ":a]";
                fl += "amix=inputs=" + std::to_string(stems.size()) +
                      ":duration=longest:normalize=0[aout]\" -map 0:v -map \"[aout]\" ";
                c += fl;
            }
            char tail[768];
            snprintf(tail, sizeof tail, "%s -c:a aac -b:a 320k -t %.6f \"%s\"",
                     vcodec.c_str(), t1 - t0, out.c_str());
            c += tail;
            snprintf(cmd, sizeof cmd, "%s", c.c_str());
        }
        printf("encoding %d frames (beat %.1f..%.1f)\n", frameCount, beatA, beatB);
        enc = _popen(cmd, "wb");
        if (!enc) { fprintf(stderr, "cannot start ffmpeg\n"); return 1; }
    }

    using Clk = std::chrono::high_resolution_clock;
    double msDraw = 0, msRead = 0, msWrite = 0;
    auto ms = [](Clk::time_point a, Clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    for (int frame = 0; frame < frameCount; ++frame) {
        auto tFrame = Clk::now();
        // --sync delays the video CONTENT by syncMs: frame i shows the state
        // syncMs earlier. For overlaying in real CH, where the measured offset
        // was the video running ~50ms ahead of the game's own notes.
        double beat = previewOnly ? previewBeat
                                  : chart.secToBeat(t0 + double(frame) / fps - syncMs / 1000.0);

        // Resolves into the core's own post FBO -- an offline run's default
        // framebuffer is the 64x64 hidden window, which would clip the frame.
        R.drawFrame(chart, beat, opt, R.postFbo());

        glFinish();
        auto tDraw = Clk::now();
        glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        auto tRead = Clk::now();

        if (previewOnly) {
            const std::string pcmd = nc::nc_ffmpeg() +
                " -y -hide_banner -loglevel error -f image2pipe"
                " -vcodec ppm -i - preview.png";
            FILE* p = _popen(pcmd.c_str(), "wb");
            fprintf(p, "P6\n%d %d\n255\n", W, H);
            for (int y = H - 1; y >= 0; --y)
                fwrite(&pixels[size_t(y) * W * 3], 1, size_t(W) * 3, p);
            _pclose(p);
            // After the render, not before: a command body only runs
            // during the frame, so its diagnostics do not exist at
            // load time. This is what makes a chart that silently
            // stops animating diagnosable.
            for (const auto& m : actors.log())
                printf("actor: %s%c", m.c_str(), 10);
            printf("wrote preview.png at beat %.2f\n", previewBeat);
        } else {
            flipRows();
            fwrite(flipped.data(), 1, flipped.size(), enc);
            auto tWrite = Clk::now();
            msDraw += ms(tFrame, tDraw); msRead += ms(tDraw, tRead);
            msWrite += ms(tRead, tWrite);
            if (!bench && frame % (fps * 5) == 0)
                printf("  %d/%d (%.0f%%)\n", frame, frameCount, 100.0 * frame / frameCount);
        }
    }

    if (enc) {
        // ffmpeg's exit status, not a guess. Reporting "wrote out.mp4" when
        // ffmpeg never launched or died mid-encode is the worst failure this
        // program can have -- it is indistinguishable from success until
        // someone goes looking for the file. _pclose returns ffmpeg's code.
        const int encRc = _pclose(enc);
        double tot = msDraw + msRead + msWrite;
        if (frameCount > 0 && tot > 0) {
            printf("%c[%s] %d frames @ %dx%d%c", 10, enc_name.c_str(), frameCount, W, H, 10);
            printf("  draw     %8.1f ms  %5.2f ms/f  %4.1f%%%c", msDraw,  msDraw/frameCount,  100*msDraw/tot, 10);
            if (bg.benchMs() > 0)   // movie decode+upload, already inside draw
                printf("  bg       %8.1f ms  %5.2f ms/f  (part of draw)%c",
                       bg.benchMs(), bg.benchMs()/frameCount, 10);
            printf("  readback %8.1f ms  %5.2f ms/f  %4.1f%%%c", msRead,  msRead/frameCount,  100*msRead/tot, 10);
            printf("  pipe     %8.1f ms  %5.2f ms/f  %4.1f%%%c", msWrite, msWrite/frameCount, 100*msWrite/tot, 10);
            printf("  TOTAL    %8.1f ms  ->  %.1f fps%c", tot, 1000.0*frameCount/tot, 10);
        }
        if (encRc != 0) {
            fprintf(stderr, "%cffmpeg exited %d -- NOTHING WAS WRITTEN.%c",
                    10, encRc, 10);
            fprintf(stderr, "  ran: %s%c", nc::nc_ffmpeg().c_str(), 10);
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc);
            ReleaseDC(hwnd, dc);
            return 1;
        }
        if (!bench) {
            // Also confirm the file is actually there and non-empty: ffmpeg can
            // exit 0 having written a zero-byte file if the muxer got nothing.
            FILE* chk = fopen(out.c_str(), "rb");
            long sz = 0;
            if (chk) { fseek(chk, 0, SEEK_END); sz = ftell(chk); fclose(chk); }
            if (sz <= 0) {
                fprintf(stderr, "ffmpeg reported success but %s is missing or "
                                "empty%c", out.c_str(), 10);
                wglMakeCurrent(nullptr, nullptr);
                wglDeleteContext(rc);
                ReleaseDC(hwnd, dc);
                return 1;
            }
            printf("wrote %s (%.1f MB)%c", out.c_str(), sz / 1048576.0, 10);
        }
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return 0;
}
