#include "audio.h"

// nc_ffmpeg(): a bundled ffmpeg.exe beside the editor wins over PATH.
#include "renderer.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace nce {

namespace {

template <class T>
void release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

double qpc100ns() {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return double((long double)now.QuadPart * 10000000.0L /
                  (long double)freq.QuadPart);
}

}  // namespace

struct Audio::Wasapi {
    enum class Command { None, Play, Pause, Seek, Rate, Quit };

    struct Published {
        double sourceAnchor = 0.0;
        double clockAnchor = 0.0;
        double clockFrequency = 1.0;
        double samplePosition = 0.0;
        double sampleQpc100ns = 0.0;
        double submittedSource = 0.0;
        bool running = false;
    };

    explicit Wasapi(Audio* a) : owner(a) {}

    Audio* owner = nullptr;
    HANDLE wakeEvent = nullptr;
    HANDLE audioEvent = nullptr;
    std::thread thread;

    std::mutex mutex;
    std::condition_variable cv;
    Command command = Command::None;
    double commandSec = 0.0;
    float commandRate = 1.0f;
    bool commandResult = false;
    bool initialized = false;
    bool initOk = false;
    bool stopped = false;
    Published published;

    std::atomic<bool> alive{false};
    std::atomic<bool> atEnd{false};

    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    IAudioClock* clock = nullptr;
    IAudioClockAdjustment* adjustment = nullptr;
    UINT32 bufferFrames = 0;

    size_t cursor = 0;
    double writeSec = 0.0;
    double sourceAnchor = 0.0;
    UINT64 clockAnchor = 0;
    UINT64 clockFrequency = 1;
    float rate = 1.0f;
    bool playing = false;
    bool primed = false;

    HANDLE mmcss = nullptr;
    DWORD mmcssTask = 0;
    bool coOwned = false;

