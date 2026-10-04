#include "cinema.h"
#include "stereo.h"
#include "shadow.h"
#include "uisign.h"
#include "log.h"
#include "fakepad.h"
#include <Windows.h>

static bool g_on = false;
static wchar_t g_ini[MAX_PATH] = {};
static float g_dist = 2.5f, g_size = 3.0f, g_height = 0.0f;
static float g_aspect = 16.0f / 9.0f; // [xr] cinemaAspect: the screen's shape (the game's view is widened to fill it)

// The flat frame, resolved/copied out of the swap chain each frame.
static ID3D11Texture2D* g_frame = nullptr;
static ID3D11ShaderResourceView* g_frameSrv = nullptr;
static UINT g_w = 0, g_h = 0;
static DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
// Render target views on the two eye images (swap chain buffer, its twin).
static ID3D11Resource* g_rtvRes[2] = {};
static ID3D11RenderTargetView* g_rtv[2] = {};

// [xr] cinematicMode=auto (default): the screen for the title, menus, the
// intro and other cutscenes, full VR while you play. The 2D layer (menu,
// title cards) being drawn means "screen"; it stays on after that until you
// have control: a tutorial prompt appears, or you walk (left stick) with no
// 2D layer up. The thumbstick click overrides until the next automatic switch.
static bool g_auto = true;
static bool g_latched = true;     // screen until control: set at launch and by the 2D layer
static DWORD g_lastUi = 0;        // last 2D-layer draw (GetTickCount)
static DWORD g_walkSince = 0;     // left stick held since
static int g_manual = -1;         // thumbstick override: -1 none, 0 VR, 1 screen
static bool g_autoWant = true;

void CinemaNoteUiLayer() { g_lastUi = GetTickCount(); g_latched = true; }
void CinemaNotePrompt()
{
    // Not while a menu is up (its button glyphs are the same kind of image).
    if (g_latched && (!g_lastUi || GetTickCount() - g_lastUi > 1000)) { g_latched = false; Log("[cinema] prompt seen: you have control"); }
}

static DWORD g_lastTitle = 0; // a title card (the "JOURNEY" logo) was drawn

void CinemaNoteTitle() { g_lastTitle = GetTickCount(); }

void CinemaNoteCameraCut(float distToPlayer, bool settled)
{
    const DWORD now = GetTickCount();
    // The idle screen: a flight of far shots with the logo over them -> the screen.
    if (!settled && distToPlayer >= 4.0f && g_lastTitle && now - g_lastTitle < 3000)
    {
        if (!g_latched) Log("[cinema] title over a camera flight (idle screen): screen");
        CinemaNoteUiLayer();
        return;
    }
    if (settled && g_latched && distToPlayer < 4.0f && (!g_lastUi || now - g_lastUi > 1000))
    {
        g_latched = false;
        Log("[cinema] settled on the player (%.1f units away): full VR", distToPlayer);
    }
}

void CinemaInit(const wchar_t* ini)
{
    wcscpy_s(g_ini, ini);
    wchar_t mode[32], buf[32];
    GetPrivateProfileStringW(L"xr", L"cinematicMode", L"auto", mode, 32, ini);
    g_auto = _wcsicmp(mode, L"screen") != 0 && _wcsicmp(mode, L"follow") != 0;
    g_on = g_auto || _wcsicmp(mode, L"screen") == 0;
    GetPrivateProfileStringW(L"xr", L"cinemaDistance", L"2.5", buf, 32, ini); g_dist = (float)_wtof(buf);
    GetPrivateProfileStringW(L"xr", L"cinemaSize", L"3.0", buf, 32, ini); g_size = (float)_wtof(buf);
    GetPrivateProfileStringW(L"xr", L"cinemaAspect", L"1.7778", buf, 32, ini); g_aspect = (float)_wtof(buf);
    if (g_aspect < 1.0f || g_aspect > 4.0f) g_aspect = 16.0f / 9.0f;
    StereoSetMono(g_on);
    Log("[cinema] mode: %s (screen %.1f m wide, %.1f m ahead)", g_auto ? "auto" : g_on ? "screen" : "follow (full VR)", g_size, g_dist);
}

bool CinemaActive() { return g_on; }
float CinemaAspect() { return g_aspect; }

void CinemaToggle()
{
    g_on = !g_on;
    StereoSetMono(g_on);
    if (g_auto) { g_manual = g_on ? 1 : 0; Log("[cinema] switched to %s (until the next automatic switch)", g_on ? "screen" : "full VR"); return; }
    if (g_ini[0]) WritePrivateProfileStringW(L"xr", L"cinematicMode", g_on ? L"screen" : L"follow", g_ini);
    Log("[cinema] switched to %s", g_on ? "screen" : "follow (full VR)");
}

