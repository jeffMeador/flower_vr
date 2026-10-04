#include "cinema.h"
#include "stereo.h"
#include "shadow.h"
#include "uisign.h"
#include "log.h"
#include "fakepad.h"
#include "journeycam.h"
#include <Windows.h>
#include <cmath>
#include <d3dcompiler.h>

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

// [xr] cinematicMode=auto (default): the cinema screen for the title and
// menus, the intro until it hands over to you, and the idle screen; full VR
// for everything else, cutscenes between levels included. Before the game
// starts the 2D layer (menu, title) means "screen", latched until you have
// control: the intro settles on the character, a tutorial prompt appears, or
// you walk; Continue (a level load after the menu) hands over at once, so
// chapter intros are in VR. In the game the pause menu takes the screen while up.
// The idle screen (the logo over a flight of far shots after a minute without
// input) brings the screen back until you walk. Every switch fades through
// white. The thumbstick click overrides until the next automatic switch.
// Journey: once its camera director is found (journeycam), its mode decides
// instead: Journey running the camera = the screen, you = full VR.
static bool g_auto = true;
static bool g_latched = true;     // before the game: screen until control
static bool g_inGame = false;     // control was handed over once
static bool g_idleScreen = false; // the idle screen is running
static DWORD g_lastUi = 0;        // last 2D-layer draw (GetTickCount)
static DWORD g_walkSince = 0;     // left stick held since
static DWORD g_lastInput = 0;     // last left stick movement
static int g_manual = -1;         // thumbstick override: -1 none, 0 VR, 1 screen
static bool g_autoWant = true;
// Fade through white: 0 clear .. 1 white; dir +1 fading out, -1 fading in.
static float g_fade = 0.0f;
static int g_fadeDir = 0;
static bool g_fadeTo = false;     // the mode to switch to at full white
static DWORD g_fadeTick = 0;

static void HandOver(const char* why)
{
    if (!g_latched && g_inGame) return;
    g_latched = false;
    g_inGame = true;
    Log("[cinema] %s: full VR", why);
}

void CinemaNoteUiLayer() { g_lastUi = GetTickCount(); if (!g_inGame) g_latched = true; }

// A level started loading. The title menu runs inside the first level, so
// Start (the opening intro) loads none; Continue loads your chapter's level,
// whose intro stays in full VR.
void CinemaNoteLevel(const wchar_t* level)
{
    static wchar_t first[64] = {}; // the level the title menu runs in
    if (!first[0]) { wcscpy_s(first, level); return; }
    if (!g_inGame && _wcsicmp(level, first) != 0) HandOver("chapter start (Continue)");
}
void CinemaNotePrompt()
{
    // Not while a menu is up (its button glyphs are the same kind of image).
    if (g_latched && (!g_lastUi || GetTickCount() - g_lastUi > 1000)) HandOver("prompt seen");
}

static bool g_noIdleScreen = false;
static bool g_paused = false;     // Journey's pause screen is up
static DWORD g_lastTitle = 0; // a title card (the "JOURNEY" logo) was drawn

// Journey's pause: the camera cuts to a scenic shot with the logo, no menu
// text. Logo shown within 3 s of the pause button = paused, until the logo
// has been gone for a second.
void CinemaNoteTitle()
{
    const DWORD now = GetTickCount();
    g_lastTitle = now;
    const DWORD start = FakePadLastStart();
    if (!g_paused && g_inGame && start && now - start < 3000) { g_paused = true; Log("[cinema] paused: screen"); }
}

void CinemaNoteCameraCut(float distToPlayer, bool settled)
{
    const DWORD now = GetTickCount();
    // The idle screen: far shots with the logo over them, after a minute
    // without input (the logo in play shows while you walk).
    if (!settled && distToPlayer >= 4.0f && g_lastTitle && now - g_lastTitle < 3000 &&
        g_lastInput && now - g_lastInput > 60000 && !g_idleScreen && !g_noIdleScreen && !g_paused)
    {
        g_idleScreen = true;
        Log("[cinema] idle screen: screen");
        return;
    }
    if (settled && g_latched && distToPlayer < 4.0f && (!g_lastUi || now - g_lastUi > 1000))
        HandOver("settled on the player");
}

void CinemaInit(const wchar_t* ini)
{
    wcscpy_s(g_ini, ini);
    wchar_t mode[32], buf[32];
    GetPrivateProfileStringW(L"xr", L"cinematicMode", L"auto", mode, 32, ini);
    g_auto = _wcsicmp(mode, L"screen") != 0 && _wcsicmp(mode, L"follow") != 0;
    g_noIdleScreen = GetPrivateProfileIntW(L"debug", L"noIdleScreen", 0, ini) != 0; // tests: the idle screen in VR
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
    if (g_auto) { g_manual = g_on ? 0 : 1; Log("[cinema] switching to %s (until the next automatic switch)", g_manual ? "screen" : "full VR"); return; }
    g_on = !g_on;
    StereoSetMono(g_on);
    if (g_ini[0]) WritePrivateProfileStringW(L"xr", L"cinematicMode", g_on ? L"screen" : L"follow", g_ini);
    Log("[cinema] switched to %s", g_on ? "screen" : "follow (full VR)");
}

