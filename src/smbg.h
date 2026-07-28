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

// The SM5 timing subset used by these charts: #OFFSET, #BPMS, #STOPS, with
// legacy negative/huge BPMs normalized into warp segments by the same state
// machine as NotesLoaderSM::ProcessBPMsAndStops.
struct SmTiming {
    double offset = 0.0;                          // #OFFSET, seconds
    std::vector<std::pair<long, double>> bpms;    // (row, bpm), sorted
    std::vector<std::pair<long, double>> stops;   // (row, seconds), sorted
    std::vector<std::pair<long, long>> warps;     // (start row, length), sorted

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
        offset = 0.0;
        bpms.clear(); stops.clear(); warps.clear();
        const std::string s = stripComments(raw);
        std::string v;
        if (tagValue(s, "OFFSET", v)) offset = atof(v.c_str());
        auto pairs = [](const std::string& body) {
            std::vector<std::pair<double, double>> out;
            size_t a = 0;
            while (a <= body.size()) {
                size_t b = body.find(',', a);
                const std::string t = body.substr(
                    a, b == std::string::npos ? std::string::npos : b - a);
                const size_t eq = t.find('=');
                if (eq != std::string::npos) {
                    out.push_back({atof(t.c_str()), atof(t.c_str() + eq + 1)});
                }
                if (b == std::string::npos) break;
                a = b + 1;
            }
            std::stable_sort(out.begin(), out.end());
            return out;
        };
        std::vector<std::pair<double, double>> rawBpms, rawStops;
        if (tagValue(s, "BPMS", v)) rawBpms = pairs(v);
        if (tagValue(s, "STOPS", v)) rawStops = pairs(v);

        constexpr double FAST_BPM_WARP = 9999999.0;
        auto addBpm = [&](double beat, double bpm) {
            bpms.push_back({lrint(beat * 48.0), bpm});
        };
        auto addStop = [&](double beat, double sec) {
            stops.push_back({lrint(beat * 48.0), sec});
        };
        auto addWarp = [&](double start, double end) {
            const long row = lrint(start * 48.0);
            const long len = lrint(end * 48.0) - row;
            if (len > 0) warps.push_back({row, len});
        };

        size_t si = 0, bi = 0;
        while (si < rawStops.size() && rawStops[si].first < 0.0) {
            // m_fBeat0OffsetInSeconds starts at -#OFFSET; SM subtracts a
            // pre-zero stop from it, which is offset += stop in this form.
            offset += rawStops[si].second;
            ++si;
        }
        double bpm = 0.0;
        while (bi < rawBpms.size() && rawBpms[bi].first <= 0.0)
            bpm = rawBpms[bi++].second;
        if (bpm == 0.0) {
            if (bi < rawBpms.size()) bpm = rawBpms[bi++].second;
            else bpm = 60.0;
        }

        double prevBeat = 0.0, warpStart = -1.0, preWarpBpm = 0.0;
        double timeOffset = 0.0;
        if (bpm > 0.0 && bpm <= FAST_BPM_WARP) addBpm(0.0, bpm);

