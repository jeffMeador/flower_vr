#include "fileredirect.h"
#include "log.h"
#include "terrain.h"
#include <MinHook.h>
#include <cwchar>
#include <cwctype>
#include <string>

// File redirects, so the mod never has to modify the user's or the game's files:
//
//  - Documents\Flower\Flower.cfg (resolution, MSAA, ... shared with the non-VR
//    Steam install) -> <game dir>\vrmod_Flower.cfg, if that file exists, so
//    VR can use a square render resolution.
//  - Any game data file <...>\Data\<relative path> -> <game dir>\vrmod_overrides\
//    <relative path>, if an override file exists there (e.g. a Scripts\
//    MovieBarn.lua that turns the level movies off). Delete the folder to undo.

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t realCreateFileW;
static CreateFileA_t realCreateFileA;
static std::wstring g_cfgTarget;     // empty = no settings redirect
static std::wstring g_overrideDir;   // empty = no overrides folder
static bool g_cameraFlights = false; // [xr] cameraFlights=1: skip the camera-grab (trigger) overrides
static bool g_loggedCfg = false;

static bool EndsWithI(const wchar_t* path, const wchar_t* suffix)
{
    size_t n = wcslen(path), m = wcslen(suffix);
    if (n < m) return false;
    const wchar_t* tail = path + n - m;
    for (size_t i = 0; i < m; ++i)
    {
        wchar_t a = tail[i] == L'/' ? L'\\' : towlower(tail[i]);
        if (a != towlower(suffix[i])) return false;
    }
    return true;
}

// Returns the override path for a game data file, or empty.
static std::wstring OverrideFor(const wchar_t* path)
{
    if (g_overrideDir.empty()) return {};
    if (g_cameraFlights && EndsWithI(path, L"\\TriggerInstances.lua")) return {};
    std::wstring p(path);
    for (auto& c : p) if (c == L'/') c = L'\\';
    std::wstring lower = p;
    for (auto& c : lower) c = towlower(c);
    size_t at = lower.rfind(L"\\data\\");
    if (at == std::wstring::npos) return {};
    std::wstring candidate = g_overrideDir + L"\\" + p.substr(at + 6);
    return GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES ? candidate : std::wstring();
}

static HANDLE Redirected(const wchar_t* name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl, bool* handled)
{
    *handled = false;
    if (!name) return INVALID_HANDLE_VALUE;
    TerrainNotifyFileOpened(name); // remembers the level's heightmap
    if (!g_cfgTarget.empty() && EndsWithI(name, L"\\Flower\\Flower.cfg"))
    {
        if (!g_loggedCfg) { g_loggedCfg = true; Log("[redirect] %ls -> %ls", name, g_cfgTarget.c_str()); }
        *handled = true;
        return realCreateFileW(g_cfgTarget.c_str(), access, share, sa, disp, flags, tmpl);
    }
    if (!(access & GENERIC_WRITE))
    {
        std::wstring o = OverrideFor(name);
        if (!o.empty())
        {
            Log("[redirect] override: %ls -> %ls", name, o.c_str());
            *handled = true;
            return realCreateFileW(o.c_str(), access, share, sa, disp, flags, tmpl);
        }
    }
    return INVALID_HANDLE_VALUE;
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    bool handled;
    HANDLE h = Redirected(name, access, share, sa, disp, flags, tmpl, &handled);
    return handled ? h : realCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (name)
    {
        wchar_t w[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH))
        {
            bool handled;
            HANDLE h = Redirected(w, access, share, sa, disp, flags, tmpl, &handled);
            if (handled) return h;
        }
    }
    return realCreateFileA(name, access, share, sa, disp, flags, tmpl);
}

bool FileRedirectInstall(const wchar_t* gameDir)
{
    std::wstring cfg = std::wstring(gameDir) + L"\\vrmod_Flower.cfg";
    std::wstring dir = std::wstring(gameDir) + L"\\vrmod_overrides";
    bool haveCfg = GetFileAttributesW(cfg.c_str()) != INVALID_FILE_ATTRIBUTES;
    DWORD da = GetFileAttributesW(dir.c_str());
    bool haveDir = da != INVALID_FILE_ATTRIBUTES && (da & FILE_ATTRIBUTE_DIRECTORY);
    if (haveCfg) g_cfgTarget = cfg; else Log("[redirect] no vrmod_Flower.cfg; using the normal settings file");
    if (haveDir) { g_overrideDir = dir; Log("[redirect] data overrides from %ls", dir.c_str()); }
    std::wstring ini = std::wstring(gameDir) + L"\\vrmod.ini";
    g_cameraFlights = GetPrivateProfileIntW(L"xr", L"cameraFlights", 0, ini.c_str()) != 0;
    if (g_cameraFlights) Log("[redirect] cameraFlights=1: the game's camera flights stay on");
    if (!haveCfg && !haveDir) return false;

    MH_Initialize(); // harmless if already initialized
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    void* w = kb ? (void*)GetProcAddress(kb, "CreateFileW") : nullptr;
    void* a = kb ? (void*)GetProcAddress(kb, "CreateFileA") : nullptr;
    bool ok = w && a &&
        MH_CreateHook(w, (void*)&Hook_CreateFileW, (void**)&realCreateFileW) == MH_OK && MH_EnableHook(w) == MH_OK &&
        MH_CreateHook(a, (void*)&Hook_CreateFileA, (void**)&realCreateFileA) == MH_OK && MH_EnableHook(a) == MH_OK;
    Log("[redirect] file redirect hooks: %s", ok ? "installed" : "FAILED");
    return ok && haveCfg; // display modes are only needed for the VR settings file
}
