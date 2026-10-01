#include "camoverride.h"
#include "log.h"
#include "keys.h"
#include "stereo.h"
#include "terrain.h"
#include <Windows.h>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <vector>

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
static bool g_terrainClamp = false; // [xr] terrainClamp: keep the viewpoint above ground (experimental)

// Gameplay must keep seeing the un-turned camera: the steering code converts
// stick input to a world direction with it (GOG Flower.exe+0x10B30F..0x10B32B,
// found with camfind F4 - it only runs while there is input). Turning it by
// the head reversed the controls and fed back into the chase camera (spin).
// Those four loads (28 bytes) are patched to jump to a stub that loads the
// un-turned matrix instead whenever [rcx+90h] is the camera we turned. (This
// used to be two hardware execute breakpoints; code patches also work where
// debug registers don't, e.g. x86 emulation on the Steam Frame.)
// Patch sites are found by byte pattern (-1 = any byte), so one DLL works on
// the GOG and Steam builds (different compiles, different addresses).
struct SteerSignature
{
    const char* build;
    std::vector<int> bytes;
    int startOffset; // first of the four matrix loads (28 bytes, replaced)
    std::vector<uint8_t> loads; // the same four loads re-encoded with [rax] as the matrix (run by the stub)
};
static const std::vector<SteerSignature> kSteerSigs = {
    // GOG: movups xmm6,[rcx+90h] / xmm3,[rcx+A0h] / xmm5,[rcx+B0h] / xmm7,[rcx+C0h]; lea rcx,[rsp+70h]
    { "GOG", { 0x0F, 0x10, 0xB1, 0x90, 0x00, 0x00, 0x00, 0x0F, 0x10, 0x99, 0xA0, 0x00, 0x00, 0x00,
               0x0F, 0x10, 0xA9, 0xB0, 0x00, 0x00, 0x00, 0x0F, 0x10, 0xB9, 0xC0, 0x00, 0x00, 0x00,
               0x48, 0x8D, 0x4C, 0x24, 0x70 }, 0,
      { 0x0F, 0x10, 0x30,          // movups xmm6,[rax]
        0x0F, 0x10, 0x58, 0x10,    // movups xmm3,[rax+10h]
        0x0F, 0x10, 0x68, 0x20,    // movups xmm5,[rax+20h]
        0x0F, 0x10, 0x78, 0x30 } },// movups xmm7,[rax+30h]
    // Steam: the loads are folded into the multiplies (same function, Flower.exe+0xE5E4F):
    // mulps xmm0,[rcx+90h] / xmm1,[rcx+C0h] / xmm3,[rcx+B0h] / xmm2,[rcx+A0h]; lea rcx,[rsp+70h]
    { "Steam", { 0x0F, 0x59, 0x81, 0x90, 0x00, 0x00, 0x00, 0x0F, 0x59, 0x89, 0xC0, 0x00, 0x00, 0x00,
                 0x0F, 0x59, 0x99, 0xB0, 0x00, 0x00, 0x00, 0x0F, 0x59, 0x91, 0xA0, 0x00, 0x00, 0x00,
                 0x48, 0x8D, 0x4C, 0x24, 0x70 }, 0,
      { 0x0F, 0x59, 0x00,          // mulps xmm0,[rax]
        0x0F, 0x59, 0x48, 0x30,    // mulps xmm1,[rax+30h]
        0x0F, 0x59, 0x58, 0x20,    // mulps xmm3,[rax+20h]
        0x0F, 0x59, 0x50, 0x10 } },// mulps xmm2,[rax+10h]
};
static const int kSteerLoadsSize = 28;
static uint8_t* g_steerStart = nullptr;

// Unique match of a pattern in the exe's code, or null (logs the match count).
static uint8_t* FindInExe(const std::vector<int>& pat, const char* what)
{
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* found = nullptr;
    int count = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec)
    {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        for (size_t i = 0; i + pat.size() <= n; ++i)
        {
            size_t j = 0;
            for (; j < pat.size(); ++j) if (pat[j] >= 0 && p[i + j] != (uint8_t)pat[j]) break;
            if (j == pat.size()) { if (!found) found = p + i; ++count; }
        }
    }
    if (count != 1)
        Log("[camoverride] %s: pattern matched %d times (need exactly 1)", what, count);
    return count == 1 ? found : nullptr;
}

static float* volatile g_camMatrix = nullptr; // node+0x90 of the camera we turned (read by the steering stub)
alignas(16) static float g_origMatrix[16];    // its un-turned matrix this frame (aligned: Steam's mulps reads it)
static volatile LONG g_steerSwaps = 0;

static void Emit(uint8_t*& p, std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; }
static void Emit64(uint8_t*& p, uint64_t v) { memcpy(p, &v, 8); p += 8; }