        while (bi < rawBpms.size() || si < rawStops.size()) {
            const bool isBpm = si == rawStops.size() ||
                (bi < rawBpms.size() && rawBpms[bi].first <= rawStops[si].first);
            const auto change = isBpm ? rawBpms[bi] : rawStops[si];
            if (bpm <= FAST_BPM_WARP) {
                timeOffset += (change.first - prevBeat) * 60.0 / bpm;
                if (warpStart >= 0.0 && bpm > 0.0 && timeOffset > 0.0) {
                    const double warpEnd = change.first - timeOffset * bpm / 60.0;
                    addWarp(warpStart, warpEnd);
                    if (bpm != preWarpBpm) addBpm(warpStart, bpm);
                    warpStart = -1.0;
                }
            }
            prevBeat = change.first;

            if (isBpm) {
                if (warpStart < 0.0 &&
                    (change.second < 0.0 || change.second > FAST_BPM_WARP)) {
                    warpStart = change.first;
                    preWarpBpm = bpm;
                    timeOffset = 0.0;
                } else if (warpStart < 0.0) {
                    addBpm(change.first, change.second);
                }
                bpm = change.second;
                ++bi;
            } else {
                if (warpStart < 0.0 && change.second < 0.0) {
                    warpStart = change.first;
                    preWarpBpm = bpm;
                    timeOffset = change.second;
                } else if (warpStart < 0.0) {
                    addStop(change.first, change.second);
                } else {
                    timeOffset += change.second;
                    if (change.second > 0.0 && timeOffset > 0.0) {
                        addWarp(warpStart, change.first);
                        addStop(change.first, timeOffset);
                        if (bpm < 0.0 || bpm > FAST_BPM_WARP) {
                            warpStart = change.first;
                            timeOffset = 0.0;
                        } else {
                            if (bpm != preWarpBpm) addBpm(warpStart, bpm);
                            warpStart = -1.0;
                        }
                    }
                }
                ++si;
            }
        }
        if (warpStart >= 0.0) {
            const double warpEnd = (bpm < 0.0 || bpm > FAST_BPM_WARP)
                ? 99999999.0 : prevBeat - timeOffset * bpm / 60.0;
            addWarp(warpStart, warpEnd);
            if (bpm != preWarpBpm) addBpm(warpStart, bpm);
        }

        std::stable_sort(bpms.begin(), bpms.end());
        std::stable_sort(stops.begin(), stops.end());
        std::stable_sort(warps.begin(), warps.end());
        // TimingData keeps one segment of each type at a row. Later BPMs win;
        // simultaneous stops add.
        std::vector<std::pair<long, double>> cleanBpms;
        for (const auto& p : bpms) {
            if (!cleanBpms.empty() && cleanBpms.back().first == p.first)
                cleanBpms.back().second = p.second;
            else cleanBpms.push_back(p);
        }
        bpms.swap(cleanBpms);
        std::vector<std::pair<long, double>> cleanStops;
        for (const auto& p : stops) {
            if (!cleanStops.empty() && cleanStops.back().first == p.first)
                cleanStops.back().second += p.second;
            else cleanStops.push_back(p);
        }
        stops.swap(cleanStops);
        if (bpms.empty()) bpms.push_back({0, 60.0});
    }

    // Elapsed audio seconds at a source-SM beat. Warp ranges contribute zero
    // time; a stop at exactly `beat` has not happened yet.
    double beatToSec(double beat) const {
        double t = -offset;
        if (beat < 0.0) return t + beat * 60.0 / bpms.front().second;
        for (const auto& s : stops) {
            if (double(s.first) / 48.0 >= beat) break;
            t += s.second;
        }
        for (size_t i = 0; i < bpms.size(); ++i) {
            const double start = std::max(0.0, double(bpms[i].first) / 48.0);
            const double end = std::min(beat, i + 1 < bpms.size()
                ? double(bpms[i + 1].first) / 48.0 : beat);
            if (end <= start) continue;
            double visible = end - start;
            for (const auto& w : warps) {
                const double ws = double(w.first) / 48.0;
                const double we = double(w.first + w.second) / 48.0;
                visible -= std::max(0.0, std::min(end, we) - std::max(start, ws));
            }
            t += std::max(0.0, visible) * 60.0 / bpms[i].second;
            if (end == beat) break;
        }
        return t;
    }

    // Largest source beat reached at `sec`. This definition naturally holds a
    // stop at its row and chooses the far end of a zero-time warp.
    double secToBeat(double sec) const {
        double lo = -64.0, hi = 1.0;
        while (beatToSec(hi) <= sec && hi < 134217728.0) hi *= 2.0;
        for (int i = 0; i < 80; ++i) {
            const double mid = (lo + hi) * 0.5;
            if (beatToSec(mid) <= sec) lo = mid; else hi = mid;
        }
        return lo;
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
