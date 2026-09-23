#pragma once
#include <d3d11.h>
#include <dxgi.h>

// Phase 2b: OpenXR output. Creates a session on the game's own D3D11 device
// and, every Present, copies the frame the game just rendered (one eye under
// alternate-eye stereo) into that eye's swapchain, then submits both eyes.
//
// Head tracking: each frame the predicted eye poses (relative to a yaw-only
// reference, recentered at start / F6 / SteamVR recenter) are handed to the
// stereo code, and images are submitted in LOCAL space with the exact pose
// they were rendered with.

void XrInit(ID3D11Device* device, const wchar_t* dllDir);

// Called from Present before the real Present. `renderedEye` is the eye the
// finished frame was drawn for: -1 left, +1 right, 0 mono (copy to both),
// 2 = both (double render: right eye is in the backbuffer's twin).
void XrSubmitFrame(IDXGISwapChain* swapChain, int renderedEye);

// True while an OpenXR session is running (headset showing our frames).
bool XrSessionActive();
