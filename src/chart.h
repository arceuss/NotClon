// .chart parsing.
//
// Handles the subset NotClon needs: Resolution/Offset, the tempo map, section
// markers and one difficulty track. Frets 0-4 are notes, 5 marks forced HOPO,
// 6 marks tap, 7 is an open note.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace nc {

enum class NoteType { Strum, Hopo, Tap };

struct Note {
    double beat = 0.0;
    int    tick = 0;
    NoteType type = NoteType::Strum;
    NoteType openType = NoteType::Strum;
    int    frets = 0;        // bitmask over lanes 0..4
    bool   open = false;
    bool   tap = false;
    bool   forced = false;
    double sustain[5] = {0, 0, 0, 0, 0};   // in beats, per lane
    double openSustain = 0.0;
};

struct Section {
    double beat;
    std::string name;
};

enum class PhraseType { StarPower, Solo };

struct Phrase {
    int tick = 0;
    int length = 0;
    PhraseType type = PhraseType::StarPower;
};

struct BpmPoint {
    int    tick;
    double bpm;
};

// [SyncTrack] `TS <num> [<log2 den>]` -- the denominator is stored as a power
// of two, absent means 4 (Song.Sync.cs:81-93).
struct TimeSig {
    int tick;
    int num = 4, den = 4;
};

// One line of the beat schedule, resolved to audio seconds at load.
// Styles match CH's BeatStyle enum order (Chart.cs:8-13).
struct BeatLine {
    double sec;
    int    style;      // 0 MEASURE, 1 BEAT_STRONG, 2 BEAT_WEAK
};

class Chart {
public:
    int    resolution = 192;
    double offset = 0.0;
    std::string name, artist, charter;
    // [Song] MusicStream -- the audio file beside the chart. CH writes it; a
    // .sm import carries #MUSIC into it, which is how an imported song keeps
    // its own filename instead of having to be renamed song.ogg.
    std::string musicStream;
    std::vector<BpmPoint> bpms;
    // SM #STOPS, carried through the .chart as `E "ncstop <sec>"` markers (the
    // timing itself is baked into the BPM map as a wedge -- see smimport.cpp).
    // Resolved at load into (audio second the stop starts, duration).
    std::vector<std::pair<double,double>> stops;
    std::vector<TimeSig>  timesigs;
    std::vector<BeatLine> beatlines;
    std::vector<Section>  sections;
    std::vector<Phrase>   phrases;
    std::vector<Note>     notes;

    bool load(const std::string& path, const std::string& track = "ExpertSingle");
    void resolveNoteTypes();

    const Phrase* phraseAt(PhraseType type, int tick) const {
        const Phrase* current = nullptr;
        for (const Phrase& phrase : phrases) {
            if (phrase.tick > tick) break;
            if (phrase.type == type) current = &phrase;
        }
        if (!current) return nullptr;
        if (current->length == 0) return tick == current->tick ? current : nullptr;
        const long long end = (long long)current->tick + current->length;
        const bool inside = type == PhraseType::StarPower
                          ? tick >= current->tick && tick < end
                          : tick >= current->tick && tick <= end;
        return inside ? current : nullptr;
    }

    // Tick -> seconds, walking the tempo map.
    double tickToSec(double tick) const {
        double sec = 0.0;
        int prevTick = 0;
        double prevBpm = bpms.empty() ? 120.0 : bpms[0].bpm;
        for (size_t i = 1; i < bpms.size(); ++i) {
            if (bpms[i].tick >= tick) break;
            sec += (bpms[i].tick - prevTick) / double(resolution) * 60.0 / prevBpm;
            prevTick = bpms[i].tick;
            prevBpm  = bpms[i].bpm;
        }
        sec += (tick - prevTick) / double(resolution) * 60.0 / prevBpm;
        // CH's sign, verified at source: TimeSync.cs:90 returns
        // `songTime + songOffset` where GameManager.cs:484-492 sets
        // songOffset = -Offset (or -delay/1000 when song.ini overrides), so a
        // note plays at audio = chartTime + Offset. This line shipped as
        // `sec - offset` for a long time -- self-consistent with an importer
        // that also had the sign flipped, so the videos were right, but the
        // .chart files were CH-incompatible and REM III (authored Offset
        // 0.025) rendered 50ms off from real CH. Flipping it moved every
        // pinned hash; see the regression section.
        return sec + offset;
    }
    double beatToSec(double beat) const { return tickToSec(beat * resolution); }

