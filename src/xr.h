#pragma once
#include <d3d11.h>
#include <dxgi.h>

// Phase 2b: OpenXR output. Creates a session on the game's own D3D11 device
// and, every Present, copies the frame the game just rendered (one eye under
// alternate-eye stereo) into that eye's swapchain, then submits both eyes.
//
// For now the layer is head-locked (VIEW space): the game camera does not
// follow the HMD yet; the image is shown with the game's own FOV.

void XrInit(ID3D11Device* device, const wchar_t* dllDir);

// Called from Present before the real Present. `renderedEye` is the eye the
// finished frame was drawn for: -1 left, +1 right, 0 mono (copy to both).
void XrSubmitFrame(IDXGISwapChain* swapChain, int renderedEye);

// True while an OpenXR session is running (headset showing our frames).
bool XrSessionActive();
