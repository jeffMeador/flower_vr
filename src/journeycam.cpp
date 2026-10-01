#include "journeycam.h"
#include "game.h"
#include "log.h"
#include <Windows.h>
#include <MinHook.h>
#include <cstring>

// Journey (Steam) camera object, found with the camfind watchpoints:
//   +0x10  view matrix (3 float4s)        +0x40  projection (4 float4s)
//   +0xC8  near   +0xCC  far   +0xD0  aspect
//   +0x150 perspective flag   +0x154 vertical FOV in degrees
// Flower.exe-style code sites (Journey.exe RVAs, checked byte for byte):
//   0x2D6820  camera->UpdateProjection(): builds +0x40 from +0x154 (and near/far/aspect)
//   0x3E1F50  BuildSceneCameraParams(out, &camera->view, ...): the camera the scene renders with
// UpdateProjection runs with our FOV in +0x154 for the scene camera, and the
// game's own value is put back right after, so the game logic never sees it.

static const uintptr_t kUpdateProjRva = 0x2D6820;
static const uint8_t kUpdateProjBytes[] = { 0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x80, 0xB9, 0x50,
                                            0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x74, 0x3A, 0xF3, 0x0F, 0x10 };
static const uintptr_t kSceneParamsRva = 0x3E1F50;
static const uint8_t kSceneParamsBytes[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
                                             0x89, 0x70, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xA0 };

using UpdateProj_t = void*(__fastcall*)(uint8_t* cam);
using SceneParams_t = void*(__fastcall*)(void* out, uint8_t* cam, void* a3, void* a4);
static UpdateProj_t realUpdateProj;
static SceneParams_t realSceneParams;

static uint8_t* volatile g_sceneCam = nullptr;
static volatile bool g_enabled = false;
static float g_fov = 125.0f;

static void* __fastcall Hook_UpdateProj(uint8_t* cam)
{
    if (!g_enabled || cam != g_sceneCam || !cam[0x150]) return realUpdateProj(cam);
    float* fov = (float*)(cam + 0x154);
    float game = *fov;
    *fov = g_fov;
    void* r = realUpdateProj(cam);
    *fov = game;
    return r;
}

static void* __fastcall Hook_SceneParams(void* out, uint8_t* view, void* a3, void* a4)
{
    // It's passed the camera's view matrix (object + 0x10), not the object.
    uint8_t* cam = view ? view - 0x10 : nullptr;
    if (cam != g_sceneCam)
    {
        g_sceneCam = cam;
        Log("[journeycam] scene camera %p (game fov %.1f deg)", cam, cam ? *(float*)(cam + 0x154) : 0.0f);
    }
    return realSceneParams(out, view, a3, a4);
}

static bool Matches(uintptr_t rva, const uint8_t* bytes, size_t n)
{
    const uint8_t* p = (const uint8_t*)GetModuleHandleW(nullptr) + rva;
    __try { return memcmp(p, bytes, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool JourneyCamInstall(float fovDegrees)
{
    if (wcscmp(Game().name, L"Journey") != 0) return false;
    g_fov = fovDegrees;
    if (!Matches(kUpdateProjRva, kUpdateProjBytes, sizeof(kUpdateProjBytes)) ||
        !Matches(kSceneParamsRva, kSceneParamsBytes, sizeof(kSceneParamsBytes)))
    {
        Log("[journeycam] camera code not found (different Journey build?); FOV override off");
        return false;
    }
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    MH_Initialize(); // harmless if already initialized
    bool ok = MH_CreateHook(base + kUpdateProjRva, (void*)&Hook_UpdateProj, (void**)&realUpdateProj) == MH_OK &&
              MH_EnableHook(base + kUpdateProjRva) == MH_OK &&
              MH_CreateHook(base + kSceneParamsRva, (void*)&Hook_SceneParams, (void**)&realSceneParams) == MH_OK &&
              MH_EnableHook(base + kSceneParamsRva) == MH_OK;
    Log("[journeycam] camera hooks %s (fov %.0f deg in VR)", ok ? "installed" : "FAILED", g_fov);
    return ok;
}

void JourneyCamTick(bool enable)
{
    if (enable != g_enabled) Log("[journeycam] FOV override %s", enable ? "on" : "off");
    g_enabled = enable;
}
