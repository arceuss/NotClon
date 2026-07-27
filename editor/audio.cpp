#include "audio.h"

#include <cstdio>

namespace nce {

namespace {
// How much song to keep queued. Generous, because the editor renders the whole
// highway between top-ups and a GL hitch here costs an underrun. The cost of
// being generous is only that a scrub takes this long to take effect, and a
// scrub flushes the buffer anyway.
const double BUFFER_SEC = 0.40;
}  // namespace

bool Audio::loadMix(const std::vector<std::string>& paths) {
    if (paths.empty()) return false;
    if (paths.size() == 1) return load(paths[0]);
    close();

    // One ffmpeg invocation, amix over all stems. normalize=0 keeps unity
    // gain -- CH plays stems at full volume on top of each other, and amix's
    // default 1/n scaling would make a two-stem song half as loud.
    std::string cmd = "ffmpeg -hide_banner -loglevel error";
    for (const auto& p : paths) cmd += " -i \"" + p + "\"";
    char tail[256];
    snprintf(tail, sizeof tail,
             " -filter_complex amix=inputs=%d:duration=longest:normalize=0 "
             "-f s16le -acodec pcm_s16le -ac %d -ar %d -",
             int(paths.size()), CH, RATE);
    cmd += tail;

    FILE* p = _popen(cmd.c_str(), "rb");
    if (!p) return false;
    std::vector<short> buf(1 << 16);
    size_t n;
    while ((n = fread(buf.data(), sizeof(short), buf.size(), p)) > 0)
        pcm_.insert(pcm_.end(), buf.begin(), buf.begin() + n);
    _pclose(p);
    if (pcm_.empty()) return false;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = CH;
    spec.freq = RATE;
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &spec, nullptr, nullptr);
    if (!stream_) { pcm_.clear(); return false; }
    return true;
}

bool Audio::load(const std::string& path) {
    close();

    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -i \"%s\" "
             "-f s16le -acodec pcm_s16le -ac %d -ar %d -",
             path.c_str(), CH, RATE);
    FILE* p = _popen(cmd, "rb");
    if (!p) return false;

    std::vector<short> buf(1 << 16);
    size_t n;
    while ((n = fread(buf.data(), sizeof(short), buf.size(), p)) > 0)
        pcm_.insert(pcm_.end(), buf.begin(), buf.begin() + n);
    _pclose(p);
    if (pcm_.empty()) return false;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = CH;
    spec.freq = RATE;
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &spec, nullptr, nullptr);
    if (!stream_) { pcm_.clear(); return false; }
    return true;
}

void Audio::close() {
    if (stream_) { SDL_DestroyAudioStream(stream_); stream_ = nullptr; }
    pcm_.clear();
    cursor_ = 0;
    playing_ = false;
}

size_t Audio::queuedFrames() const {
    if (!stream_) return 0;
    const int bytes = SDL_GetAudioStreamAvailable(stream_);
    return bytes <= 0 ? 0 : size_t(bytes) / (sizeof(short) * CH);
}

// SDL_GetAudioStreamAvailable reports bytes in the OUTPUT format, so with a
// frequency ratio of r each queued output frame stands for r song frames --
// hence the speed_ term. Without it the playhead ran ahead of the music at any
// rate above 1x and behind it below.
double Audio::rawTime() const {
    if (!stream_) return 0.0;
    return writeSec_ - double(queuedFrames()) * double(speed_) / double(RATE);
}

double Audio::time() {
    const double raw = rawTime();
    const double wall = double(SDL_GetPerformanceCounter()) /
                        double(SDL_GetPerformanceFrequency());

    if (raw != lastRaw_) {
        lastRaw_ = raw;
        lastRawWall_ = wall;
        if (raw < smooth_ - 0.10) smooth_ = raw;   // a seek moved us backwards
    }

    double est = lastRaw_ + (wall - lastRawWall_) * double(speed_);
    // If the device stalls, do not let extrapolation run away from it.
    if (est > lastRaw_ + 0.10) est = lastRaw_ + 0.10;
    if (est < smooth_) est = smooth_;              // never go backwards
    smooth_ = est;
    return est;
}

bool Audio::exhausted() const {
    return !stream_ || (cursor_ >= pcm_.size() / CH && queuedFrames() == 0);
}

void Audio::setPlaying(bool on) {
    if (!stream_ || playing_ == on) return;
    playing_ = on;
    if (on) SDL_ResumeAudioStreamDevice(stream_);
    else    SDL_PauseAudioStreamDevice(stream_);
}

void Audio::setRate(float rate) {
    if (!stream_ || rate == speed_) return;
    speed_ = rate;
    // Resampling the whole stream is what gives off-speed playback its
    // chipmunk pitch -- which is what a rhythm game editor wants, because
    // pitch is the cue that you are not at 1x.
    SDL_SetAudioStreamFrequencyRatio(stream_, rate);
}

void Audio::seek(double sec) {
    if (!stream_) return;
    SDL_ClearAudioStream(stream_);
    const size_t frames = pcm_.size() / CH;
    const size_t f = sec <= 0.0 ? 0 : size_t(sec * RATE);
    cursor_ = f > frames ? frames : f;
    writeSec_ = sec;
    lastRaw_ = -1.0;
    smooth_ = sec;
}

void Audio::pump() {
    if (!stream_ || !playing_) return;
    const size_t frames = pcm_.size() / CH;

    const size_t want = size_t(BUFFER_SEC * RATE);
    const size_t have = queuedFrames();
    if (have >= want) return;
    size_t push = want - have;

    // Before second zero there is nothing to decode, so the lead-in is queued
    // as silence. It costs a buffer of zeroes and buys a single clock: the
    // device is authoritative from the first negative beat right through the
    // start of the song, with no handover for the playhead to jump across.
    if (writeSec_ < 0.0) {
        const size_t lead = size_t(-writeSec_ * RATE) + 1;
        if (push > lead) push = lead;
        std::vector<short> quiet(push * CH, 0);
        SDL_PutAudioStreamData(stream_, quiet.data(),
                               int(push * CH * sizeof(short)));
        writeSec_ += double(push) / double(RATE);
        return;
    }

    if (cursor_ >= frames) return;
    if (cursor_ + push > frames) push = frames - cursor_;
    SDL_PutAudioStreamData(stream_, &pcm_[cursor_ * CH],
                           int(push * CH * sizeof(short)));
    cursor_ += push;
    writeSec_ += double(push) / double(RATE);
}

}  // namespace nce
