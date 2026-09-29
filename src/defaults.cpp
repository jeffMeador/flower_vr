#include "defaults.h"
#include "log.h"
#include <Windows.h>
#include <string>
#include <cstdio>

// Same values as vrmod.ini.example, except the square size, which matches the
// auto-created vrmod_Flower.cfg below.
static const char kDefaultIni[] =
    "[stereo]\r\n"
    "render=double\r\n"
    "enabled=1\r\n"
    "separation=0.0650\r\n"
    "shiftEyePosition=0\r\n"
    "lens=off\r\n"
    "worldScale=0.3000\r\n"
    "motionBlur=1\r\n"
    "depthOfField=1\r\n"
    "dofNear=1\r\n"
    "dofFar=0\r\n"
    "\r\n"
    "[debug]\r\n"
    "fakepad=0\r\n"
    "\r\n"
    "[xr]\r\n"
    "depth=0\r\n"
    "headCamera=1\r\n"
    "cameraBack=1.50\r\n"
    "cameraUp=0.55\r\n"
    "cameraSide=-0.25\r\n"
    "cameraFlights=1\r\n"
    "maxSquare=2160\r\n"
    "steering=motion\r\n";


static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static bool ReadAll(const std::wstring& p, std::string& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static bool WriteAll(const std::wstring& p, const std::string& data)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

// The Documents folder, from the registry (the shell API isn't safe in DllMain).
static std::wstring DocumentsFolder()
{
    wchar_t raw[MAX_PATH] = {};
    DWORD size = sizeof(raw);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
            L"Personal", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw, &size) != ERROR_SUCCESS)
        return {};
    wchar_t expanded[MAX_PATH] = {};
    if (!ExpandEnvironmentStringsW(raw, expanded, MAX_PATH)) return {};
    return expanded;
}

void EnsureDefaultSettings(const wchar_t* dllDir)
{
    std::wstring dir(dllDir);

    std::wstring ini = dir + L"\\vrmod.ini";
    if (!Exists(ini))
    {
        bool ok = WriteAll(ini, kDefaultIni);
        Log("[defaults] no vrmod.ini: %s one with the recommended settings", ok ? "wrote" : "could NOT write");
    }

    std::wstring cfg = dir + L"\\vrmod_Flower.cfg";
    if (Exists(cfg)) return;
    std::wstring docs = DocumentsFolder();
    std::wstring src = docs.empty() ? std::wstring() : docs + L"\\Flower\\Flower.cfg";
    std::string text;
    if (src.empty() || !ReadAll(src, text))
    {
        Log("[defaults] no vrmod_Flower.cfg and no Documents\\Flower\\Flower.cfg to base it on (start the game once without the mod)");
        return;
    }
    size_t a = text.find("<Screen ");
    size_t b = a == std::string::npos ? a : text.find("/>", a);
    if (b == std::string::npos)
    {
        Log("[defaults] %ls has no <Screen> setting; vrmod_Flower.cfg not created", src.c_str());
        return;
    }
    // Square size = the ini's maxSquare, so the mode list offers exactly this size.
    UINT side = GetPrivateProfileIntW(L"xr", L"maxSquare", 2160, ini.c_str());
    if (!side) side = 2160;
    char screen[200];
    sprintf_s(screen, "<Screen Anisotrophy=\"16\" FullScreen=\"false\" Height=\"%u\" MultiSampleCount=\"4\" VSync=\"true\" Width=\"%u\"/>", side, side);
    text.replace(a, b + 2 - a, screen);
    bool ok = WriteAll(cfg, text);
    Log("[defaults] no vrmod_Flower.cfg: %s one from %ls with a square %u x %u, 4x MSAA, windowed screen",
        ok ? "made" : "could NOT make", src.c_str(), side, side);
}
