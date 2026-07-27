// Clone Hero highway geometry, 1:1.
//
// Every constant here was read out of the Unity project rather than eyeballed:
//   Guitar Player 1.prefab      camera, neck, sidebar, string, fret transforms
//   Editor/NeckPlane.asset      the 112-vertex neck mesh
//   Scripts/VertexColors.cs     the baked vertex-alpha fade
//   Scripts/Note/HighwayScroll.cs   highway UV scroll rate
//   Scripts/Note/NoteContainer.cs   note layer composition + tints
//   Scripts/Player Controls/PlayerProfile.cs   noteSpeed default
//   *.png.meta                  PPU, pivots and sprite rects
#pragma once

#include <cmath>
#include <vector>

namespace ch {

// --- camera (Guitar Player 1.prefab) --------------------------------------
static const float CAM_X = 0.0f, CAM_Y = 1.0f, CAM_Z = -3.98f;
static const float CAM_PITCH_DEG = 6.0f;
static const float CAM_FOV = 20.0f;      // vertical
static const float CAM_NEAR = 1.0f, CAM_FAR = 20.0f;

// --- neck (NeckPlane.asset + Neck transform) ------------------------------
static const int   NECK_ROWS   = 56;      // 112 verts = 56 rows x 2
static const float NECK_POS_Z  = 3.92f;
static const float NECK_SCALE_X = 0.96f, NECK_SCALE_Z = 9.0f;
static const float NECK_HALF_W = NECK_SCALE_X * 0.5f;          // 0.48
static const float NECK_Z0 = NECK_POS_Z - NECK_SCALE_Z * 0.5f; // -0.58
static const float NECK_Z1 = NECK_POS_Z + NECK_SCALE_Z * 0.5f; //  8.42
static const float NECK_TILES_V = 1.5f;   // spr_highway_gh6.mat _MainTex m_Scale.y

// Scene has no lights and m_AmbientMode 3 (Flat) with sky colour 0.8705506.
// Lambert with no directional light collapses to ambient * albedo.
static const float AMBIENT = 0.9412f;

// --- scroll (HighwayScroll.cs) --------------------------------------------
//   offset += songDeltaTime * noteSpeed / 6.0
// integrated over absolute song time, which is what an offline renderer wants
static const float HIGHWAY_TILE_UNITS = 6.0f;

// --- note travel (BaseNoteRenderer.cs / PlayerProfile.cs) -----------------
static const float NOTE_SPEED_DEFAULT = 10.0f;  // world units per second, 1..20
static const float NOTE_CULL_FAR  =  7.87f;
static const float NOTE_CULL_NEAR = -1.06f;
static const float NOTE_FADE_LEN  =  3.0f;      // alpha = (7.87 - z)/3

// note lane centres are NOT the fret centres in CH
static inline float noteX(int fret) { return -0.387f + 0.1935f * float(fret); }
static const float FRET_X[5] = {-0.4f, -0.2f, 0.0f, 0.2f, 0.4f};
static const float FRET_Z = 0.09f;
static const float STRIKE_Z = 0.0f;   // notes are placed relative to z = 0

// --- gem scale (GlobalVariables.cs gem_size default 100) ------------------
static const float GEM_SCALE = 1.1f;

// --- sprite sheets --------------------------------------------------------
// --- sustains (GuitarNoteRenderer.cs, Sustain.prefab, spr_sustain_strip6.meta)
//
// Widths are NOT scaled by GEM_SCALE. GuitarNoteRenderer.cs:87 is
// `sustainScale = GlobalVariables.instance.gemSize.GetFloatPercent`, i.e.
// gem_size/100, default 1.0 -- while NOTES get gemSize * noteScaleX (1.1).
// That 1.1 is where GEM_SCALE comes from and it does not apply here.
//
// And 0.30 / 0.85 are the GLOW widths, not "held body" widths: the names
// heldSustainSize / heldOpenSustainSize mislead. The body never changes size --
// CreateSustainPool assigns .size once (:111, :115) and nothing reassigns it.
static const float SUS_BODY_W      = 0.13f;   // GuitarNoteRenderer.cs:88, :111
static const float SUS_GLOW_W      = 0.30f;   // :89,  :115
static const float SUS_OPEN_W      = 0.90f;   // :90,  :136  (Phase 3, unused)
static const float SUS_OPEN_GLOW_W = 0.85f;   // :91,  :140  (Phase 3, unused)

static const float SUS_FAR_LIMIT  = 7.75f;       // Sustains.mat / SustainGlow.mat
static const float SUS_LEN_OFFSET = 0.27142857f; // (border.w/2)/rect.height = (38/2)/70, :60
static const float SUS_Z_OFFSET   = 0.30f;       // unheld: noteZPosition + 0.3f, :497
static const float SUS_TOP_BORDER = 0.38f;       // border.w 38 px / PPU 100
static const float SUS_GLOW_EDGE  = 0.45f;       // SustainGlow.shader EDGE_LENGTH

// Port of OpenITG's fYStep (NoteDisplay.cpp:983): 16 screen pixels, the
// non-z-mod value -- NeedZBuffer is only true for bumpy/twirl and NotClon has
// no GetZPos at all. renderer.cpp maps yOffset = z * ARROW_SIZE * 1.6, i.e.
// 102.4 px per world unit.
//
// Written in terms of that same 1.6 rather than baked to 0.15625: the constant
// is unsettled (see AGENTS.md "Mods"), and if it drops to ~0.84 the step gets
// COARSER, which is the safe direction only if this tracks it.
static const float SUS_STEP_Z = 16.0f / (64.0f * 1.6f);   // 0.15625

static const int SUS_FRAME_STRUM = 1;   // Sustains[0], :508  note.IsTap ? [1] : [0]
static const int SUS_FRAME_TAP   = 2;   // Sustains[1], :508
static const int SUS_FRAME_HELD  = 0;   // Sustains[3], :581
// Sustains[2] (frame 5) is the missed sprite; NotClon has no miss state.

// NoteColors.Sustains (NoteContainer.cs:29-36), used only while HELD. Differs
// from NOTE_TINT on blue and orange only -- both get brighter when held.
static const float SUSTAIN_TINT[5][3] = {
    {0.0f, 1.0f,   0.0f},
    {1.0f, 0.0f,   0.0f},
    {1.0f, 1.0f,   0.0f},
    {0.0f, 0.776f, 1.0f},
    {1.0f, 0.827f, 0.235f},
};

// spr_sustain_strip6.png is 440x72; frame f rect = (1 + 73f, 1, 73, 70),
// pivot (0.5, 0), border top 38. v0 is the gem end, vB the 9-slice seam, v1 the
// tail cap. Verified against the .meta and the PNG IHDR.
inline void sustainFrameUV(int f, float& u0, float& u1,
                           float& v0, float& vB, float& v1) {
    u0 = (1.0f + 73.0f * float(f)) / 440.0f;
    u1 = (74.0f + 73.0f * float(f)) / 440.0f;
    v0 =  1.0f / 72.0f;
    vB = 33.0f / 72.0f;
    v1 = 71.0f / 72.0f;
}

static const float PPU_NOTES = 575.0f;   // spr_newnotes_strip4, note_anim, open
static const float NOTE_PIVOT_Y = 0.16f;    // spr_newnotes_strip4 / open
static const float ANIM_PIVOT_Y = 0.138f;   // spr_note_anim_strip16

// --- tints (NoteContainer.cs NoteColors) ----------------------------------
static const float NOTE_TINT[6][3] = {
    {0.0f, 1.0f, 0.0f},      // green
    {1.0f, 0.0f, 0.0f},      // red
    {1.0f, 1.0f, 0.0f},      // yellow
    {0.0f, 0.541f, 1.0f},    // blue
    {1.0f, 0.702f, 0.0f},    // orange
    {0.733f, 0.0f, 1.0f},    // open
};
static const float ANIM_TINT[5][3] = {
    {0.0f, 1.0f, 0.0f},
    {1.0f, 0.549f, 0.549f},
    {1.0f, 1.0f, 0.345f},
    {0.47f, 0.823f, 1.0f},
    {1.0f, 0.749f, 0.16f},
};

// --- beat lines (BeatRenderer.cs, Beatline.prefab, beatline.png.meta) -----
// The prefab's rotation is IDENTITY: a beatline is an UPRIGHT sprite facing
// the camera, like a note -- not a stripe painted on the board. beatline.png
// is 1024x16 at PPU 1100, pivot (0.5, 0.2), so the natural quad is
// 0.9309 x 0.014545 world units straddling y = 0 at 20/80.
static const float BEAT_FAR       = 7.87f;         // beatPosFarLimit, prefab:77946
static const float BEAT_Z_OFFSET  = 0.1f;          // beatZOffset, BeatRenderer.cs:43
static const float BEAT_SPRITE_W  = 1024.0f / 1100.0f;
static const float BEAT_SPRITE_H  = 16.0f / 1100.0f;
static const float BEAT_PIVOT_Y   = 0.2f;
// localScale.y per style (BeatRenderer.cs:37-39) x beatWidth (1, prefab:77948)
static const float BEAT_SCALE[3]  = {2.2f, 0.6f, 0.4f};   // MEASURE, STRONG, WEAK
static const float BEAT_ALPHA[3]  = {1.0f, 0.7f, 0.4f};   // :110-112

// --- PIU display mode (pump/defaultsm5 noteskin) ---------------------------
//
// The noteskin's own Lua, flattened to constants: NotClon will never load a
// custom PIU noteskin, so the generality that Lua exists to provide is dead
// weight. Full derivation in devdocs/spec/piu-noteskin.md.
//
// Only THREE arts exist. NoteSkin.lua's BaseRotY is 180 for UpRight and
// DownRight, and the redirect files confirm it (`DownRight Tap Note.lua` is
// literally LoadActor("DownLeft","Tap Note")) -- so those two columns are
// horizontal mirrors of UpLeft and DownLeft. Exactly the trick CH's own frets
// use, and reproduced the same way: swap u0/u1.
//
// Lane order left to right: DownLeft, UpLeft, Center, UpRight, DownRight.
static const int  PIU_ART[5]    = {0, 1, 2, 1, 0};   // 0 DownLeft 1 UpLeft 2 Center
static const bool PIU_MIRROR[5] = {false, false, false, true, true};

// metrics.ini: AnimationIsBeatBased=0, TapNoteAnimationLength=0.25 -- the
// 6-frame cycle is TIME based, one pass per quarter second, and global rather
// than per-note (same shape as CH's note_anim).
static const float PIU_ANIM_LEN = 0.25f;
static const int   PIU_TAP_FRAMES = 6;      // 3x2 sheet
static const int   PIU_HOLD_FRAMES = 6;     // 6x1 sheet

// Five 0.1935-wide panels span 5 * 0.1935 = 0.9675, against a neck 0.96 wide.
// A pump pad is a contiguous row of squares, and at one lane per panel it
// lands on the highway almost exactly.
static const float PIU_PANEL_W = 0.1935f;

// A frame from a cols x rows sheet under flipY=true loading (PNG row 0 at
// v=1 -- the convention every other note texture here uses). SM numbers frames
// left to right, then top to bottom. `mirror` is BaseRotationY=180.
inline void piuSheetUV(int cols, int rows, int frame, bool mirror,
                       float& u0, float& vBot, float& u1, float& vTop) {
    const int c = frame % cols;
    const int r = (frame / cols) % rows;
    u0 = float(c) / float(cols);
    u1 = float(c + 1) / float(cols);
    if (mirror) { const float t = u0; u0 = u1; u1 = t; }
    vTop = 1.0f - float(r) / float(rows);
    vBot = 1.0f - float(r + 1) / float(rows);
}

// The receptor's on-beat pulse, straight out of UpLeft Receptor.lua's update:
//   part = clamp(beat % 1, 0, 0.5); eff = scale(part, 0, 0.5, 1, 0)
// A pure function of the beat, so it seeks for free -- which is the only
// reason a per-frame update function can be ported at all.
inline float piuReceptorGlow(double beat) {
    if (beat < 0.0) return 0.0f;
    float part = float(beat - floor(beat));
    if (part > 0.5f) part = 0.5f;
    return 1.0f - part / 0.5f;
}

// --- sidebars (sidebar.png 64x512, PPU 600) -------------------------------
// transform pos (+-0.52, 0.01, 3.52) euler (90,0,0) scale (1,10,1);
// right side has scale.x = -1, which mirrors U only.
static const float SIDEBAR_Y   = 0.01f;
static const float SIDEBAR_X_OUT = 0.5733333f;   // u = 0
static const float SIDEBAR_X_IN  = 0.4666667f;   // u = 1
static const float SIDEBAR_Z0  = -0.7466667f;    // v = 0
static const float SIDEBAR_Z1  =  7.7866667f;    // v = 1

// --- lane strings (Guitarstring_wor_remake2.png 32x512, PPU 100) ----------
static const float STRING_X[5] = {-0.384f, -0.187f, 0.0f, 0.187f, 0.384f};
static const float STRING_HALF_W = 0.024f;
static const float STRING_Z0 = -0.036f;    // v = 0
static const float STRING_Z1 =  5.084f;    // v = 1
static const float STRING_TINT = 0.5758621f;

// --- the baked neck fade (VertexColors.cs) --------------------------------
//   percentToStart 0.8, startAlpha 0, step 0.12, clamped at 1
//   i > 0.8 * 112  ->  i >= 90
// The counter advances per VERTEX, not per row, so within a row the right
// vertex is 0.12 darker than the left. That is a generator quirk in CH; it
// ships, so it is reproduced.
inline float neckVertexAlpha(int flatIndex) {
    if (flatIndex < 90) return 1.0f;
    float a = 1.0f - 0.12f * float(flatIndex - 90);
    return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}


// --- fret buttons (Guitar Player 1.prefab, Fret_* subtrees) ---------------
// Each fret is a stack of sprites, not one quad. Verified draw order by
// sortingOrder: base(-999) lift(-998) cover(-998) head(-997) headCover(-996)
// halfCover(-994). headLight(-995) is authored but Fret_Animator.cs forces
// m_Enabled false every frame at rest, so it is never drawn.
//
// The whole assembly sits BEHIND the notes (notes run -4N-2 .. 0) and in
// front of sustain bodies (-1000).
static const float FRET_PPU   = 650.0f;   // bottom_strip12 + head_strip6
static const float LIFT_PPU   = 680.0f;   // spr_targets_lift
static const float FRET_PIVOT_Y = 0.4f;

// 128x64 @ 650
static const float FRET_W = 128.0f / FRET_PPU;              // 0.19692308
static const float FRET_H =  64.0f / FRET_PPU;              // 0.09846154
// 66x18 @ 680, transform scale (1,2,1), local y -0.01
static const float LIFT_W = 66.0f / LIFT_PPU;               // 0.09705882
static const float LIFT_H = 18.0f / LIFT_PPU * 2.0f;        // 0.05294118
static const float LIFT_Y = -0.01f;

// Per-lane sheet frames. Blue mirrors Red and Orange mirrors Green: they reuse
// the same frames with m_FlipX, so only three distinct fret arts exist.
struct FretLane {
    int   base12, cover12, half12;   // spr_newtargets_bottom_strip12 (12 frames)
    int   head6, headCover6;         // spr_newtargets_head_strip6 (6 frames)
    bool  flip;
    float cover[3];                  // cover + halfCover tint
    float headCover[3];              // headCover tint (differs on blue)
};
static const FretLane FRETS[5] = {
    // green
    {0, 6,  9, 0, 3, false, {0.0f, 1.0f, 0.0f},          {0.0f, 1.0f, 0.0f}},
    // red
    {1, 7, 10, 1, 4, false, {1.0f, 0.0f, 0.0f},          {1.0f, 0.0f, 0.0f}},
    // yellow
    {2, 8, 11, 2, 5, false, {1.0f, 1.0f, 0.0f},          {1.0f, 1.0f, 0.0f}},
    // blue -- mirrored red, and its headCover tint is NOT its cover tint
    {1, 7, 10, 1, 4, true,  {0.0f, 0.45517254f, 1.0f},   {0.0f, 0.22224426f, 1.0f}},
    // orange -- mirrored green
    {0, 6,  9, 0, 3, true,  {1.0f, 0.55f, 0.0f},         {1.0f, 0.55f, 0.0f}},
};

// spr_newtargets_bottom_strip12.png.meta has an authoring slip: frame 2's rect
// starts at x=257, not 256. Reproduced rather than normalised.
inline void frameU12(int f, float& u0, float& u1) {
    if (f == 2) { u0 = 257.0f / 1536.0f; u1 = 384.0f / 1536.0f; return; }
    u0 = float(f) / 12.0f; u1 = float(f + 1) / 12.0f;
}
inline void frameU6(int f, float& u0, float& u1) {
    u0 = float(f) / 6.0f; u1 = float(f + 1) / 6.0f;
}


// --- the fret pop (Scripts/Player Controls/Fret_Animator.cs) --------------
// Not an Animator: there is no AnimationClip anywhere in the project that
// touches a fret. Play() snaps the head UP instantly, then Update() slides it
// back down at a constant rate. Linear, no easing, no overshoot, no scale
// change and no tint change -- the only animated channel is head.y.
//
//   Play():  position.y = maxHeight                 (instant, no ease-in)
//   Update(): position.y -= dt * animSpeed * rate   (rate 2 once below rest)
static const float POP_A     =  0.0375f;   // maxHeight  above rest
static const float POP_HELD  = -0.0175f;   // heldHeight below rest
static const float POP_V     =  0.275f;    // animSpeed, world units/sec
static const float POP_T1    =  POP_A / POP_V;              // 0.13636 s

// t = seconds since the note was struck.
inline float fretPopY(float t, bool held, bool sustaining) {
    if (sustaining) return POP_A;          // frozen for the whole sustain
    if (t < 0.0f) return held ? POP_HELD : 0.0f;
    if (t < POP_T1) return POP_A - POP_V * t;
    if (!held) return 0.0f;
    float y = -2.0f * POP_V * (t - POP_T1);   // rate doubles below rest
    return y < POP_HELD ? POP_HELD : y;
}

// While the head is above rest, Fret_Animator::EnableTopmost() lifts the whole
// fret stack from -999..-994 to 20000..20005 -- i.e. the popping fret draws IN
// FRONT of the notes. It snaps back the moment y reaches 0.
inline bool fretOnTop(float popY) { return popY > 0.0f; }

}  // namespace ch
