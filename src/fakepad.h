#pragma once
#include <Windows.h>

// Hooks XInputGetState so a script can drive a virtual controller through
// vrmod_pad.txt next to the DLL. Enabled by vrmod.ini [debug] fakepad=1.
// Requires MinHook to be initialized.
void InstallFakePad(const wchar_t* dllDir);
