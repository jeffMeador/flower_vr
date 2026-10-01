#include "displaymodes.h"
#include "log.h"
#include "vhook.h"
#include "game.h"
#include <MinHook.h>
#include <dxgi.h>
#include <vector>

// The game only accepts a resolution from its settings file if the monitor
// reports it as a display mode. For VR we want a square render resolution, so
// IDXGIOutput::GetDisplayModeList gets extra square modes appended.

static const UINT kSquareSizes[] = { 1440, 1728, 2048, 2160, 2644, 3072 };
static UINT g_maxSquare = 0;  // [xr] maxSquare: advertise no square mode above this (0 = all)
static const UINT kMaxModes = 32; // total we report (the game's table is smaller than 67)

using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);
using GetDisplayModeList_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*);
static CreateFactory_t realCreateFactory, realCreateFactory1;
static GetDisplayModeList_t realGetDisplayModeList;

// Journey sizes its window from the screen height, which would cut a square
// 2644 x 2644 window down to 2644 x 2141. Report a screen tall enough for the
// square (Flower doesn't need this).
using GetSystemMetrics_t = int(WINAPI*)(int);
static GetSystemMetrics_t realGetSystemMetrics;
static int g_tallScreen = 0;
static int WINAPI Hook_GetSystemMetrics(int index)
{
    int v = realGetSystemMetrics(index);
    int out = v;
    if (index == SM_CYSCREEN || index == SM_CYFULLSCREEN || index == SM_CYMAXIMIZED ||
        index == SM_CYVIRTUALSCREEN || index == SM_CYMAXTRACK)
        out = v < g_tallScreen ? g_tallScreen : v;
    return out;
}

// Windows also caps a captioned window at the screen size (WM_GETMINMAXINFO's
// max track size), so the game's window procedure is wrapped to raise that.
// RegisterClassA is hooked to wrap each class's procedure (a few at most).
using RegisterClassA_t = ATOM(WINAPI*)(const WNDCLASSA*);
static RegisterClassA_t realRegisterClassA;
static WNDPROC g_origProcs[4];
static int g_wrapped = 0;
template <int N>
static LRESULT CALLBACK WrappedProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    LRESULT r = CallWindowProcA(g_origProcs[N], h, msg, w, l);
    if (msg == WM_GETMINMAXINFO && l)
    {
        MINMAXINFO* mm = (MINMAXINFO*)l;
        if (mm->ptMaxTrackSize.y < g_tallScreen + 200) mm->ptMaxTrackSize.y = g_tallScreen + 200;
        if (mm->ptMaxTrackSize.x < g_tallScreen + 200) mm->ptMaxTrackSize.x = g_tallScreen + 200;
    }
    return r;
}
static const WNDPROC kWrappers[4] = { WrappedProc<0>, WrappedProc<1>, WrappedProc<2>, WrappedProc<3> };
static ATOM WINAPI Hook_RegisterClassA(const WNDCLASSA* wc)
{
    if (!wc || !wc->lpfnWndProc || g_wrapped >= 4) return realRegisterClassA(wc);
    WNDCLASSA copy = *wc;
    g_origProcs[g_wrapped] = wc->lpfnWndProc;
    copy.lpfnWndProc = kWrappers[g_wrapped++];
    Log("[modes] window class '%s': max window size raised for a square window", HIWORD((ULONG_PTR)wc->lpszClassName) ? wc->lpszClassName : "#atom");
    return realRegisterClassA(&copy);
}

