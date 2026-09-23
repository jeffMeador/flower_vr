#include "stereo.h"
#include "log.h"
#include "keys.h"
#include <cstdlib>

static StereoConfig g_cfg;
static int   g_eye = -1;
static uint64_t g_frame = 0;
static uint64_t g_key = 1;
static wchar_t g_iniPath[MAX_PATH] = {};

// Game projection, recovered from the camera's ViewProj.
static bool  g_projKnown = false;
static float g_xs = 0, g_ys = 0, g_A = 0, g_B = 0;
static float g_right[3] = {}, g_up[3] = {}, g_fwd[3] = {}; // canonical axes in world space

struct EyePose { bool set = false; float rot[9]; float pos[3]; };
struct DisplayFov { bool set = false; float tanL, tanR, tanU, tanD, cropX, cropY; };
static EyePose g_pendingPose[2], g_activePose;
static DisplayFov g_display[2];

// Cached K = P_new * T * P_game^-1 for the current key.
static uint64_t g_kKey = 0;
static Mat4 g_K;
static float g_viewShiftX = 0; // canonical x translation of T (for modelView)
static float g_eyeWorld[3] = {};

StereoConfig& Stereo() { return g_cfg; }

static float ReadIniFloat(const wchar_t* key, float def)
{
    wchar_t buf[64], defStr[64];
    swprintf_s(defStr, L"%g", def);
    GetPrivateProfileStringW(L"stereo", key, defStr, buf, 64, g_iniPath);
    return (float)_wtof(buf);
}

void StereoLoadConfig(const wchar_t* dllDir)
{
    swprintf_s(g_iniPath, L"%s\\vrmod.ini", dllDir);
    g_cfg.enabled = ReadIniFloat(L"enabled", 1.0f) != 0.0f;
    g_cfg.separation = ReadIniFloat(L"separation", g_cfg.separation);
    g_cfg.worldScale = ReadIniFloat(L"worldScale", g_cfg.worldScale);
    g_cfg.shiftEyePosition = ReadIniFloat(L"shiftEyePosition", 0.0f) != 0.0f;
    wchar_t lens[32];
    GetPrivateProfileStringW(L"stereo", L"lens", L"game", lens, 32, g_iniPath);
    g_cfg.lensMode = _wcsicmp(lens, L"fixed") == 0 ? 1 : _wcsicmp(lens, L"off") == 0 ? 2 : 0;
    Log("[stereo] config: enabled=%d separation=%.4f worldScale=%.3f shiftEyePosition=%d lens=%d",
        g_cfg.enabled, g_cfg.separation, g_cfg.worldScale, g_cfg.shiftEyePosition, g_cfg.lensMode);
}

static void SaveIni(const wchar_t* key, const wchar_t* value)
{
    WritePrivateProfileStringW(L"stereo", key, value, g_iniPath);
}

static void SaveScale()
{
    wchar_t v[32];
    swprintf_s(v, L"%.4f", g_cfg.worldScale); SaveIni(L"worldScale", v);
    swprintf_s(v, L"%.4f", g_cfg.separation); SaveIni(L"separation", v);
}

static bool KeyPressed(int vk)
{
    return KeyEdge(vk);
}

static int EyeIndex()
{
    return (g_eye > 0 && g_cfg.enabled) ? 1 : 0;
}

void StereoFrameBoundary()
{
    ++g_frame;
    ++g_key;
    g_eye = -g_eye;
    g_activePose = g_pendingPose[EyeIndex()];

    if (KeyPressed(VK_F9))
    {
        g_cfg.enabled = !g_cfg.enabled;
        Log("[stereo] F9: enabled=%d", g_cfg.enabled);
    }
    if (KeyPressed(VK_F10))
    {
        g_cfg.separation /= 1.25f;
        g_cfg.worldScale /= 1.25f;
        Log("[stereo] F10: separation=%.4f worldScale=%.3f", g_cfg.separation, g_cfg.worldScale);
        SaveScale();
    }
    if (KeyPressed(VK_F11))
    {
        g_cfg.separation *= 1.25f;
        g_cfg.worldScale *= 1.25f;
        Log("[stereo] F11: separation=%.4f worldScale=%.3f", g_cfg.separation, g_cfg.worldScale);
        SaveScale();
    }
    if (KeyPressed(VK_F2))
    {
        static const wchar_t* names[3] = { L"game", L"fixed", L"off" };
        g_cfg.lensMode = (g_cfg.lensMode + 1) % 3;
        g_cfg.lensLockPending = g_cfg.lensMode == 1;
        SaveIni(L"lens", names[g_cfg.lensMode]);
        Log("[stereo] F2: lens mode %ls", names[g_cfg.lensMode]);
    }
    if (KeyPressed(VK_F8))
    {
        g_cfg.shiftEyePosition = !g_cfg.shiftEyePosition;
        Log("[stereo] F8: shiftEyePosition=%d", g_cfg.shiftEyePosition);
    }
}