    // Audio seconds -> scroll seconds: the axis notes travel on. Identical to
    // audio time except inside a stop, where it holds still -- SM freezes the
    // field while the music plays on, and this is that, as a pure function of
    // time (seekable, no integration). With no stops it returns the input
    // unchanged, bit for bit, which is what keeps the pinned hashes pinned.
    double scrollSec(double sec) const {
        if (stops.empty()) return sec;
        double out = sec;
        for (const auto& s : stops) {
            if      (sec >= s.first + s.second) out -= s.second;
            else if (sec >  s.first)            out -= (sec - s.first);
            else break;
        }
        return out;
    }

    double secToBeat(double sec) const {
        // monotonic, so a bisect is fine and exact enough for frame timing.
        //
        // `lo` is NEGATIVE on purpose. A chart with a negative #OFFSET puts
        // beat 0 some way into the audio -- with -2.733, beat 0 is at 2.733s
        // and everything before it is at a negative beat. Clamping lo
        // to 0 silently returned beat 0 for the whole intro, which is how the
        // intro came to be cut off the front of a render.
        double lo = -1024.0, hi = 4096.0;
        for (int i = 0; i < 60; ++i) {
            double mid = 0.5 * (lo + hi);
            if (beatToSec(mid) < sec) lo = mid; else hi = mid;
        }
        return 0.5 * (lo + hi);
    }

    double bpmAt(double beat) const {
        double t = beat * resolution, r = bpms.empty() ? 120.0 : bpms[0].bpm;
        for (const auto& b : bpms) { if (b.tick <= t) r = b.bpm; else break; }
        return r;
    }
};

