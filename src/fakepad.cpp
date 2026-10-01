#include <cstdlib>
#include "fakepad.h"
#include "log.h"
#include <MinHook.h>
#include <Xinput.h>
#include <cstdio>
#include <cstring>
#include <cmath>

// Virtual XInput controller #0, merged with any real pad #0. Two sources:
//  - VR motion controllers (FakePadSetXR, from xr.cpp)
//  - a text file, so scripts can drive the game (the game's raw-input path
//    ignores injected SendInput events)
//
// vrmod_pad.txt, one line:  <lx> <ly> [A] [B] [X] [Y] [START] [BACK] [LT] [RT] [RX=<v>] [RY=<v>]
// lx/ly in -1..1. Missing file = neutral pad. The pad only appears when
// vrmod.ini has [debug] fakepad=1.

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
static XInputGetState_t g_realXInputGetState = nullptr;

static wchar_t g_padPath[MAX_PATH] = {};
static FILETIME g_lastWrite = {};
static DWORD g_lastCheck = 0;
static float g_lx = 0, g_ly = 0, g_rx = 0, g_ry = 0;
static WORD g_buttons = 0;
static BYTE g_lt = 0, g_rt = 0;
static DWORD g_packet = 0;
static bool g_fileEnabled = false;

// Second source: VR motion controllers (set every frame by the XR code).
static volatile float g_xrLx = 0, g_xrLy = 0, g_xrRx = 0, g_xrRy = 0;
static volatile WORD g_xrButtons = 0;
static volatile BYTE g_xrLt = 0, g_xrRt = 0;
static volatile DWORD g_xrPacket = 0;

void FakePadSetXR(float lx, float ly, WORD buttons, BYTE lt, BYTE rt, float rx, float ry)
{
    if (lx != g_xrLx || ly != g_xrLy || buttons != g_xrButtons || lt != g_xrLt || rt != g_xrRt || rx != g_xrRx || ry != g_xrRy) g_xrPacket++;
    g_xrLx = lx; g_xrLy = ly; g_xrButtons = buttons; g_xrLt = lt; g_xrRt = rt; g_xrRx = rx; g_xrRy = ry;
}

static void ReloadPadFile()
{
    DWORD now = GetTickCount();
    if (now - g_lastCheck < 30) return;
    g_lastCheck = now;

    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(g_padPath, GetFileExInfoStandard, &attr))
    {
        g_lx = g_ly = g_rx = g_ry = 0; g_buttons = 0;
        return;
    }
    if (CompareFileTime(&attr.ftLastWriteTime, &g_lastWrite) == 0) return;
    g_lastWrite = attr.ftLastWriteTime;

    FILE* f = nullptr;
    if (_wfopen_s(&f, g_padPath, L"r") != 0 || !f) return;
    char line[256] = {};
    fgets(line, sizeof(line), f);
    fclose(f);

    float lx = 0, ly = 0, rx = 0, ry = 0;
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
        else if (!strncmp(tok, "RX=", 3)) rx = (float)atof(tok + 3);
        else if (!strncmp(tok, "RY=", 3)) ry = (float)atof(tok + 3);
    }
    g_lx = lx; g_ly = ly; g_rx = rx; g_ry = ry; g_buttons = b; g_lt = lt; g_rt = rt;
    g_packet++;
    Log("[fakepad] lx=%.2f ly=%.2f rx=%.2f ry=%.2f buttons=0x%04X lt=%u rt=%u", lx, ly, rx, ry, b, lt, rt);
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

    if (g_fileEnabled) ReloadPadFile();
    if (r != ERROR_SUCCESS)
        memset(state, 0, sizeof(*state));
    state->dwPacketNumber += g_packet + g_xrPacket;
    state->Gamepad.wButtons |= g_buttons | g_xrButtons;
    BYTE lt = g_lt > g_xrLt ? g_lt : g_xrLt, rt = g_rt > g_xrRt ? g_rt : g_xrRt;
    if (lt > state->Gamepad.bLeftTrigger) state->Gamepad.bLeftTrigger = lt;
    if (rt > state->Gamepad.bRightTrigger) state->Gamepad.bRightTrigger = rt;
    float lx = g_lx != 0 ? g_lx : g_xrLx, ly = g_ly != 0 ? g_ly : g_xrLy;
    if (lx != 0) state->Gamepad.sThumbLX = ToAxis(lx);
    if (ly != 0) state->Gamepad.sThumbLY = ToAxis(ly);
    float rx = g_rx != 0 ? g_rx : g_xrRx, ry = g_ry != 0 ? g_ry : g_xrRy;
    if (rx != 0) state->Gamepad.sThumbRX = ToAxis(rx);
    if (ry != 0) state->Gamepad.sThumbRY = ToAxis(ry);
    return ERROR_SUCCESS;
}

void InstallFakePad(const wchar_t* dllDir)
{
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\vrmod.ini", dllDir);
    // Always hooked (VR controllers feed it); the script-driven file source
    // only with [debug] fakepad=1.
    g_fileEnabled = GetPrivateProfileIntW(L"debug", L"fakepad", 0, ini) != 0;
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
