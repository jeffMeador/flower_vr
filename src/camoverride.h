#pragma once

// Patch the engine's per-frame FOV update so we can force a wide vertical
// FOV (degrees) and a square aspect while in VR. See camoverride.cpp.
bool CamOverrideInstall(float fovDegrees);

// Call once per Present. enable = headset session active.
void CamOverrideTick(bool enable);