void CinemaUpdate()
{
    if (!g_auto) return;
    const DWORD now = GetTickCount();
    const bool uiUp = g_lastUi && now - g_lastUi < 1000;
    if (FakePadLeftStick() > 0.5f && !uiUp)
    {
        if (!g_walkSince) g_walkSince = now;
        if (now - g_walkSince > 500) g_latched = false;
    }
    else g_walkSince = 0;
    const bool want = uiUp || g_latched;
    if (want != g_autoWant)
    {
        g_autoWant = want;
        g_manual = -1;
        Log("[cinema] auto: %s", want ? "screen (menu, title or cutscene)" : "full VR (you have control)");
    }
    const bool on = g_manual >= 0 ? g_manual == 1 : want;
    if (on != g_on) { g_on = on; StereoSetMono(on); }
}

static ID3D11RenderTargetView* EyeRtv(ID3D11Device* dev, int eye, ID3D11Resource* res)
{
    if (g_rtvRes[eye] == res && g_rtv[eye]) return g_rtv[eye];
    if (g_rtv[eye]) { g_rtv[eye]->Release(); g_rtv[eye] = nullptr; }
    g_rtvRes[eye] = res;
    if (FAILED(dev->CreateRenderTargetView(res, nullptr, &g_rtv[eye]))) g_rtv[eye] = nullptr;
    return g_rtv[eye];
}

void CinemaCompose(IDXGISwapChain* swapChain, ID3D11DeviceContext* ctx)
{
    if (!g_on || !ctx || !StereoHasEyePoses()) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);

    if (!g_frame || g_w != d.Width || g_h != d.Height || g_fmt != d.Format)
    {
        if (g_frameSrv) { g_frameSrv->Release(); g_frameSrv = nullptr; }
        if (g_frame) { g_frame->Release(); g_frame = nullptr; }
        D3D11_TEXTURE2D_DESC fd = d;
        fd.SampleDesc = { 1, 0 };
        fd.Usage = D3D11_USAGE_DEFAULT;
        // Full mip chain: the screen shows the frame much smaller than its
        // pixels; sampling without mips made text and edges jagged.
        fd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        fd.CPUAccessFlags = 0;
        fd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        fd.MipLevels = 0;
        fd.ArraySize = 1;
        if (FAILED(dev->CreateTexture2D(&fd, nullptr, &g_frame)) || FAILED(dev->CreateShaderResourceView(g_frame, nullptr, &g_frameSrv)))
        {
            Log("[cinema] could not create the frame texture");
            if (g_frame) { g_frame->Release(); g_frame = nullptr; }
            dev->Release(); bb->Release();
            return;
        }
        g_w = d.Width; g_h = d.Height; g_fmt = d.Format;
    }

    // The flat frame (left image; in cinema mode both eyes rendered the same).
    if (d.SampleDesc.Count > 1) ctx->ResolveSubresource(g_frame, 0, bb, 0, d.Format);
    else ctx->CopySubresourceRegion(g_frame, 0, 0, 0, 0, bb, 0, nullptr);
    ctx->GenerateMips(g_frameSrv);

    ID3D11RenderTargetView* oldRtv = nullptr; ID3D11DepthStencilView* oldDsv = nullptr;
    ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    D3D11_VIEWPORT oldVp = {}; UINT nvp = 1; ctx->RSGetViewports(&nvp, &oldVp);
    ID3D11DepthStencilState* oldDs = nullptr; UINT oldRef = 0; ctx->OMGetDepthStencilState(&oldDs, &oldRef);

    D3D11_VIEWPORT vp = { 0, 0, (float)d.Width, (float)d.Height, 0, 1 };
    const float dark[4] = { 0.02f, 0.02f, 0.02f, 1.0f };
    for (int eye = 0; eye < 2; ++eye)
    {
        ID3D11Resource* target = eye == 0 ? (ID3D11Resource*)bb : ShadowOfResource(bb);
        if (!target) continue;
        if (ID3D11RenderTargetView* rtv = EyeRtv(dev, eye, target))
        {
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ctx->RSSetViewports(1, &vp);
            ctx->ClearRenderTargetView(rtv, dark);
            UiPanelDraw(ctx, eye, g_frameSrv, g_dist, g_size, g_height, g_aspect);
        }
        if (eye == 1) target->Release(); // ShadowOfResource is AddRef'd
    }

    ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
    ctx->RSSetViewports(1, &oldVp);
    ctx->OMSetDepthStencilState(oldDs, oldRef);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
    if (oldDs) oldDs->Release();
    dev->Release();
    bb->Release();
}
