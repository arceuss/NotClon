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
    std::string fgActor;                   // #FGCHANGES folder, if any
    std::vector<std::string> warnings;     // unknown tokens, dropped rows, ...
    std::vector<std::string> stubbed;      // knobs imported but not rendered
};

// Reads `smPath`, writes `<outDir>/notes.chart` and `<outDir>/<name>.ncmod`.
// outDir must already exist and should be the folder holding the audio, since
// .chart carries no audio path and the editor looks for song.<ext> beside it.
SmImportReport importSm(const std::string& smPath, const std::string& outDir,
                        const SmImportOpts& opts);

}  // namespace nc
