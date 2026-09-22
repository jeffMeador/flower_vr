#include "stereo.h"
#include "log.h"
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
    Log("[stereo] config: enabled=%d separation=%.4f worldScale=%.3f shiftEyePosition=%d",
        g_cfg.enabled, g_cfg.separation, g_cfg.worldScale, g_cfg.shiftEyePosition);
}

static bool KeyPressed(int vk)
{
    return (GetAsyncKeyState(vk) & 1) != 0; // "pressed since last query"
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
    }
    if (KeyPressed(VK_F11))
    {
        g_cfg.separation *= 1.25f;
        g_cfg.worldScale *= 1.25f;
        Log("[stereo] F11: separation=%.4f worldScale=%.3f", g_cfg.separation, g_cfg.worldScale);
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
        // OpenXR eye pose (R, p) in x right / y up / z back; canonical flips z.
        // View = [R^T | -R^T p]; T = S * View * S, S = diag(1, 1, -1).
        const float* R = g_activePose.rot;
        float p[3] = { g_activePose.pos[0] * g_cfg.worldScale, g_activePose.pos[1] * g_cfg.worldScale, g_activePose.pos[2] * g_cfg.worldScale };
        const float s[3] = { 1, 1, -1 };
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                t.m[i][j] = s[i] * R[j * 3 + i] * s[j];   // S R^T S
        for (int i = 0; i < 3; ++i)
        {
            float v = 0;
            for (int j = 0; j < 3; ++j) v -= R[j * 3 + i] * p[j]; // -R^T p
            t.m[i][3] = s[i] * v;
            eyeCanon[i] = s[i] * p[i];
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