int StereoCurrentEye()
{
    return (g_cfg.enabled && g_projKnown) ? g_eye : 0;
}

static float Len3(float a, float b, float c) { return sqrtf(a * a + b * b + c * c); }

void StereoObserveViewProj(const Mat4& vp)
{
    // VP = P * V with V rigid: row 0 = xs * right, row 1 = ys * up,
    // row 3 = forward (unit), row 2 = A * forward + (0,0,0,B).
    float c0 = Len3(vp.m[0][0], vp.m[0][1], vp.m[0][2]);
    float c1 = Len3(vp.m[1][0], vp.m[1][1], vp.m[1][2]);
    float c3 = Len3(vp.m[3][0], vp.m[3][1], vp.m[3][2]);
    if (c0 < 1e-4f || c1 < 1e-4f || c3 < 1e-4f)
        return; // not a perspective camera (UI/ortho)

    float A = (vp.m[2][0] * vp.m[3][0] + vp.m[2][1] * vp.m[3][1] + vp.m[2][2] * vp.m[3][2]) / (c3 * c3);
    float B = vp.m[2][3] - A * vp.m[3][3];
    if (fabsf(B) < 1e-9f) return;

    if (c0 != g_xs || c1 != g_ys || A != g_A || B != g_B) ++g_key;
    g_xs = c0; g_ys = c1; g_A = A; g_B = B;
    for (int i = 0; i < 3; ++i)
    {
        g_right[i] = vp.m[0][i] / c0;
        g_up[i] = vp.m[1][i] / c1;
        g_fwd[i] = vp.m[3][i] / c3;
    }

    if (!g_projKnown || (g_frame % 600) == 0)
        Log("[stereo] proj from VP: xs=%.4f ys=%.4f |wrow|=%.4f A=%.5f B=%.5f right=(%.3f %.3f %.3f) up=(%.3f %.3f %.3f)",
            c0, c1, c3, A, B, g_right[0], g_right[1], g_right[2], g_up[0], g_up[1], g_up[2]);
    g_projKnown = true;
}

bool StereoProjection(float& xs, float& ys)
{
    xs = g_xs; ys = g_ys;
    return g_projKnown;
}

void StereoSetEyePose(int eyeIndex, const float rot[9], const float pos[3])
{
    EyePose& p = g_pendingPose[eyeIndex];
    p.set = true;
    memcpy(p.rot, rot, sizeof(p.rot));
    memcpy(p.pos, pos, sizeof(p.pos));
}

// Head pose: written by the XR code (render thread), consumed by the engine's
// camera update (possibly another thread).
static SRWLOCK g_headLock = SRWLOCK_INIT;
static EyePose g_pendingHead;   // latest prediction
static EyePose g_appliedHead;   // what the engine camera was actually turned by

void StereoSetHeadPose(const float rot[9], const float pos[3])
{
    AcquireSRWLockExclusive(&g_headLock);
    g_pendingHead.set = true;
    memcpy(g_pendingHead.rot, rot, sizeof(g_pendingHead.rot));
    memcpy(g_pendingHead.pos, pos, sizeof(g_pendingHead.pos));
    ReleaseSRWLockExclusive(&g_headLock);
}

void StereoHeadNotApplied()
{
    AcquireSRWLockExclusive(&g_headLock);
    if (g_appliedHead.set) { g_appliedHead.set = false; ++g_key; }
    ReleaseSRWLockExclusive(&g_headLock);
}