    bool send(Command c, double sec = 0.0, float newRate = 1.0f) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!initialized || !initOk || stopped) return false;
        cv.wait(lock, [&] { return command == Command::None || stopped; });
        if (stopped) return false;
        command = c;
        commandSec = sec;
        commandRate = newRate;
        SetEvent(wakeEvent);
        cv.wait(lock, [&] { return command == Command::None || stopped; });
        return !stopped && commandResult;
    }

    bool initialize() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(co) && co != RPC_E_CHANGED_MODE) return false;
        coOwned = SUCCEEDED(co);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      (void**)&enumerator);
        if (SUCCEEDED(hr))
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr))
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  (void**)&client);
        release(device);
        release(enumerator);
        if (FAILED(hr)) return false;

        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = Audio::CH;
        fmt.nSamplesPerSec = Audio::RATE;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            AUDCLNT_STREAMFLAGS_RATEADJUST |
                            AUDCLNT_STREAMFLAGS_NOPERSIST;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0,
                                &fmt, nullptr);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(audioEvent);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
        if (SUCCEEDED(hr))
            hr = client->GetService(__uuidof(IAudioRenderClient),
                                    (void**)&render);
        if (SUCCEEDED(hr))
            hr = client->GetService(__uuidof(IAudioClock), (void**)&clock);
        if (SUCCEEDED(hr))
            hr = client->GetService(__uuidof(IAudioClockAdjustment),
                                    (void**)&adjustment);
        if (SUCCEEDED(hr)) hr = clock->GetFrequency(&clockFrequency);
        if (SUCCEEDED(hr)) hr = adjustment->SetSampleRate(float(Audio::RATE));
        if (FAILED(hr) || bufferFrames == 0 || clockFrequency == 0) return false;

        mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);
        alive.store(true);
        publish();
        return true;
    }

    void shutdown() {
        if (client && playing) client->Stop();
        playing = false;
        if (mmcss) {
            AvRevertMmThreadCharacteristics(mmcss);
            mmcss = nullptr;
        }
        release(adjustment);
        release(clock);
        release(render);
        release(client);
        if (coOwned) CoUninitialize();
        coOwned = false;
        alive.store(false);
    }

    bool getPosition(UINT64& pos, UINT64* qpc = nullptr) {
        UINT64 stamp = 0;
        const HRESULT hr = clock->GetPosition(&pos, qpc ? qpc : &stamp);
        if (FAILED(hr)) { alive.store(false); return false; }
        return true;
    }

    double sourceAt(UINT64 pos) const {
        return detail::sourceFrameAt(sourceAnchor, double(clockAnchor),
                                     double(pos), double(clockFrequency),
                                     double(Audio::RATE), double(rate));
    }

    bool rebase(double source) {
        UINT64 pos = 0;
        if (!getPosition(pos)) return false;
        sourceAnchor = source;
        clockAnchor = pos;
        return true;
    }

    bool pump() {
        if (!primed || !alive.load()) return true;

        UINT32 padding = 0;
        HRESULT hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) { alive.store(false); return false; }
        if (padding >= bufferFrames) return true;

        const UINT32 available = bufferFrames - padding;
        const size_t totalFrames = owner->pcm_.size() / Audio::CH;
        const size_t lead = writeSec < 0.0
                          ? size_t(std::ceil(-writeSec * Audio::RATE)) : 0;
        const size_t audio = cursor < totalFrames ? totalFrames - cursor : 0;
        const UINT32 count = UINT32(std::min<size_t>(available, lead + audio));
        if (count == 0) return true;

        BYTE* bytes = nullptr;
        hr = render->GetBuffer(count, &bytes);
        if (FAILED(hr)) { alive.store(false); return false; }
        short* dst = reinterpret_cast<short*>(bytes);
        size_t written = 0;

        const size_t quiet = std::min<size_t>(count, lead);
        if (quiet) {
            std::memset(dst, 0, quiet * Audio::CH * sizeof(short));
            written += quiet;
            writeSec += double(quiet) / double(Audio::RATE);
            if (quiet == lead) writeSec = 0.0;
        }

        const size_t copy = std::min<size_t>(count - written, audio);
        if (copy) {
            std::memcpy(dst + written * Audio::CH,
                        &owner->pcm_[cursor * Audio::CH],
                        copy * Audio::CH * sizeof(short));
            cursor += copy;
            written += copy;
            writeSec += double(copy) / double(Audio::RATE);
        }

        hr = render->ReleaseBuffer(count, 0);
        if (FAILED(hr)) { alive.store(false); return false; }
        return true;
    }

    void publish() {
        if (!clock) return;
        UINT64 pos = 0, qpc = 0;
        if (!getPosition(pos, &qpc)) return;

        UINT32 padding = 0;
        const bool paddingOk = SUCCEEDED(client->GetCurrentPadding(&padding));
        const size_t totalFrames = owner->pcm_.size() / Audio::CH;
        atEnd.store(writeSec >= 0.0 && cursor >= totalFrames &&
                    paddingOk && padding == 0);

        std::lock_guard<std::mutex> lock(mutex);
        published.sourceAnchor = sourceAnchor;
        published.clockAnchor = double(clockAnchor);
        published.clockFrequency = double(clockFrequency);
        published.samplePosition = double(pos);
        published.sampleQpc100ns = double(qpc);
        published.submittedSource = writeSec * double(Audio::RATE);
        published.running = playing;
    }

    bool doSeek(double sec) {
        const bool resume = playing;
        if (playing && FAILED(client->Stop())) return false;
        playing = false;
        if (FAILED(client->Reset())) return false;

        const size_t frames = owner->pcm_.size() / Audio::CH;
        const size_t f = sec <= 0.0 ? 0 : size_t(sec * Audio::RATE);
        cursor = std::min(f, frames);
        writeSec = sec;
        primed = true;
        atEnd.store(false);
        if (FAILED(clock->GetFrequency(&clockFrequency)) ||
            !rebase(sec * double(Audio::RATE)) || !pump())
            return false;

        if (resume) {
            if (FAILED(client->Start())) return false;
            playing = true;
        }
        publish();
        return true;
    }

    bool doPause() {
        if (!playing) return true;
        if (FAILED(client->Stop())) return false;
        playing = false;
        UINT64 pos = 0;
        if (!getPosition(pos)) return false;
        const double source = sourceAt(pos);
        sourceAnchor = source;
        clockAnchor = pos;
        publish();
        return true;
    }

    bool doPlay() {
        if (playing || !primed) return primed;
        UINT64 pos = 0;
        if (!getPosition(pos)) return false;
        const double source = sourceAt(pos);
        sourceAnchor = source;
        clockAnchor = pos;
        if (!pump() || FAILED(client->Start())) return false;
        playing = true;
        publish();
        return true;
    }

    bool doRate(float newRate) {
        if (newRate == rate) return true;
        const bool resume = playing;
        if (playing && FAILED(client->Stop())) return false;
        playing = false;

        UINT64 oldPos = 0;
        if (!getPosition(oldPos)) return false;
        const double source = sourceAt(oldPos);

        // Microsoft explicitly forbids SetSampleRate on a real-time thread.
        // Commands run while the client is stopped, and the MMCSS registration
        // is dropped for this call before the pump thread returns to real-time
        // work.
        if (mmcss) {
            AvRevertMmThreadCharacteristics(mmcss);
            mmcss = nullptr;
        }
        HRESULT hr = adjustment->SetSampleRate(float(Audio::RATE) * newRate);
        mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);
        if (FAILED(hr)) {
            if (resume) {
                if (FAILED(client->Start())) {
                    alive.store(false);
                    return false;
                }
                playing = true;
            }
            publish();
            return false;
        }

        rate = newRate;
        if (FAILED(clock->GetFrequency(&clockFrequency)) || !rebase(source))
            return false;
        if (resume) {
            if (FAILED(client->Start())) return false;
            playing = true;
        }
        publish();
        return true;
    }

    bool execute(Command c, double sec, float newRate) {
        if (c == Command::Quit) return true;
        if (!alive.load()) return false;
        if (c == Command::Play)  return doPlay();
        if (c == Command::Pause) return doPause();
        if (c == Command::Seek)  return doSeek(sec);
        if (c == Command::Rate)  return doRate(newRate);
        return true;
    }

    void run() {
        const bool ok = initialize();
        {
            std::lock_guard<std::mutex> lock(mutex);
            initOk = ok;
            initialized = true;
        }
        cv.notify_all();
        if (!ok) { shutdown(); return; }

        HANDLE waits[2] = {wakeEvent, audioEvent};
        bool quit = false;
        while (!quit) {
            const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (which == WAIT_OBJECT_0) {
                Command c;
                double sec;
                float newRate;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    c = command;
                    sec = commandSec;
                    newRate = commandRate;
                }
                const bool result = execute(c, sec, newRate);
                quit = c == Command::Quit;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    commandResult = result;
                    command = Command::None;
                }
                cv.notify_all();
            } else if (which == WAIT_OBJECT_0 + 1) {
                if (playing) pump();
                publish();
            } else {
                alive.store(false);
                quit = true;
            }
        }
        shutdown();
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
            command = Command::None;
        }
        cv.notify_all();
    }
};

