#include "camoverride.h"
#include "log.h"
#include "keys.h"
#include "stereo.h"
#include <Windows.h>
#include <cstring>
#include <cstdint>
#include <initializer_list>

// Overrides the engine camera's FOV/aspect so its frustum (and therefore its
// CPU culling) covers the headset's view. Found with camfind (F7):
//
//   Flower.exe+0x3F5DE  mov   rcx,[rbx+28h]        ; engine camera object
//   Flower.exe+0x3F5E2  movss xmm0,[rbx+60h]       ; FOV game logic wants (vertical deg)
//   Flower.exe+0x3F5E7  ucomiss xmm0,[rcx+134h]    ; changed?
//   Flower.exe+0x3F5EE  je skip                    ; else store + vtable[0x98] (rebuild proj)
//
// We replace those 16 bytes with an absolute jmp to a stub that records the
// camera pointer and the game's FOV, substitutes ours when enabled, redoes
// the ucomiss and jumps back to the je. Camera layout: fov +0x134, near
// +0x138, far +0x13C, aspect +0x140.

static const uintptr_t kPatchRva = 0x3F5DE;
static const uint8_t kExpected[16] = {
    0x48, 0x8B, 0x4B, 0x28,                    // mov rcx,[rbx+28h]
    0xF3, 0x0F, 0x10, 0x43, 0x60,              // movss xmm0,[rbx+60h]
    0x0F, 0x2E, 0x81, 0x34, 0x01, 0x00, 0x00   // ucomiss xmm0,[rcx+134h]
};

#pragma pack(push, 1)
struct Ctl
{
    int32_t enabled;   // +0
    float   fov;       // +4  override value
    uint8_t* camera;   // +8  last camera object seen
    float   gameFov;   // +16 what the game asked for
};
#pragma pack(pop)

static Ctl* g_ctl = nullptr;
static float g_wantFov = 125.0f;
static bool g_installed = false;
static bool g_headCamera = false; // [xr] headCamera: turn engine camera with the head

// Gameplay must keep seeing the un-turned camera: the steering code converts
// stick input to a world direction with it (Flower.exe+0x10B30F..0x10B32B,
// found with camfind F4 - it only runs while there is input). Turning it by
// the head reversed the controls and fed back into the chase camera (spin).
// Two hardware execute breakpoints on the game thread swap the original
// matrix in for exactly those four loads and the head-turned one back after.
static const uintptr_t kSteerStartRva = 0x10B30F; // movups xmm6,[rcx+90h]
static const uintptr_t kSteerEndRva = 0x10B32B;   // lea rcx,[rsp+70h]
static const uint8_t kSteerStartBytes[7] = { 0x0F, 0x10, 0xB1, 0x90, 0x00, 0x00, 0x00 };
static const uint8_t kSteerEndBytes[5] = { 0x48, 0x8D, 0x4C, 0x24, 0x70 };

static float* g_camMatrix = nullptr;       // node+0x90 of the camera we turned
static float g_origMatrix[16], g_headMatrix[16];
static volatile LONG g_steerSwaps = 0;

static LONG WINAPI SteerBreakpointHandler(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    PCONTEXT ctx = info->ContextRecord;
    if (!(ctx->Dr6 & 0x6)) return EXCEPTION_CONTINUE_SEARCH; // DR1 / DR2
    if (g_camMatrix)
    {
        if (ctx->Dr6 & 0x2) { memcpy(g_camMatrix, g_origMatrix, 64); InterlockedIncrement(&g_steerSwaps); }
        if (ctx->Dr6 & 0x4) memcpy(g_camMatrix, g_headMatrix, 64);
    }
    ctx->Dr6 = 0;
    ctx->EFlags |= 0x10000; // RF: resume past the execute breakpoint
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI ArmSteerBreakpoints(LPVOID threadIdPtr)
{
    DWORD tid = (DWORD)(uintptr_t)threadIdPtr;
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!h) return 0;
    if (SuspendThread(h) != (DWORD)-1)
    {
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &c))
        {
            c.Dr1 = (DWORD64)(base + kSteerStartRva);
            c.Dr2 = (DWORD64)(base + kSteerEndRva);
            c.Dr7 &= ~((DWORD64)0xFF << 20);    // RW1/LEN1/RW2/LEN2 = 0: execute, 1 byte
            c.Dr7 |= (1ull << 2) | (1ull << 4); // L1, L2
            SetThreadContext(h, &c);
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    Log("[camoverride] steering breakpoints armed on thread %lu", tid);
    return 0;
}

