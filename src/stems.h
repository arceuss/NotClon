// Audio discovery, the Clone Hero way.
//
// CH does not read [Song] MusicStream for playback -- Song.Header.cs parses it
// and nothing consumes it. BassAudioManager.cs:212 loads audio purely by
// probing songFolder/<stem>.<ext> over two fixed tables (:49-71), and EVERY
// stem found plays simultaneously. So "the song's audio" is a LIST, and a
// folder whose audio is named anything else is silent in real CH.
//
// NotClon keeps MusicStream as a last-resort fallback (our own importer wrote
// it before this was understood), and accepts .opus as an extra container --
// ffmpeg decodes it even though this CH build's table does not list it.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace nc {

// BassAudioManager.cs:49-64, in CH's order.
static const char* const CH_STEMS[] = {
    "guitar", "bass", "rhythm", "vocals", "vocals_1", "vocals_2",
    "drums", "drums_1", "drums_2", "drums_3", "drums_4",
    "keys", "song", "crowd",
};
// :66-71, plus opus as a NotClon extra.
static const char* const CH_AUDIO_EXTS[] = {".ogg", ".mp3", ".wav", ".opus"};

// Every stem file present in `dir`, CH's probe order. Falls back to
// `musicStream` (resolved against dir) only when no stem exists at all.
inline std::vector<std::string> findAudioStems(const std::string& dir,
                                               const std::string& musicStream) {
    std::vector<std::string> out;
    for (const char* stem : CH_STEMS) {
        for (const char* ext : CH_AUDIO_EXTS) {
            const std::string p = dir + "/" + stem + ext;
            if (FILE* f = fopen(p.c_str(), "rb")) {
                fclose(f);
                out.push_back(p);
                break;                       // one container per stem, like CH
            }
        }
    }
    if (out.empty() && !musicStream.empty()) {
        const std::string p = dir + "/" + musicStream;
        if (FILE* f = fopen(p.c_str(), "rb")) { fclose(f); out.push_back(p); }
    }
    return out;
}

}  // namespace nc