inline std::string nc_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline bool Chart::load(const std::string& path, const std::string& track) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    std::string txt;
    { char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) txt.append(buf, n); }
    fclose(f);
    if (txt.size() > 3 && (unsigned char)txt[0] == 0xEF) txt = txt.substr(3);  // BOM

    std::map<int, Note> byTick;
    std::vector<std::pair<int,double>> stopTicks;
    int soloStart = -1, nextSoloStart = -1;
    std::string section;
    size_t pos = 0;

    while (pos < txt.size()) {
        size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = nc_trim(txt.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty()) continue;

        if (line[0] == '[') { section = line.substr(1, line.find(']') - 1); continue; }
        if (line == "{" || line == "}") continue;

        if (section == "Song") {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = nc_trim(line.substr(0, eq));
            std::string v = nc_trim(line.substr(eq + 1));
            if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
            if      (k == "Resolution") resolution = atoi(v.c_str());
            else if (k == "Offset")     offset     = atof(v.c_str());
            else if (k == "Name")       name       = v;
            else if (k == "Artist")     artist     = v;
            else if (k == "Charter")    charter    = v;
            else if (k == "MusicStream") musicStream = v;
        } else if (section == "SyncTrack") {
            int tick = 0, val = 0, den = -1;
            char kind[4] = {0};
            const int n = sscanf(line.c_str(), "%d = %3s %d %d", &tick, kind, &val, &den);
            if (n >= 3 && kind[0] == 'B' && kind[1] == 0)
                bpms.push_back({tick, val / 1000.0});
            else if (n >= 3 && kind[0] == 'T' && kind[1] == 'S')
                // denominator is a power of two; absent -> 4 (Song.Sync.cs:81-93)
                timesigs.push_back({tick, val, den >= 0 ? (1 << den) : 4});
        } else if (section == "Events") {
            int tick = 0;
            char buf[512] = {0};
            double stopSec = 0.0;
            if (sscanf(line.c_str(), "%d = E \"section %511[^\"]\"", &tick, buf) == 2)
                sections.push_back({tick / double(resolution), buf});
            else if (sscanf(line.c_str(), "%d = E \"ncstop %lf\"", &tick, &stopSec) == 2)
                stopTicks.push_back({tick, stopSec});
        } else if (section == track) {
            int tick = 0, a = 0, b = 0; char kind = 0;
            if (sscanf(line.c_str(), "%d = %c %d %d", &tick, &kind, &a, &b) == 4 && kind == 'N') {
                Note& n = byTick[tick];
                n.beat = tick / double(resolution);
                n.tick = tick;
                if (a <= 4)      { n.frets |= (1 << a); if (b > 0) n.sustain[a] = b / double(resolution); }
                else if (a == 5) { n.forced = true; }
                else if (a == 6) { n.tap = true; }
                else if (a == 7) { n.open = true; if (b > 0) n.openSustain = b / double(resolution); }
            } else if (sscanf(line.c_str(), "%d = %c %d %d", &tick, &kind, &a, &b) == 4 &&
                       kind == 'S' && a == 2) {
                phrases.push_back({tick,b,PhraseType::StarPower});
            } else {
                char event[512] = {0};
                if (sscanf(line.c_str(), "%d = E \"%511[^\"]\"", &tick, event) != 2)
                    continue;
                std::string name = nc_trim(event);
                const size_t left = name.find('[');
                const size_t right = left == std::string::npos
                                   ? std::string::npos : name.find(']',left+1);
                if (right != std::string::npos)
                    name = nc_trim(name.substr(left+1,right-left-1));
                if (name == "solo") {
                    if (soloStart < 0) soloStart = tick;
                    else nextSoloStart = tick;
                } else if (name == "soloend" && soloStart >= 0) {
                    phrases.push_back({soloStart,tick-soloStart,PhraseType::Solo});
                    if (nextSoloStart == tick) {
                        soloStart = nextSoloStart;
                        nextSoloStart = -1;
                    } else {
                        soloStart = nextSoloStart = -1;
                    }
                }
            }
        }
    }

    if (bpms.empty()) bpms.push_back({0, 120.0});
    if (timesigs.empty()) timesigs.push_back({0, 4, 4});   // Song.cs:118 default
    std::sort(timesigs.begin(), timesigs.end(),
              [](const TimeSig& x, const TimeSig& y) { return x.tick < y.tick; });
    // stopTicks resolved below, after the tempo map is sorted.
    std::sort(bpms.begin(), bpms.end(),
              [](const BpmPoint& x, const BpmPoint& y) { return x.tick < y.tick; });
    stops.clear();
    for (const auto& st : stopTicks) stops.push_back({tickToSec(st.first), st.second});
    std::sort(stops.begin(), stops.end());

    for (auto& kv : byTick)
        if (kv.second.frets || kv.second.open) notes.push_back(kv.second);

    // The beat schedule -- a faithful port of Chart.cs GetBeats (:177-231).
    // One entry per 1/denominator note. In 4/4 that is one line per beat:
    // MEASURE on the downbeat, BEAT_STRONG on the rest, and WEAK lines only
    // exist when a signature's denominator exceeds 4. The `beats` counter
    // starts at the first signature's numerator so the very first line is a
    // measure (:212), the `>= numerator - 1` comparison and the forced measure
    // on a signature change are as written, and so is the retroactive
    // demotion of the line before a measure (:203-206) -- dead in 4/4, live
    // in x/8. Integer division and all.
    beatlines.clear();
    {
        const int lastTick = notes.empty() ? resolution * 16 : notes.back().tick;
        auto prevTS = [&](int t) -> const TimeSig& {
            const TimeSig* r = &timesigs[0];
            for (const auto& ts : timesigs) { if (ts.tick <= t) r = &ts; else break; }
            return *r;
        };
        const TimeSig* lastTS = nullptr;
        int beatsCtr = prevTS(0).num;
        int tick = 0;
        while (tick <= lastTick) {
            const TimeSig& cur = prevTS(tick);
            const int subBeat = cur.den / 4;
            if (beatsCtr >= cur.num - 1 ||
                (lastTS && lastTS->tick != cur.tick &&
                 (lastTS->num != cur.num || lastTS->den != cur.den))) {
                if (!beatlines.empty() && cur.den > 4 && (beatsCtr % subBeat) == 0)
                    beatlines.back().style = 2;                    // :203-206
                beatsCtr = 0;
                beatlines.push_back({tickToSec(tick), 0});         // MEASURE
            } else {
                ++beatsCtr;
                beatlines.push_back({tickToSec(tick),
                    (cur.den > 4 && (beatsCtr % subBeat) > 0) ? 2 : 1});
            }
            tick += (resolution * 4) / cur.den;
            lastTS = &cur;
        }
    }

    std::sort(notes.begin(), notes.end(),
              [](const Note& x, const Note& y) { return x.beat < y.beat; });
    std::stable_sort(phrases.begin(), phrases.end(),
              [](const Phrase& x, const Phrase& y) {
                  if (x.tick != y.tick) return x.tick < y.tick;
                  return int(x.type) < int(y.type);
              });
    resolveNoteTypes();
    return true;
}