static void EnsureSteerBreakpoints()
{
    static bool done = false;
    if (done) return;
    done = true;
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (memcmp(base + kSteerStartRva, kSteerStartBytes, sizeof(kSteerStartBytes)) != 0 ||
        memcmp(base + kSteerEndRva, kSteerEndBytes, sizeof(kSteerEndBytes)) != 0)
    {
        Log("[camoverride] steering code bytes differ (different build?); head camera would break steering");
        return;
    }
    AddVectoredExceptionHandler(1, SteerBreakpointHandler);
    // We're on the game thread; a helper thread must suspend it to set its
    // debug registers. Don't wait (it has to suspend us).
    HANDLE t = CreateThread(nullptr, 0, ArmSteerBreakpoints, (LPVOID)(uintptr_t)GetCurrentThreadId(), 0, nullptr);
    if (t) CloseHandle(t);
}

// Runs on the engine's camera update, right after it writes the render
// camera's matrix (node+0x90: x axis, y axis, z axis, position as float4s)
// and before anything renders with it. While in VR we turn/move that camera
// by the head pose, so culling, LOD and grass generation follow the head.
static void OnCameraUpdate(uint8_t* node, uint8_t* wrapper)
{
    static DWORD firstThread = 0;
    if (!firstThread)
    {
        firstThread = GetCurrentThreadId();
        Log("[camoverride] camera update runs on thread %lu", firstThread);
    }
    g_camMatrix = nullptr; // no swaps unless we turn the camera this frame
    if (!g_ctl || !g_ctl->enabled || !g_headCamera || !node) { StereoHeadNotApplied(); return; }

    float* m = (float*)(node + 0x90);
    float R[9], t[3];
    if (!StereoTakeHeadForCamera(m, R, t)) return;
    EnsureSteerBreakpoints();
    memcpy(g_origMatrix, m, 64);

    float e[3][4], pos[4];
    memcpy(e, m, sizeof(e));
    memcpy(pos, m + 12, sizeof(pos));
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k)
            m[j * 4 + k] = e[0][k] * R[0 * 3 + j] + e[1][k] * R[1 * 3 + j] + e[2][k] * R[2 * 3 + j];
    for (int k = 0; k < 3; ++k)
        m[12 + k] = pos[k] + e[0][k] * t[0] + e[1][k] * t[1] + e[2][k] * t[2];
    memcpy(g_headMatrix, m, 64);
    g_camMatrix = m;

    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 10000)
    {
        lastLog = GetTickCount();
        Log("[camoverride] head camera active; steering reads given original camera %ld times so far", g_steerSwaps);
    }
}

static void Emit(uint8_t*& p, std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; }
static void Emit64(uint8_t*& p, uint64_t v) { memcpy(p, &v, 8); p += 8; }

void* CamOverrideCameraNode() { return g_ctl ? g_ctl->camera : nullptr; }

void CamOverrideSetHeadCamera(bool on) { g_headCamera = on; }

