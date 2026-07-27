#include "video.h"

// nc_ffmpeg(): a bundled ffmpeg.exe beside the exe wins over PATH.
#include "renderer.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace nc {

bool VideoStream::open(const std::string& path, float scale) {
    path_ = path;
    // Probe once. avg_frame_rate is a rational ("25/1"); nb_frames is absent
    // for several containers (mpeg-ps among them), so the count falls back to
    // round(duration * fps).
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "ffprobe -v error -select_streams v:0 "
             "-show_entries stream=width,height,avg_frame_rate "
             "-show_entries format=duration -of default=nw=1 \"%s\"",
             path.c_str());
    FILE* p = _popen(cmd, "r");
    if (!p) return false;
    char line[256];
    double num = 0, den = 1;
    while (fgets(line, sizeof line, p)) {
        if      (!strncmp(line, "width=", 6))  w_ = atoi(line + 6);
        else if (!strncmp(line, "height=", 7)) h_ = atoi(line + 7);
        else if (!strncmp(line, "duration=", 9)) dur_ = atof(line + 9);
        else if (!strncmp(line, "avg_frame_rate=", 15)) {
            num = atof(line + 15);
            const char* sl = strchr(line + 15, '/');
            den = sl ? atof(sl + 1) : 1.0;
        }
    }
    _pclose(p);
    if (w_ <= 0 || h_ <= 0 || num <= 0 || den <= 0 || dur_ <= 0) {
        fprintf(stderr, "bg: ffprobe could not read %s\n", path.c_str());
        return false;
    }
    fps_ = num / den;
    nframes_ = int(dur_ * fps_ + 0.5);
    if (nframes_ < 1) nframes_ = 1;

    dw_ = w_; dh_ = h_;
    if (scale > 0.0f && scale < 1.0f) {
        dw_ = int(w_ * scale + 0.5f); if (dw_ < 1) dw_ = 1;
        dh_ = int(h_ * scale + 0.5f); if (dh_ < 1) dh_ = 1;
    }
    buf_.resize(size_t(dw_) * dh_ * 3);

    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    // GL_RGB8, not RGBA: avoids a 33% CPU-side widening. Project colour space
    // is Gamma -- plain RGB8, never SRGB8 (AGENTS.md, Blend modes).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, dw_, dh_, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void VideoStream::closePipe() {
    if (pipe_) { _pclose(pipe_); pipe_ = nullptr; }
    cur_ = -1;
}

void VideoStream::close() {
    closePipe();
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
    fps_ = 0.0;
}

bool VideoStream::reopenAt(int index) {
    closePipe();
    char sc[64] = "";
    if (dw_ != w_ || dh_ != h_)
        snprintf(sc, sizeof sc, "-vf scale=%d:%d ", dw_, dh_);
    char cmd[1024];
    // -ss before -i is an accurate seek (decode from keyframe, discard), so the
    // first frame out is the one containing T = index/fps. Deterministic for a
    // given command + file + ffmpeg build; +-1 frame is possible between two
    // DIFFERENT seek points, and that is a recorded limitation
    // (devdocs/spec/background.md section 4.4).
    // -loglevel fatal, not error: closing the pipe mid-stream (a reopen, or
    // process exit) makes this ffmpeg print "Error writing trailer: Broken
    // pipe" at error level, which is expected and pure noise here. A file that
    // cannot be decoded at all still fails loudly -- the short read is handled
    // and ffprobe already validated the file at open().
    snprintf(cmd, sizeof cmd,
             "%s -hide_banner -loglevel fatal -ss %.6f -i \"%s\" "
             "%s-f rawvideo -pix_fmt rgb24 -",
             nc_ffmpeg().c_str(), double(index) / fps_, path_.c_str(), sc);
    pipe_ = _popen(cmd, "rb");
    if (!pipe_) return false;
    // The pipe, not the codec, is the historic bottleneck (AGENTS.md,
    // Performance): one large read + a 4 MB buffer.
    iobuf_.resize(4 << 20);
    setvbuf(pipe_, iobuf_.data(), _IOFBF, iobuf_.size());
    cur_ = index - 1;               // next readOne yields `index`
    return readOne();
}

bool VideoStream::readOne() {
    if (!pipe_) return false;
    const size_t want = buf_.size();
    const size_t got = fread(buf_.data(), 1, want, pipe_);
    if (got != want) { closePipe(); return false; }
    ++cur_;
    return true;
}

void VideoStream::upload() {
    if (uploaded_ == cur_) return;
    glBindTexture(GL_TEXTURE_2D, tex_);
    // rgb24 rows are 3-byte pixels; a scaled width may not be 4-aligned.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dw_, dh_, GL_RGB,
                    GL_UNSIGNED_BYTE, buf_.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    uploaded_ = cur_;
}

bool VideoStream::ensureFrame(int index) {
    if (!ok()) return false;
    if (index < 0) index = 0;
    if (index >= nframes_) index = nframes_ - 1;
    if (index == uploaded_) return true;

    const auto t0 = std::chrono::high_resolution_clock::now();
    bool have = (index == cur_);
    if (!have) {
        if (pipe_ && index > cur_ && index <= cur_ + SKIP_MAX) {
            have = true;
            while (cur_ < index && (have = readOne())) {}
        }
        if (!have) have = reopenAt(index);
    }
    if (have) upload();
    ms_ += std::chrono::duration<double, std::milli>(
               std::chrono::high_resolution_clock::now() - t0).count();
    return have;
}

}  // namespace nc
