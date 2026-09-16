#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdarg>

inline FILE* g_logFile = nullptr;

inline void LogInit(const wchar_t* dllDir)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\vrmod.log", dllDir);
    _wfopen_s(&g_logFile, path, L"w");
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