// Replaces the steering code's four matrix loads with a jump to a stub.
static bool PatchSteering(uint8_t* site, uint8_t* stub, const std::vector<uint8_t>& loads)
{
    uint8_t* p = stub;
    Emit(p, { 0x9C, 0x50, 0x52 });                           // pushfq; push rax; push rdx
    Emit(p, { 0x48, 0x8D, 0x81, 0x90, 0x00, 0x00, 0x00 });   // lea rax,[rcx+90h]
    Emit(p, { 0x48, 0xBA }); Emit64(p, (uint64_t)&g_camMatrix); // mov rdx,&g_camMatrix
    Emit(p, { 0x48, 0x3B, 0x02 });                           // cmp rax,[rdx]
    Emit(p, { 0x75, 0x17 });                                 // jne loads (+23)
    Emit(p, { 0x48, 0xB8 }); Emit64(p, (uint64_t)g_origMatrix); // mov rax,g_origMatrix
    Emit(p, { 0x48, 0xBA }); Emit64(p, (uint64_t)&g_steerSwaps); // mov rdx,&g_steerSwaps
    Emit(p, { 0xF0, 0xFF, 0x02 });                           // lock inc dword [rdx]
    // loads:
    for (uint8_t b : loads) *p++ = b;                        // the build's four loads, from [rax]
    Emit(p, { 0x5A, 0x58, 0x9D });                           // pop rdx; pop rax; popfq
    Emit(p, { 0xFF, 0x25, 0, 0, 0, 0 }); Emit64(p, (uint64_t)(site + kSteerLoadsSize)); // jmp back

    DWORD old;
    if (!VirtualProtect(site, kSteerLoadsSize, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t patch[kSteerLoadsSize];
    uint8_t* q = patch;
    Emit(q, { 0xFF, 0x25, 0, 0, 0, 0 }); Emit64(q, (uint64_t)stub); // jmp [rip+0] -> stub
    memset(q, 0x90, patch + kSteerLoadsSize - q);
    memcpy(site, patch, kSteerLoadsSize);
    VirtualProtect(site, kSteerLoadsSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, kSteerLoadsSize);
    return true;
}

// Find and patch this build's steering code (at startup, so it's logged even without a headset).
static void LocateSteering(uint8_t* stub)
{
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    for (const SteerSignature& sig : kSteerSigs)
    {
        if (uint8_t* p = FindInExe(sig.bytes, sig.build))
        {
            if (!PatchSteering(p + sig.startOffset, stub, sig.loads)) break;
            g_steerStart = p + sig.startOffset;
            Log("[camoverride] steering code patched (%s build) at Flower.exe+0x%llX", sig.build,
                (unsigned long long)(g_steerStart - base));
            return;
        }
    }
    Log("[camoverride] steering code not found (unknown build); head camera stays off (it would reverse the controls)");
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
    TerrainObserveCamera(m[12], m[13], m[14]); // game camera: learns heightmap alignment
    float R[9], t[3];
    if (!g_steerStart) { StereoHeadNotApplied(); return; } // unknown build: turning the camera would reverse steering
    if (!StereoTakeHeadForCamera(m, R, t)) return;
    memcpy(g_origMatrix, m, 64);

    float e[3][4], pos[4];
    memcpy(e, m, sizeof(e));
    memcpy(pos, m + 12, sizeof(pos));
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k)
            m[j * 4 + k] = e[0][k] * R[0 * 3 + j] + e[1][k] * R[1 * 3 + j] + e[2][k] * R[2 * 3 + j];
    for (int k = 0; k < 3; ++k)
        m[12 + k] = pos[k] + e[0][k] * t[0] + e[1][k] * t[1] + e[2][k] * t[2];

    // Terrain clamp: the VR viewpoint sits behind/above the game camera (and
    // moves with the head), so it can end up inside a slope the game camera
    // itself avoids. Lift it to stay above the ground.
    // Experimental: [xr] terrainClamp=1 (off by default until verified in-headset).
    float ground;
    const float kMargin = 0.5f; // game units above the terrain surface
    if (g_terrainClamp && TerrainHeight(m[12], m[14], ground) && m[13] < ground + kMargin)
    {
        static DWORD lastLog = 0;
        if (GetTickCount() - lastLog > 2000)
        {
            lastLog = GetTickCount();
            Log("[terrain] viewpoint lifted %.2f units out of the ground", ground + kMargin - m[13]);
        }
        m[13] = ground + kMargin;
    }
    g_camMatrix = m;

    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 10000)
    {
        lastLog = GetTickCount();
        Log("[camoverride] head camera active; steering reads given original camera %ld times so far", g_steerSwaps);
    }
}

void* CamOverrideCameraNode() { return g_ctl ? g_ctl->camera : nullptr; }

void CamOverrideSetHeadCamera(bool on) { g_headCamera = on; }

void CamOverrideSetTerrainClamp(bool on) { g_terrainClamp = on; }

bool CamOverrideInstall(float fovDegrees)
{
    g_wantFov = fovDegrees;
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    uint8_t* site = FindInExe(std::vector<int>(kExpected, kExpected + sizeof(kExpected)), "camera FOV update");
    if (!site)
    {
        Log("[camoverride] camera FOV update code not found (unknown build?); FOV override disabled");
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
    LocateSteering(mem + 1024); // steering stub in the same page, after the camera stub
    if (p > mem + 1024) Log("[camoverride] BUG: camera stub overlaps the steering stub");
    Log("[camoverride] patched Flower.exe+0x%llX -> stub %p (fov override %.1f deg)", (unsigned long long)(site - base), stub, fovDegrees);
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