void CinemaUpdate()
{
    if (!g_auto) return;
    const DWORD now = GetTickCount();
    const bool uiUp = g_lastUi && now - g_lastUi < 1000;
    if (!g_lastInput) g_lastInput = now;
    if (FakePadLeftStick() > 0.2f || (FakePadLastStart() && now - FakePadLastStart() < 100)) g_lastInput = now;
    if (FakePadLeftStick() > 0.5f && !(uiUp && !g_inGame))
    {
        if (!g_walkSince) g_walkSince = now;
        if (now - g_walkSince > 500)
        {
            if (g_latched) HandOver("walking");
            if (g_idleScreen) { g_idleScreen = false; Log("[cinema] idle screen over"); }
        }
    }
    else g_walkSince = 0;
    if (g_paused && (!g_lastTitle || now - g_lastTitle > 1000)) { g_paused = false; Log("[cinema] unpaused"); }
    // Menus (the 2D layer) and the pause screen also take the screen while up.
    bool want = uiUp || (!g_inGame && g_latched) || g_idleScreen || g_paused;

    // Journey's camera director, when found, decides alone: every time Journey
    // runs the camera (cutscenes, intros, idle, pause, menu) it's the screen;
    // when you control it (mode 2), full VR. Menus (the 2D layer) too.
    float mode = 0.0f, blend = 0.0f;
    if (JourneyCamDirector(&mode, &blend))
    {
        static int lastMode = -1;
        static DWORD notYoursSince = 0;
        // The value slides between 2 (yours) and 4 (Journey's) while the camera
        // blends; above 3 for 0.3 s = Journey's, back under 2.5 = yours.
        const int m = mode >= 3.0f ? 4 : mode <= 2.5f ? 2 : 3;
        if (m != lastMode) { Log("[cinema] camera director %.2f (%s)", mode, m == 4 ? "Journey's" : m == 2 ? "yours" : "blending"); lastMode = m; }
        static bool journeys = false;
        if (mode >= 3.0f) { if (!notYoursSince) notYoursSince = now; if (now - notYoursSince > 300) journeys = true; }
        else notYoursSince = 0;
        if (mode <= 2.5f) journeys = false;
        const bool cutscene = journeys;
        want = cutscene || uiUp;
        if (!want && g_latched) HandOver("camera handed to you");
    }
    if (want != g_autoWant)
    {
        g_autoWant = want;
        g_manual = -1;
        Log("[cinema] auto: %s", want ? "screen" : "full VR");
    }
    const bool target = g_manual >= 0 ? g_manual == 1 : want;

    // Fade through white: out (0.25 s), switch at full white, back in (0.35 s).
    const float dt = g_fadeTick ? (now - g_fadeTick) * 0.001f : 0.0f;
    g_fadeTick = now;
    // Fade through white: out (0.3 s), switch, hold white (1 s: the switch and
    // Journey's camera blend happen unseen), back in (0.4 s).
    static float hold = 0.0f;
    if (g_fadeDir == 0 && target != g_on) { g_fadeDir = 1; g_fadeTo = target; }
    if (g_fadeDir == 1)
    {
        g_fade += dt / 0.3f;
        if (g_fade >= 1.0f) { g_fade = 1.0f; g_on = g_fadeTo; StereoSetMono(g_on); g_fadeDir = 2; hold = 0.0f; }
    }
    else if (g_fadeDir == 2)
    {
        hold += dt;
        if (target != g_on) { g_on = target; StereoSetMono(g_on); hold = 0.0f; } // changed its mind: still white
        if (hold >= 1.0f) g_fadeDir = -1;
    }
    else if (g_fadeDir < 0)
    {
        g_fade -= dt / 0.4f;
        if (g_fade <= 0.0f) { g_fade = 0.0f; g_fadeDir = 0; }
    }
}

static ID3D11RenderTargetView* EyeRtv(ID3D11Device* dev, int eye, ID3D11Resource* res)
{
    if (g_rtvRes[eye] == res && g_rtv[eye]) return g_rtv[eye];
    if (g_rtv[eye]) { g_rtv[eye]->Release(); g_rtv[eye] = nullptr; }
    g_rtvRes[eye] = res;
    if (FAILED(dev->CreateRenderTargetView(res, nullptr, &g_rtv[eye]))) g_rtv[eye] = nullptr;
    return g_rtv[eye];
}

// The white of the fade: a full-screen triangle, blended.
static ID3D11VertexShader* g_fadeVS = nullptr;
static ID3D11PixelShader* g_fadePS = nullptr;
static ID3D11Buffer* g_fadeCB = nullptr;
static ID3D11BlendState* g_fadeBlend = nullptr;
static bool g_fadeFailed = false;