bool Audio::openDevice() {
    out_ = new Wasapi(this);
    out_->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    out_->audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!out_->wakeEvent || !out_->audioEvent) {
        close();
        return false;
    }

    out_->thread = std::thread([this] { out_->run(); });
    {
        std::unique_lock<std::mutex> lock(out_->mutex);
        out_->cv.wait(lock, [&] { return out_->initialized; });
        if (out_->initOk) return true;
    }

    out_->thread.join();
    CloseHandle(out_->audioEvent);
    CloseHandle(out_->wakeEvent);
    delete out_;
    out_ = nullptr;
    pcm_.clear();
    return false;
}

bool Audio::loadMix(const std::vector<std::string>& paths) {
    if (paths.empty()) return false;
    if (paths.size() == 1) return load(paths[0]);
    close();

    std::string cmd = nc::nc_ffmpeg() + " -hide_banner -loglevel error";
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
    return openDevice();
}

bool Audio::load(const std::string& path) {
    close();

    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "%s -hide_banner -loglevel error -i \"%s\" "
             "-f s16le -acodec pcm_s16le -ac %d -ar %d -",
             nc::nc_ffmpeg().c_str(), path.c_str(), CH, RATE);
    FILE* p = _popen(cmd, "rb");
    if (!p) return false;

    std::vector<short> buf(1 << 16);
    size_t n;
    while ((n = fread(buf.data(), sizeof(short), buf.size(), p)) > 0)
        pcm_.insert(pcm_.end(), buf.begin(), buf.begin() + n);
    _pclose(p);
    if (pcm_.empty()) return false;
    return openDevice();
}

void Audio::close() {
    if (out_) {
        if (out_->thread.joinable()) {
            out_->send(Wasapi::Command::Quit);
            out_->thread.join();
        }
        if (out_->audioEvent) CloseHandle(out_->audioEvent);
        if (out_->wakeEvent) CloseHandle(out_->wakeEvent);
        delete out_;
        out_ = nullptr;
    }
    pcm_.clear();
    playing_ = false;
    primed_ = false;
    speed_ = 1.0f;
    lastTime_ = 0.0;
}

bool Audio::ok() const {
    return out_ && out_->alive.load();
}

double Audio::time() {
    if (!out_) return lastTime_;
    Wasapi::Published p;
    {
        std::lock_guard<std::mutex> lock(out_->mutex);
        p = out_->published;
    }

    double position = p.samplePosition;
    if (p.running) {
        const double delta = (qpc100ns() - p.sampleQpc100ns) / 10000000.0;
        if (delta > 0.0) position += delta * p.clockFrequency;
    }
    double source = detail::sourceFrameAt(
        p.sourceAnchor, p.clockAnchor, position, p.clockFrequency,
        double(RATE), double(speed_));
    if (source > p.submittedSource) source = p.submittedSource;
    double sec = source / double(RATE);
    if (playing_ && sec < lastTime_) sec = lastTime_;
    lastTime_ = sec;
    return sec;
}

bool Audio::exhausted() const {
    return !out_ || out_->atEnd.load();
}

void Audio::setPlaying(bool on) {
    if (!out_ || playing_ == on) return;
    const bool changed = out_->send(on ? Wasapi::Command::Play
                                       : Wasapi::Command::Pause);
    if (changed) playing_ = on;
}

void Audio::setRate(float rate) {
    if (!out_ || rate == speed_) return;
    if (out_->send(Wasapi::Command::Rate, 0.0, rate)) speed_ = rate;
}

void Audio::seek(double sec) {
    if (!out_) return;
    if (out_->send(Wasapi::Command::Seek, sec)) {
        primed_ = true;
        lastTime_ = sec;
    }
}

}  // namespace nce
