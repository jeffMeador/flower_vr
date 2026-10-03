#include "journeycam.h"
#include "game.h"
#include "log.h"
#include "stereo.h"
#include "capture.h"
#include <Windows.h>
#include <MinHook.h>
#include <cstring>
#include <cmath>
#include <cstdio>

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
// The camera's local transform (camera + 0x170: rows right, up, z, position;
// world = local here, the camera has no parent) and its FOV (+0x154) are what
// many render-side systems build their culling frustums from (terrain, grass,
// particles: 0x10EDD0, 0x115690, 0xE3710, ...), after UpdateView. They get the
// head-turned camera and our FOV from UpdateView until Present; the game
// rewrites both from its own camera state at the start of the next frame, and
// its one earlier read of them (0xDB582) comes after Present, so it never sees ours.
static uint8_t* g_localCam = nullptr;
static float g_localOrig[16], g_localOurs[16], g_fovOrig = 0.0f;
static float* g_frameWorld = nullptr;            // the transform turned this frame (null: none)
static float g_frameOrig[12], g_frameTurned[12]; // the game's values and ours
static bool g_headCamera = false;
static float g_fakeYaw = 0.0f;  // [debug] fakeHeadYaw (degrees): a fixed head turn for desktop tests
static bool g_fakeYawOn = false;
static float g_steadyTau = 3.0f;  // [xr] cameraSteady: seconds to smooth the camera's distance to the player (0: off)
static bool g_localTurn = true;   // [debug] localTurn=0: leave the camera's local transform and FOV alone (A/B test)
static bool g_noPassHook = false; // [debug] noPassHook=1: leave pass setup alone (A/B test)

using UpdateProj_t = void*(__fastcall*)(uint8_t* cam);
using SceneParams_t = void*(__fastcall*)(void* out, uint8_t* cam, void* a3, void* a4);
static UpdateProj_t realUpdateProj;
static SceneParams_t realSceneParams;

static uint8_t* volatile g_sceneCam = nullptr;
static volatile bool g_enabled = false;
static float g_fov = 125.0f;
static float g_camPos[3] = {}, g_camSpeed = 0.0f; // scene camera position, distance moved last frame
static volatile float g_gameFov = 0.0f; // the game's own vertical FOV for the scene camera (before ours)

static volatile float g_cinemaAspect = 0.0f; // cinema screen: widen the game's view to this aspect (0: off)

static void* __fastcall Hook_UpdateProj(uint8_t* cam)
{
    if (!g_enabled && g_cinemaAspect > 1.0f && cam == g_sceneCam && cam[0x150])
    {
        // Cinema screen: the square frame gets the game's vertical FOV in its
        // middle band and the wider horizontal one of a widescreen view across.
        float* fov = (float*)(cam + 0x154);
        const float game = *fov;
        g_gameFov = game;
        if (game > 1.0f && game < 120.0f)
            *fov = 2.0f * atanf(tanf(game * 0.5f * 0.0174533f) * g_cinemaAspect) * 57.29578f;
        void* r = realUpdateProj(cam);
        *fov = game;
        return r;
    }
    if (!g_enabled || cam != g_sceneCam || !cam[0x150]) return realUpdateProj(cam);
    float* fov = (float*)(cam + 0x154);
    const bool held = cam == g_localCam; // our FOV is already in place for the rest of this frame
    float game = held ? g_gameFov : *fov;
    g_gameFov = game;
    *fov = g_fov;
    void* r = realUpdateProj(cam);
    if (!held) *fov = game;
    return r;
}

static void* __fastcall Hook_SceneParams(void* out, uint8_t* view, void* a3, void* a4)
{
    // It's passed the camera's view matrix (object + 0x10), not the object.
    uint8_t* cam = view ? view - 0x10 : nullptr;
    if (cam != g_sceneCam)
    {
        g_sceneCam = cam;
        Log("[journeycam] scene camera %p (game fov %.1f deg), world transform %p", cam, cam ? *(float*)(cam + 0x154) : 0.0f,
            cam ? *(void**)(cam + 0xC0) : nullptr);
    }
    // It also copies the camera's world transform (*(view + 0xB0)) into the
    // scene setup, which picks the terrain to draw: give it this frame's turned
    // transform too, or terrain beyond the game's own view went missing and
    // flickered when looking around (big gaps in fast flights).
    float* w = view ? *(float**)(view + 0xB0) : nullptr;
    if (g_noPassHook || !g_enabled || !w || w != g_frameWorld || memcmp(w, g_frameOrig, sizeof(g_frameOrig)) != 0)
        return realSceneParams(out, view, a3, a4);
    memcpy(w, g_frameTurned, sizeof(g_frameTurned));
    void* r = realSceneParams(out, view, a3, a4);
    memcpy(w, g_frameOrig, sizeof(g_frameOrig));
    return r;
}

