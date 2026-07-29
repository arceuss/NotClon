// .sm -> .chart + .ncmod import.
//
// This is a converter, not part of the render path, so it lives outside
// Renderer entirely: it reads a StepMania file and writes the two files
// NotClon already knows how to open. Nothing here is called during a frame.
//
// The 4-panel -> 5-fret question AGENTS.md open work section 5 raises is
// answered by SmImport::PanelMap, and the answer is a *choice* -- see the
// comment there. The other two questions it raises do not arise for every
// file: #STOPS have no .chart representation and are reported rather than
// approximated, and the HOPO explosion is reported as a count so the damage
// is visible instead of implied.
#pragma once

#include "modfile.h"

#include <string>
#include <vector>

namespace nc {

struct SmImportOpts {
    // Which #NOTES block, by 0-based index. -1 picks the hardest dance-single.
    int  diffIndex = -1;
    // Force every note to strum by emitting Moonscraper's forced flag on the
    // ones that would otherwise resolve to a natural HOPO. A 240bpm 16th
    // stream is almost entirely natural HOPO otherwise, which is correct CH
    // behaviour but changes every gem's sprite.
    bool allStrum = false;
};

// What a mod-string parse produced. Separate from SmImportReport because the
// Lua actor bridge parses the same grammar without importing anything.
struct ModStringStats {
    int entries = 0, intervals = 0;
    std::vector<std::string> unknown;   // token names with no knob
    std::vector<std::string> stubbed;   // parsed but not rendered
};

// One OITG mod string ("*10 50 flip, *5 no drunk") -> entries in `doc` at
// `tick`, live for `len` ticks (0 = until something else changes the knob).
void addModString(ModDoc& doc, const std::string& modsField,
                  int tick, int len, ModStringStats& st);

struct SmImportReport {
    bool        ok = false;
    std::string error;

    std::string title, artist, music;
    std::string chosenDiff;             // "dance-single Hard 10"
    int         diffCount = 0;

    int    notes = 0, holds = 0, mines = 0, rolls = 0;
    int    strum = 0, hopo = 0, tap = 0;   // after Moonscraper resolution
    int    bpmPoints = 0;
    int    stops = 0;                      // reported, NOT converted
    double lastSec = 0.0;

    int    modLines = 0, modEntries = 0, modIntervals = 0;
    int    copiedAssets = 0;
    std::string fgActor;                   // #FGCHANGES folder, if any
    std::vector<std::string> warnings;     // unknown tokens, dropped rows, ...
    std::vector<std::string> stubbed;      // knobs imported but not rendered
};

// Reads `smPath`, writes `<outDir>/notes.chart` and `<outDir>/<name>.ncmod`.
// When the output differs from the input folder, referenced media and complete
// actor folders are copied beside them.
SmImportReport importSm(const std::string& smPath, const std::string& outDir,
                        const SmImportOpts& opts);

}  // namespace nc
