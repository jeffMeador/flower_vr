#pragma once
#include <d3d11.h>
#include <dxgi.h>

// Called right after the real D3D11CreateDeviceAndSwapChain succeeds.
// Patches the returned swapchain/device-context vtables to route through our hooks.
void InstallHooksOnSwapChain(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context);

// Directory the proxy DLL lives in, used for screenshot/log output paths.
void SetHooksDllDir(const wchar_t* dir);

// [debug] passthrough=1: install nothing but the virtual gamepad (for isolating
// game problems from mod problems). Valid after SetHooksDllDir.
bool HooksPassthrough();
