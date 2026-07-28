// ArrowEffects, ported from OpenITG (src/ArrowEffects.cpp) to a 5-lane
// guitar highway.
//
// The formulas are kept as they are in the original rather than "improved",
// because the whole point is that drunk looks like drunk and tornado looks
// like tornado. Two things are remapped for this game:
//
//   * StepMania works in pixels with ARROW_SIZE 64 and SCREEN_HEIGHT 480.
//     Those constants are kept, and the result is scaled into highway units
//     at the end (LANE_W / 64), so the motion has the same proportions
//     relative to a lane that it does in ITG.
//   * fYOffset is distance-to-receptor in pixels. Here that is beats-until-hit
//     converted with the same 64px-per-arrow convention.
#pragma once

#include <cfloat>
#include <cmath>

namespace nc {

static const float PI_F        = 3.14159265358979f;
static const float ARROW_SIZE  = 64.0f;
static const float SM_SCREEN_H = 480.0f;
static const int   NUM_LANES   = 5;
// The NOTE ladder, not the fret ladder. CH uses two: notes sit at
// highway.h noteX() spacing 0.1935, frets at FRET_X spacing 0.2. Mods displace
// notes, so this must be the note spacing -- with 0.1935 the identity
//     pxToUnits(laneXPixels(col)) == ch::noteX(col)
// holds exactly for all five columns, because laneXPixels(col) = (col-2)*64 and
// pxToUnits scales by LANE_W/64. At 0.2, flip and invert overshot the true
// lane-0-to-lane-4 span by 0.026 units, 2.7% of the neck's full width.
static const float LANE_W      = 0.1935f;

// lane centre in StepMania pixel space, so the ported math sees what it expects
inline float laneXPixels(int col) { return (col - 2) * ARROW_SIZE; }

inline float scale(float x, float l1, float h1, float l2, float h2) {
    return (x - l1) * (h2 - l2) / (h1 - l1) + l2;
}

// Every mod NotClon supports. Values are "percent" in ITG terms: 1.0 == 100%.
struct Mods {
    float tornado = 0, drunk = 0, flip = 0, invert = 0, beat = 0;
    float bumpy = 0, bumpyOffset = 0;
    float tipsy = 0, tipsyOffset = 0;
    float dizzy = 0, confusion = 0, roll = 0, twirl = 0;
    // Per-note zoom, 0.5^tiny (SM5 ArrowEffects.cpp:813-818). Distinct from
    // mini, which zooms the whole field (Player.cpp:723).
    float tiny = 0;
    float wave = 0, expand = 0, boomerang = 0;
    float boost = 0;
    float brake = 0;         // boost's partner accel (ArrowEffects.cpp:73-82)
    float mini = 0;          // field zoom, 1 - 0.5*mini (Player.cpp:723)
    // xmod. 1.0 is 1x -- the one knob whose neutral value is not 0.
    float scrollspeed = 1.0f;
    // m_fPerspectiveTilt in [-1, +1]: hallway is -tilt, distant is +tilt,
    // overhead drives it to 0 (PlayerOptions.cpp:349-353). One underlying
    // value, which is why the importer maps all three names onto this knob.
    float tilt = 0;
    // Not a PlayerOption: Saitama2000's lua/default.xml calls P1:wag() with
    // effectmagnitude(0,0,21) effectperiod(2) effectclock('bgm') -- a +-21
    // degree rotZ of the whole player on a 2-beat sine. This knob is that
    // exact call, scaled by percent; a NotClon extension until the actor
    // layer exists, named after the Actor effect it stands in for.
    float wag = 0;
    float reverse = 0;
    float centered = 0;
    // NotClon only. ITG arrows carry a direction glyph, so a player can see
    // that flip/invert moved a note somewhere else. A CH gem carries only its
    // lane colour, so a displaced note reads as "wrong colour over the right
    // fret" and a column swap is close to unreadable. This lerps a displaced
    // gem toward the colour of the lane it actually lands on, by percent.
    // Zero -- the default -- changes nothing.
    float swaptint = 0;
    // `hide`: any nonzero percentage hides the whole playfield.
    float hide = 0;
    // Drops the BOARD only -- neck, sidebars, lane strings, beat lines -- and
    // leaves the notes, frets and sustains floating. The mod form of the
    // --playfield flag, so a modchart can take the highway away mid-song
    // instead of it being an all-or-nothing property of the whole render.
    float hideboard = 0;
    // PIU display mode: any nonzero percentage renders gems as Pump It Up
    // panels (SM5's pump/default noteskin) and the fret stack as pump
    // receptors. Sections meant for pump should be CHARTED AS TAP NOTES --
    // pump/default itself charts hold heads as taps (NoteSkin.lua:18-19
    // redirects Hold Head to Tap Note), and NotClon sustains keep their CH
    // ribbons under this mode.
    float piu = 0;
    float sudden = 0, hidden = 0;
    float suddenOffset = 0, hiddenOffset = 0;
    // Per-column reverse contributions (PlayerOptions.cpp:1313-1332
    // GetReversePercentForColumn). Not separate effects: each adds to the
    // reverse percent of the columns it selects, so `split` reverses the right
    // half, `alternate` every odd column, `cross` the middle band.
    float split = 0, cross = 0, alternate = 0;

