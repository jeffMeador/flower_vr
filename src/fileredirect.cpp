#include "fileredirect.h"
#include "log.h"
#include <MinHook.h>
#include <cwchar>
#include <cwctype>
#include <string>

// The game keeps its settings (resolution, MSAA, ...) in
// Documents\Flower\Flower.cfg, shared with the non-VR Steam install. In VR we
// want a square render resolution, so reads/writes of that file are redirected
// to <game dir>\vrmod_Flower.cfg when it exists. The user's own file is never
// touched.

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t realCreateFileW;
static CreateFileA_t realCreateFileA;
static std::wstring g_target;
static bool g_loggedOnce = false;

static bool IsGameCfg(const wchar_t* path)
{
    if (!path) return false;
    size_t n = wcslen(path);
    const wchar_t* suffix = L"\\Flower\\Flower.cfg";
    size_t m = wcslen(suffix);
    if (n < m) return false;
    const wchar_t* tail = path + n - m;
    for (size_t i = 0; i < m; ++i)
    {
        wchar_t a = tail[i] == L'/' ? L'\\' : towlower(tail[i]);
        wchar_t b = towlower(suffix[i]);
        if (a != b) return false;
    }
    return true;
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (IsGameCfg(name))
    {
        if (!g_loggedOnce) { g_loggedOnce = true; Log("[redirect] %ls -> %ls", name, g_target.c_str()); }
        return realCreateFileW(g_target.c_str(), access, share, sa, disp, flags, tmpl);
    }
    return realCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (name)
    {
        wchar_t w[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH) && IsGameCfg(w))
        {
            if (!g_loggedOnce) { g_loggedOnce = true; Log("[redirect] %s -> %ls", name, g_target.c_str()); }
            return realCreateFileW(g_target.c_str(), access, share, sa, disp, flags, tmpl);
        }
    }
    return realCreateFileA(name, access, share, sa, disp, flags, tmpl);
}

bool FileRedirectInstall(const wchar_t* gameDir)
{
    g_target = std::wstring(gameDir) + L"\\vrmod_Flower.cfg";
    if (GetFileAttributesW(g_target.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Log("[redirect] no vrmod_Flower.cfg; using the normal settings file");
        return false;
    }
    MH_Initialize(); // harmless if already initialized
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    void* w = kb ? (void*)GetProcAddress(kb, "CreateFileW") : nullptr;
    void* a = kb ? (void*)GetProcAddress(kb, "CreateFileA") : nullptr;
    bool ok = w && a &&
        MH_CreateHook(w, (void*)&Hook_CreateFileW, (void**)&realCreateFileW) == MH_OK && MH_EnableHook(w) == MH_OK &&
        MH_CreateHook(a, (void*)&Hook_CreateFileA, (void**)&realCreateFileA) == MH_OK && MH_EnableHook(a) == MH_OK;
    Log("[redirect] settings file redirect to %ls: %s", g_target.c_str(), ok ? "installed" : "FAILED");
    return ok;
}
