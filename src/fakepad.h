#pragma once
#include <Windows.h>

// Hooks XInputGetState so a script can drive a virtual controller through
// vrmod_pad.txt next to the DLL. Enabled by vrmod.ini [debug] fakepad=1.
// Requires MinHook to be initialized.
void InstallFakePad(const wchar_t* dllDir);

// VR controller state for the virtual pad (stick -1..1, XINPUT_GAMEPAD_* bits, triggers 0..255).
void FakePadSetXR(float lx, float ly, WORD buttons, BYTE lt, BYTE rt, float rx = 0, float ry = 0);

// How far the left stick was pushed (0..1) when the game last read the pad.
float FakePadLeftStick();
// When the game last saw START (the pause button) held, GetTickCount; 0 never.
DWORD FakePadLastStart();
