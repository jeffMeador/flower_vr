#pragma once
#include <Windows.h>
#include <cstdint>
#include "mat4.h"

// Per-eye view synthesis by rewriting the game's clip-space matrices.
//
// Flower's cbuffers use the column-vector convention (clip = MVP * v, stored
// row-major: translation in column 3, clip.w from row 3). The game projection
// is symmetric: rows (xs,0,0,0) (0,ys,0,0) (0,0,A,B) (0,0,1,0) in "canonical
// view space" c = (x right, y up, d forward) as seen on screen. Everything is
// recovered from the camera's ViewProj (see StereoObserveViewProj), so for any
// perspective matrix M the game uploads we can form
//
//   M_eye = P_new * T_eye * P_game^-1 * M
//
// where T_eye is the eye's pose relative to the game camera (headset pose, or
// a plain +/- separation/2 shift without a headset) and P_new is either the
// game's own projection or the headset eye's FOV mapped into the centered
// crop the XR code copies out of the backbuffer.
//
// Eyes alternate every Present (alternate-eye rendering).

struct StereoConfig
{
    bool  enabled = true;
    float separation = 0.065f;       // world units between eyes (no headset)
    float worldScale = 0.3f;         // game units per real-world meter (headset); 0.3 chosen by A/B test vs 1.0
    bool  shiftEyePosition = false;  // billboard toward each eye vs. head center
    bool  doubleRender = false;      // [stereo] render=double: both eyes every frame
    int   lensMode = 0;              // game fisheye post pass: 0 game, 1 fixed, 2 off
    bool  lensLockPending = false;   // fixed: lock at the next observed strength
    bool  motionBlur = false;        // game motion blur while in VR
    bool  depthOfField = false;      // game depth-of-field blur while in VR
    float sparkleSize = 0.5f;        // VR: scale of the grass sparkles (FillerFlower point sprites)
    float dofNear = 1.0f, dofFar = 1.0f; // with it on: scale of the near/far blur ramps (1 = the game's)
};

StereoConfig& Stereo();
void StereoLoadConfig(const wchar_t* dllDir);

// Called once per Present: flips eye, latches this frame's eye view, hotkeys.
void StereoFrameBoundary();

// Double render: which eye the following draws are patched for (0 left, 1 right).
void StereoSetRenderEye(int eyeIndex);

// Alternate-eye mode: current eye: -1 = left, +1 = right, 0 = mono.
int StereoCurrentEye();

// Feed a recovered mono ViewProj (from any draw exposing both model and MVP).
void StereoObserveViewProj(const Mat4& viewProj);

// True if a 3D game camera was seen in the last few frames (false in menus,
// title screens and videos).
bool StereoProjectionFresh();

// Near/far plane of the game camera's depth mapping, in game units.
bool StereoDepthRange(float& nearUnits, float& farUnits);

// Projection scales of the game camera (P[0][0], P[1][1]); false until seen.
bool StereoProjection(float& xs, float& ys);

// Headset eye pose relative to the recentered reference, OpenXR axes
// (x right, y up, z back), meters; rot is row-major 3x3. Takes effect at the
// next frame boundary. eyeIndex 0 = left, 1 = right.
void StereoSetEyePose(int eyeIndex, const float rot[9], const float pos[3]);

// Headset head (center) pose, same conventions. The engine camera is turned
// by it (see camoverride.cpp) so culling follows the head; eye views then
// only apply the residual inverse(eye) * head.
void StereoSetHeadPose(const float rot[9], const float pos[3]);

// Called from the engine's camera update with its camera matrix (x/y/z axes,
// position as float4s). Returns the head rotation (row-major, in the engine
// camera's local axes) and translation (game units) to apply, and records it
// as the head pose the upcoming frame is rendered with.
bool StereoTakeHeadForCamera(const float* cameraMatrix, float rot[9], float pos[3]);
void StereoHeadNotApplied();

// Headset eye FOV (tan of each edge; left/down < 0) and the crop the XR code
// copies (crop size / backbuffer size).
void StereoSetDisplayFov(int eyeIndex, float tanL, float tanR, float tanU, float tanD, float cropFracX, float cropFracY);

// Nonzero when draws need patching; changes whenever the wanted cbuffer
// contents change (eye flip, pose/projection change), so patches can be cached.
uint64_t StereoPatchKey();

// Rewrite a perspective clip matrix / a modelView matrix for the current eye,
// in place (row-major float[16]).
void StereoPatchClip(float* m);
// Sideways NDC shift that makes a flat overlay appear `meters` ahead in this
// eye (crossed disparity from the tracked eye separation); 0 without it.
float StereoOverlayNdcShift(int eye, float meters);
// Where a point at (x, y) on the game's screen (NDC, far away) lands on an
// eye's screen (NDC). False while no 3D camera is live.
bool StereoMapNdc(int eye, float x, float y, float* ox, float* oy);
// Where a point in the headset's reference space (meters, OpenXR axes: x right,
// y up, z back; the game camera looks along -z) lands on an eye's screen (NDC),
// using that eye's tracked pose and display FOV. False without them or if the
// point is behind the eye.
bool StereoProjectRefPoint(int eye, const float p[3], float* ox, float* oy);
void StereoSetMono(bool mono); // cinema mode: no per-eye changes at all
bool StereoHasEyePoses(); // both eyes' tracked poses and display FOVs are known
// The same point as clip-space coordinates for drawing (perspective-correct,
// depth just past the near plane); points behind the eye come out with w <= 0.
bool StereoRefPointToClip(int eye, const float p[3], float clip[4]);
void StereoPatchView(float* m);
// Eye currently being patched for (double render: 0 left, 1 right).
int StereoRenderEye();
// The game camera's view-projection (row-major, clip = M v) as last observed,
// and its forward axis in world space.
bool StereoGameViewProj(float m[16], float fwd[3]);
// Depth-to-world rays for an eye. Journey's screen-space effects rebuild a
// pixel's world position as eye + depth * (O + (1-u) U + (1-v) V) from the
// game camera's values; this gives the eye's own O, U, V and position.
bool StereoEyeRays(int eye, const float O[3], const float U[3], const float V[3], const float E[3],
                   float O2[3], float U2[3], float V2[3], float E2[3]);

// World-space offset of the current eye from the game camera (for
// eyePositionWS), or false when disabled.
bool StereoWorldEyeOffset(float out[3]);

// True if the matrix is a perspective transform (w depends on position).
inline bool IsPerspective(const Mat4& m)
{
    return fabsf(m.m[3][0]) + fabsf(m.m[3][1]) + fabsf(m.m[3][2]) > 1e-4f; // w row
}