static HRESULT STDMETHODCALLTYPE Hook_GetDisplayModeList(IDXGIOutput* self, DXGI_FORMAT fmt, UINT flags, UINT* num, DXGI_MODE_DESC* desc)
{
    if (!num) return realGetDisplayModeList(self, fmt, flags, num, desc);
    UINT extra = 0;
    for (UINT s : kSquareSizes) if (!g_maxSquare || s <= g_maxSquare) ++extra;

    UINT realCount = 0;
    HRESULT hr = realGetDisplayModeList(self, fmt, flags, &realCount, nullptr);
    if (FAILED(hr) || realCount == 0) return realGetDisplayModeList(self, fmt, flags, num, desc);

    // Build the full list for the count query too, so both calls agree.
    std::vector<DXGI_MODE_DESC> modes(realCount);
    hr = realGetDisplayModeList(self, fmt, flags, &realCount, modes.data());
    if (FAILED(hr)) return hr;

    // Our square modes copy a real mode's timing/format. The game asks for
    // 60 Hz and picks garbage (e.g. a refresh rate read as the size) when the
    // mode it wants isn't at 60 Hz, so prefer the largest ~60 Hz mode; else the
    // last (highest-refresh) one.
    DXGI_MODE_DESC base = modes.back();
    for (const DXGI_MODE_DESC& m : modes)
    {
        UINT hz = m.RefreshRate.Denominator ? m.RefreshRate.Numerator / m.RefreshRate.Denominator : 0;
        if (hz >= 59 && hz <= 61) base = m;
    }
    // Long lists (62 real modes under Proton on the Steam Frame) plus ours are
    // too many for the game: keep only the base refresh rate's modes, largest last.
    if (realCount + extra > kMaxModes)
    {
        std::vector<DXGI_MODE_DESC> kept;
        for (const DXGI_MODE_DESC& m : modes)
            if (m.RefreshRate.Numerator == base.RefreshRate.Numerator &&
                m.RefreshRate.Denominator == base.RefreshRate.Denominator) kept.push_back(m);
        if (kept.size() + extra > kMaxModes) kept.erase(kept.begin(), kept.end() - (kMaxModes - extra));
        modes.swap(kept);
        realCount = (UINT)modes.size();
    }
    if (g_maxSquare)
    {
        // The game uses the mode *after* the one matching its settings file (on
        // the Steam Frame the last entry then reads past the list: 60000x1000,
        // 0x1376936, ...). Report only the wanted square size, repeated, so any
        // neighbor it picks is that size.
        DXGI_MODE_DESC m = base;
        m.Width = m.Height = g_maxSquare;
        modes.assign(16, m);
        realCount = 0;
        extra = (UINT)modes.size();
    }
    else
    for (UINT s : kSquareSizes)
    {
        DXGI_MODE_DESC m = base;
        m.Width = s; m.Height = s;
        modes.push_back(m);
    }
    static int calls = 0;
    if (++calls <= 20)
        Log("[modes] call %d: fmt %d flags %u, %s, caller count %u, list %u (%u real + %u square)",
            calls, (int)fmt, flags, desc ? "fill" : "count", *num, (UINT)modes.size(), realCount, extra);
    if (!desc)
    {
        *num = (UINT)modes.size();
        return S_OK;
    }
    UINT n =(UINT)modes.size() < *num ? (UINT)modes.size() : *num;
    memcpy(desc, modes.data(), n * sizeof(DXGI_MODE_DESC));
    *num = n;
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("[modes] advertised %u real + %u square display modes (fmt %d)", realCount, extra, (int)fmt);
        for (const DXGI_MODE_DESC& m : modes)
            Log("[modes]   %ux%u @ %u/%u fmt %d scan %d scale %d", m.Width, m.Height, m.RefreshRate.Numerator,
                m.RefreshRate.Denominator, (int)m.Format, (int)m.ScanlineOrdering, (int)m.Scaling);
    }
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
        void** vt = *reinterpret_cast<void***>(output);
        if (!HookMethod(vt, 8, (void*)&Hook_GetDisplayModeList, (void**)&realGetDisplayModeList, "IDXGIOutput::GetDisplayModeList"))
            realGetDisplayModeList = nullptr;
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

void DisplayModesInstall(const wchar_t* dllDir)
{
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\vrmod.ini", dllDir);
    // The game prefers larger square modes over the size in its settings file,
    // so cap what it can pick (e.g. 1728 on the Steam Frame).
    g_maxSquare = GetPrivateProfileIntW(L"xr", L"maxSquare", 0, ini);
    if (g_maxSquare) Log("[modes] square modes capped at %u", g_maxSquare);
    MH_Initialize(); // harmless if already initialized
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) { Log("[modes] dxgi.dll not loaded"); return; }
    void* f0 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory");
    void* f1 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (f0 && MH_CreateHook(f0, (void*)&Hook_CreateFactory, (void**)&realCreateFactory) == MH_OK) MH_EnableHook(f0);
    if (f1 && MH_CreateHook(f1, (void*)&Hook_CreateFactory1, (void**)&realCreateFactory1) == MH_OK) MH_EnableHook(f1);
    Log("[modes] hooked CreateDXGIFactory/CreateDXGIFactory1");

    if (!wcscmp(Game().name, L"Journey"))
    {
        g_tallScreen = (int)(g_maxSquare ? g_maxSquare : 3072) + 200;
        HMODULE user = GetModuleHandleW(L"user32.dll");
        void* gsm = user ? (void*)GetProcAddress(user, "GetSystemMetrics") : nullptr;
        bool ok = gsm && MH_CreateHook(gsm, (void*)&Hook_GetSystemMetrics, (void**)&realGetSystemMetrics) == MH_OK && MH_EnableHook(gsm) == MH_OK;
        void* rc = user ? (void*)GetProcAddress(user, "RegisterClassA") : nullptr;
        ok = ok && rc && MH_CreateHook(rc, (void*)&Hook_RegisterClassA, (void**)&realRegisterClassA) == MH_OK && MH_EnableHook(rc) == MH_OK;
        Log("[modes] screen height reported as at least %d for a square window: %s", g_tallScreen, ok ? "hooked" : "FAILED");
    }
}
