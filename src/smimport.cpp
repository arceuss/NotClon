#include "smimport.h"

#include "chart.h"
#include "modfile.h"
#include "stems.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <map>
#include <filesystem>

namespace nc {

namespace {

// --- MSD ------------------------------------------------------------------
// MsdFile.cpp:34-118. Only the parts that matter here: `//` is erased to end of
// line before anything else (MsdFile.cpp:43-52), `#` opens a value, `:` splits
// params, `;` closes. There is no escaping (MsdFile.cpp:14-17).
struct MsdValue { std::vector<std::string> params; };

std::vector<MsdValue> msdParse(const std::string& raw) {
    std::string t = raw;
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (t[i] == '/' && t[i + 1] == '/') {
            while (i < t.size() && t[i] != '\n') t[i++] = ' ';
        }
    }
    std::vector<MsdValue> out;
    size_t i = 0;
    while (i < t.size()) {
        if (t[i] != '#') { ++i; continue; }
        ++i;
        MsdValue v;
        std::string cur;
        while (i < t.size() && t[i] != ';') {
            // A '#' at the start of a line recovers from a missing ';'
            // (MsdFile.cpp:55-81).
            if (t[i] == '#') {
                size_t j = i;
                while (j > 0 && (t[j - 1] == ' ' || t[j - 1] == '\t')) --j;
                if (j == 0 || t[j - 1] == '\n') break;
            }
            if (t[i] == ':') { v.params.push_back(cur); cur.clear(); ++i; continue; }
            cur += t[i++];
        }
        v.params.push_back(cur);
        if (i < t.size() && t[i] == ';') ++i;
        if (!v.params.empty()) out.push_back(v);
    }
    return out;
}

std::string trim(const std::string& s) { return nc_trim(s); }

std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c -= 32;
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(d, a);
        out.push_back(s.substr(a, b == std::string::npos ? b : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

// --- 4 panel -> 5 fret ------------------------------------------------------
// AGENTS.md open work section 5 lists three candidate strategies. This is a
// CHOICE, not a derivation, and it is the conservative one:
//
//   Left -> green(0)   Down -> red(1)   Up -> yellow(2)   Right -> blue(3)
//
// Direct 4->4, leaving ORANGE UNUSED. The alternatives were rejected for this
// first cut: spreading 4 across 5 lanes makes every jump a non-adjacent pair
// and destroys the left-to-right shape a dance chart is written in, and
// chording up to 5 invents notes the author never wrote. Keeping the panels in
// their own order means a stair reads as a stair. The cost is that the orange
// fret is never lit, which looks odd on a 5-fret highway and is the honest
// signal that this chart came from four panels.
const int PANEL_TO_FRET[4] = {0, 1, 2, 3};

struct SmNote { int tick; int fret; int len; };   // len in ticks, 0 = tap

}  // namespace

SmImportReport importSm(const std::string& smPath, const std::string& outDir,
                        const SmImportOpts& opts) {
    SmImportReport R;

    FILE* f = fopen(smPath.c_str(), "rb");
    if (!f) { R.error = "cannot open " + smPath; return R; }
    std::string raw;
    { char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) raw.append(b, n); }
    fclose(f);
    if (raw.size() > 3 && (unsigned char)raw[0] == 0xEF) raw = raw.substr(3);

    const std::vector<MsdValue> vals = msdParse(raw);

    // --- header -------------------------------------------------------------
    double offset = 0.0;
    std::vector<std::pair<double, double>> bpms;      // (beat, bpm)
    std::vector<std::pair<double, double>> stops;     // (beat, seconds)
    struct NotesBlock { std::string type, desc, diff; int meter; std::string body; };
    std::vector<NotesBlock> blocks;
    std::vector<std::string> modLines;

    for (const MsdValue& v : vals) {
        if (v.params.empty()) continue;
        const std::string tag = upper(trim(v.params[0]));
        const std::string p1  = v.params.size() > 1 ? v.params[1] : "";

        if      (tag == "TITLE")  R.title  = trim(p1);
        else if (tag == "ARTIST") R.artist = trim(p1);
        else if (tag == "MUSIC")  R.music  = trim(p1);
        else if (tag == "OFFSET") offset = atof(trim(p1).c_str());
        else if (tag == "BPMS") {
            for (const std::string& e : split(p1, ',')) {
                const std::string s = trim(e);
                if (s.empty()) continue;
                const size_t eq = s.find('=');
                if (eq == std::string::npos) continue;
                bpms.push_back({atof(s.substr(0, eq).c_str()),
                                atof(s.substr(eq + 1).c_str())});
            }
        } else if (tag == "STOPS" || tag == "FREEZES") {
            for (const std::string& e : split(p1, ',')) {
                const std::string t = trim(e);
                if (t.empty()) continue;
                const size_t eq = t.find('=');
                if (eq == std::string::npos) continue;
                stops.push_back({atof(t.substr(0, eq).c_str()),
                                 atof(t.substr(eq + 1).c_str())});
                ++R.stops;
            }
        } else if (tag == "MODS" || tag == "ATTACKS") {
            // #ATTACKS is the same TIME=/LEN=/MODS= grammar as #MODS -- it is
            // the tag StepMania itself writes, where #MODS is the OpenITG
            // spelling. Saitama2000 uses #ATTACKS exclusively, and reading only
            // #MODS silently produced an empty modchart.
            //
            // Rejoin: the whole value after the tag is one line, and it
            // contains ':' separators we split on above.
            std::string joined;
            for (size_t i = 1; i < v.params.size(); ++i) {
                if (i > 1) joined += ":";
                joined += v.params[i];
            }
            modLines.push_back(joined);
        } else if (tag == "FGCHANGES") {
            // Only to explain an empty .ncmod. A chart whose modchart is a Lua
            // actor tree has no #MODS at all, and the first field of the first
            // change names the folder it lives in.
            for (size_t i = 1; i < v.params.size() && R.fgActor.empty(); ++i) {
                const std::string t = trim(v.params[i]);
                const size_t a = t.find('=');
                if (a == std::string::npos) continue;
                const size_t b = t.find('=', a + 1);
                std::string nm = t.substr(a + 1, b == std::string::npos
                                                 ? std::string::npos : b - a - 1);
                if (!nm.empty()) R.fgActor = nm;
            }
        } else if (tag == "NOTES" || tag == "NOTES2") {
            if (v.params.size() < 7) continue;
            NotesBlock b;
            b.type  = trim(v.params[1]);
            b.desc  = trim(v.params[2]);
            b.diff  = trim(v.params[3]);
            b.meter = atoi(trim(v.params[4]).c_str());
            b.body  = v.params[6];
            blocks.push_back(b);
        }
    }

    if (bpms.empty()) { R.error = "no #BPMS"; return R; }
    std::stable_sort(bpms.begin(), bpms.end(),
                     [](const std::pair<double,double>& a,
                        const std::pair<double,double>& b) { return a.first < b.first; });
    R.bpmPoints = int(bpms.size());
    R.diffCount = int(blocks.size());
    if (blocks.empty()) { R.error = "no #NOTES block"; return R; }
    // #STOPS -> BPM wedges. .chart has no stop event, but a stop is exactly
    // expressible: insert a segment of dt ticks whose bpm makes crossing it
    // take S seconds, and shift every tick strictly after the stop by dt. Then
    //   t(x*res + dt) = t(stopTick) + S + (x - B)*60/bpm  ==  t_sm(x) + S
    // identically -- audio-exact in real CH, not an approximation. A note AT
    // the stop beat is not shifted: SM's TimingData uses >=, "the exact beat
    // of a stop comes before the stop".
    //
    // dt is chosen so the milli-bpm .chart stores is integral (search 1..192);
    // when no dt is exact the residual is bounded by 0.0016*S^2/dt seconds.
    // The stop is also written as an `E "ncstop <S>"` marker so NotClon's
    // renderer can freeze the scroll axis for the SM look; CH ignores it.
    struct Wedge { double beat, sec; int tick, dt; long long mbpm; };
    std::vector<Wedge> wedges;
    std::stable_sort(stops.begin(), stops.end());
    {
        const int RES0 = 192;
        int shift = 0;
        for (const auto& sp : stops) {
            if (sp.second <= 0.0) {
                R.warnings.push_back("negative/zero stop at beat " +
                                     std::to_string(sp.first) + " dropped (a warp, not a stop)");
                continue;
            }
            int bestDt = 1; long long bestM = 1; double bestErr = 1e18;
            for (int dt = 1; dt <= 192; ++dt) {
                const double m = dt * 60000.0 / (192.0 * sp.second);
                long long mr = (long long)(m + 0.5);
                if (mr < 1) mr = 1;
                const double sAct = dt * 312.5 / double(mr);
                const double err = sAct > sp.second ? sAct - sp.second : sp.second - sAct;
                if (err < bestErr - 1e-15) { bestErr = err; bestDt = dt; bestM = mr; }
                if (bestErr < 1e-9) break;
            }
            Wedge w;
            w.beat = sp.first; w.sec = sp.second;
            w.tick = int(sp.first * RES0 + 0.5) + shift;
            w.dt = bestDt; w.mbpm = bestM;
            wedges.push_back(w);
            shift += bestDt;
            char msg[160];
            snprintf(msg, sizeof msg,
                     "stop at beat %g (%gs) -> wedge B %lld over %d tick%s, residual %.3g us",
                     sp.first, sp.second, w.mbpm, w.dt, w.dt == 1 ? "" : "s",
                     bestErr * 1e6);
            R.warnings.push_back(msg);
        }
    }
    // ticks strictly after a stop's beat shift by that stop's width
    auto shiftAt = [&](double beat) {
        int sh = 0;
        for (const auto& w : wedges) if (w.beat < beat - 1e-9) sh += w.dt;
        return sh;
    };

    // --- pick a chart -------------------------------------------------------
    int pick = opts.diffIndex;
    if (pick < 0) {
        int bestMeter = -1;
        for (int i = 0; i < int(blocks.size()); ++i) {
            if (lower(blocks[i].type) != "dance-single") continue;
            if (blocks[i].meter > bestMeter) { bestMeter = blocks[i].meter; pick = i; }
        }
        if (pick < 0) pick = 0;
    }
    if (pick >= int(blocks.size())) { R.error = "no such difficulty index"; return R; }
    const NotesBlock& NB = blocks[pick];
    R.chosenDiff = NB.type + " " + NB.diff + " " + std::to_string(NB.meter);
    if (lower(NB.type) != "dance-single")
        R.warnings.push_back("block is '" + NB.type +
                             "', not dance-single; the 4-panel map assumes 4 columns");

    // --- notes --------------------------------------------------------------
    const int RES = 192;                        // project standard
    const int TICKS_PER_MEASURE = RES * 4;

    std::vector<SmNote> notes;
    int openHead[4] = {-1, -1, -1, -1};         // tick of an unclosed hold head

    const std::vector<std::string> measures = split(NB.body, ',');
    for (int mi = 0; mi < int(measures.size()); ++mi) {
        std::vector<std::string> rows;
        for (const std::string& ln : split(measures[mi], '\n')) {
            const std::string s = trim(ln);
            if (!s.empty()) rows.push_back(s);
        }
        if (rows.empty()) continue;
        const int RN = int(rows.size());
        if (TICKS_PER_MEASURE % RN != 0)
            R.warnings.push_back("measure " + std::to_string(mi) + " has " +
                                 std::to_string(RN) + " rows, which does not divide " +
                                 std::to_string(TICKS_PER_MEASURE) + " ticks; rounded");
        for (int r = 0; r < RN; ++r) {
            const int rawTick = mi * TICKS_PER_MEASURE + int(double(r) * TICKS_PER_MEASURE / RN + 0.5);
            const int tick = rawTick + shiftAt(rawTick / 192.0);
            const std::string& row = rows[r];
            for (int c = 0; c < 4 && c < int(row.size()); ++c) {
                const char ch = row[c];
                const int fret = PANEL_TO_FRET[c];
                switch (ch) {
                    case '1': notes.push_back({tick, fret, 0}); ++R.notes; break;
                    case 'L': notes.push_back({tick, fret, 0}); ++R.notes; break;  // lift
                    case '2': openHead[c] = tick; break;
                    case '4': openHead[c] = tick; ++R.rolls; break;                // roll -> hold
                    case '3':
                        if (openHead[c] >= 0) {
                            notes.push_back({openHead[c], fret, tick - openHead[c]});
                            ++R.notes; ++R.holds;
                            openHead[c] = -1;
                        }
                        break;
                    case 'M': ++R.mines; break;     // no CH equivalent
                    default: break;                 // '0', 'F' (fake), anything else
                }
            }
        }
    }
    if (R.mines)
        R.warnings.push_back(std::to_string(R.mines) +
                             " mines dropped -- CH has no mine");
    if (R.rolls)
        R.warnings.push_back(std::to_string(R.rolls) +
                             " rolls converted to plain holds");
    for (int c = 0; c < 4; ++c)
        if (openHead[c] >= 0)
            R.warnings.push_back("unterminated hold on panel " + std::to_string(c) +
                                 " at tick " + std::to_string(openHead[c]) + " -- dropped");

    std::stable_sort(notes.begin(), notes.end(),
                     [](const SmNote& a, const SmNote& b) {
                         return a.tick != b.tick ? a.tick < b.tick : a.fret < b.fret;
                     });

    // --- write notes.chart --------------------------------------------------
    const std::string chartPath = outDir + "/notes.chart";
    FILE* o = fopen(chartPath.c_str(), "wb");
    if (!o) { R.error = "cannot write " + chartPath; return R; }
    fprintf(o, "[Song]\n{\n");
    fprintf(o, "  Name = \"%s\"\n", R.title.c_str());
    fprintf(o, "  Artist = \"%s\"\n", R.artist.c_str());
    fprintf(o, "  Charter = \"sm2chart\"\n");
    // #MUSIC, so the imported song keeps its own filename instead of having to
    // be renamed song.ogg. This is CH's own [Song] field, not an invention.
    if (!R.music.empty()) fprintf(o, "  MusicStream = \"%s\"\n", R.music.c_str());
    fprintf(o, "  Resolution = %d\n", RES);
    // SM: audio(beat0) = -#OFFSET (TimingData::GetElapsedTimeFromBeat starts
    // at -offset). CH: audio(beat0) = +Offset (TimeSync.cs:90 via
    // GameManager.cs:484-492). OPPOSITE signs, so the value flips here.
    // Saitama2000's #OFFSET -2.733 becomes Offset 2.733: beat 0 plays 2.733s
    // into the audio in both engines.
    fprintf(o, "  Offset = %g\n", -offset);
    fprintf(o, "}\n[SyncTrack]\n{\n");
    {
        // Assignment order matters: a bpm change AT a stop's beat shares the
        // wedge's start tick, and the wedge must win there -- the change takes
        // effect at the resume point instead. std::map iteration gives sorted
        // ticks; later assignment to the same key is the winner.
        auto bpmAtBeat = [&](double beat) {
            double r = bpms[0].second;
            for (const auto& b : bpms) if (b.first <= beat + 1e-9) r = b.second;
            return r;
        };
        std::map<int, long long> sync;
        for (const auto& b : bpms)
            sync[int(b.first * RES + 0.5) + shiftAt(b.first)] =
                (long long)(b.second * 1000.0 + 0.5);
        for (const auto& w : wedges) {
            sync[w.tick] = w.mbpm;
            sync[w.tick + w.dt] = (long long)(bpmAtBeat(w.beat) * 1000.0 + 0.5);
        }
        for (const auto& kv : sync)
            fprintf(o, "  %d = B %lld\n", kv.first, kv.second);
    }
    fprintf(o, "}\n[Events]\n{\n");
    for (const auto& w : wedges)
        fprintf(o, "  %d = E \"ncstop %.6f\"\n", w.tick, w.sec);
    fprintf(o, "}\n");
    fprintf(o, "[ExpertSingle]\n{\n");
    for (const SmNote& n : notes)
        fprintf(o, "  %d = N %d %d\n", n.tick, n.fret, n.len);
    fprintf(o, "}\n");
    fclose(o);

    // Real CH loads audio ONLY by the fixed stem names (BassAudioManager.cs:
    // 212 probes songFolder/<stem>.<ext>; MusicStream is parsed and never
    // used). A folder whose audio is named anything else is SILENT in CH. If
    // no stem exists, hard-link the .sm's audio as song.<ext> -- free on the
    // same NTFS volume -- falling back to a copy.
    if (!R.music.empty()) {
        bool haveStem = false;
        for (const char* stem : CH_STEMS)
            for (const char* ext : CH_AUDIO_EXTS) {
                if (FILE* f = fopen((outDir + "/" + stem + ext).c_str(), "rb")) {
                    fclose(f); haveStem = true;
                }
            }
        const size_t dot = R.music.find_last_of('.');
        const std::string srcAudio = outDir + "/" + R.music;
        if (!haveStem && dot != std::string::npos) {
            const std::string dst = outDir + "/song" + R.music.substr(dot);
            std::error_code ec;
            std::filesystem::create_hard_link(srcAudio, dst, ec);
            if (ec) std::filesystem::copy_file(srcAudio, dst, ec);
            if (!ec)
                R.warnings.push_back("linked " + R.music +
                                     " as song" + R.music.substr(dot) +
                                     " -- CH only loads fixed stem names");
            else
                R.warnings.push_back("could not create song" + R.music.substr(dot) +
                                     ": " + ec.message() +
                                     " -- rename the audio by hand or CH will be silent");
        }
    }

    // song.ini. CH reads [song] delay in ms and prefers it over the chart's
    // Offset when nonzero (SongEntry.cs:290, GameManager.cs:491-492), and it
    // is what the wider CH ecosystem keys on -- so a converted folder carries
    // both, agreeing. Created if absent; if one exists, only the delay line is
    // rewritten, because the rest of it belongs to the user.
    {
        const std::string iniPath = outDir + "/song.ini";
        const long long delayMs = (long long)((-offset) * 1000.0 +
                                              ((-offset) >= 0 ? 0.5 : -0.5));
        FILE* fi = fopen(iniPath.c_str(), "rb");
        if (!fi) {
            FILE* w = fopen(iniPath.c_str(), "wb");
            if (w) {
                fprintf(w, "[song]\n");
                fprintf(w, "name = %s\n", R.title.c_str());
                fprintf(w, "artist = %s\n", R.artist.c_str());
                fprintf(w, "charter = sm2chart\n");
                fprintf(w, "delay = %lld\n", delayMs);
                fclose(w);
            }
        } else {
            std::string txt;
            { char b[8192]; size_t n;
              while ((n = fread(b, 1, sizeof b, fi)) > 0) txt.append(b, n); }
            fclose(fi);
            const size_t dp = txt.find("delay");
            char line[64];
            snprintf(line, sizeof line, "delay = %lld", delayMs);
            if (dp != std::string::npos) {
                const size_t eol = txt.find_first_of("\r\n", dp);
                txt.replace(dp, (eol == std::string::npos ? txt.size() : eol) - dp, line);
            } else {
                const size_t sect = txt.find("[song]");
                const size_t ins = sect == std::string::npos
                                 ? 0 : txt.find('\n', sect) + 1;
                txt.insert(ins == std::string::npos ? txt.size() : ins,
                           std::string(line) + "\n");
            }
            FILE* w = fopen(iniPath.c_str(), "wb");
            if (w) { fwrite(txt.data(), 1, txt.size(), w); fclose(w); }
            R.warnings.push_back("song.ini existed; rewrote only its delay line");
        }
        if (delayMs != 0)
            R.warnings.push_back("song.ini delay set to " + std::to_string(delayMs) +
                                 " ms (from #OFFSET " + std::to_string(offset) + ")");
    }

    // Reload through the real parser so the reported note-type split is what
    // NotClon will actually render, not what this file guessed.
    Chart back;
    if (!back.load(chartPath)) { R.error = "wrote " + chartPath + " but cannot read it back"; return R; }
    for (const Note& n : back.notes) {
        int lanes = 0;
        for (int l = 0; l < 5; ++l) if (n.frets & (1 << l)) ++lanes;
        if      (n.type == NoteType::Tap)  R.tap  += lanes;
        else if (n.type == NoteType::Hopo) R.hopo += lanes;
        else                               R.strum += lanes;
    }
    R.lastSec = back.notes.empty() ? 0.0 : back.beatToSec(back.notes.back().beat);

    if (opts.allStrum && R.hopo)
        R.warnings.push_back("--sm-strum is not implemented; "
                             + std::to_string(R.hopo) + " notes resolved to HOPO");
    else if (R.hopo > R.strum * 4)
        R.warnings.push_back(std::to_string(R.hopo) + " of " +
                             std::to_string(R.hopo + R.strum + R.tap) +
                             " notes resolve to HOPO -- expected for a dense chart at this "
                             "BPM, and it is what CH itself would do");

    // --- #MODS: -> .ncmod ---------------------------------------------------
    R.modLines = int(modLines.size());
    ModDoc doc;
    // Absolute: a .ncmod is opened from anywhere, so a path relative to
    // whatever the importer's cwd happened to be is a path that breaks.
    { std::error_code ec;
      const auto abs = std::filesystem::absolute(outDir, ec);
      doc.chartDir = ec ? outDir : abs.generic_string(); }
    std::vector<std::string> unknown;

    for (const std::string& line : modLines) {
        double timeSec = 0.0, lenSec = 0.0;
        bool haveLen = false;
        std::string modsField;
        for (const std::string& part : split(line, ':')) {
            const size_t eq = part.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = upper(trim(part.substr(0, eq)));
            const std::string v = trim(part.substr(eq + 1));
            if      (k == "TIME")   timeSec = atof(v.c_str());
            else if (k == "LEN")  { lenSec = atof(v.c_str()); haveLen = true; }
            else if (k == "END")  { lenSec = atof(v.c_str()) - timeSec; haveLen = true; }
            else if (k == "MODS")   modsField = v;
        }
        if (modsField.empty()) continue;

        // TIME=/END= are AUDIO-FILE seconds: OITG compares fStartSecond
        // against m_fMusicSeconds (GameState.cpp:656), whose origin is the
        // music file, offset already folded in. back.secToBeat expects exactly
        // that timebase, so the conversion is the identity -- do NOT add the
        // chart offset here. (It was added once; on a -2.733 offset that
        // shifted every mod 2.7s early, and it stayed invisible on the charts
        // whose offset happened to be 0.)
        const int tick = int(back.secToBeat(timeSec) * RES + 0.5);
        int len = 0;
        if (haveLen && lenSec > 0.0) {
            const int endTick = int(back.secToBeat(timeSec + lenSec) * RES + 0.5);
            len = endTick - tick;
            if (len < 1) len = 1;      // a window that rounds away is worse than a 1-tick one
        }

        for (const std::string& tokRaw : split(modsField, ',')) {
            std::string tok = trim(tokRaw);
            if (tok.empty()) continue;

            float approach = -1.0f;    // no '*' means snap, as OITG treats a bare token
            float percent  = 1.0f;
            // `*<rate>` prefix
            if (tok[0] == '*') {
                const size_t sp = tok.find(' ');
                approach = float(atof(tok.substr(1, sp - 1).c_str()));
                tok = (sp == std::string::npos) ? "" : trim(tok.substr(sp + 1));
            }
            if (tok.empty()) continue;
            // `<n>%` prefix
            const size_t pc = tok.find('%');
            if (pc != std::string::npos) {
                const std::string head = trim(tok.substr(0, pc));
                bool numeric = !head.empty();
                for (char c : head)
                    if (!(isdigit((unsigned char)c) || c == '.' || c == '-' || c == '+'))
                        numeric = false;
                if (numeric) {
                    percent = float(atof(head.c_str())) / 100.0f;
                    tok = trim(tok.substr(pc + 1));
                }
            }
            // `No <mod>` == 0%
            if (lower(tok).compare(0, 3, "no ") == 0) {
                percent = 0.0f;
                tok = trim(tok.substr(3));
            }
            if (tok.empty()) continue;

            std::string name = lower(tok);
            // The perspective family is one signed knob (PlayerOptions.cpp:
            // 349-353): hallway = -tilt, distant = +tilt, overhead -> 0.
            // incoming/space are hallway/distant PLUS m_fSkew -- and skew is
            // provably a no-op here: Player.cpp:707,716 lerps the frustum's
            // vanishing point from the notefield's own X toward SCREEN_CENTER_X
            // (LoadMenuPerspective, RageDisplay.cpp:449-495), so its entire
            // visible effect requires an OFF-CENTRE field (P1/P2 side by side).
            // NotClon's one highway is centred: GetX() == SCREEN_CENTER_X and
            // skew is the identity at every value. The tilt half imports
            // losslessly; the skew half is dropped with proof, not lossily.
            if      (name == "hallway")  { name = "tilt"; percent = -percent; }
            else if (name == "distant")  { name = "tilt"; }
            else if (name == "overhead") { name = "tilt"; percent = 0.0f; }
            else if (name == "incoming") {
                name = "tilt"; percent = -percent;
                fprintf(stderr, "  note: incoming -> tilt (its skew half is "
                        "identically zero for a centred field)\n");
            }
            else if (name == "space") {
                name = "tilt";
                fprintf(stderr, "  note: space -> tilt (its skew half is "
                        "identically zero for a centred field)\n");
            }
            int id = modFromName(name);
            if (id < 0 && name.size() > 1 && name.back() == 'x') {
                // "2x" and friends -- a scroll-speed multiplier.
                bool numeric = true;
                for (size_t i = 0; i + 1 < name.size(); ++i)
                    if (!(isdigit((unsigned char)name[i]) || name[i] == '.')) numeric = false;
                if (numeric) {
                    id = MOD_SCROLLSPEED;
                    percent = float(atof(name.c_str()));
                }
            }
            if (id < 0) {
                if (std::find(unknown.begin(), unknown.end(), name) == unknown.end())
                    unknown.push_back(name);
                continue;
            }
            if (modIsStub(id) &&
                std::find(R.stubbed.begin(), R.stubbed.end(), name) == R.stubbed.end())
                R.stubbed.push_back(name);

            doc.entries.push_back(ModEntry{tick, id, percent, approach, len, true});
            ++R.modEntries;
            if (len > 0) ++R.modIntervals;
        }
    }
    for (const std::string& u : unknown)
        R.warnings.push_back("unknown mod token '" + u + "' -- dropped");

    doc.rebuild(back);
    std::string base = smPath;
    { const size_t s = base.find_last_of("/\\"); if (s != std::string::npos) base = base.substr(s + 1); }
    { const size_t d = base.find_last_of('.');   if (d != std::string::npos) base = base.substr(0, d); }
    if (!doc.save(outDir + "/" + base + ".ncmod")) {
        R.error = "cannot write " + outDir + "/" + base + ".ncmod";
        return R;
    }

    R.ok = true;
    return R;
}

}  // namespace nc
