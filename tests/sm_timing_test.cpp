#include "smbg.h"

#include <cmath>
#include <cstdio>

namespace {

bool near(double actual, double expected, double tolerance = 1e-6) {
    if (std::fabs(actual - expected) <= tolerance) return true;
    std::fprintf(stderr, "expected %.9f, got %.9f\n", expected, actual);
    return false;
}

}  // namespace

int main() {
    const char* sm =
        "#OFFSET:-0.260;\n"
        "#BPMS:0=205,456=175,712=190,1000=150,1006.771=-150,1010.271=150;\n"
        "#STOPS:1006.750=2.800;\n";
    nc::SmTiming timing;
    timing.parse(sm);

    if (timing.warps.size() != 1 || timing.warps[0].first != 48325 ||
        timing.warps[0].second != 336) {
        std::fprintf(stderr, "negative BPM did not normalize to the expected 7-beat warp\n");
        return 1;
    }

    const double beforeStop = 0.260 + 456.0 * 60.0 / 205.0 +
        256.0 * 60.0 / 175.0 + 288.0 * 60.0 / 190.0 +
        6.75 * 60.0 / 150.0;
    const double warpBeat = 48325.0 / 48.0;
    const double warpEnd = warpBeat + 7.0;
    const double warpSec = beforeStop + 2.8 +
        (warpBeat - 1006.75) * 60.0 / 150.0;

    if (!near(timing.beatToSec(0), 0.260) ||
        !near(timing.beatToSec(1006.75), beforeStop) ||
        !near(timing.beatToSec(warpBeat), warpSec) ||
        !near(timing.beatToSec(warpEnd), warpSec) ||
        !near(timing.secToBeat(beforeStop + 1.0), 1006.75, 1e-5) ||
        !near(timing.secToBeat(warpSec), warpEnd, 1e-5))
        return 1;
    return 0;
}
