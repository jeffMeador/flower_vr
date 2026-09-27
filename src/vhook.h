#pragma once
#include <Windows.h>
#include <MinHook.h>
#include <unordered_set>
#include "log.h"

// Hooks a COM method (vtable[index]).
//  - Windows: inline hook (MinHook) on the implementation. The D3D11 runtime
//    keeps rewriting its own vtable slots, so swapping them doesn't stick.
//  - Wine/Proton (DXVK): swap the vtable slot. DXVK leaves its vtables alone,
//    and on the Steam Frame its code is ARM64 (ARM64EC), where MinHook's x86
//    jumps would corrupt it.
inline bool RunningUnderWine()
{
    static int wine = -1;
    if (wine < 0)
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        wine = ntdll && GetProcAddress(ntdll, "wine_get_version") ? 1 : 0;
    }
    return wine == 1;
}

inline bool HookMethod(void** vtable, int index, void* detour, void** original, const char* name)
{
    static std::unordered_set<void*> hooked; // vtable slots (Wine) or implementations (Windows)
    if (RunningUnderWine())
    {
        void** slot = &vtable[index];
        if (hooked.count(slot)) return true;
        DWORD old;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        {
            Log("[hook] %s: VirtualProtect failed on vtable slot %p", name, slot);
            return false;
        }
        *original = *slot;
        *slot = detour;
        VirtualProtect(slot, sizeof(void*), old, &old);
        hooked.insert(slot);
        Log("[hook] vtable-hooked %s (slot %d at %p)", name, index, slot);
        return true;
    }

    void* target = vtable[index];
    if (hooked.count(target)) return true; // same implementation already hooked
    MH_Initialize(); // harmless if already initialized
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED)
    {
        Log("[hook] inline hook %s at %p FAILED: %s", name, target, MH_StatusToString(st));
        return false;
    }
    hooked.insert(target);
    Log("[hook] inline-hooked %s at %p", name, target);
    return true;
}
