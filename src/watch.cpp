#include "watch.h"
#include "log.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <psapi.h>

static void* g_watchAddress = nullptr;

static const char* ModuleNameForAddress(void* addr, char* buf, size_t bufLen)
{
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)addr, &mod))
    {
        if (GetModuleFileNameA(mod, buf, (DWORD)bufLen) > 0)
            return buf;
    }
    strcpy_s(buf, bufLen, "<unknown module>");
    return buf;
}

static LONG WINAPI WatchpointHandler(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    PCONTEXT ctx = info->ContextRecord;
    if (!(ctx->Dr6 & 0x1)) // bit 0 = DR0 triggered
        return EXCEPTION_CONTINUE_SEARCH;

    char modBuf[MAX_PATH] = {};
    ModuleNameForAddress((void*)ctx->Rip, modBuf, sizeof(modBuf));
    Log("[watchpoint] write to %p from RIP=%p module=%s", g_watchAddress, (void*)ctx->Rip, modBuf);

    ctx->Dr6 = 0; // clear status
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void ArmOnThread(HANDLE hThread, void* address)
{
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(hThread, &ctx))
        return;

    ctx.Dr0 = (DWORD64)address;
    // Dr7: L0=1 (bit0), RW0=01 (write-only, bits16-17), LEN0=10 (8 bytes, bits18-19)
    ctx.Dr7 &= ~((DWORD64)0xF << 16);      // clear RW0/LEN0
    ctx.Dr7 |= ((DWORD64)0x1 << 16);       // RW0 = 01 (write)
    ctx.Dr7 |= ((DWORD64)0x2 << 18);       // LEN0 = 10 (8 bytes)
    ctx.Dr7 |= 0x1;                        // L0 = 1 (enable local)

    SetThreadContext(hThread, &ctx);
}

void InstallWriteWatchpointAllThreads(void* address)
{
    g_watchAddress = address;
    AddVectoredExceptionHandler(1, WatchpointHandler);

    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
    {
        Log("[watchpoint] failed to snapshot threads");
        return;
    }

    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    int armed = 0;
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!hThread) continue;

            if (te.th32ThreadID == tid)
            {
                ArmOnThread(hThread, address);
                armed++;
            }
            else
            {
                if (SuspendThread(hThread) != (DWORD)-1)
                {
                    ArmOnThread(hThread, address);
                    ResumeThread(hThread);
                    armed++;
                }
            }
            CloseHandle(hThread);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    Log("[watchpoint] armed on %d thread(s) for address %p", armed, address);
}
