#include "camfind.h"
#include "log.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <cmath>
#include <cstring>
#include <cstdio>

// Debug tool (F7): locate the engine's perspective-camera block
// {fov_deg, near, far, aspect} by value, then watch the fov float with a
// hardware read/write watchpoint for ~2 seconds and log every instruction
// that touches it (module + RVA), to find where to override the FOV.

static const float kNear = 0.1f, kFar = 1330.5f;

static volatile LONG g_ripCount = 0;
static void* g_rips[32];
static volatile LONG g_ripHits[32];
static volatile LONG g_seqCount = 0;
static unsigned char g_seq[96]; // order of hits by distinct-RIP index
static void* g_watchAddr = nullptr;
static PVOID g_veh = nullptr;
static DWORD g_disarmAt = 0;

static LONG WINAPI Handler(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    PCONTEXT ctx = info->ContextRecord;
    if (!(ctx->Dr6 & 0x1)) return EXCEPTION_CONTINUE_SEARCH;
    void* rip = (void*)ctx->Rip; // instruction *after* the access (data breakpoints are traps)
    LONG n = g_ripCount;
    int i = 0;
    for (; i < n; ++i) if (g_rips[i] == rip) break;
    if (i == n && n < 32) { g_rips[n] = rip; InterlockedIncrement(&g_ripCount); }
    if (i < 32) InterlockedIncrement(&g_ripHits[i]);
    LONG sq = InterlockedIncrement(&g_seqCount) - 1;
    if (sq < 96) g_seq[sq] = (unsigned char)i;
    ctx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void SetWatchAllThreads(void* addr, bool arm)
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof(te) };
    int count = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!h) continue;
        bool isSelf = te.th32ThreadID == self;
        if (isSelf || SuspendThread(h) != (DWORD)-1)
        {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (isSelf) c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &c))
            {
                if (arm)
                {
                    c.Dr0 = (DWORD64)addr;
                    c.Dr7 &= ~((DWORD64)0xF << 16);
                    c.Dr7 |= ((DWORD64)0x3 << 16); // RW0 = 11: read or write
                    c.Dr7 |= ((DWORD64)0x3 << 18); // LEN0 = 11: 4 bytes
                    c.Dr7 |= 0x1;
                }
                else
                {
                    c.Dr0 = 0;
                    c.Dr7 &= ~((DWORD64)0xF << 16);
                    c.Dr7 &= ~(DWORD64)0x1;
                }
                SetThreadContext(h, &c);
                count++;
            }
            if (!isSelf) ResumeThread(h);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    Log("[camfind] watchpoint %s on %d threads", arm ? "armed" : "disarmed", count);
}

// Run from a helper thread so the caller (the game's render thread) gets
// suspended and armed like every other thread.
struct WatchJob { void* addr; bool arm; };
static DWORD WINAPI WatchThread(LPVOID p)
{
    WatchJob* j = (WatchJob*)p;
    SetWatchAllThreads(j->addr, j->arm);
    return 0;
}
static void SetWatch(void* addr, bool arm)
{
    WatchJob j = { addr, arm };
    HANDLE t = CreateThread(nullptr, 0, WatchThread, &j, 0, nullptr);
    if (t) { WaitForSingleObject(t, 5000); CloseHandle(t); }
}

static void LogRip(void* rip, LONG hits)
{
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    Log("[camfind]   after-RIP %p = %s+0x%llX  hits=%ld", rip, base ? base + 1 : name,
        (unsigned long long)((char*)rip - (char*)mod), hits);
}

static void ScanRegionUnsafe(float* f, size_t n, float aspect, float** best, int* found)
{
    for (size_t i = 2; i + 1 < n; ++i)
    {
        if (f[i] != kFar || f[i - 1] != kNear) continue;
        if (fabsf(f[i + 1] - aspect) > 1e-3f) continue;
        float fov = f[i - 2];
        if (!(fov > 1.0f && fov < 179.0f)) continue;
        Log("[camfind] candidate at %p: fov=%.3f near=%.3f far=%.1f aspect=%.4f", &f[i - 2], fov, f[i - 1], f[i], f[i + 1]);
        if (!*best) *best = &f[i - 2];
        (*found)++;
    }
}

// Regions can be freed by other threads mid-scan; don't crash the game.
static void ScanRegion(float* f, size_t n, float aspect, float** best, int* found)
{
    __try { ScanRegionUnsafe(f, n, aspect, best, found); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static float* FindCamera(float aspect)
{
    float* best = nullptr;
    int found = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (char* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi)); p = (char*)mbi.BaseAddress + mbi.RegionSize)
    {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        ScanRegion((float*)mbi.BaseAddress, mbi.RegionSize / 4, aspect, &best, &found);
    }
    Log("[camfind] %d candidate(s)", found);
    return best;
}

#include "camoverride.h"

static void StartWatch(void* addr, const char* what)
{
    g_watchAddr = addr;
    g_ripCount = 0;
    g_seqCount = 0;
    for (auto& h : g_ripHits) h = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Handler);
    Log("[camfind] watching %s at %p (present thread %lu)", what, addr, GetCurrentThreadId());
    SetWatch(addr, true);
    g_disarmAt = GetTickCount() + 2000;
}

void CamFindTick(float backbufferAspect)
{
    if (g_disarmAt) { LONG sq = InterlockedIncrement(&g_seqCount) - 1; if (sq < 96) g_seq[sq] = 99; } // 99 = Present
    if (g_disarmAt && GetTickCount() >= g_disarmAt)
    {
        g_disarmAt = 0;
        SetWatch(nullptr, false);
        Log("[camfind] %ld distinct instruction(s) touched fov at %p:", g_ripCount, g_watchAddr);
        for (LONG i = 0; i < g_ripCount; ++i) { Log("[camfind]  #%ld:", i); LogRip(g_rips[i], g_ripHits[i]); }
        char buf[400] = {}; size_t n = 0;
        for (LONG k = 0; k < g_seqCount && k < 96; ++k) n += sprintf_s(buf + n, sizeof(buf) - n, "%d ", g_seq[k]);
        Log("[camfind] hit order (first 96): %s", buf);
        Log("[camfind] (99 = Present)");
        return;
    }
    if (g_disarmAt) return;
    if (GetAsyncKeyState(VK_F4) & 1)
    {
        // F4: who reads/writes the render camera matrix (x axis at node+0x90)?
        void* node = CamOverrideCameraNode();
        if (node) StartWatch((char*)node + 0x90, "camera matrix");
        return;
    }
    if (!(GetAsyncKeyState(VK_F7) & 1)) return;

    float* cam = FindCamera(backbufferAspect);
    if (!cam) return;
    g_watchAddr = cam;
    g_ripCount = 0;
    g_seqCount = 0;
    for (auto& h : g_ripHits) h = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Handler);
    SetWatch(cam, true);
    g_disarmAt = GetTickCount() + 2000;
}
