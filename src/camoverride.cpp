#include "camoverride.h"
#include "log.h"
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

static void Emit(uint8_t*& p, std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; }
static void Emit64(uint8_t*& p, uint64_t v) { memcpy(p, &v, 8); p += 8; }

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
