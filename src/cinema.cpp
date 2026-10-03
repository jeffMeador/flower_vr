#include "cinema.h"
#include "stereo.h"
#include "shadow.h"
#include "uisign.h"
#include "log.h"
#include <Windows.h>

static bool g_on = false;
static wchar_t g_ini[MAX_PATH] = {};
static float g_dist = 2.5f, g_size = 3.0f, g_height = 0.0f;

// The flat frame, resolved/copied out of the swap chain each frame.
static ID3D11Texture2D* g_frame = nullptr;
static ID3D11ShaderResourceView* g_frameSrv = nullptr;
static UINT g_w = 0, g_h = 0;
static DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
// Render target views on the two eye images (swap chain buffer, its twin).
static ID3D11Resource* g_rtvRes[2] = {};
static ID3D11RenderTargetView* g_rtv[2] = {};

void CinemaInit(const wchar_t* ini)
{
    wcscpy_s(g_ini, ini);
    wchar_t mode[32], buf[32];
    GetPrivateProfileStringW(L"xr", L"cinematicMode", L"follow", mode, 32, ini);
    g_on = _wcsicmp(mode, L"screen") == 0;
    GetPrivateProfileStringW(L"xr", L"cinemaDistance", L"2.5", buf, 32, ini); g_dist = (float)_wtof(buf);
    GetPrivateProfileStringW(L"xr", L"cinemaSize", L"3.0", buf, 32, ini); g_size = (float)_wtof(buf);
    StereoSetMono(g_on);
    Log("[cinema] mode: %s (screen %.1f m wide, %.1f m ahead)", g_on ? "screen" : "follow (full VR)", g_size, g_dist);
}

bool CinemaActive() { return g_on; }

void CinemaToggle()
{
    g_on = !g_on;
    StereoSetMono(g_on);
    if (g_ini[0]) WritePrivateProfileStringW(L"xr", L"cinematicMode", g_on ? L"screen" : L"follow", g_ini);
    Log("[cinema] switched to %s", g_on ? "screen" : "follow (full VR)");
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
        fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        fd.CPUAccessFlags = 0;
        fd.MiscFlags = 0;
        fd.MipLevels = 1;
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
    else ctx->CopyResource(g_frame, bb);

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
            UiPanelDraw(ctx, eye, g_frameSrv, g_dist, g_size, g_height);
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
