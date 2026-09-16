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
