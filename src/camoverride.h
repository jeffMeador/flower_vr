#pragma once

// Patch the engine's per-frame FOV update so we can force a wide vertical
// FOV (degrees) and a square aspect while in VR. See camoverride.cpp.
bool CamOverrideInstall(float fovDegrees);

// Call once per Present. enable = headset session active.
void CamOverrideTick(bool enable);

// Render camera object (engine), or null until the first camera update.
void* CamOverrideCameraNode();

// Turn the engine camera by the head pose (culling follows gaze).
void CamOverrideSetHeadCamera(bool on);
