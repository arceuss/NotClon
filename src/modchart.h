// The modchart itself: a timeline over the chart's own section markers.
//
// Section beats come from notes.chart [Events], which line up with the FLP
// arrangement of the track (Distant Intro 0, Supersaw Intro 8, Violins and
// Synths 72, Supersaw Melody 136, Violin Break 200, Violin Medley 232,
// Formant Medley 296, Guitar Solo 1 335, Supersaw Arp 404, Violin Break 2
// 464, Guitar Solo 2 526, Violin Medley 2 560, Formant Outro 624).
#pragma once

#include "mods.h"
#include <cmath>

struct PostFx {
    float aberration = 0.05f;
    float glow = 0.55f;
    float vignette = 0.35f;
    float desat = 0.0f;
    float shake = 0.0f;
};

inline float ncRamp(double beat, double a, double b) {
    if (beat <= a) return 0.0f;
    if (beat >= b) return 1.0f;
    float x = float((beat - a) / (b - a));
    return x * x * (3.0f - 2.0f * x);
}

inline void modchartAt(double beat, nc::Mods& m, PostFx& fx) {
    const float t = float(beat);

    // 8 -- Supersaw Intro: the kit drops in
    if (beat >= 8) {
        m.drunk = 0.25f * ncRamp(beat, 8, 40);
        m.tipsy = 0.20f * ncRamp(beat, 24, 56);
        m.tipsyOffset = t;
    }

    // 72 -- Violins and Synths
    if (beat >= 72) {
        m.drunk = 0.35f;
        m.bumpy = 0.30f * ncRamp(beat, 72, 104);
        m.bumpyOffset = t * 0.25f;
    }

    // 136 -- Supersaw Melody: first full chorus
    if (beat >= 136) {
        m.drunk = 0.45f;
        m.tornado = 0.35f * ncRamp(beat, 136, 168);
        m.beat = 0.6f * ncRamp(beat, 152, 168);
    }

    // 200 -- Violin Break: melancholic, washed out, almost still
    if (beat >= 200 && beat < 232) {
        m = nc::Mods{};
        m.drunk = 0.18f;
        m.bumpy = 0.10f;
        m.bumpyOffset = t * 0.12f;
        fx.desat = 0.60f;
        fx.glow = 0.28f;
        fx.aberration = 0.03f;
        fx.vignette = 0.45f;
    }

    // 232 -- Violin Medley: the drop
    if (beat >= 232) {
        m.drunk = 0.55f;
        m.tornado = 0.45f * ncRamp(beat, 232, 248);
        m.beat = 0.85f;
        m.bumpy = 0.40f;
        m.bumpyOffset = t * 0.5f;
        fx.aberration = 0.22f;
        fx.glow = 0.85f;
        fx.desat = 0.0f;
        fx.vignette = 0.35f;
    }

    // 296 -- Formant Medley: key change to Dm
    if (beat >= 296) {
        m.confusion = 0.04f * sinf(t * 0.06f);
        m.invert = (fmod(floor((beat - 296) / 16.0), 2.0) == 0.0) ? 0.0f : 1.0f;
    }

    // 335 -- Guitar Solo 1
    if (beat >= 335) {
        m.invert = 0.0f;
        m.drunk = 0.65f;
        m.dizzy = 0.05f;
        m.tornado = 0.30f;
    }

    // 404 -- Supersaw Arp
    if (beat >= 404) {
        m.dizzy = 0.0f;
        m.tornado = 0.55f;
        m.bumpy = 0.55f;
        m.flip = 0.5f + 0.5f * sinf(t * 0.05f);
    }

    // 464 -- Violin Break 2: mercy, and the palette drains again
    if (beat >= 464 && beat < 496) {
        m = nc::Mods{};
        m.drunk = 0.15f;
        m.bumpy = 0.08f;
        m.bumpyOffset = t * 0.1f;
        fx.desat = 0.50f;
        fx.glow = 0.32f;
        fx.aberration = 0.04f;
        fx.vignette = 0.30f;
    }

    // 496 -- the chorus proper / 526 Guitar Solo 2
    if (beat >= 496) {
        m.drunk = 0.70f;
        m.tornado = 0.60f;
        m.beat = 1.0f;
        m.bumpy = 0.50f;
        m.bumpyOffset = t * 0.6f;
        m.confusion = 0.05f * sinf(t * 0.15f);
        fx.aberration = 0.28f;
        fx.glow = 1.0f;
        fx.vignette = 0.30f;
        fx.desat = 0.0f;
    }

    // 560 -- Violin Medley 2, the recap: replay the 232 drop
    if (beat >= 560) {
        m.tornado = 0.45f;
        m.confusion = 0.0f;
        m.flip = 0.0f;
    }

    // 624 -- Formant Outro
    if (beat >= 624) {
        m.dizzy = 0.06f;
        m.beat = 1.0f;
        m.tornado = 0.60f;
    }

    // land clean for the last note at 656
    if (beat >= 650) {
        float k = fminf(1.0f, float((beat - 650) / 8.0));
        m.drunk *= (1 - k); m.tornado *= (1 - k); m.beat *= (1 - k);
        m.bumpy *= (1 - k); m.dizzy *= (1 - k); m.confusion *= (1 - k);
        m.tipsy *= (1 - k); m.flip *= (1 - k); m.invert *= (1 - k);
        fx.vignette = 0.35f + 0.60f * k;
        fx.glow = 1.0f * (1 - k) + 0.3f * k;
    }
}
