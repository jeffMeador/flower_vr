#pragma once
#include <Windows.h>

// True once per physical press. GetAsyncKeyState's "pressed since last call"
// bit fired on several consecutive frames here, and the down state itself can
// flicker while a key is held, so: up -> down edge, debounced to 250 ms.
// Call from one place per key (it keeps per-key state).
// Only while the game's own window has focus: GetAsyncKeyState sees every key
// on the system, so typing "." in another app used to raise the VR camera.
inline bool GameHasFocus()
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

inline bool KeyEdge(int vk)
{
    static bool wasDown[256] = {};
    static DWORD lastEdge[256] = {};
    int k = vk & 0xFF;
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0 && GameHasFocus();
    bool edge = down && !wasDown[k];
    wasDown[k] = down;
    if (!edge) return false;
    DWORD now = GetTickCount();
    if (now - lastEdge[k] < 250) return false;
    lastEdge[k] = now;
    return true;
}
