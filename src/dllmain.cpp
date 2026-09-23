#include <Windows.h>
#include <d3d11.h>
#include "log.h"
#include "hooks.h"
#include "stereo.h"
#include "fileredirect.h"
#include "displaymodes.h"

HMODULE g_realD3D11 = nullptr;
wchar_t g_dllDir[MAX_PATH] = {};

PFN_D3D11_CREATE_DEVICE g_real_D3D11CreateDevice = nullptr;
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_real_D3D11CreateDeviceAndSwapChain = nullptr;

static void LoadRealD3D11(HMODULE self)
{
    GetModuleFileNameW(self, g_dllDir, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(g_dllDir, L'\\');
    if (lastSlash) *lastSlash = L'\0';

    wchar_t origPath[MAX_PATH];
    swprintf_s(origPath, L"%s\\d3d11_orig.dll", g_dllDir);

    g_realD3D11 = LoadLibraryW(origPath);
    if (!g_realD3D11)
    {
        // Fallback: shouldn't happen if build.bat deployed d3d11_orig.dll correctly.
        g_realD3D11 = LoadLibraryW(L"d3d11_orig.dll");
    }

    if (g_realD3D11)
    {
        g_real_D3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(g_realD3D11, "D3D11CreateDevice");
        g_real_D3D11CreateDeviceAndSwapChain = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(g_realD3D11, "D3D11CreateDeviceAndSwapChain");
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        LoadRealD3D11(hModule);
        LogInit(g_dllDir);
        SetHooksDllDir(g_dllDir);
        Log("VRMod proxy d3d11.dll attached. real d3d11 loaded: %d", g_realD3D11 != nullptr);
        StereoLoadConfig(g_dllDir);
        if (FileRedirectInstall(g_dllDir))
            DisplayModesInstall(); // let the VR config's square resolution be accepted
    }
    return TRUE;
}