// Ported from Moonscraper Chart Editor (Assets/Scripts/Game/Charts/Events/Note.cs),
// which is the parser Clone Hero uses -- so this IS the canonical rule.
//
//   isNaturalHopo:
//     false if this note is a chord, or there is no previous note
//     else if prevIsChord || rawNote != previous.rawNote
//          then (tick - previous.tick) <= HOPODistance
//   isHopo = isNaturalHopo XOR forced      <- forced INVERTS, it does not set
//   type   = tap ? Tap : (isHopo ? Hopo : Strum)
//
//   HOPODistance = (int)(FORCED_NOTE_TICK_THRESHOLD * resolution / STANDARD)
//                = (int)(65.0f * resolution / 192.0f)   -> 65 ticks at res 192
//
// Moonscraper's `previous` is the immediately preceding note in the flat list
// INCLUDING same-tick chord siblings, not the previous note group. That only
// ever matters for a single note following a chord, because isChord()
// short-circuits first -- and in that case prevIsChord is true, so the
// fret-equality test is skipped anyway. Grouping by tick is therefore safe;
// `type` describes the fretted components and `openType` keeps CH's rule that
// an open component ignores the shared tap flag.
inline void Chart::resolveNoteTypes() {
    const int hopoDist = int(65.0f * float(resolution) / 192.0f);

    for (size_t i = 0; i < notes.size(); ++i) {
        Note& n = notes[i];

        int lanes = 0;
        for (int f = 0; f < 5; ++f) if (n.frets & (1 << f)) ++lanes;
        const bool isChord = lanes + int(n.open) > 1;

        bool natural = false;
        if (!isChord && i > 0) {
            const Note& p = notes[i - 1];
            int plane = 0;
            for (int f = 0; f < 5; ++f) if (p.frets & (1 << f)) ++plane;
            const bool prevIsChord = plane + int(p.open) > 1;

            // rawNote: the single lane, or 7 for an open note
            auto raw = [](const Note& x) {
                if (x.open) return 7;
                for (int f = 0; f < 5; ++f) if (x.frets & (1 << f)) return f;
                return -1;
            };
            if (prevIsChord || raw(n) != raw(p))
                natural = (n.tick - p.tick) <= hopoDist;
        }

        const bool hopo = n.forced ? !natural : natural;
        n.openType = hopo ? NoteType::Hopo : NoteType::Strum;
        n.type = n.tap && n.frets ? NoteType::Tap : n.openType;
    }
}

}  // namespace nc