bool StereoTakeHeadForCamera(const float* cam, float rot[9], float pos[3])
{
    AcquireSRWLockExclusive(&g_headLock);
    EyePose h = g_pendingHead;
    g_appliedHead = h;
    ++g_key;
    ReleaseSRWLockExclusive(&g_headLock);
    if (!h.set) return false;

    // The engine camera's axes vs. what the draws show: expect x = screen
    // right, y = screen up, and z = either back (OpenXR-like) or forward.
    float dx = cam[0] * g_right[0] + cam[1] * g_right[1] + cam[2] * g_right[2];
    float dy = cam[4] * g_up[0] + cam[5] * g_up[1] + cam[6] * g_up[2];
    float dz = cam[8] * g_fwd[0] + cam[9] * g_fwd[1] + cam[10] * g_fwd[2];
    static int logged = 0;
    if (logged++ < 3)
        Log("[stereo] engine camera axes vs screen: x.right=%.3f y.up=%.3f z.fwd=%.3f", dx, dy, dz);

    // OpenXR pose is x right / y up / z back. If the engine camera's z points
    // forward, flip z (conjugate by diag(1,1,-1)). Decide this ONCE: the
    // screen axes come from the previous frame, which already includes the
    // head turn, so beyond +/-90 deg of yaw the dot product changes sign and
    // re-deciding every frame mirrored horizontal tracking.
    static int zSign = 0;
    if (zSign == 0 && fabsf(dz) > 0.9f)
    {
        zSign = dz > 0 ? -1 : 1;
        Log("[stereo] engine camera z axis points %s; locked", zSign < 0 ? "forward (flipping)" : "back (OpenXR-like)");
    }
    if (zSign == 0)
    {
        StereoHeadNotApplied(); // not decided yet: leave the camera alone this frame
        return false;
    }
    const float s[3] = { 1, 1, (float)zSign };
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j) rot[i * 3 + j] = s[i] * h.rot[i * 3 + j] * s[j];
        pos[i] = s[i] * h.pos[i] * g_cfg.worldScale;
    }
    return true;
}

void StereoSetDisplayFov(int eyeIndex, float tanL, float tanR, float tanU, float tanD, float cropFracX, float cropFracY)
{
    DisplayFov& d = g_display[eyeIndex];
    if (d.set && d.tanL == tanL && d.tanR == tanR && d.tanU == tanU && d.tanD == tanD && d.cropX == cropFracX && d.cropY == cropFracY)
        return;
    if (!d.set)
        Log("[stereo] display FOV eye %d: tan L=%.3f R=%.3f U=%.3f D=%.3f, crop %.3f x %.3f",
            eyeIndex, tanL, tanR, tanU, tanD, cropFracX, cropFracY);
    d = { true, tanL, tanR, tanU, tanD, cropFracX, cropFracY };
    ++g_key;
}

static const DisplayFov* CurrentDisplay()
{
    const DisplayFov& d = g_display[EyeIndex()];
    return d.set ? &d : nullptr;
}

uint64_t StereoPatchKey()
{
    if (!g_projKnown) return 0;
    return (StereoCurrentEye() != 0 || CurrentDisplay() || g_activePose.set) ? g_key : 0;
}

static Mat4 Identity()
{
    Mat4 m{};
    for (int i = 0; i < 4; ++i) m.m[i][i] = 1.0f;
    return m;
}