bool CamOverrideInstall(float fovDegrees)
{
    g_wantFov = fovDegrees;
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    uint8_t* site = base + kPatchRva;
    if (memcmp(site, kExpected, sizeof(kExpected)) != 0)
    {
        Log("[camoverride] unexpected bytes at Flower.exe+0x%llX (different build?); FOV override disabled",
            (unsigned long long)kPatchRva);
        return false;
    }

    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;
    g_ctl = (Ctl*)mem;
    memset(g_ctl, 0, sizeof(Ctl));
    g_ctl->fov = fovDegrees;

    uint8_t* stub = mem + 64;
    uint8_t* p = stub;
    // Call OnCameraUpdate(node = [rbx+28h], wrapper = rbx) with all volatile
    // registers preserved. rsp is 16-aligned at the patch site (push rbx;
    // sub rsp,60h), so 7 pushes + 0x88 keeps it aligned for the call.
    Emit(p, { 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 }); // push rax,rcx,rdx,r8-r11
    Emit(p, { 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 });   // sub rsp,88h
    Emit(p, { 0x0F, 0x11, 0x44, 0x24, 0x20 });               // movups [rsp+20h],xmm0
    Emit(p, { 0x0F, 0x11, 0x4C, 0x24, 0x30 });               // movups [rsp+30h],xmm1
    Emit(p, { 0x0F, 0x11, 0x54, 0x24, 0x40 });               // movups [rsp+40h],xmm2
    Emit(p, { 0x0F, 0x11, 0x5C, 0x24, 0x50 });               // movups [rsp+50h],xmm3
    Emit(p, { 0x0F, 0x11, 0x64, 0x24, 0x60 });               // movups [rsp+60h],xmm4
    Emit(p, { 0x0F, 0x11, 0x6C, 0x24, 0x70 });               // movups [rsp+70h],xmm5
    Emit(p, { 0x48, 0x8B, 0x4B, 0x28 });                     // mov rcx,[rbx+28h]
    Emit(p, { 0x48, 0x89, 0xDA });                           // mov rdx,rbx
    Emit(p, { 0x48, 0xB8 }); Emit64(p, (uint64_t)&OnCameraUpdate); // mov rax, OnCameraUpdate
    Emit(p, { 0xFF, 0xD0 });                                 // call rax
    Emit(p, { 0x0F, 0x10, 0x44, 0x24, 0x20 });               // movups xmm0,[rsp+20h]
    Emit(p, { 0x0F, 0x10, 0x4C, 0x24, 0x30 });
    Emit(p, { 0x0F, 0x10, 0x54, 0x24, 0x40 });
    Emit(p, { 0x0F, 0x10, 0x5C, 0x24, 0x50 });
    Emit(p, { 0x0F, 0x10, 0x64, 0x24, 0x60 });
    Emit(p, { 0x0F, 0x10, 0x6C, 0x24, 0x70 });
    Emit(p, { 0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00 });   // add rsp,88h
    Emit(p, { 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58 }); // pop r11-r8,rdx,rcx,rax

    Emit(p, { 0x48, 0x8B, 0x4B, 0x28 });             // mov rcx,[rbx+28h]
    Emit(p, { 0xF3, 0x0F, 0x10, 0x43, 0x60 });       // movss xmm0,[rbx+60h]
    Emit(p, { 0x50 });                               // push rax
    Emit(p, { 0x48, 0xB8 }); Emit64(p, (uint64_t)g_ctl); // mov rax, g_ctl
    Emit(p, { 0x48, 0x89, 0x48, 0x08 });             // mov [rax+8],rcx
    Emit(p, { 0xF3, 0x0F, 0x11, 0x40, 0x10 });       // movss [rax+10h],xmm0
    Emit(p, { 0x83, 0x38, 0x00 });                   // cmp dword [rax],0
    Emit(p, { 0x74, 0x05 });                         // je +5
    Emit(p, { 0xF3, 0x0F, 0x10, 0x40, 0x04 });       // movss xmm0,[rax+4]
    Emit(p, { 0x58 });                               // pop rax
    Emit(p, { 0x0F, 0x2E, 0x81, 0x34, 0x01, 0x00, 0x00 }); // ucomiss xmm0,[rcx+134h]
    Emit(p, { 0xFF, 0x25, 0, 0, 0, 0 }); Emit64(p, (uint64_t)(site + 16)); // jmp back to the je

    DWORD old;
    VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &old);
    uint8_t patch[16];
    uint8_t* q = patch;
    Emit(q, { 0xFF, 0x25, 0, 0, 0, 0 }); Emit64(q, (uint64_t)stub); // jmp [rip+0] -> stub
    Emit(q, { 0x90, 0x90 });
    memcpy(site, patch, 16);
    VirtualProtect(site, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 16);

    g_installed = true;
    Log("[camoverride] patched Flower.exe+0x%llX -> stub %p (fov override %.1f deg)", (unsigned long long)kPatchRva, stub, fovDegrees);
    return true;
}

void CamOverrideTick(bool enable)
{
    if (!g_installed) return;
    if (KeyEdge(VK_F3))
    {
        g_headCamera = !g_headCamera;
        Log("[camoverride] F3: head camera = %d", g_headCamera);
    }
    g_ctl->enabled = enable ? 1 : 0;
    uint8_t* cam = g_ctl->camera;
    if (!cam) return;

    float* fov = (float*)(cam + 0x134);
    float* aspect = (float*)(cam + 0x140);
    float wantAspect = enable ? 1.0f : 0.0f;
    static float savedAspect = 0.0f;
    static bool wasEnabled = false;

    if (enable && !wasEnabled)
    {
        savedAspect = *aspect;
        Log("[camoverride] enabling: game fov %.2f, aspect %.4f -> fov %.1f, aspect 1", g_ctl->gameFov, savedAspect, g_ctl->fov);
    }
    if (enable && *aspect != wantAspect)
    {
        *aspect = wantAspect;
        *fov = -1.0f; // force the engine to rebuild its projection next tick
    }
    if (!enable && wasEnabled && savedAspect > 0.0f)
    {
        *aspect = savedAspect;
        *fov = -1.0f;
        Log("[camoverride] disabled: aspect restored to %.4f", savedAspect);
    }
    wasEnabled = enable;
}
