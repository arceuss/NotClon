#include "audio.h"

#include <cmath>
#include <cstdio>

namespace {

bool near(double actual, double expected) {
    if (std::fabs(actual - expected) <= 1e-9) return true;
    std::fprintf(stderr, "expected %.9f, got %.9f\n", expected, actual);
    return false;
}

}  // namespace

int main() {
    using nce::detail::sourceFrameAt;

    // Seek to frame 96000, then play one device second at 1x.
    double source = sourceFrameAt(96000.0, 5000.0, 15000.0, 10000.0,
                                  48000.0, 1.0);
    if (!near(source, 144000.0)) return 1;

    // Rebase at the rate transition; half a device second at 2x advances one
    // full second of source without assuming the device position starts at 0.
    source = sourceFrameAt(source, 15000.0, 20000.0, 10000.0,
                           48000.0, 2.0);
    if (!near(source, 192000.0)) return 1;

    // A paused clock does not move: callers pass the frozen device position.
    source = sourceFrameAt(source, 20000.0, 20000.0, 10000.0,
                           48000.0, 2.0);
    if (!near(source, 192000.0)) return 1;

    // Rebase again, then run a quarter second at half speed.
    source = sourceFrameAt(source, 20000.0, 22500.0, 10000.0,
                           48000.0, 0.5);
    if (!near(source, 198000.0)) return 1;
    return 0;
}