static bool FadeSetup(ID3D11Device* dev)
{
    if (g_fadePS) return true;
    if (g_fadeFailed) return false;
    g_fadeFailed = true;
    static const char kVS[] = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2 - 1, 0, 1); }";
    static const char kPS[] = "cbuffer F : register(b0) { float4 c; }; float4 main() : SV_Target { return c; }";
    ID3DBlob* vb = nullptr, * pb = nullptr;
    if (FAILED(D3DCompile(kVS, sizeof(kVS) - 1, "fade_vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vb, nullptr)) ||
        FAILED(D3DCompile(kPS, sizeof(kPS) - 1, "fade_ps", nullptr, nullptr, "main", "ps_4_0", 0, 0, &pb, nullptr)))
    {
        if (vb) vb->Release();
        Log("[cinema] fade shaders failed to compile");
        return false;
    }
    bool ok = SUCCEEDED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_fadeVS)) &&
              SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_fadePS));
    vb->Release(); pb->Release();
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_fadeCB));
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g_fadeBlend));
    if (!ok) { Log("[cinema] fade setup failed"); return false; }
    g_fadeFailed = false;
    return true;
}

static void DrawFade(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h)
{
    const float c[4] = { 1, 1, 1, g_fade };
    ctx->UpdateSubresource(g_fadeCB, 0, nullptr, c, 0, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_fadeVS, nullptr, 0);
    ctx->PSSetShader(g_fadePS, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_fadeCB);
    ctx->OMSetBlendState(g_fadeBlend, nullptr, 0xFFFFFFFF);
    ctx->Draw(3, 0);
}

static ID3D11RenderTargetView* EyeRtv(ID3D11Device* dev, int eye, ID3D11Resource* res);

static void FadeOverlay(IDXGISwapChain* swapChain, ID3D11DeviceContext* ctx)
{
    if (g_fade <= 0.0f) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (FadeSetup(dev))
    {
        ID3D11RenderTargetView* oRtv = nullptr; ID3D11DepthStencilView* oDsv = nullptr; ctx->OMGetRenderTargets(1, &oRtv, &oDsv);
        D3D11_VIEWPORT oVp = {}; UINT nvp = 1; ctx->RSGetViewports(&nvp, &oVp);
        ID3D11InputLayout* oIl = nullptr; ctx->IAGetInputLayout(&oIl);
        D3D11_PRIMITIVE_TOPOLOGY oTopo; ctx->IAGetPrimitiveTopology(&oTopo);
        ID3D11VertexShader* oVs = nullptr; ctx->VSGetShader(&oVs, nullptr, nullptr);
        ID3D11PixelShader* oPs = nullptr; ctx->PSGetShader(&oPs, nullptr, nullptr);
        ID3D11Buffer* oCb = nullptr; ctx->PSGetConstantBuffers(0, 1, &oCb);
        ID3D11BlendState* oBl = nullptr; float oF[4]; UINT oM = 0; ctx->OMGetBlendState(&oBl, oF, &oM);
        ID3D11DepthStencilState* oDs = nullptr; UINT oRef = 0; ctx->OMGetDepthStencilState(&oDs, &oRef);
        ctx->OMSetDepthStencilState(nullptr, 0);
        for (int eye = 0; eye < 2; ++eye)
        {
            ID3D11Resource* target = eye == 0 ? (ID3D11Resource*)bb : ShadowOfResource(bb);
            if (!target) continue;
            if (ID3D11RenderTargetView* rtv = EyeRtv(dev, eye, target)) DrawFade(ctx, rtv, d.Width, d.Height);
            if (eye == 1) target->Release();
        }
        ctx->OMSetRenderTargets(1, &oRtv, oDsv);
        ctx->RSSetViewports(1, &oVp);
        ctx->IASetInputLayout(oIl);
        ctx->IASetPrimitiveTopology(oTopo);
        ctx->VSSetShader(oVs, nullptr, 0);
        ctx->PSSetShader(oPs, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &oCb);
        ctx->OMSetBlendState(oBl, oF, oM);
        ctx->OMSetDepthStencilState(oDs, oRef);
        IUnknown* olds[] = { oRtv, oDsv, oIl, oVs, oPs, oCb, oBl, oDs };
        for (IUnknown* u : olds) if (u) u->Release();
    }
    dev->Release();
    bb->Release();
}

static void ComposeScreen(IDXGISwapChain* swapChain, ID3D11DeviceContext* ctx);

void CinemaCompose(IDXGISwapChain* swapChain, ID3D11DeviceContext* ctx)
{
    if (!ctx) return;
    if (g_on && StereoHasEyePoses()) ComposeScreen(swapChain, ctx);
    FadeOverlay(swapChain, ctx);
}

static void ComposeScreen(IDXGISwapChain* swapChain, ID3D11DeviceContext* ctx)
{
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
