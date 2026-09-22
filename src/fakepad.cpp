#include "fakepad.h"
#include "log.h"
#include <MinHook.h>
#include <Xinput.h>
#include <cstdio>
#include <cstring>
#include <cmath>

// Virtual XInput controller #0 driven by a text file, so the game can be
// steered by scripts (the game's raw-input path ignores injected
// SendInput events). Later this is where VR controller input plugs in.
//
// vrmod_pad.txt, one line:  <lx> <ly> [A] [B] [X] [Y] [START] [BACK] [LT] [RT]
// lx/ly in -1..1. Missing file = neutral pad. The pad only appears when
// vrmod.ini has [debug] fakepad=1.

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
static XInputGetState_t g_realXInputGetState = nullptr;

static wchar_t g_padPath[MAX_PATH] = {};
static FILETIME g_lastWrite = {};
static DWORD g_lastCheck = 0;
static float g_lx = 0, g_ly = 0;
static WORD g_buttons = 0;
static BYTE g_lt = 0, g_rt = 0;
static DWORD g_packet = 0;

static void ReloadPadFile()
{
    DWORD now = GetTickCount();
    if (now - g_lastCheck < 30) return;
    g_lastCheck = now;

    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(g_padPath, GetFileExInfoStandard, &attr))
    {
        g_lx = g_ly = 0; g_buttons = 0;
        return;
    }
    if (CompareFileTime(&attr.ftLastWriteTime, &g_lastWrite) == 0) return;
    g_lastWrite = attr.ftLastWriteTime;

    FILE* f = nullptr;
    if (_wfopen_s(&f, g_padPath, L"r") != 0 || !f) return;
    char line[256] = {};
    fgets(line, sizeof(line), f);
    fclose(f);

    float lx = 0, ly = 0;
    sscanf_s(line, "%f %f", &lx, &ly);
    WORD b = 0;
    BYTE lt = 0, rt = 0;
    char* ctx = nullptr;
    for (char* tok = strtok_s(line, " \t\r\n", &ctx); tok; tok = strtok_s(nullptr, " \t\r\n", &ctx))
    {
        if (!strcmp(tok, "A")) b |= XINPUT_GAMEPAD_A;
        else if (!strcmp(tok, "B")) b |= XINPUT_GAMEPAD_B;
        else if (!strcmp(tok, "X")) b |= XINPUT_GAMEPAD_X;
        else if (!strcmp(tok, "Y")) b |= XINPUT_GAMEPAD_Y;
        else if (!strcmp(tok, "START")) b |= XINPUT_GAMEPAD_START;
        else if (!strcmp(tok, "BACK")) b |= XINPUT_GAMEPAD_BACK;
        else if (!strcmp(tok, "LT")) lt = 255;
        else if (!strcmp(tok, "RT")) rt = 255;
    }
    g_lx = lx; g_ly = ly; g_buttons = b; g_lt = lt; g_rt = rt;
    g_packet++;
    Log("[fakepad] lx=%.2f ly=%.2f buttons=0x%04X lt=%u rt=%u", lx, ly, b, lt, rt);
}

static SHORT ToAxis(float v)
{
    if (v > 1) v = 1; if (v < -1) v = -1;
    return (SHORT)(v * 32767.0f);
}

static DWORD WINAPI Hook_XInputGetState(DWORD user, XINPUT_STATE* state)
{
    DWORD r = g_realXInputGetState(user, state);
    if (user != 0 || !state) return r;

    ReloadPadFile();
    if (r != ERROR_SUCCESS)
        memset(state, 0, sizeof(*state));
    state->dwPacketNumber += g_packet;
    state->Gamepad.wButtons |= g_buttons;
    if (g_lt) state->Gamepad.bLeftTrigger = g_lt;
    if (g_rt) state->Gamepad.bRightTrigger = g_rt;
    if (g_lx != 0) state->Gamepad.sThumbLX = ToAxis(g_lx);
    if (g_ly != 0) state->Gamepad.sThumbLY = ToAxis(g_ly);
    return ERROR_SUCCESS;
}

void InstallFakePad(const wchar_t* dllDir)
{
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\vrmod.ini", dllDir);
    if (!GetPrivateProfileIntW(L"debug", L"fakepad", 0, ini))
        return;
    swprintf_s(g_padPath, L"%s\\vrmod_pad.txt", dllDir);

    HMODULE xi = GetModuleHandleW(L"XINPUT9_1_0.dll");
    if (!xi) xi = LoadLibraryW(L"XINPUT9_1_0.dll");
    void* target = xi ? (void*)GetProcAddress(xi, "XInputGetState") : nullptr;
    if (!target)
    {
        Log("[fakepad] XInputGetState not found");
        return;
    }
    MH_STATUS st = MH_CreateHook(target, (void*)&Hook_XInputGetState, (void**)&g_realXInputGetState);
    if (st == MH_OK) st = MH_EnableHook(target);
    Log("[fakepad] hooked XInputGetState at %p: %s (pad file %ls)", target, MH_StatusToString(st), g_padPath);
}
