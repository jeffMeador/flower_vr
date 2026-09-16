#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>

inline FILE* g_logFile = nullptr;

inline void LogInit(const wchar_t* dllDir)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\vrmod.log", dllDir);
    // _SH_DENYNO: allow other processes (us, tailing it) to read while the game holds it open.
    g_logFile = _wfsopen(path, L"w", _SH_DENYNO);
}

inline void Log(const char* fmt, ...)
{
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}
