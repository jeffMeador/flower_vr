#include "journeycam.h"
#include "game.h"
#include "log.h"
#include "stereo.h"
#include <Windows.h>
#include <MinHook.h>
#include <cstring>
#include <cmath>

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

// Head camera: camera->UpdateView() (0x2D3410, called with &camera->view)
// builds the view from the camera's world transform, *(view + 0xB0): 3 float4s
// packed as (up.xyz, right.x) (z.xyz, right.y) (pos.xyz, right.z). While it
// runs, that transform is turned by the head pose (as Flower turns its camera
// node), so rendering and culling follow the head; the game's own transform
// is put back right after, so gameplay (walking direction, the chase camera)
// never sees the head.
static const uintptr_t kUpdateViewRva = 0x2D3410;
static const uint8_t kUpdateViewBytes[] = { 0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x0F,
                                            0x28, 0x05, 0x5E, 0xEA, 0x2E, 0x00, 0x48, 0x8B, 0xD9, 0x0F, 0x28, 0x0D };
using UpdateView_t = void*(__fastcall*)(uint8_t* view);
static UpdateView_t realUpdateView;

// Culling: each render pass sets up its camera in PassSetCamera(pass, &camera->view)
// (0x34F4D0, three times a frame, after UpdateView) from the same world
// transform, so the passes culled and picked terrain for the un-turned camera
// (gaps and popping behind/around you). It gets this frame's turned transform
// for the call too, restored right after; leaving it in place instead made
// the game's camera logic build on the head (it drifted).
static const uintptr_t kPassSetupRva = 0x34F4D0;
static const uint8_t kPassSetupBytes[] = { 0x40, 0x53, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x82,
                                           0xB0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x48, 0x89, 0x91, 0xB0, 0x00 };
using PassSetup_t = void*(__fastcall*)(void* pass, uint8_t* view);
static PassSetup_t realPassSetup;
static float* g_frameWorld = nullptr;            // the transform turned this frame (null: none)
static float g_frameOrig[12], g_frameTurned[12]; // the game's values and ours
static bool g_headCamera = false;
static float g_fakeYaw = 0.0f;  // [debug] fakeHeadYaw (degrees): a fixed head turn for desktop tests
static bool g_noPassHook = false; // [debug] noPassHook=1: leave pass setup alone (A/B test)

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

static void* __fastcall Hook_UpdateView(uint8_t* view)
{
    uint8_t* cam = view ? view - 0x10 : nullptr;
    float* w = view ? *(float**)(view + 0xB0) : nullptr;
    if (!g_enabled || !g_headCamera || cam != g_sceneCam || !w) return realUpdateView(view);

    // Unpack to Flower's layout: rows right, up, z, position.
    float m[16] = { w[3], w[7], w[11], 0,  w[0], w[1], w[2], 0,  w[4], w[5], w[6], 0,  w[8], w[9], w[10], 1 };
    float R[9], t[3];
    if (!StereoTakeHeadForCamera(m, R, t)) return realUpdateView(view);
    float e[3][3], pos[3];
    for (int i = 0; i < 3; ++i) { for (int k = 0; k < 3; ++k) e[i][k] = m[i * 4 + k]; pos[i] = m[12 + i]; }
    float n[3][3], np[3];
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k)
            n[j][k] = e[0][k] * R[0 * 3 + j] + e[1][k] * R[1 * 3 + j] + e[2][k] * R[2 * 3 + j];
    for (int k = 0; k < 3; ++k) np[k] = pos[k] + e[0][k] * t[0] + e[1][k] * t[1] + e[2][k] * t[2];

    float saved[12];
    memcpy(saved, w, sizeof(saved));
    const float packed[12] = { n[1][0], n[1][1], n[1][2], n[0][0],  n[2][0], n[2][1], n[2][2], n[0][1],  np[0], np[1], np[2], n[0][2] };
    memcpy(w, packed, sizeof(packed));
    void* r = realUpdateView(view);
    memcpy(w, saved, sizeof(saved));
    memcpy(g_frameOrig, saved, sizeof(saved));
    memcpy(g_frameTurned, packed, sizeof(packed));
    g_frameWorld = w;

    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 10000) { lastLog = GetTickCount(); Log("[journeycam] head camera active"); }
    return r;
}

static void* __fastcall Hook_PassSetup(void* pass, uint8_t* view)
{
    float* w = view ? *(float**)(view + 0xB0) : nullptr;
    if (!g_enabled || !w || w != g_frameWorld || memcmp(w, g_frameOrig, sizeof(g_frameOrig)) != 0)
        return realPassSetup(pass, view);
    memcpy(w, g_frameTurned, sizeof(g_frameTurned));
    void* r = realPassSetup(pass, view);
    memcpy(w, g_frameOrig, sizeof(g_frameOrig));
    return r;
}

static bool Matches(uintptr_t rva, const uint8_t* bytes, size_t n)
{
    const uint8_t* p = (const uint8_t*)GetModuleHandleW(nullptr) + rva;
    __try { return memcmp(p, bytes, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool JourneyCamInstall(float fovDegrees, bool headCamera)
{
    if (wcscmp(Game().name, L"Journey") != 0) return false;
    g_fov = fovDegrees;
    g_headCamera = headCamera;
    {
        extern wchar_t g_dllDir[MAX_PATH];
        wchar_t ini[MAX_PATH], buf[32];
        swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
        GetPrivateProfileStringW(L"debug", L"fakeHeadYaw", L"0", buf, 32, ini);
        g_fakeYaw = (float)_wtof(buf);
        g_noPassHook = GetPrivateProfileIntW(L"debug", L"noPassHook", 0, ini) != 0;
        if (g_fakeYaw != 0.0f) Log("[journeycam] DEBUG fake head yaw %.0f deg", g_fakeYaw);
    }
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
    if (ok && g_headCamera)
    {
        bool hv = Matches(kUpdateViewRva, kUpdateViewBytes, sizeof(kUpdateViewBytes)) &&
                  MH_CreateHook(base + kUpdateViewRva, (void*)&Hook_UpdateView, (void**)&realUpdateView) == MH_OK &&
                  MH_EnableHook(base + kUpdateViewRva) == MH_OK;
        Log("[journeycam] head camera hook %s", hv ? "installed" : "not installed (code not found)");
        bool hp = hv && !g_noPassHook && Matches(kPassSetupRva, kPassSetupBytes, sizeof(kPassSetupBytes)) &&
                  MH_CreateHook(base + kPassSetupRva, (void*)&Hook_PassSetup, (void**)&realPassSetup) == MH_OK &&
                  MH_EnableHook(base + kPassSetupRva) == MH_OK;
        Log("[journeycam] culling follows the head: %s", hp ? "yes (pass setup hooked)" : "no (code not found)");
    }
    return ok;
}

void JourneyCamTick(bool enable)
{
    g_frameWorld = nullptr; // Present: this frame's turned transform is used up
    if (g_fakeYaw != 0.0f && enable && !StereoHasEyePoses())
    {
        const float a = g_fakeYaw * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
        const float rot[9] = { c, 0, s,  0, 1, 0,  -s, 0, c }, pos[3] = { 0, 0, 0 };
        StereoSetHeadPose(rot, pos);
    }
    if (enable != g_enabled) Log("[journeycam] FOV override %s", enable ? "on" : "off");
    if (!enable && g_enabled && g_headCamera) StereoHeadNotApplied(); // the shaders take the head again
    g_enabled = enable;
}
