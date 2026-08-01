// Song playback for the editor.
//
// Decoding is delegated to ffmpeg, which this project already requires to
// encode. The whole song is held as s16 stereo because scrubbing needs random
// access. Playback itself uses WASAPI: the stream's IAudioClock reports the
// sample currently reaching the speakers, so the preview follows heard audio
// rather than bytes that SDL has merely handed to the endpoint buffer.
#pragma once

#include <string>
#include <vector>

namespace nce {

namespace detail {

inline double sourceFrameAt(double sourceAnchor, double clockAnchor,
                            double clockNow, double clockFrequency,
                            double sourceRate, double playbackRate) {
    if (clockFrequency <= 0.0) return sourceAnchor;
    return sourceAnchor + (clockNow - clockAnchor) / clockFrequency
                        * sourceRate * playbackRate;
}

}  // namespace detail

class Audio {
public:
    ~Audio() { close(); }

    // False if the file is missing, ffmpeg is unavailable, or the playback
    // endpoint cannot be opened. Audio remains optional in the editor.
    bool load(const std::string& path);
    bool loadMix(const std::vector<std::string>& paths);
    void close();
    bool ok() const;

    double duration() const {
        return pcm_.empty() ? 0.0 : double(pcm_.size() / CH) / double(RATE);
    }

    void setPlaying(bool on);
    void setRate(float rate);

    // Song seconds corresponding to the sample currently reaching the
    // speakers. The latest IAudioClock reading is QPC-interpolated between
    // audio-thread updates.
    double time();
    bool exhausted() const;

    // Hard cut. WASAPI Stop + Reset discards the endpoint buffer before the
    // new position is primed, so stale audio cannot survive a scrub.
    void seek(double sec);
    bool primed() const { return primed_; }

private:
    static const int RATE = 48000;
    static const int CH   = 2;

    struct Wasapi;
    bool openDevice();

    std::vector<short> pcm_;           // interleaved stereo
    Wasapi*            out_ = nullptr;
    bool               playing_ = false;
    bool               primed_ = false;
    float              speed_ = 1.0f;
    double             lastTime_ = 0.0;
};

}  // namespace nce
