#include <Windows.h>
#include <d3d11.h>
#include "log.h"
#include "hooks.h"

extern PFN_D3D11_CREATE_DEVICE g_real_D3D11CreateDevice;
extern PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_real_D3D11CreateDeviceAndSwapChain;

extern "C" HRESULT WINAPI Hook_D3D11CreateDevice(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    CONST D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    Log("D3D11CreateDevice called (no swapchain).");
    return g_real_D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels,
        FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
}

extern "C" HRESULT WINAPI Hook_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    CONST D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    CONST DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    Log("D3D11CreateDeviceAndSwapChain called. requestedFormat=%d width=%u height=%u refresh=%u/%u windowed=%d",
        pSwapChainDesc ? (int)pSwapChainDesc->BufferDesc.Format : -1,
        pSwapChainDesc ? pSwapChainDesc->BufferDesc.Width : 0,
        pSwapChainDesc ? pSwapChainDesc->BufferDesc.Height : 0,
        pSwapChainDesc ? pSwapChainDesc->BufferDesc.RefreshRate.Numerator : 0,
        pSwapChainDesc ? pSwapChainDesc->BufferDesc.RefreshRate.Denominator : 0,
        pSwapChainDesc ? (int)pSwapChainDesc->Windowed : -1);

    HRESULT hr = g_real_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
        ppDevice, pFeatureLevel, ppImmediateContext);

    Log("D3D11CreateDeviceAndSwapChain returned hr=0x%08X swapChain=%p device=%p",
        (unsigned int)hr,
        ppSwapChain ? (void*)*ppSwapChain : nullptr,
        ppDevice ? (void*)*ppDevice : nullptr);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
    {
        ID3D11Device* dev = ppDevice ? *ppDevice : nullptr;
        ID3D11DeviceContext* ctx = ppImmediateContext ? *ppImmediateContext : nullptr;
        InstallHooksOnSwapChain(*ppSwapChain, dev, ctx);
    }

    return hr;
}
