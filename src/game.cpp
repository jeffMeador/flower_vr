#include "game.h"
#include <Windows.h>
#include <cwctype>

// A user shell folder ("Personal" = Documents, "Local AppData"), from the
// registry: the shell API isn't safe to use in DllMain.
static std::wstring ShellFolder(const wchar_t* value)
{
    wchar_t raw[MAX_PATH] = {};
    DWORD size = sizeof(raw);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
            value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw, &size) != ERROR_SUCCESS)
        return {};
    wchar_t expanded[MAX_PATH] = {};
    if (!ExpandEnvironmentStringsW(raw, expanded, MAX_PATH)) return {};
    return expanded;
}

static GameInfo Detect()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* file = wcsrchr(exe, L'\\');
    file = file ? file + 1 : exe;

    GameInfo g;
    if (_wcsicmp(file, L"Journey.exe") == 0)
    {
        g.name = L"Journey";
        g.nameA = "Journey";
        std::wstring local = ShellFolder(L"Local AppData");
        if (!local.empty()) g.settingsPath = local + L"\\Annapurna Interactive\\Journey\\Steam\\Journey.cfg";
        g.settingsTail = L"\\Journey\\Steam\\Journey.cfg";
    }
    else
    {
        g.name = L"Flower";
        g.nameA = "Flower";
        std::wstring docs = ShellFolder(L"Personal");
        if (!docs.empty()) g.settingsPath = docs + L"\\Flower\\Flower.cfg";
        g.settingsTail = L"\\Flower\\Flower.cfg";
    }
    g.vrSettingsName = std::wstring(L"vrmod_") + g.name + L".cfg";
    return g;
}

const GameInfo& Game()
{
    static GameInfo g = Detect();
    return g;
}
