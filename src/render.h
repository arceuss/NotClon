// Layer assembly for the CH highway.
//
// Everything is painter's-algorithm: depth testing is off, because every
// Clone Hero highway layer is ZWrite Off. Order is decided entirely by the
// sequence of Layers pushed here.
//
// Draw order comes from the prefab's sortingOrders, with one correction that
// is easy to get wrong: the neck draws FIRST because its material carries
// m_CustomRenderQueue: 0 (the only material in the project with a non-default
// queue). Its MeshRenderer sortingOrder is 0, which would otherwise put it
// after every highway sprite (those sit at -25000..-28000) and cover them.
#pragma once

#include "highway.h"
#include "chart.h"
#include "mods.h"
#include <vector>
#include <algorithm>
#include <cmath>

namespace ch {

struct Vtx { float x, y, z, u, v, r, g, b, a; };

enum Blend {
    BLEND_NECK = 0,   // SrcAlpha, OneMinusSrcAlpha   (Custom/CubeShader alpha)
    BLEND_SPRITE = 1, // One, OneMinusSrcAlpha        (premultiplied in shader)
    BLEND_ADD = 2,    // One, One
};

struct Layer {
    unsigned tex = 0;
    int blend = BLEND_SPRITE;
    bool useTex = true;
    std::vector<Vtx> verts;
};

// A quad lying flat on the highway plane (constant y), spanning x and z.
inline void quadFlat(std::vector<Vtx>& out, float x0, float z0, float x1, float z1,
                     float y, float u0, float v0, float u1, float v1,
                     float r, float g, float b, float a) {
    Vtx q[4] = {
        {x0, y, z0, u0, v0, r, g, b, a},
        {x1, y, z0, u1, v0, r, g, b, a},
        {x1, y, z1, u1, v1, r, g, b, a},
        {x0, y, z1, u0, v1, r, g, b, a},
    };
    out.push_back(q[0]); out.push_back(q[1]); out.push_back(q[2]);
    out.push_back(q[0]); out.push_back(q[2]); out.push_back(q[3]);
}

// An upright quad facing -Z (constant z), spanning x and y. Notes use this:
// they are NOT laid flat on the board -- Note.prefab has euler (0,0,0), and
// only the strings/sidebars/sustain roots carry the (90,0,0) that lays a
// sprite down onto the highway.
inline void quadUp(std::vector<Vtx>& out, float x0, float y0, float x1, float y1,
                   float z, float u0, float v0, float u1, float v1,
                   float r, float g, float b, float a) {
    Vtx q[4] = {
        {x0, y0, z, u0, v0, r, g, b, a},
        {x1, y0, z, u1, v0, r, g, b, a},
        {x1, y1, z, u1, v1, r, g, b, a},
        {x0, y1, z, u0, v1, r, g, b, a},
    };
    out.push_back(q[0]); out.push_back(q[1]); out.push_back(q[2]);
    out.push_back(q[0]); out.push_back(q[2]); out.push_back(q[3]);
}

// An upright quad (the note billboard) rotated about its own pivot, ITG-style.
// Local extents are given relative to the pivot, which is at the local origin:
// CH's pivot is x=0.5 of the width but y=NOTE_PIVOT_Y (0.16) of the height, so
// hy0/hy1 are asymmetric -- exactly the y0/y1 the note loop already computes.
//
// Order matches Actor::BeginDraw (Actor.cpp:393-421): PreMultMatrix pushes
// TranslateAndScale then Rotation, and with pre-multiplication the LAST push
// applies to the vertex FIRST -- so the vertex order is rotate about pivot,
// zoom, translate. The three lines in P are RageMatrixRotationXYZ's rows for
// a row vector (x,y,0), degrees converted inside as Rage does.
//
// Angles are DEGREES, already sign-corrected for NotClon's Y-up world (the
// caller passes -rotX, +rotY, -rotZ -- conjugation by diag(1,-1,1) flips X
// and Z, leaves Y; ITG screen +Y is down).
//
// The zero-rotation, unit-zoom path MUST be bit-identical to quadUp or the
// pinned hashes move: (x-cx)+cx != x in IEEE754. Hence the early-out, which
// is exact because cx + (-h) == cx - h.
//
// Depth caveat: ITG turns the Z buffer ON under bumpy/twirl (NeedZBuffer,
// ArrowEffects.cpp:533-541). NotClon cannot -- painter's order is
// load-bearing and every layer is ZWrite Off -- so a quad twirled past 90
// degrees self-sorts by draw order rather than depth. Accepted.
inline void quadUpRot(std::vector<Vtx>& out,
                      float cx, float cy, float z,
                      float hx0, float hy0, float hx1, float hy1,
                      float rx, float ry, float rz, float zoom,
                      float u0, float v0, float u1, float v1,
                      float r, float g, float b, float a) {
    if (rx == 0.0f && ry == 0.0f && rz == 0.0f && zoom == 1.0f) {
        quadUp(out, cx + hx0, cy + hy0, cx + hx1, cy + hy1, z,
               u0, v0, u1, v1, r, g, b, a);
        return;
    }
    const float D = 3.14159265358979f / 180.0f;
    const float cX = cosf(rx*D), sX = sinf(rx*D);
    const float cY = cosf(ry*D), sY = sinf(ry*D);
    const float cZ = cosf(rz*D), sZ = sinf(rz*D);

    auto P = [&](float x, float y, float u, float v) {
        float px = x*(cZ*cY)            + y*(-sZ*cY);
        float py = x*(cZ*sY*sX+sZ*cX)   + y*(-sZ*sY*sX+cZ*cX);
        float pz = x*(cZ*sY*cX-sZ*sX)   + y*(-sZ*sY*cX-cZ*sX);
        Vtx t; t.x = cx + zoom*px; t.y = cy + zoom*py; t.z = z + zoom*pz;
        t.u = u; t.v = v; t.r = r; t.g = g; t.b = b; t.a = a;
        return t;
    };
    Vtx q0 = P(hx0, hy0, u0, v0), q1 = P(hx1, hy0, u1, v0);
    Vtx q2 = P(hx1, hy1, u1, v1), q3 = P(hx0, hy1, u0, v1);
    out.push_back(q0); out.push_back(q1); out.push_back(q2);
    out.push_back(q0); out.push_back(q2); out.push_back(q3);
}

// --- sustains ---------------------------------------------------------------
//
// A sustain is a long ribbon and the mods displace notes as a function of
// yOffset, so ONE quad would detach the tail from its gem the moment drunk or
// tornado is nonzero. It is tessellated in z at a fixed step, the way OpenITG
// tessellates a hold body (NoteDisplay.cpp:675-727), re-evaluating the mods per
// row. OpenITG parameterises that loop by screen position and has to invert
// back to an offset; NotClon parameterises by z, which IS the offset domain, so
// every quantity below is an algebraic function of the beat and a cold seek is
// exact.
//
// The sprite is 9-sliced with a top border only, so the mesh has three rows in
// Y: 0, L - 0.38, L. 9-slicing is piecewise linear in v, so inserting rows
// INSIDE either span with v interpolated linearly is texturally a no-op --
// provided the seam is a hard row and v comes from the two-segment map rather
// than one global lerp. That is the whole reconciliation.

// Sorted row positions across [zLo, zHi], forcing every value of hard[] that
// lies strictly inside. hard[] may be unsorted. Returns the count.
inline int sustainRows(float zLo, float zHi, const float* hard, int nHard,
                       float* z, int maxRows) {
    int n = 0;
    if (zHi <= zLo || maxRows < 2) return 0;
    z[n++] = zLo;
    // Uniform grid, then the hard rows merged in and the whole thing sorted.
    for (float t = zLo + SUS_STEP_Z; t < zHi && n < maxRows - 1; t += SUS_STEP_Z)
        z[n++] = t;
    for (int i = 0; i < nHard && n < maxRows - 1; ++i)
        if (hard[i] > zLo && hard[i] < zHi) z[n++] = hard[i];
    z[n++] = zHi;
    std::sort(z, z + n);
    n = int(std::unique(z, z + n) - z);
    return n;
}

// One ribbon. `lane` 0..4; `zStart` is gemZ + SUS_Z_OFFSET unheld or 0 held;
// `len` is remainingSeconds*noteSpeed + SUS_LEN_OFFSET.
inline void buildSustainBody(std::vector<Vtx>& out, const nc::Mods& mods,
                             int lane, float zStart, float len,
                             float halfW, int frame, const float tint[3],
                             float songTime, float songBeat, float bpm,
                             std::vector<Vtx>* glowOut = nullptr) {
    const float zEnd = zStart + len;
    const float zLo  = fmaxf(zStart, 0.0f);
    const float zHi  = fminf(zEnd, SUS_FAR_LIMIT);
    if (zHi <= zLo) return;

    const float border = fminf(SUS_TOP_BORDER, len);
    const float zB     = zEnd - border;
    // The 9-slice seam, the start of the far-fade cubic, and the ribbon end.
    const float hard[3] = { zB, SUS_FAR_LIMIT - 1.0f, zEnd };

    float u0, u1, v0, vB, v1;
    sustainFrameUV(frame, u0, u1, v0, vB, v1);

    float zr[192];
    const int n = sustainRows(zLo, zHi, hard, 3, zr, 192);
    if (n < 2) return;

    Vtx L[192], R[192], GL[192], GR[192];
    for (int i = 0; i < n; ++i) {
        const float z = zr[i];
        // v from the two-segment 9-slice map, and from the UNCLIPPED span --
        // clipping must not slide the texture (NoteDisplay.cpp:657-669 makes
        // exactly this point about fYBodyTop/fYBodyBottom).
        float v;
        if (zB > zStart && z <= zB) v = v0 + (vB - v0) * (z - zStart) / (zB - zStart);
        else                        v = vB + (v1 - vB) * (z - zB) / border;

        // 1 - smoothstep(_FarLimit - 1, _FarLimit, z), baked into vertex alpha.
        const float t    = fminf(1.0f, fmaxf(0.0f, z - (SUS_FAR_LIMIT - 1.0f)));
        const float fade = 1.0f - t * t * (3.0f - 2.0f * t);

        const float y0 = z*nc::ARROW_SIZE*1.6f;
        const float yOff = nc::ApplyYMods(mods,lane,y0,songBeat);
        const float a  = fade * nc::GetAlpha(mods, yOff, songTime);
        const float cx = noteX(lane) +
                         nc::pxToUnits(nc::GetXPos(mods, lane, yOff, songTime, songBeat, bpm));
        const float dy = nc::pxToUnits(nc::GetYPosBump(mods, lane, yOff)) * 0.5f;

        const float zAdjusted = yOff == y0 ? z
                                           : yOff/(nc::ARROW_SIZE*1.6f);
        const float zD = nc::ApplyScrollZ(mods,zAdjusted,lane);
        L[i] = {cx - halfW, dy, zD, u0, v, tint[0], tint[1], tint[2], a};
        R[i] = {cx + halfW, dy, zD, u1, v, tint[0], tint[1], tint[2], a};
        // The ITG bGlow pass, per row: flat white at GetGlow's alpha, under the
        // same far fade the body gets. Peaks exactly where GetAlpha flips, so
        // the ribbon crossfades to a silhouette instead of vanishing.
        if (glowOut) {
            const float g = fade * nc::GetGlow(mods, yOff, songTime);
            GL[i] = L[i]; GR[i] = R[i];
            GL[i].r = GL[i].g = GL[i].b = 1.0f; GL[i].a = g;
            GR[i].r = GR[i].g = GR[i].b = 1.0f; GR[i].a = g;
        }
    }
    for (int i = 0; i + 1 < n; ++i) {
        out.push_back(L[i]); out.push_back(R[i]);   out.push_back(R[i+1]);
        out.push_back(L[i]); out.push_back(R[i+1]); out.push_back(L[i+1]);
    }
    if (glowOut) {
        for (int i = 0; i + 1 < n; ++i) {
            glowOut->push_back(GL[i]); glowOut->push_back(GR[i]);   glowOut->push_back(GR[i+1]);
            glowOut->push_back(GL[i]); glowOut->push_back(GR[i+1]); glowOut->push_back(GL[i+1]);
        }
    }
}

// The held glow, sortingOrder -999, additive, always anchored at z = 0.
// SustainGlow's fragment needs two terms that are exactly linear in z, so they
// ride in the vertex stream: vUV.y = endDistance, vCol.a = z * 10.
inline void buildSustainGlow(std::vector<Vtx>& out, const nc::Mods& mods,
                             int lane, float len, float halfW,
                             const float tint[3],
                             float songTime, float songBeat, float bpm) {
    const float zHi = fminf(len, SUS_FAR_LIMIT);
    if (zHi <= 0.0f) return;
    const float tailEnd = fminf(SUS_FAR_LIMIT, len);
    const float hard[2] = { tailEnd - SUS_GLOW_EDGE, tailEnd };

    float zr[192];
    const int n = sustainRows(0.0f, zHi, hard, 2, zr, 192);
    if (n < 2) return;

    Vtx L[192], R[192];
    for (int i = 0; i < n; ++i) {
        const float z = zr[i];
        float e = 1.0f - fminf(1.0f, fmaxf(0.0f,
                      (z - (tailEnd - SUS_GLOW_EDGE)) / SUS_GLOW_EDGE));
        const float y0 = z*nc::ARROW_SIZE*1.6f;
        const float yOff = nc::ApplyYMods(mods,lane,y0,songBeat);
        const float cx = noteX(lane) +
                         nc::pxToUnits(nc::GetXPos(mods, lane, yOff, songTime, songBeat, bpm));
        const float dy = nc::pxToUnits(nc::GetYPosBump(mods, lane, yOff)) * 0.5f;

        const float zAdjusted = yOff == y0 ? z
                                           : yOff/(nc::ARROW_SIZE*1.6f);
        const float zD = nc::ApplyScrollZ(mods,zAdjusted,lane);
        L[i] = {cx - halfW, dy, zD, -1.0f, e, tint[0], tint[1], tint[2], z * 10.0f};
        R[i] = {cx + halfW, dy, zD,  1.0f, e, tint[0], tint[1], tint[2], z * 10.0f};
    }
    for (int i = 0; i + 1 < n; ++i) {
        out.push_back(L[i]); out.push_back(R[i]);   out.push_back(R[i+1]);
        out.push_back(L[i]); out.push_back(R[i+1]); out.push_back(L[i+1]);
    }
}

// The neck: the real 112-vertex NeckPlane mesh with its baked vertex fade.
// Reproducing the mesh rather than using one big quad matters, because the
// fade is interpolated between those specific rows.
inline void buildNeck(std::vector<Vtx>& out, float songTime, float noteSpeed) {
    const float vOffset = songTime * noteSpeed / HIGHWAY_TILE_UNITS;
    const int R = NECK_ROWS - 1;   // 55 spans

    for (int r = 0; r < R; ++r) {
        int i0 = 2 * r, i1 = 2 * r + 1, i2 = 2 * r + 2, i3 = 2 * r + 3;
        float zA = NECK_Z0 + (NECK_Z1 - NECK_Z0) * (float(r) / float(R));
        float zB = NECK_Z0 + (NECK_Z1 - NECK_Z0) * (float(r + 1) / float(R));
        float vA = (float(r) / float(R)) * NECK_TILES_V + vOffset;
        float vB = (float(r + 1) / float(R)) * NECK_TILES_V + vOffset;

        float aL0 = neckVertexAlpha(i0), aR0 = neckVertexAlpha(i1);
        float aL1 = neckVertexAlpha(i2), aR1 = neckVertexAlpha(i3);

        // RGB ramps with A, so the falloff is effectively squared. Tint by the
        // collapsed Lambert ambient term.
        auto V = [&](float x, float z, float u, float v, float a) {
            Vtx t; t.x = x; t.y = 0.0f; t.z = z; t.u = u; t.v = v;
            t.r = a * AMBIENT; t.g = a * AMBIENT; t.b = a * AMBIENT; t.a = a;
            return t;
        };
        Vtx L0 = V(-NECK_HALF_W, zA, 0.0f, vA, aL0);
        Vtx R0 = V( NECK_HALF_W, zA, 1.0f, vA, aR0);
        Vtx L1 = V(-NECK_HALF_W, zB, 0.0f, vB, aL1);
        Vtx R1 = V( NECK_HALF_W, zB, 1.0f, vB, aR1);

        // NeckPlane winding: (2r, 2r+2, 2r+1), (2r+2, 2r+3, 2r+1)
        out.push_back(L0); out.push_back(L1); out.push_back(R0);
        out.push_back(L1); out.push_back(R1); out.push_back(R0);
    }
}

inline void buildSidebars(std::vector<Vtx>& out) {
    // left: u 0 at the outer edge
    quadFlat(out, -SIDEBAR_X_OUT, SIDEBAR_Z0, -SIDEBAR_X_IN, SIDEBAR_Z1,
             SIDEBAR_Y, 0.0f, 0.0f, 1.0f, 1.0f, 1, 1, 1, 1);
    // right: scale.x = -1 mirrors U only, never V
    quadFlat(out, SIDEBAR_X_OUT, SIDEBAR_Z0, SIDEBAR_X_IN, SIDEBAR_Z1,
             SIDEBAR_Y, 0.0f, 0.0f, 1.0f, 1.0f, 1, 1, 1, 1);
}

inline void buildStrings(std::vector<Vtx>& out) {
    const float c = STRING_TINT;
    for (int i = 0; i < 5; ++i) {
        quadFlat(out, STRING_X[i] - STRING_HALF_W, STRING_Z0,
                      STRING_X[i] + STRING_HALF_W, STRING_Z1,
                 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, c, c, c, 1.0f);
    }
}

// Port of BeatRenderer.RenderBeats (BeatRenderer.cs:115-196): the schedule
// comes from the chart (Chart.cs GetBeats, precomputed at load), a line sits at
// strikeLine + (t - songTime)*speed + BEAT_Z_OFFSET as an UPRIGHT quad, and the
// fade runs (BEAT_FAR - z) over the note fade region. The old version of this
// function drew flat stripes at invented sizes with a major line every fourth
// beat -- plausible, and wrong on every count.
inline void buildBeatLines(std::vector<Vtx>& out, const nc::Chart& chart,
                           double beat, float songTime, float noteSpeed,
                           const nc::Mods* mods = nullptr) {
    (void)beat;
    for (const auto& b : chart.beatlines) {
        float z = float(chart.scrollSec(b.sec) - songTime) * noteSpeed;
        if (mods) z = nc::ApplyScrollZ(*mods, z);   // reverse/centered
        z += BEAT_Z_OFFSET;                          // BeatRenderer.cs:152
        if (z > BEAT_FAR) break;                     // :154, schedule is sorted
        if (z <= NOTE_CULL_NEAR) continue;           // :157 noteZPosCloseLimit
        // :163-165 -- (far - pos) / (far - positionFullyVisible), the same
        // region the notes fade over; clamped where Unity's colour pipe clamps.
        const float fade = fminf(1.0f, fmaxf(0.0f,
                           (BEAT_FAR - z) / NOTE_FADE_LEN));
        const float a = BEAT_ALPHA[b.style] * fade;
        const float H = BEAT_SPRITE_H * BEAT_SCALE[b.style];
        quadUp(out, -BEAT_SPRITE_W * 0.5f, -BEAT_PIVOT_Y * H,
                     BEAT_SPRITE_W * 0.5f, (1.0f - BEAT_PIVOT_Y) * H,
               z, 0.0f, 0.0f, 1.0f, 1.0f, 1, 1, 1, a);
    }
}


// One fret button. Six quads, back to front, mirroring the prefab's
// sortingOrders. `press` is 0 at rest and 1 fully pressed; the pop curve is
// applied by the caller as a y offset / scale, so this stays purely layout.
//
// Blue and Orange set m_FlipX, which mirrors u about the quad centre; the
// geometry is unchanged because pivot.x is 0.5 and the extents are symmetric.
// CH lefty flip swaps the four outer FretAnimator positions and negates their
// local X scale; the centre fret is left alone. Partial `flip` translates a
// full-width fret and switches its facing after the midpoint. Interpolating
// the signed scale would collapse every outer fret to zero width at 50%.
// One fret button, emitted into SIX separate buckets -- one per sortingOrder.
//
// These must stay separate: base, cover and halfCover all share the same sheet,
// but halfCover sorts at -994, AFTER head(-997) and headCover(-996). Batching
// them together by texture (the obvious optimisation) silently reorders the
// stack and the cover ends up painted over the head.
//
// Order: base(-999) lift(-998) cover(-998) head(-997) headCover(-996) half(-994)
inline void buildFret(std::vector<Vtx>& oBase, std::vector<Vtx>& oLift,
                      std::vector<Vtx>& oCover, std::vector<Vtx>& oHead,
                      std::vector<Vtx>& oHeadCover, std::vector<Vtx>& oLight,
                      std::vector<Vtx>& oHalf,
                      int lane, float headDy, bool held, float flip) {
    const FretLane& F = FRETS[lane];
    float x = FRET_X[lane];
    const int opposite = 4 - lane;
    if (flip != 0.0f && opposite != lane)
        x += (FRET_X[opposite] - x) * flip;
    const float face = opposite != lane && flip >= 0.5f ? -1.0f : 1.0f;
    const float hw = FRET_W * 0.5f * face;
    const float y0 = -FRET_PIVOT_Y * FRET_H;
    const float y1 = (1.0f - FRET_PIVOT_Y) * FRET_H;
    const float z = FRET_Z;

    auto U = [&](float u0, float u1, float& a, float& b) {
        if (F.flip) { a = u1; b = u0; } else { a = u0; b = u1; }
    };
    float u0, u1, a, b;

    frameU12(F.base12, u0, u1); U(u0, u1, a, b);
    quadUp(oBase, x - hw, y0, x + hw, y1, z, a, 0.0f, b, 1.0f, 1, 1, 1, 1);

    {   // 66x18 @ PPU 680, transform scale (1,2,1), local y -0.01
        float lw = LIFT_W * 0.5f * face;
        float ly0 = LIFT_Y - FRET_PIVOT_Y * LIFT_H;
        float ly1 = LIFT_Y + (1.0f - FRET_PIVOT_Y) * LIFT_H;
        quadUp(oLift, x - lw, ly0, x + lw, ly1, z, 0.0f, 0.0f, 1.0f, 1.0f, 1, 1, 1, 1);
    }

    frameU12(F.cover12, u0, u1); U(u0, u1, a, b);
    quadUp(oCover, x - hw, y0, x + hw, y1, z, a, 0.0f, b, 1.0f,
           F.cover[0], F.cover[1], F.cover[2], 1);

    // Fret_Animator moves ONLY the head transform; headCover is its child and
    // inherits the translation. base/lift/cover/halfCover never move.
    frameU6(F.head6, u0, u1); U(u0, u1, a, b);
    quadUp(oHead, x - hw, y0 + headDy, x + hw, y1 + headDy, z, a, 0.0f, b, 1.0f,
           1, 1, 1, 1);
    frameU6(F.headCover6, u0, u1); U(u0, u1, a, b);
    quadUp(oHeadCover, x - hw, y0 + headDy, x + hw, y1 + headDy, z,
           a, 0.0f, b, 1.0f, F.headCover[0], F.headCover[1], F.headCover[2], 1);

    // headLight (-995) -- Head_Lights.png, ADDITIVE (Additive.mat), tinted with
    // the head-group colour. Drawn iff the button is held; Fret_Animator.cs:117
    // sets `headLight.enabled = isHeld` every frame and Play() never touches it,
    // so this is a "button is down" light that merely coincides with hits.
    // It is a child of head, so it rides the pop.
    if (held) {
        frameU6(F.head6, u0, u1); U(u0, u1, a, b);
        quadUp(oLight, x - hw, y0 + headDy, x + hw, y1 + headDy, z,
               a, 0.0f, b, 1.0f,
               F.headCover[0], F.headCover[1], F.headCover[2], 1);
    }

    frameU12(F.half12, u0, u1); U(u0, u1, a, b);
    quadUp(oHalf, x - hw, y0, x + hw, y1, z, a, 0.0f, b, 1.0f,
           F.cover[0], F.cover[1], F.cover[2], 1);
}

}  // namespace ch