static void BuildK()
{
    // P_game^-1: canonical (x, y, d, 1) from clip (cx, cy, cz, cw).
    Mat4 pinv{};
    pinv.m[0][0] = 1.0f / g_xs;
    pinv.m[1][1] = 1.0f / g_ys;
    pinv.m[2][3] = 1.0f;
    pinv.m[3][2] = 1.0f / g_B;
    pinv.m[3][3] = -g_A / g_B;

    // T: world (game camera) canonical space -> eye canonical space.
    Mat4 t = Identity();
    float eyeCanon[3] = {};
    if (g_activePose.set)
    {
        // OpenXR poses (R, p) in x right / y up / z back; canonical flips z.
        // The rendered camera already includes the applied head pose H, so
        // the eye view relative to it is inverse(E) * H = [Re^T Rh | Re^T (ph - pe)].
        // T = S * that * S, S = diag(1, 1, -1).
        AcquireSRWLockShared(&g_headLock);
        EyePose h = g_appliedHead;
        ReleaseSRWLockShared(&g_headLock);
        static const float I3[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        const float* Rh = h.set ? h.rot : I3;
        float ph[3] = {};
        if (h.set) for (int i = 0; i < 3; ++i) ph[i] = h.pos[i] * g_cfg.worldScale;

        const float* Re = g_activePose.rot;
        float pe[3] = { g_activePose.pos[0] * g_cfg.worldScale, g_activePose.pos[1] * g_cfg.worldScale, g_activePose.pos[2] * g_cfg.worldScale };
        const float s[3] = { 1, 1, -1 };
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                float v = 0;
                for (int k = 0; k < 3; ++k) v += Re[k * 3 + i] * Rh[k * 3 + j]; // (Re^T Rh)[i][j]
                t.m[i][j] = s[i] * v * s[j];
            }
            float v = 0;
            for (int k = 0; k < 3; ++k) v += Re[k * 3 + i] * (ph[k] - pe[k]);
            t.m[i][3] = s[i] * v;
        }
        // Eye position relative to the rendered camera, in its canonical axes.
        for (int i = 0; i < 3; ++i)
        {
            float v = 0;
            for (int k = 0; k < 3; ++k) v += Rh[k * 3 + i] * (pe[k] - ph[k]);
            eyeCanon[i] = s[i] * v;
        }
    }
    else
    {
        float e = StereoCurrentEye() * 0.5f * g_cfg.separation;
        t.m[0][3] = -e;
        eyeCanon[0] = e;
    }
    g_viewShiftX = t.m[0][3];
    for (int i = 0; i < 3; ++i)
        g_eyeWorld[i] = eyeCanon[0] * g_right[i] + eyeCanon[1] * g_up[i] + eyeCanon[2] * g_fwd[i];

    // P_new: headset eye FOV into the crop, or the game's own projection.
    Mat4 pn{};
    const DisplayFov* d = CurrentDisplay();
    if (d)
    {
        pn.m[0][0] = d->cropX * 2.0f / (d->tanR - d->tanL);
        pn.m[0][2] = -d->cropX * (d->tanR + d->tanL) / (d->tanR - d->tanL);
        pn.m[1][1] = d->cropY * 2.0f / (d->tanU - d->tanD);
        pn.m[1][2] = -d->cropY * (d->tanU + d->tanD) / (d->tanU - d->tanD);
    }
    else
    {
        pn.m[0][0] = g_xs;
        pn.m[1][1] = g_ys;
    }
    pn.m[2][2] = g_A;
    pn.m[2][3] = g_B;
    pn.m[3][2] = 1.0f;

    g_K = Mat4Mul(Mat4Mul(pn, t), pinv);
    g_kKey = g_key;
    if ((g_frame % 450) == 0)
    {
        // eye forward in reference space is -R[:,2]; yaw 0 = straight ahead, + = left
        const float* R = g_activePose.rot;
        Log("[stereo] K eye=%d pose=%d yaw=%.1f pitch=%.1f pos=(%.3f %.3f %.3f) T0=(%.3f %.3f %.3f %.3f)", g_eye, g_activePose.set,
            g_activePose.set ? atan2f(R[2], R[8]) * 57.2958f : 0.0f, g_activePose.set ? asinf(-R[5]) * 57.2958f : 0.0f,
            g_activePose.pos[0], g_activePose.pos[1], g_activePose.pos[2], t.m[0][0], t.m[0][1], t.m[0][2], t.m[0][3]);
    }
}

void StereoPatchClip(float* m)
{
    if (g_kKey != g_key) BuildK();
    Mat4 in;
    memcpy(&in, m, 64);
    Mat4 out = Mat4Mul(g_K, in);
    memcpy(m, &out, 64);
}

void StereoPatchView(float* m)
{
    // modelView is in the game's own view space, whose axis signs we don't
    // know beyond x; apply only the sideways part of the eye offset.
    if (g_kKey != g_key) BuildK();
    m[3] += g_viewShiftX; // [0][3]
}

bool StereoWorldEyeOffset(float out[3])
{
    if (!g_cfg.shiftEyePosition || StereoPatchKey() == 0) return false;
    if (g_kKey != g_key) BuildK();
    memcpy(out, g_eyeWorld, sizeof(g_eyeWorld));
    return true;
}
