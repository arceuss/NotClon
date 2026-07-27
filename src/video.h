// Movie decode for background changes: ffprobe at open, then an ffmpeg rawvideo
// pipe -- the established pattern (editor/audio.cpp decodes the song the same
// way), but STREAMED, never slurped: 1080p rgb24 is 6.2 MB/frame, so a long
// movie held whole would be tens of GB.
//
// The stream is a CACHE, not state: the frame index a caller asks for is a pure
// function of the beat, and ensureFrame(index) reads forward when that is cheap
// or re-opens the pipe with -ss when it is not. The same index always yields
// the same picture (a given ffmpeg command against a given file is
// deterministic), which is what --preview determinism needs.
#pragma once

#include "gl.h"

#include <cstdio>
#include <string>
#include <vector>

namespace nc {

class VideoStream {
public:
    // Probes with ffprobe and allocates the texture; the decode pipe opens
    // lazily on the first ensureFrame. scale < 1 decodes downsized via
    // -vf scale (the background is behind an opaque board over 16% of the
    // frame and then runs through the post glow, so full-res detail is not
    // recoverable anyway -- devdocs/spec/background.md section 4.5).
    bool   open(const std::string& path, float scale);
    void   close();
    bool   ok() const { return fps_ > 0.0; }

    double fps() const { return fps_; }
    double duration() const { return dur_; }
    int    frameCount() const { return nframes_; }

    // Make tex() hold video frame `index` (clamped to the file's range).
    bool   ensureFrame(int index);
    // True once any frame has been uploaded -- lets a caller keep drawing the
    // last good frame if a read hits EOF (frame count is a rounded estimate).
    bool   hasFrame() const { return uploaded_ >= 0; }
    GLuint tex() const { return tex_; }
    int    texW() const { return dw_; }
    int    texH() const { return dh_; }

    double benchMs() const { return ms_; }   // decode+upload time, for --bench

private:
    bool reopenAt(int index);
    bool readOne();                 // one frame into buf_, ++cur_
    void upload();
    void closePipe();

    FILE*  pipe_ = nullptr;
    std::vector<char> iobuf_;       // 4 MB setvbuf backing store
    GLuint tex_  = 0;
    std::vector<unsigned char> buf_;   // dw_*dh_*3, rgb24, top-down
    std::string path_;
    int    w_ = 0, h_ = 0, dw_ = 0, dh_ = 0;
    int    nframes_ = 0;
    int    cur_ = -1;               // frame index currently in buf_
    int    uploaded_ = -1;          // frame index currently in tex_
    double fps_ = 0.0, dur_ = 0.0;
    double ms_ = 0.0;
    // Forward gaps up to this many frames are read-and-discarded; anything
    // larger (or any backward step) re-opens with -ss.
    static const int SKIP_MAX = 8;
};

}  // namespace nc
