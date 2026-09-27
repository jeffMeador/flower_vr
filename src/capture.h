#pragma once
#include <d3d11.h>

// Patches ID3D11Device (CreateVertexShader) and ID3D11DeviceContext
// (VSSetShader/VSSetConstantBuffers/Map/Unmap/UpdateSubresource/Draw*)
// vtables to build a per-vertex-shader cbuffer offset map (via D3DReflect)
// and log recovered ViewProj matrices for validation. Read-only / logging
// only for now - does not alter rendering yet.
void InstallCaptureHooks(ID3D11Device* device, ID3D11DeviceContext* context);

// Call once per Present to drive the periodic logging budget.
void NotifyCaptureFrameBoundary();

// Scene depth buffer of the last frame's 3D draws (left eye; the right eye's
// is its shadow twin). AddRef'd, may be null.
ID3D11Resource* CaptureSceneDepth();

// Batched double render: write the game's latest data back into a constant
// buffer after the right eye's command list overwrote it.
void CaptureRestoreOriginal(ID3D11DeviceContext* ctx, ID3D11Resource* buf);