static void* __fastcall Hook_UpdateView(uint8_t* view)
{
    uint8_t* cam = view ? view - 0x10 : nullptr;
    float* w = view ? *(float**)(view + 0xB0) : nullptr;
    if (cam == g_sceneCam && w) { g_camPos[0] = w[8]; g_camPos[1] = w[9]; g_camPos[2] = w[10]; }
    if (!g_enabled || !g_headCamera || cam != g_sceneCam || !w) return realUpdateView(view);

    // Unpack to Flower's layout: rows right, up, z, position.
    float m[16] = { w[3], w[7], w[11], 0,  w[0], w[1], w[2], 0,  w[4], w[5], w[6], 0,  w[8], w[9], w[10], 1 };

    // Steady distance: Journey's chase camera falls behind while you walk and
    // swings in when you stop, then drifts back out - on a monitor it's
    // subtle, in VR it's you being pulled back and forth. The distance to the
    // player is smoothed over g_steadyTau seconds (direction untouched).
    {
        static float smooth = 0.0f;
        static DWORD last = 0;
        const DWORD now = GetTickCount();
        const float dt = last ? (now - last) * 0.001f : 0.0f;
        last = now;
        float c[3];
        if (g_steadyTau > 0.05f && CapturePlayerPos(c))
        {
            const float v[3] = { m[12] - c[0], m[13] - c[1], m[14] - c[2] };
            const float d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (d > 0.01f)
            {
                if (smooth <= 0.0f || dt > 0.5f || fabsf(d - smooth) > 0.5f * smooth) smooth = d; // cut or jump
                else smooth += (d - smooth) * (1.0f - expf(-dt / g_steadyTau));
                const float k = smooth / d;
                for (int i = 0; i < 3; ++i) m[12 + i] = c[i] + v[i] * k;
            }
        }
        else smooth = 0.0f;
    }
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

    float* L = (float*)(cam + 0x170);
    const float worldAsLocal[16] = { saved[3], saved[7], saved[11], L[3],  saved[0], saved[1], saved[2], L[7],
                                     saved[4], saved[5], saved[6], L[11],  saved[8], saved[9], saved[10], L[15] };
    bool sameAsWorld = true;
    for (int i = 0; i < 16; ++i) if (fabsf(L[i] - worldAsLocal[i]) > 1e-3f) sameAsWorld = false;
    if (g_localTurn && !g_localCam && sameAsWorld)
    {
        memcpy(g_localOrig, L, sizeof(g_localOrig));
        g_fovOrig = *(float*)(cam + 0x154);
        const float ours[16] = { n[0][0], n[0][1], n[0][2], L[3],  n[1][0], n[1][1], n[1][2], L[7],
                                 n[2][0], n[2][1], n[2][2], L[11],  np[0], np[1], np[2], L[15] };
        memcpy(g_localOurs, ours, sizeof(ours));
        memcpy(L, ours, sizeof(ours));
        *(float*)(cam + 0x154) = g_fov;
        g_localCam = cam;
    }
    else if (g_localTurn && !sameAsWorld)
    {
        static bool logged = false;
        if (!logged) { logged = true; Log("[journeycam] camera has a parent transform; its local copy is left alone"); }
    }

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
        g_localTurn = GetPrivateProfileIntW(L"debug", L"localTurn", 1, ini) != 0;
        GetPrivateProfileStringW(L"xr", L"cameraSteady", L"3.0", buf, 32, ini);
        g_steadyTau = (float)_wtof(buf);
        g_fakeYawOn = g_fakeYaw != 0.0f;
        if (g_fakeYawOn) Log("[journeycam] DEBUG fake head yaw %.0f deg", g_fakeYaw);
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

float JourneyCamGameFov() { return g_gameFov; }
float JourneyCamSpeed() { return g_camSpeed; }
void JourneyCamSetCinema(float aspect) { g_cinemaAspect = aspect; }

void JourneyCamTick(bool enable)
{
    g_frameWorld = nullptr; // Present: this frame's turned transform is used up
    if (g_localCam)
    {
        float* L = (float*)(g_localCam + 0x170);
        if (memcmp(L, g_localOurs, sizeof(g_localOurs)) == 0) memcpy(L, g_localOrig, sizeof(g_localOrig));
        float* fov = (float*)(g_localCam + 0x154);
        if (*fov == g_fov) *fov = g_fovOrig;
        g_localCam = nullptr;
    }
    {
        static float last[3] = {};
        const float d[3] = { g_camPos[0] - last[0], g_camPos[1] - last[1], g_camPos[2] - last[2] };
        g_camSpeed = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        memcpy(last, g_camPos, sizeof(last));
    }
    // Desktop tests: a file vrmod_fov holding "<fov> [<fake head yaw>]" sets them live (read and deleted).
    static int tick = 0;
    if (++tick % 30 == 0)
    {
        extern wchar_t g_dllDir[MAX_PATH];
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\vrmod_fov", g_dllDir);
        if (FILE* f = _wfopen(path, L"r"))
        {
            float v = 0, yaw = 0;
            const int n = fscanf(f, "%f %f", &v, &yaw);
            if (n >= 1 && v > 10.0f && v < 170.0f) { g_fov = v; Log("[journeycam] DEBUG fov %.1f", v); }
            if (n == 2) { g_fakeYaw = yaw; g_fakeYawOn = true; Log("[journeycam] DEBUG fake head yaw %.1f", yaw); }
            fclose(f);
            DeleteFileW(path);
        }
    }
    if (g_fakeYawOn && enable && !StereoHasEyePoses())
    {
        const float a = g_fakeYaw * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
        const float rot[9] = { c, 0, s,  0, 1, 0,  -s, 0, c }, pos[3] = { 0, 0, 0 };
        StereoSetHeadPose(rot, pos);
    }
    if (enable != g_enabled) Log("[journeycam] FOV override %s", enable ? "on" : "off");
    if (!enable && g_enabled && g_headCamera) StereoHeadNotApplied(); // the shaders take the head again
    g_enabled = enable;
}
