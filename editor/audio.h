// Song playback for the editor.
//
// Decoding is delegated to ffmpeg, which this project already requires to
// encode. That covers every container Clone Hero accepts -- ogg, opus, mp3,
// wav -- instead of the one format a vendored decoder would give us, and costs
// no new dependency. The whole song is pulled into memory as s16 stereo once
// (about 45 MB for four minutes), because scrubbing wants random access and
// streaming a seekable decode would be far more machinery than this needs.
//
// THE AUDIO DEVICE IS THE CLOCK while playing. The obvious arrangement -- run
// the playhead off frame delta time and nudge audio to match -- does not work:
// a free-running visual clock and a crystal-timed audio device drift apart
// continuously, so the correction fires every second or so, and each
// correction is a buffer flush, which is an audible click. Reading the
// playhead out of the device instead makes drift structurally impossible.
// time() therefore reports what the listener has actually heard, and the
// caller derives the tick from it.
//
// Chart seconds and song seconds are the same timeline (Chart::tickToSec
// already folds in Offset), so nothing extra lines them up.
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace nce {

class Audio {
public:
    ~Audio() { close(); }

    // False if the file is missing or ffmpeg is not on PATH. Not fatal: the
    // editor is perfectly usable silent, so callers just report it.
    bool load(const std::string& path);
    void close();
    // Decode-and-mix several stems into one stereo buffer, the way CH plays
    // every stem it finds at once. One path behaves exactly like load().
    bool loadMix(const std::vector<std::string>& paths);
    bool ok() const { return stream_ != nullptr; }

    double duration() const {
        return pcm_.empty() ? 0.0 : double(pcm_.size() / CH) / double(RATE);
    }

    void setPlaying(bool on);
    void setRate(float rate);

    // Song seconds the listener has actually heard, interpolated between
    // device updates. Not const: it carries the smoothing state.
    double time();

    // True once the song has run out -- the caller must fall back to its own
    // clock, because time() has stopped advancing.
    bool exhausted() const;

    // Refill the device. Call once a frame while playing.
    void pump();

    // Hard cut. Use when the user scrubs, jumps, or presses play.
    void seek(double sec);

private:
    static const int RATE = 48000;
    static const int CH   = 2;

    std::vector<short> pcm_;           // interleaved stereo
    SDL_AudioStream*   stream_ = nullptr;
    size_t             cursor_ = 0;    // next sample frame to queue
    bool               playing_ = false;
    float              speed_ = 1.0f;
    // Song seconds handed to the device so far. Starts wherever seek() put it,
    // which may be NEGATIVE: a chart with a negative offset has beats before
    // the audio file starts, and those are played as silence rather than
    // clamped away. Clamping them was what made the playhead lurch forward by
    // the whole lead-in the moment the song proper began -- the caller had been
    // free-running on frame delta through the negative region while the device
    // was already several seconds into the file.
    double             writeSec_ = 0.0;

    size_t queuedFrames() const;
    double rawTime() const;

    // The device drains in chunks of a thousand-odd frames, so rawTime() moves
    // in ~20 ms steps. At editor frame rates that reads as judder, so the gap
    // between steps is filled in from the wall clock and the result is held
    // monotone.
    double lastRaw_ = -1.0, lastRawWall_ = 0.0, smooth_ = 0.0;
};

}  // namespace nce
