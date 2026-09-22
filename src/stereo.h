#pragma once
#include <Windows.h>
#include "mat4.h"

// Phase 2a: alternate-eye-rendering (AER) stereo.
//
// Every Present flips the active eye. For each draw whose vertex shader exposes
// a perspective transform in its slot-0 cbuffer, we shift the camera sideways
// by +/- separation/2 along its local X axis before the draw goes through.
//
// Flower's cbuffers use the column-vector convention (clip = MVP * v, stored
// row-major: translation lives in column 3, clip.w comes from row 3).
// With MVP = Proj * View * Model and a symmetric projection whose column 0 is
// (xs, 0, 0, 0), translating the view by dx along view-space X gives
//   MVP_eye = Proj * T(dx) * View * Model = MVP + (dx * Proj.col0) * e3^T
// (Model/View affine), i.e. simply
//   MVP_eye[0][3] += dx * xs
// a constant clip-space X offset per eye -> parallax proportional to 1/w.

struct StereoConfig
{
    bool  enabled = true;
    float separation = 0.065f;   // world units between the eyes
    bool  shiftEyePosition = false; // billboard toward each eye vs. head center
};

StereoConfig& Stereo();
void StereoLoadConfig(const wchar_t* dllDir);

// Called once per Present: flips eye, handles hotkeys.
void StereoFrameBoundary();

// Current eye: -1 = left, +1 = right, 0 = mono (stereo disabled or not ready).
int StereoCurrentEye();

// Feed a recovered mono ViewProj (from any draw exposing both model and MVP)
// so we know the projection's horizontal scale.
void StereoObserveViewProj(const Mat4& viewProj);

// Clip-space X shift to add to [0][3] of a perspective MVP for the current eye
// (0 when mono or projection not yet known).
float StereoClipShift();

// View-space X shift to add to [0][3] of a modelView matrix for the current eye.
float StereoViewShift();

// World-space eye offset for the current eye (to shift eyePositionWS), or false.
bool StereoWorldEyeOffset(float out[3]);

// True if the matrix is a perspective transform (w depends on position).
inline bool IsPerspective(const Mat4& m)
{
    return fabsf(m.m[3][0]) + fabsf(m.m[3][1]) + fabsf(m.m[3][2]) > 1e-4f; // w row
}