    // --- waveform shape parameters -------------------------------------------
    // Periods and phase offsets on the waveforms already computed above. Every
    // one is a DIVISOR TERM of the form (period*K)+K, so 0 is the identity and
    // the un-parameterised behaviour is preserved exactly.
    float drunkPeriod = 0, drunkOffsetP = 0;
    float bumpyPeriod = 0;
    float wavePeriod = 0;
    float tornadoPeriod = 0, tornadoOffsetP = 0;
    float confusionOffset = 0, confusionYOffset = 0;
    // New periodic shapes, same input as drunk/bumpy but a different function:
    // triangle, ramp, step and quantised-sine (ArrowEffects.cpp:766-811).
    float zigzag = 0, zigzagPeriod = 0, zigzagOffset = 0;
    float sawtooth = 0, sawtoothPeriod = 0;
    float square = 0, squarePeriod = 0, squareOffset = 0;
    float digital = 0, digitalPeriod = 0, digitalOffset = 0, digitalSteps = 0;
    // tan instead of cos, and `cosec` swaps that again for cosecant
    // (SelectTanType, :142-148). NotITG spells the flag `cosec`; SM5 spells it
    // `cosecant` and does not accept the short form.
    float tandrunk = 0;
    float cosecant = 0;
    // The beat envelope's own parameters, and its Y/Z dimensions. ITG runs
    // UpdateBeat once per dimension with independent offset/mult.
    float beatOffset = 0, beatMult = 0;
    float beaty = 0, beatyOffset = 0, beatyMult = 0;
    float beatz = 0, beatzOffset = 0, beatzMult = 0;
    // Per-column Y displacement (PlayerOptions.cpp:1105-1111, m_fMovesY).
    //
    // INDEX BASE: NotITG spells these movey0..movey<N-1> (column N), SM5
    // spells the same array movey1..movey<N> -- `ssprintf("movey%d", i+1)`.
    // NotClon reads NotITG-lineage modcharts, so movey0 is column 0 here. A
    // chart written against SM5's spelling is off by one, which is why the
    // SM5 ports of such charts rewrite these rows.
    float moveyCol[NUM_LANES] = {0, 0, 0, 0, 0};
    // Z-axis twins of the periodic shapes (ArrowEffects.cpp:1200-1290). Same
    // functions, applied to depth instead of sideways.
    float zigzagZ = 0, zigzagZPeriod = 0;
    float sawtoothZ = 0, sawtoothZPeriod = 0;
    float digitalZ = 0, digitalZPeriod = 0, digitalZOffset = 0, digitalZSteps = 0;
    // A flag, not an amount: lets dizzy spin hold heads too.
    float dizzyHolds = 0;
    float stealth = 0;
    // Stealth's partners (ArrowEffects.cpp:470-481). blink strobes everything
    // on a fixed clock; randomvanish blanks a band around the center line.
    float blink = 0, randomvanish = 0;
    // Receptor (fret stack) fade, ReceptorArrowRow.cpp:40-43 -- the only
    // renderer consumer of m_fDark in the whole OITG tree. Notes untouched.
    float dark = 0;
    // ITG m_fCover -- a black quad over the background plane only, at alpha
    // == cover (BrightnessOverlay::SetActualBrightness, Background.cpp:958-984).
    // Consumed directly by drawFrame, not by any ArrowEffects function.
    float cover = 0;
};

// ---------------------------------------------------------------------------
// GetXPos -- lateral displacement. Tornado / drunk / flip / invert / beat.
// ---------------------------------------------------------------------------
// RageMath.cpp:656-665. The <0.01 nudge is upstream's, and its comment says
// why: without it a hold flickers on the frame before it is hit.
inline float rageSquare(float angle) {
    float a = fmodf(angle, 2.0f * 3.14159265f);
    if (a < 0.01f) a += 2.0f * 3.14159265f;
    return a >= 3.14159265f ? -1.0f : 1.0f;
}

// RageMath.cpp:667-687 -- a triangle wave over 0..2pi, peaking at pi/2.
inline float rageTriangle(float angle) {
    const float PI_ = 3.14159265f;
    float a = fmodf(angle, 2.0f * PI_);
    if (a < 0.0f) a += 2.0f * PI_;
    const float r = a * (1.0f / PI_);
    if (r < 0.5f)  return r * 2.0f;
    if (r < 1.5f)  return 1.0f - ((r - 0.5f) * 2.0f);
    return -4.0f + (r * 2.0f);
}

// SelectTanType, :142-148. Cosecant is 1/sin, which is unbounded near a
// multiple of pi; ITG lets it run, and so does this, but the caller clamps
// what it feeds a position so one frame cannot throw a note to infinity.
inline float selectTanType(float angle, bool isCosec) {
    if (!isCosec) return tanf(angle);
    const float s = sinf(angle);
    const float lim = 1e-3f;
    if (fabsf(s) < lim) return s < 0 ? -1.0f / lim : 1.0f / lim;
    return 1.0f / s;
}

// UpdateBeat, ArrowEffects.cpp:192-217. Returns the +-20 envelope: a squared
// ramp up over the first 0.2 of a beat, an eased fall to 0.5, nothing after,
// and the sign flipped on alternate beats.
//
// `offset` shifts which beat it fires on and `mult` multiplies the rate
// (SM passes (beat + accel + offset) * (mult + 1)); both are 0 by default,
// where (mult+1) is 1 and this is the un-parameterised expression exactly.
//
// The bpm/150 divisor is OITG's, not SM5's: it keeps the pulse readable at
// high tempo instead of strobing once per frame.
inline float BeatFactor(float songBeat, float bpm, float offset, float mult) {
    float accelTime = 0.2f, totalTime = 0.5f;
    const float div = fmaxf(1.0f, truncf(bpm / 150.0f));
    accelTime /= div;
    totalTime /= div;

    float beat = (songBeat + accelTime + offset) * (mult + 1.0f);
    beat /= div;
    if (beat < 0.0f) return 0.0f;
    const bool evenBeat = (int(beat) % 2) != 0;
    beat -= truncf(beat);
    beat += 1.0f;
    beat -= truncf(beat);
    if (beat >= totalTime) return 0.0f;
    float amount;
    if (beat < accelTime) {
        amount = scale(beat, 0.0f, accelTime, 0.0f, 1.0f);
        amount *= amount;
    } else {
        amount = scale(beat, accelTime, totalTime, 1.0f, 0.0f);
        amount = 1.0f - (1.0f - amount) * (1.0f - amount);
    }
    if (evenBeat) amount *= -1.0f;
    return amount * 20.0f;                       // :216
}

inline float GetXPos(const Mods& m, int col, float yOffset, float songTime,
                     float songBeat, float bpm) {
    float x = 0.0f;

    if (m.tornado != 0.0f) {
        const int width = 3;                       // 4 or fewer cols per player
        int startCol = col - width, endCol = col + width;
        if (startCol < 0) startCol = 0;
        if (endCol > NUM_LANES - 1) endCol = NUM_LANES - 1;

        float minX = FLT_MAX, maxX = -FLT_MAX;
        for (int i = startCol; i <= endCol; ++i) {
            minX = fminf(minX, laneXPixels(i));
            maxX = fmaxf(maxX, laneXPixels(i));
        }
        const float realX = laneXPixels(col);
        const float between = scale(realX, minX, maxX, -1.0f, 1.0f);
        float rads = acosf(fmaxf(-1.0f, fminf(1.0f, between)));
        // :163 -- (y + offset) * ((period*freq)+freq) / SCREEN_HEIGHT, where
        // the x-dimension frequency is 6.
        const float TF = 6.0f;
        rads += (yOffset + m.tornadoOffsetP) *
                ((m.tornadoPeriod * TF) + TF) / SM_SCREEN_H;
        const float adjusted = scale(cosf(rads), -1.0f, 1.0f, minX, maxX);
        x += (adjusted - realX) * m.tornado;
    }

    // CalculateDrunkAngle, ArrowEffects.cpp:174-180, with the theme metrics
    // DrunkColumnFrequency 0.2 / DrunkOffsetFrequency 10 / DrunkArrowMagnitude
    // 0.5 (metrics.ini:237-239). At period 0 and offset 0 the two frequency
    // terms collapse to the bare constants and this is the old expression
    // exactly -- which is what keeps it hash-neutral.
    if (m.drunk != 0.0f || m.tandrunk != 0.0f) {
        const float COLF = 0.2f, OFFF = 10.0f;
        const float ang = songTime
                        + col * ((m.drunkOffsetP * COLF) + COLF)
                        + yOffset * ((m.drunkPeriod * OFFF) + OFFF) / SM_SCREEN_H;
        if (m.drunk != 0.0f)
            x += m.drunk * cosf(ang) * ARROW_SIZE * 0.5f;
        if (m.tandrunk != 0.0f)
            x += m.tandrunk * selectTanType(ang, m.cosecant != 0.0f)
                 * ARROW_SIZE * 0.5f;
    }

    // The new periodic shapes. Each is the same yOffset input through a
    // different function (ArrowEffects.cpp:766-811).
    if (m.zigzag != 0.0f) {
        const float a = 3.14159265f * (1.0f / (m.zigzagPeriod + 1.0f)) *
                        ((yOffset + 100.0f * m.zigzagOffset) / ARROW_SIZE);
        x += (m.zigzag * ARROW_SIZE * 0.5f) * rageTriangle(a);
    }
    if (m.sawtooth != 0.0f) {
        const float t = (0.5f / (m.sawtoothPeriod + 1.0f) * yOffset) / ARROW_SIZE;
        x += (m.sawtooth * ARROW_SIZE) * (t - floorf(t));
    }
    if (m.square != 0.0f) {
        const float a = 3.14159265f * (yOffset + 1.0f * m.squareOffset) /
                        (ARROW_SIZE + (m.squarePeriod * ARROW_SIZE));
        x += (m.square * ARROW_SIZE * 0.5f) * rageSquare(a);
    }
    if (m.digital != 0.0f) {
        // CalculateDigitalAngle :186-189, then the sine is QUANTISED to
        // (steps+1) levels -- that rounding is what makes it read as digital.
        const float a = 3.14159265f * (yOffset + 1.0f * m.digitalOffset) /
                        (ARROW_SIZE + (m.digitalPeriod * ARROW_SIZE));
        const float steps = m.digitalSteps + 1.0f;
        x += (m.digital * ARROW_SIZE * 0.5f) * floorf(steps * sinf(a) + 0.5f) / steps;
    }

    if (m.flip != 0.0f) {
        const int newCol = NUM_LANES - 1 - col;
        x += (laneXPixels(newCol) - laneXPixels(col)) * m.flip;
    }

    if (m.invert != 0.0f) {
        const int leftOfMiddle = (NUM_LANES - 1) / 2;   // 2
        const int rightOfMiddle = (NUM_LANES + 1) / 2;  // 3
        int first, last;
        if (col <= leftOfMiddle)       { first = 0; last = leftOfMiddle; }
        else if (col >= rightOfMiddle) { first = rightOfMiddle; last = NUM_LANES - 1; }
        else                           { first = col / 2; last = col / 2; }
        int newCol = (first == last) ? col
                                     : int(scale(float(col), float(first), float(last),
                                                 float(last), float(first)) + 0.5f);
        x += (laneXPixels(newCol) - laneXPixels(col)) * m.invert;
    }

    if (m.beat != 0.0f) {
        const float amount = BeatFactor(songBeat, bpm, m.beatOffset, m.beatMult);
        if (amount != 0.0f)
            x += m.beat * (amount * sinf(yOffset / 15.0f + PI_F / 2.0f));
    }
    return x;
}

// ---------------------------------------------------------------------------
// GetYOffset extras -- what ITG does to distance-to-receptor.
// ---------------------------------------------------------------------------
inline float ApplyYMods(const Mods& m, int col, float yOffset, float songBeat) {
    float y = yOffset;
    // ArrowEffects.cpp:55-56 -- "don't mess with the arrows after they've
    // crossed 0". Only reachable with --nobot.
    if (y < 0.0f) return y;

    float adj = 0.0f;
    // :64-72. GetNoteFieldHeight reduces to SCREEN_HEIGHT with no tilt; kept
    // that way even now tilt exists, since our tilt is a world transform, not
    // a notefield resize.
    if (m.boost != 0.0f) {
        const float h = SM_SCREEN_H;
        const float ny = y * 1.5f / ((y + h / 1.2f) / h);
        float d = m.boost * (ny - y);
        if (d < -400.0f) d = -400.0f; else if (d > 400.0f) d = 400.0f;
        adj += d;
    }
    // :73-82. SCALE(y, 0, h, 0, 1) is y/h, so the contribution is
    // brake * (y^2/h - y), clamped +-400 "so that in BOOST+BRAKE, BRAKE
    // doesn't overpower BOOST" (the source's own comment).
    if (m.brake != 0.0f) {
        const float h = SM_SCREEN_H;
        const float ny = y * (y / h);
        float d = m.brake * (ny - y);
        if (d < -400.0f) d = -400.0f; else if (d > 400.0f) d = 400.0f;
        adj += d;
    }
    // :83-84
    // :546 -- WaveModMagnitude 20 / WaveModHeight 38 (metrics.ini:204-205).
    if (m.wave != 0.0f)
        adj += m.wave * 20.0f * sinf(y / ((m.wavePeriod * 38.0f) + 38.0f));
    y += adj;                                    // :86, before boomerang

    if (m.boomerang != 0.0f) {
        const float peak = 1.0f;
        float t = y / SM_SCREEN_H;
        y += m.boomerang * (-2.0f * t * t + peak) * SM_SCREEN_H * 0.5f;
    }
    if (m.expand != 0.0f) {
        const float wave = sinf(songBeat * PI_F / 2.0f);
        y *= 1.0f + m.expand * (wave * 0.5f);
    }
    return y;
}

// ---------------------------------------------------------------------------
// Reverse / centered -- ArrowGetReverseShiftAndScale, ArrowEffects.cpp:141-157.
// fYReverseOffsetPixels is GRAY_ARROWS_Y_REVERSE - GRAY_ARROWS_Y_STANDARD =
// 145 - (-125) = 270 in the default theme (metrics.ini:2942-2943).
//
// Normalised so the no-mod case is the identity REGARDLESS of mini: ITG's
// baseline shift is -R/zoom/2, so that term is subtracted back out. centered
// lerps the shift toward 0.0f (SM5's value; OITG's literal 0.5f at :154 is a
// half-pixel typo). Values outside 0..1 extrapolate, which is load-bearing:
// Saitama2000 strobes `-150% Centered` to throw the whole field off-path.
// ---------------------------------------------------------------------------
static const float Y_REVERSE_OFFSET_PX = 270.0f;

// GetReversePercentForColumn (PlayerOptions.cpp:1313-1332). `reverse` applies
// to every column; split/alternate/cross add to a subset. With all four at
// zero this is 0 for every column, and with only `reverse` set it is
// m.reverse for every column -- so the un-widened behaviour is preserved
// exactly, which is what keeps the pinned baselines pinned.
inline float ReversePercentForCol(const Mods& m, int col) {
    float f = m.reverse;
    if (col >= NUM_LANES / 2)                     f += m.split;
    if ((col % 2) == 1)                           f += m.alternate;
    const int firstCross = NUM_LANES / 4;
    const int lastCross  = NUM_LANES - 1 - firstCross;
    if (col >= firstCross && col <= lastCross)    f += m.cross;
    return f;
}

// `col` selects the per-column reverse percent. -1 means "the whole field",
// used by anything that is not a single column (the board, the camera-space
// widening). Defaulted so every existing call site keeps its meaning.
inline float ApplyScrollPos(const Mods& m, float yPx, int col = -1) {
    const float rev = (col < 0) ? m.reverse : ReversePercentForCol(m, col);
    if (rev == 0.0f && m.centered == 0.0f) return yPx;
    float zoom = 1.0f - m.mini * 0.5f;                       // :145
    if (fabsf(zoom) < 0.01f) zoom = 0.01f;                   // :148-149
    const float base = -Y_REVERSE_OFFSET_PX / zoom / 2.0f;
    float shift = scale(rev, 0.0f, 1.0f, base, -base);       // :152
    shift = scale(m.centered, 0.0f, 1.0f, shift, 0.0f);      // :154, SM5 form
    const float sc = scale(rev, 0.0f, 1.0f, 1.0f, -1.0f);    // :156
    return yPx * sc + (shift - base);
}

// The same transform in scroll-z units. Under full reverse notes come from
// BEHIND the camera and fly away toward the relocated strike line -- that is
// the faithful reading of "arrows from the other side" on a highway, and the
// caller must spatially clamp what lands behind the eye.
inline float ApplyScrollZ(const Mods& m, float z, int col = -1) {
    const float rev = (col < 0) ? m.reverse : ReversePercentForCol(m, col);
    if (rev == 0.0f && m.centered == 0.0f) return z;
    const float K = ARROW_SIZE * 1.6f;
    return ApplyScrollPos(m, z * K, col) / K;
}

// Vertical bob. ITG's bumpy rides on top of the arrow's own position.
// `songBeat`/`bpm` are only for the beat family; every other term ignores
// them, which is why they are defaulted rather than threaded everywhere.
inline float GetYPosBump(const Mods& m, int col, float yOffset,
                         float songBeat = 0.0f, float bpm = 120.0f) {
    float y = 0.0f;
    // beaty / beatz -- the Y and Z dimensions of the beat pulse. ITG's Z is
    // depth; NotClon already approximates depth displacement as a world-Y bump
    // (that is what bumpy does here), so beatz rides the same path rather
    // than inventing a second convention.
    if (m.beaty != 0.0f) {
        const float a = BeatFactor(songBeat, bpm, m.beatyOffset, m.beatyMult);
        if (a != 0.0f) y += m.beaty * a * sinf(yOffset / 15.0f + 3.14159265f / 2.0f);
    }
    if (m.beatz != 0.0f) {
        const float a = BeatFactor(songBeat, bpm, m.beatzOffset, m.beatzMult);
        if (a != 0.0f) y += m.beatz * a * sinf(yOffset / 15.0f + 3.14159265f / 2.0f);
    }
    // CalculateBumpyAngle, :182-185: (y + 100*offset) / ((period*16)+16).
    // NotClon's existing offset term is bumpyspeed's integrated phase in
    // arrow-widths, which is why it is 64 here and 100 there.
    if (m.bumpy != 0.0f)
        y += m.bumpy * 40.0f * sinf((yOffset + m.bumpyOffset * 64.0f) /
                                    ((m.bumpyPeriod * 16.0f) + 16.0f));
    if (m.tipsy != 0.0f)
        y += m.tipsy * (cosf(m.tipsyOffset * 1.2f + col * 1.8f) * ARROW_SIZE * 0.4f);
    if (col >= 0 && col < NUM_LANES && m.moveyCol[col] != 0.0f)
        y += m.moveyCol[col] * ARROW_SIZE;

    // The Z twins. ITG displaces depth; NotClon approximates depth as a
    // world-Y bump -- the same substitution bumpy and beatz already make here,
    // so all three stay consistent rather than each inventing a convention.
    const float PI2_ = 3.14159265f;
    if (m.zigzagZ != 0.0f) {
        const float a = PI2_ * (1.0f / (m.zigzagZPeriod + 1.0f)) *
                        (yOffset / ARROW_SIZE);
        y += (m.zigzagZ * ARROW_SIZE * 0.5f) * rageTriangle(a);
    }
    if (m.sawtoothZ != 0.0f) {
        const float t = (0.5f / (m.sawtoothZPeriod + 1.0f) * yOffset) / ARROW_SIZE;
        y += (m.sawtoothZ * ARROW_SIZE) * (t - floorf(t));
    }
    if (m.digitalZ != 0.0f) {
        const float a = PI2_ * (yOffset + 1.0f * m.digitalZOffset) /
                        (ARROW_SIZE + (m.digitalZPeriod * ARROW_SIZE));
        const float steps = m.digitalZSteps + 1.0f;
        y += (m.digitalZ * ARROW_SIZE * 0.5f) * floorf(steps * sinf(a) + 0.5f) / steps;
    }
    return y;
}

// ---------------------------------------------------------------------------
// Rotation. ITG spins the arrow sprite; here the same angle spins the quad
// (quadUpRot). All three return DEGREES, in ITG's screen-Y-down convention --
// the caller negates X and Z for NotClon's Y-up world (conjugation by
// diag(1,-1,1) flips rotX and rotZ, leaves rotY).
// ---------------------------------------------------------------------------

// roll -- ArrowEffects.cpp:344-352: EFFECT_ROLL * fYOffset/2 degrees. A
// function of yOffset, so the gem tumbles end-over-end as it travels; the
// input is the post-ApplyYMods offset (NoteDisplay.cpp:1023).
inline float GetRotationX(const Mods& m, float yOffset) {
    return (m.roll != 0.0f) ? m.roll * yOffset * 0.5f : 0.0f;
}

// twirl -- ArrowEffects.cpp:354-362: same law as roll, about Y (:1025).
inline float GetRotationY(const Mods& m, float yOffset) {
    return (m.twirl != 0.0f) ? m.twirl * yOffset * 0.5f : 0.0f;
}

// dizzy -- ArrowEffects.cpp:364-378: (noteBeat - songBeat) * dizzy is
// RADIANS, wrapped to 2pi, then converted to degrees. (An earlier version
// here multiplied by 180 first and converted back -- pi times too fast.)
// confusion -- SM5-only, ArrowEffects.cpp:614-629 (ReceptorGetRotationZ,
// added to every note at :598-599; OpenITG has no EFFECT_CONFUSION at all):
// songBeat * confusion in radians, wrapped, and the degree conversion is
// NEGATIVE (-180/PI), so it counter-rotates dizzy for the same sign.
// `isHoldHead` gates dizzy only. ArrowEffects.cpp:906 applies dizzy when
// `m_bDizzyHolds || !bIsHoldHead` -- so by default a hold's head does NOT
// spin, because a spinning head detaches visually from the ribbon it caps.
// `dizzyholds` opts back in. confusion is unaffected; it turns everything.
inline float GetRotationZ(const Mods& m, float noteBeat, float songBeat,
                          bool isHoldHead = false) {
    float r = 0.0f;
    if (m.confusion != 0.0f) {
        float c = songBeat * m.confusion + m.confusionOffset;
        c = fmodf(c, 2.0f * PI_F);
        r += c * (-180.0f / PI_F);
    }
    if (m.dizzy != 0.0f && (m.dizzyHolds != 0.0f || !isHoldHead)) {
        float d = (noteBeat - songBeat) * m.dizzy;
        d = fmodf(d, 2.0f * PI_F);
        r += d * (180.0f / PI_F);
    }
    return r;
}

// tiny's per-note zoom, SM5 ArrowEffects.cpp:813-818. (mini is deliberately
// NOT here: it is the whole-field transform in drawFrame, Player.cpp:723.)
inline float GetZoom(const Mods& m) {
    return (m.tiny != 0.0f) ? powf(0.5f, m.tiny) : 1.0f;
}

// tiny's column pull-together, SM5 ArrowEffects.cpp:561-566: the whole
// offset-from-centre scales by min(base^tiny, gate). The theme metrics are
// TinyPercentBase=0.5, TinyPercentGate=1 (_fallback/metrics.ini:225-226) --
// the min against 1 is what stops negative tiny pushing lanes apart.
inline float GetTinyColScale(const Mods& m) {
    return (m.tiny != 0.0f) ? fminf(powf(0.5f, m.tiny), 1.0f) : 1.0f;
}

// ---------------------------------------------------------------------------
// Appearance -- ArrowEffects.cpp:381-505, ported to ITG's actual structure.
//
// The three functions below are one system and must not be split up. ITG draws
// every arrow TWICE: a diffuse pass whose alpha is GetAlpha, and a glow pass
// (Sprite.cpp:536-541, SetTextureModeGlow -- the texture's alpha with the RGB
// replaced by a flat colour) whose alpha is GetGlow. GetAlpha is a HARD BINARY
// CUT at 50% visible (:493); the crossfade a player perceives is entirely the
// glow pass filling in around the cut, because GetGlow peaks exactly where
// GetAlpha flips (:503-504).
//
// This matters for stealth specifically. `fVisibleAdjust -= stealth` (:468-469)
// means stealth alone gives percentVisible = 1 - stealth, so:
//     stealth <  0.5  ->  alpha 1, glow < 1.3   (visible, faintly outlined)
//     stealth >= 0.5  ->  alpha 0, glow > 0     (a WHITE SILHOUETTE, not blank)
// Implementing GetAlpha without GetGlow renders any stealth >= 50% as nothing
// at all. Real modfiles lean on this: a section holding `90% Stealth` with
// short `50% Stealth` pulses over the top is two minutes of black screen under
// alpha alone, and with the glow pass it is what the author wrote -- white
// arrow silhouettes strobing between 26% and full.
//
// (devdocs/spec/unwired-mods.md B.4 lists stealth as "available for free" from
// GetAlpha. That is wrong, and this comment is the correction.)
// ---------------------------------------------------------------------------

// CENTER_LINE_Y :381, and :384-391's mini compensation.
inline float GetCenterLine(const Mods& m) {
    const float zoom = 1.0f - m.mini * 0.5f;
    return 160.0f / zoom;
}

// ArrowGetPercentVisible, :441-484.
//
// NOTE: ITG feeds this GetYPos WITHOUT reverse, i.e. yOffset plus the tipsy
// term (:443-444). NotClon applies tipsy as a world-Y bump instead of along the
// scroll axis (see devdocs/spec/unwired-mods.md B.6), so the consistent input here
// is the bare yOffset. Moving tipsy to the scroll axis is a separate decision
// with its own hash consequence; when it happens, this call site changes too.
// `songTime` feeds blink only: ITG's clock there is GetTimeSinceStartFast
// (:472), substituted with the song time so seeking is deterministic -- the
// same substitution drunk's header documents.
inline float GetPercentVisible(const Mods& m, float yOffset, float songTime) {
    if (m.sudden == 0.0f && m.hidden == 0.0f && m.stealth == 0.0f &&
        m.blink == 0.0f && m.randomvanish == 0.0f) return 1.0f;

    const float yPos = yOffset;
    if (yPos < 0.0f) return 1.0f;                       // :448-449, past the receptors

    const float C  = GetCenterLine(m);
    const float FD = 40.0f;                             // FADE_DIST_Y :382
    const float hs = m.hidden * m.sudden;               // GetHiddenSudden :393-398

    // :412-438. Read out with both offsets 0 and one appearance on (hs = 0):
    // hidden spans 160 -> 120, so it kills notes NEAR the receptor; sudden
    // spans 200 -> 160, so it kills notes FAR from it.
    const float hiddenEnd   = C + FD * scale(hs, 0.0f, 1.0f, -1.00f, -1.25f) + C * m.hiddenOffset;
    const float hiddenStart = C + FD * scale(hs, 0.0f, 1.0f,  0.00f, -0.25f) + C * m.hiddenOffset;
    const float suddenEnd   = C + FD * scale(hs, 0.0f, 1.0f, -0.00f,  0.25f) + C * m.suddenOffset;
    const float suddenStart = C + FD * scale(hs, 0.0f, 1.0f,  1.00f,  1.25f) + C * m.suddenOffset;

    float adj = 0.0f;
    if (m.hidden != 0.0f) {                             // :455-460
        float a = scale(yPos, hiddenStart, hiddenEnd, 0.0f, -1.0f);
        a = fminf(0.0f, fmaxf(-1.0f, a));
        adj += m.hidden * a;
    }
    if (m.sudden != 0.0f) {                             // :461-466
        float a = scale(yPos, suddenStart, suddenEnd, -1.0f, 0.0f);
        a = fminf(0.0f, fmaxf(-1.0f, a));
        adj += m.sudden * a;
    }
    if (m.stealth != 0.0f) adj -= m.stealth;            // :468-469
    if (m.blink != 0.0f) {                              // :470-475
        float f = sinf(songTime * 10.0f);
        // Quantize (RageUtil.h:235): int() truncates toward zero, so the
        // negative lobe quantizes asymmetrically (min level -0.6667, not -1).
        // Ported exactly. SCALE(f,0,1,-1,0) reduces to f - 1. Note the
        // percent is an on/off gate, verbatim ITG -- a .ncmod approach ramp
        // pops it on at the first nonzero frame, same class of quirk as the
        // hidden-below-50% cut.
        f = float(int((f + 0.3333f / 2.0f) / 0.3333f)) * 0.3333f;
        adj += f - 1.0f;
    }
    if (m.randomvanish != 0.0f) {                       // :476-481
        // Despite the name there is no randomness: notes vanish inside a
        // +-160px band around the center line (fully out within +-80px).
        // fDistFromCenterLine is :446; the SCALE is UNCLAMPED on purpose --
        // beyond 160px the term goes positive and genuinely counteracts
        // hidden/sudden, with only the final clamp below, as in ITG.
        const float fRealFadeDist = 80.0f;
        adj += scale(fabsf(yPos - GetCenterLine(m)), fRealFadeDist,
                     2.0f * fRealFadeDist, -1.0f, 0.0f) * m.randomvanish;
    }

    return fmaxf(0.0f, fminf(1.0f, 1.0f + adj));        // :483
}

// :486-494. Binary, deliberately. A .ncmod approach ramp will therefore snap
// the cut line into place rather than fade it -- that is ITG's behaviour and
// the glow pass is what makes it read as a fade.
inline float GetAlpha(const Mods& m, float yOffset, float songTime) {
    return (GetPercentVisible(m, yOffset, songTime) > 0.5f) ? 1.0f : 0.0f;
}

// :496-505. Flat white at this alpha, over the diffuse pass. Peaks at 1.3 where
// percentVisible is exactly 0.5; clamped here because it is used directly as a
// blend factor, where ITG passed it through a RageColor the rasteriser clamped.
inline float GetGlow(const Mods& m, float yOffset, float songTime) {
    if (m.sudden == 0.0f && m.hidden == 0.0f && m.stealth == 0.0f &&
        m.blink == 0.0f && m.randomvanish == 0.0f) return 0.0f;
    const float dist = fabsf(GetPercentVisible(m, yOffset, songTime) - 0.5f);
    return fmaxf(0.0f, fminf(1.0f, scale(dist, 0.0f, 0.5f, 1.3f, 0.0f)));
}

// Convert the pixel-space result into highway units.
inline float pxToUnits(float px) { return px * (LANE_W / ARROW_SIZE); }

}  // namespace nc
