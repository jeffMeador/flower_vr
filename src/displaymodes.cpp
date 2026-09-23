#include "displaymodes.h"
#include "log.h"
#include <MinHook.h>
#include <dxgi.h>
#include <vector>

// The game only accepts a resolution from its settings file if the monitor
// reports it as a display mode. For VR we want a square render resolution, so
// IDXGIOutput::GetDisplayModeList gets extra square modes appended.

static const UINT kSquareSizes[] = { 1440, 2048, 2160, 2644, 3072 };

using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);
using GetDisplayModeList_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*);
static CreateFactory_t realCreateFactory, realCreateFactory1;
static GetDisplayModeList_t realGetDisplayModeList;

static HRESULT STDMETHODCALLTYPE Hook_GetDisplayModeList(IDXGIOutput* self, DXGI_FORMAT fmt, UINT flags, UINT* num, DXGI_MODE_DESC* desc)
{
    if (!num) return realGetDisplayModeList(self, fmt, flags, num, desc);
    const UINT extra = (UINT)(sizeof(kSquareSizes) / sizeof(kSquareSizes[0]));

    UINT realCount = 0;
    HRESULT hr = realGetDisplayModeList(self, fmt, flags, &realCount, nullptr);
    if (FAILED(hr) || realCount == 0) return realGetDisplayModeList(self, fmt, flags, num, desc);

    if (!desc)
    {
        *num = realCount + extra;
        return hr;
    }
    std::vector<DXGI_MODE_DESC> modes(realCount);
    hr = realGetDisplayModeList(self, fmt, flags, &realCount, modes.data());
    if (FAILED(hr)) return hr;

    // Clone the highest-refresh real mode's timing/format for our square sizes.
    DXGI_MODE_DESC base = modes.back();
    for (UINT s : kSquareSizes)
    {
        DXGI_MODE_DESC m = base;
        m.Width = s; m.Height = s;
        modes.push_back(m);
    }
    UINT n = (UINT)modes.size() < *num ? (UINT)modes.size() : *num;
    memcpy(desc, modes.data(), n * sizeof(DXGI_MODE_DESC));
    *num = n;
    static bool logged = false;
    if (!logged) { logged = true; Log("[modes] advertised %u real + %u square display modes (fmt %d)", realCount, extra, (int)fmt); }
    return n < modes.size() ? DXGI_ERROR_MORE_DATA : S_OK;
}

static void HookOutputsOf(IUnknown* factoryUnk)
{
    if (realGetDisplayModeList) return;
    IDXGIFactory* factory = nullptr;
    if (FAILED(factoryUnk->QueryInterface(__uuidof(IDXGIFactory), (void**)&factory))) return;
    IDXGIAdapter* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    if (SUCCEEDED(factory->EnumAdapters(0, &adapter)) && SUCCEEDED(adapter->EnumOutputs(0, &output)))
    {
        void* target = (*reinterpret_cast<void***>(output))[8]; // IDXGIOutput::GetDisplayModeList
        MH_STATUS st = MH_CreateHook(target, (void*)&Hook_GetDisplayModeList, (void**)&realGetDisplayModeList);
        if (st == MH_OK) st = MH_EnableHook(target);
        Log("[modes] hooked IDXGIOutput::GetDisplayModeList: %s", MH_StatusToString(st));
        if (st != MH_OK) realGetDisplayModeList = nullptr;
    }
    if (output) output->Release();
    if (adapter) adapter->Release();
    factory->Release();
}

static HRESULT WINAPI Hook_CreateFactory(REFIID riid, void** out)
{
    HRESULT hr = realCreateFactory(riid, out);
    if (SUCCEEDED(hr) && out && *out) HookOutputsOf((IUnknown*)*out);
    return hr;
}
static HRESULT WINAPI Hook_CreateFactory1(REFIID riid, void** out)
{
    HRESULT hr = realCreateFactory1(riid, out);
    if (SUCCEEDED(hr) && out && *out) HookOutputsOf((IUnknown*)*out);
    return hr;
}

void DisplayModesInstall()
{
    MH_Initialize(); // harmless if already initialized
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) { Log("[modes] dxgi.dll not loaded"); return; }
    void* f0 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory");
    void* f1 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (f0 && MH_CreateHook(f0, (void*)&Hook_CreateFactory, (void**)&realCreateFactory) == MH_OK) MH_EnableHook(f0);
    if (f1 && MH_CreateHook(f1, (void*)&Hook_CreateFactory1, (void**)&realCreateFactory1) == MH_OK) MH_EnableHook(f1);
    Log("[modes] hooked CreateDXGIFactory/CreateDXGIFactory1");
}
