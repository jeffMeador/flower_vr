#include "stereo.h"
#include "log.h"
#include <cstdlib>

static StereoConfig g_cfg;
static int   g_eye = -1;
static bool  g_projKnown = false;
static float g_xs = 0.0f;          // Proj[0][0]
static float g_ys = 0.0f;          // Proj[1][1]
static float g_right[3] = {};      // camera right vector in world space
static uint64_t g_frame = 0;
static uint64_t g_key = 1;

struct DisplayFov { bool set = false; float tanL, tanR, tanU, tanD, cropX, cropY; };
static DisplayFov g_display[2];
static wchar_t g_iniPath[MAX_PATH] = {};

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
    g_cfg.shiftEyePosition = ReadIniFloat(L"shiftEyePosition", 0.0f) != 0.0f;
    Log("[stereo] config: enabled=%d separation=%.4f shiftEyePosition=%d",
        g_cfg.enabled, g_cfg.separation, g_cfg.shiftEyePosition);
}

static bool KeyPressed(int vk)
{
    return (GetAsyncKeyState(vk) & 1) != 0; // "pressed since last query"
}

void StereoFrameBoundary()
{
    ++g_frame;
    ++g_key;
    g_eye = -g_eye;

    if (KeyPressed(VK_F9))
    {
        g_cfg.enabled = !g_cfg.enabled;
        Log("[stereo] F9: enabled=%d", g_cfg.enabled);
    }
    if (KeyPressed(VK_F10))
    {
        g_cfg.separation /= 1.25f;
        Log("[stereo] F10: separation=%.4f", g_cfg.separation);
    }
    if (KeyPressed(VK_F11))
    {
        g_cfg.separation *= 1.25f;
        Log("[stereo] F11: separation=%.4f", g_cfg.separation);
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

void StereoObserveViewProj(const Mat4& vp)
{
    // VP = P * V with V rigid, so each VP row's upper-3 part is the matching
    // P row combined with V's (orthonormal) rotation rows: row 0 = xs * right,
    // row 3 = +/-forward (length 1 for a standard projection).
    float c0 = sqrtf(vp.m[0][0] * vp.m[0][0] + vp.m[0][1] * vp.m[0][1] + vp.m[0][2] * vp.m[0][2]);
    float c1 = sqrtf(vp.m[1][0] * vp.m[1][0] + vp.m[1][1] * vp.m[1][1] + vp.m[1][2] * vp.m[1][2]);
    float c3 = sqrtf(vp.m[3][0] * vp.m[3][0] + vp.m[3][1] * vp.m[3][1] + vp.m[3][2] * vp.m[3][2]);
    if (c0 < 1e-4f || c3 < 1e-4f)
        return; // not a perspective camera (UI/ortho)
    if (c0 != g_xs || c1 != g_ys) ++g_key;
    g_xs = c0;
    g_ys = c1;
    g_right[0] = vp.m[0][0] / c0;
    g_right[1] = vp.m[0][1] / c0;
    g_right[2] = vp.m[0][2] / c0;

    if (!g_projKnown || (g_frame % 600) == 0)
        Log("[stereo] proj from VP: xs=%.4f ys=%.4f |wcol|=%.4f ys/xs=%.4f right=(%.3f %.3f %.3f)",
            c0, c1, c3, c1 / c0, g_right[0], g_right[1], g_right[2]);
    g_projKnown = true;
}

// Eye camera offset along view X: left eye at -sep/2, right eye at +sep/2.
static float EyeOffset()
{
    int eye = StereoCurrentEye();
    return eye * 0.5f * g_cfg.separation;
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
    if (!g_projKnown) return nullptr;
    const DisplayFov& d = g_display[g_eye > 0 && g_cfg.enabled ? 1 : 0];
    return d.set ? &d : nullptr;
}

uint64_t StereoPatchKey()
{
    return (StereoCurrentEye() != 0 || CurrentDisplay()) ? g_key : 0;
}

void StereoPatchClip(float* m)
{
    // Eye shift: points move opposite to the camera in view space.
    m[3] += -EyeOffset() * g_xs; // [0][3]

    const DisplayFov* d = CurrentDisplay();
    if (!d) return;
    // Old NDC x = xs * tan, so tan = ndc/xs. Wanted: tan range [L,R] maps to
    // the crop's NDC range [-cropX, cropX] (same for y), i.e.
    //   x' = ax * x + bx * w,  y' = ay * y + by * w   (rows 0/1 of the matrix)
    float ax = d->cropX * 2.0f / ((d->tanR - d->tanL) * g_xs);
    float bx = -d->cropX * (d->tanR + d->tanL) / (d->tanR - d->tanL);
    float ay = d->cropY * 2.0f / ((d->tanU - d->tanD) * g_ys);
    float by = -d->cropY * (d->tanU + d->tanD) / (d->tanU - d->tanD);
    for (int c = 0; c < 4; ++c)
    {
        m[0 + c] = ax * m[0 + c] + bx * m[12 + c];
        m[4 + c] = ay * m[4 + c] + by * m[12 + c];
    }
}

void StereoPatchView(float* m)
{
    m[3] += -EyeOffset(); // [0][3]
}
bool StereoProjection(float& xs, float& ys)
{
    xs = g_xs; ys = g_ys;
    return g_projKnown;
}

bool StereoWorldEyeOffset(float out[3])
{
    if (!g_cfg.shiftEyePosition || StereoCurrentEye() == 0) return false;
    float e = EyeOffset();
    out[0] = g_right[0] * e;
    out[1] = g_right[1] * e;
    out[2] = g_right[2] * e;
    return true;
}
