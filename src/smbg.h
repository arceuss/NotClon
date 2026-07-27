// .sm #BGCHANGES parsing and just enough of StepMania's TimingData to resolve
// a change's beat to audio-file seconds. Header-only, in the style of chart.h.
//
// The .sm and the imported .chart are two timelines anchored to the SAME audio
// file, and the renderer's songTime is a timestamp into that file
// (chart.tickToSec returns sec + offset, the -ss ffmpeg gets). So everything
// here resolves to seconds against the .sm's OWN timing once, at load, and
// .sm beats are never mentioned again at render time
// (devdocs/spec/background.md section 1.6).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace nc {

// Just enough of TimingData: #OFFSET, #BPMS, #STOPS.
struct SmTiming {
    double offset = 0.0;                          // #OFFSET, seconds
    std::vector<std::pair<long, double>> bpms;    // (row, bpm), sorted
    std::vector<std::pair<long, double>> stops;   // (row, seconds), sorted

    // MSD rule: '//' to end-of-line is erased to spaces before anything else
    // (MsdFile.cpp:43-52), so a comment inside a value cannot leak into it.
    static std::string stripComments(const std::string& raw) {
        std::string s = raw;
        for (size_t p = s.find("//"); p != std::string::npos; p = s.find("//", p)) {
            size_t e = s.find('\n', p);
            if (e == std::string::npos) e = s.size();
            for (size_t i = p; i < e; ++i) s[i] = ' ';
            p = e;
        }
        return s;
    }

    static bool tagValue(const std::string& s, const char* tag, std::string& out) {
        const std::string k = std::string("#") + tag + ":";
        const size_t p = s.find(k);
        if (p == std::string::npos) return false;
        const size_t e = s.find(';', p);
        out = s.substr(p + k.size(),
                       e == std::string::npos ? std::string::npos : e - p - k.size());
        return true;
    }

    // `raw` is the whole .sm text; comments may or may not be stripped already.
    void parse(const std::string& raw) {
        const std::string s = stripComments(raw);
        std::string v;
        if (tagValue(s, "OFFSET", v)) offset = atof(v.c_str());
        auto pairs = [](const std::string& body,
                        std::vector<std::pair<long, double>>& out) {
            size_t a = 0;
            while (a <= body.size()) {
                size_t b = body.find(',', a);
                const std::string t = body.substr(
                    a, b == std::string::npos ? std::string::npos : b - a);
                const size_t eq = t.find('=');
                if (eq != std::string::npos) {
                    // BeatToNoteRow quantisation, NoteTypes.h:156,203.
                    const long row = lrint(atof(t.c_str()) * 48.0);
                    out.push_back({row, atof(t.c_str() + eq + 1)});
                }
                if (b == std::string::npos) break;
                a = b + 1;
            }
            std::sort(out.begin(), out.end());
        };
        if (tagValue(s, "BPMS", v)) pairs(v, bpms);
        if (tagValue(s, "STOPS", v)) pairs(v, stops);
        if (bpms.empty()) bpms.push_back({0, 120.0});
    }

    // Port of TimingData::GetElapsedTimeFromBeat (openitg TimingData.cpp:
    // 252-290), minus the m_fGlobalOffsetSeconds user preference. Quantises to
    // rows with lrint(beat*48) exactly as BeatToNoteRow does, so a change at
    // beat 224.0000001 still counts against the stop row at 224.
    double beatToSec(double beat) const {
        double t = -offset;
        const long row = lrint(beat * 48.0);
        for (const auto& s : stops) {
            // "The exact beat of a stop comes before the stop, not after, so
            // use >=, not >." -- TimingData.cpp:261-263. A change AT the stop
            // beat excludes its own stop.
            if (s.first >= row) break;
            t += s.second;
        }
        long r = row;
        for (size_t i = 0; i < bpms.size(); ++i) {
            const double bps = bpms[i].second / 60.0;
            if (i + 1 == bpms.size()) return t + (double(r) / 48.0) / bps;
            const long rowsIn = std::min(bpms[i + 1].first - bpms[i].first, r);
            t += (double(rowsIn) / 48.0) / bps;
            r -= rowsIn;
            if (r <= 0) return t;
        }
        return t;
    }
};

enum class BgMedia { Still, Video };

struct SmBgChange {
    double      startBeat = -1.0;   // field 1, as written
    double      startSec  = 0.0;    // resolved once, at load
    std::string file1, file2;       // fields 2, 8 -- as written
    std::string path1;              // file1 resolved against the song dir
    BgMedia     media = BgMedia::Still;
    double      rate = 1.0;         // field 3
    std::string effect;             // field 7 (or the field 5/6 fallback)
    std::string transition;         // field 9 (or the field 4 fallback)
    std::string color1, color2;     // fields 10, 11 -- parsed, never consumed:
                                    // OITG only exposes Color1 to Lua and no
                                    // BackgroundEffects xml reads it, so OITG
                                    // renders these untinted. So do we.
    bool loops() const { return effect != "StretchNoLoop"; }
};

struct SmBgList {
    SmTiming timing;
    std::vector<SmBgChange> changes;   // stable_sorted by startBeat

    // Last change whose startSec <= sec, else -1 (Background.cpp:709-724's
    // FindBGSegmentForBeat, on the resolved seconds).
    int indexAt(double sec) const {
        int lo = 0, hi = int(changes.size());
        while (lo < hi) {
            const int m = (lo + hi) / 2;
            if (changes[m].startSec <= sec) lo = m + 1; else hi = m;
        }
        return lo - 1;
    }
};

// One #BGCHANGES entry, already comma-split. Field decode is verbatim
// LoadFromBGChangesString (openitg NotesLoaderSM.cpp:163-226): '^' unescapes
// to ',' across the whole expression, '=' split KEEPS empty tokens, and the
// switch falls through high-to-low so fields 7/9 are assigned before the
// backward-compat cases 6/5/4 read them.
inline bool smBgDecode(const std::string& expr, SmBgChange& c) {
    std::string s = expr;
    for (char& ch : s) if (ch == '^') ch = ',';
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find('=', a);
        f.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    if (f.size() > 11) f.resize(11);
    const size_t n = f.size();
    if (n >= 11) c.color2 = f[10];
    if (n >= 10) c.color1 = f[9];
    if (n >= 9)  c.transition = f[8];
    if (n >= 8)  c.file2 = f[7];
    if (n >= 7)  c.effect = f[6];
    if (n >= 6 && atof(f[5].c_str()) == 0.0 && c.effect.empty())
        c.effect = "StretchNoLoop";                          // loop, inverted
    if (n >= 5 && atof(f[4].c_str()) != 0.0 && c.effect.empty())
        c.effect = "StretchRewind";                          // rewind movie
    if (n >= 4 && atof(f[3].c_str()) != 0.0 && c.transition.empty())
        c.transition = "CrossFade";                          // crossfade
    if (n >= 3)  c.rate = atof(f[2].c_str());
    if (n >= 2) { c.file1 = f[1]; c.startBeat = atof(f[0].c_str()); }
    return n >= 2;
}

}  // namespace nc
