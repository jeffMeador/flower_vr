#include "hooks.h"
#include <MinHook.h>
#include "log.h"
#include "keys.h"
#include "capture.h"
#include "fakepad.h"
#include "stereo.h"
#include "xr.h"
#include "stereo.h"
#include "camfind.h"
#include "journeycam.h"
#include "camoverride.h"
#include "shadow.h"
#include "mirror.h"
#include <cstdio>
#include <atomic>
#include <vector>

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

static Present_t g_realPresent = nullptr;
static std::atomic<uint64_t> g_frameCount{ 0 };
static wchar_t g_dllDir[MAX_PATH] = {};

bool HooksPassthrough()
{
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
    return GetPrivateProfileIntW(L"debug", L"passthrough", 0, ini) != 0;
}

static bool g_forceGameFov = false; // [debug] forceGameFov=1: Journey FOV override without VR (testing)

void SetHooksDllDir(const wchar_t* dir)
{
    wcscpy_s(g_dllDir, dir);
    CamFindSetDir(dir);
}

// Minimal 32bpp BMP writer for verifying frames visually without extra deps.
static void DumpBackbufferBMP(IDXGISwapChain* swapChain, uint64_t frameIndex, bool rightEyeTwin = false)
{
    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device)) || !device)
        return;

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);

    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) || !backbuffer)
    {
        if (ctx) ctx->Release();
        device->Release();
        return;
    }

    if (rightEyeTwin)
    {
        // Double render: the right eye lives in the backbuffer's twin.
        ID3D11Texture2D* twin = (ID3D11Texture2D*)ShadowOfResource(backbuffer);
        backbuffer->Release();
        backbuffer = twin;
        if (!backbuffer) { if (ctx) ctx->Release(); device->Release(); return; }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    backbuffer->GetDesc(&desc);

    if (desc.SampleDesc.Count > 1)
    {
        // Multisampled backbuffer (Journey): resolve it, then read the resolved copy.
        D3D11_TEXTURE2D_DESC rd = desc;
        rd.SampleDesc = { 1, 0 };
        rd.Usage = D3D11_USAGE_DEFAULT;
        rd.BindFlags = 0;
        rd.CPUAccessFlags = 0;
        rd.MiscFlags = 0;
        ID3D11Texture2D* resolved = nullptr;
        if (FAILED(device->CreateTexture2D(&rd, nullptr, &resolved)) || !resolved)
        {
            Log("[dump] could not create a resolve texture (%ux%u, %u samples)", desc.Width, desc.Height, desc.SampleDesc.Count);
            backbuffer->Release(); if (ctx) ctx->Release(); device->Release();
            return;
        }
        ctx->ResolveSubresource(resolved, 0, backbuffer, 0, desc.Format);
        backbuffer->Release();
        backbuffer = resolved;
        desc = rd;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)) || !staging)
    {
        Log("[dump] could not create a staging texture (%ux%u, fmt %d)", desc.Width, desc.Height, (int)desc.Format);
        backbuffer->Release();
        if (ctx) ctx->Release();
        device->Release();
        return;
    }

    ctx->CopyResource(staging, backbuffer);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        UINT w = desc.Width, h = desc.Height;
        std::vector<unsigned char> row(w * 3);

        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\frame_%llu%s.bmp", g_dllDir, (unsigned long long)frameIndex, rightEyeTwin ? L"_R" : L"");
        FILE* f = nullptr;
        _wfopen_s(&f, path, L"wb");
        if (f)
        {
            unsigned int rowSize = (w * 3 + 3) & ~3u;
            unsigned int imgSize = rowSize * h;
            unsigned int fileSize = 14 + 40 + imgSize;

            unsigned char fileHeader[14] = {
                'B','M',
                (unsigned char)(fileSize), (unsigned char)(fileSize >> 8), (unsigned char)(fileSize >> 16), (unsigned char)(fileSize >> 24),
                0,0,0,0,
                54,0,0,0
            };
            unsigned char infoHeader[40] = { 40,0,0,0 };
            *(int*)(infoHeader + 4) = (int)w;
            *(int*)(infoHeader + 8) = (int)h; // positive = bottom-up, matches BMP default
            *(short*)(infoHeader + 12) = 1;
            *(short*)(infoHeader + 14) = 24;
            *(unsigned int*)(infoHeader + 20) = imgSize;

            fwrite(fileHeader, 1, 14, f);
            fwrite(infoHeader, 1, 40, f);

            const unsigned char* src = (const unsigned char*)mapped.pData;
            std::vector<unsigned char> padded(rowSize, 0);
            // BMP rows are bottom-up; backbuffer rows are top-down.
            for (int y = (int)h - 1; y >= 0; --y)
            {
                const unsigned char* srcRow = src + (size_t)y * mapped.RowPitch;
                for (UINT x = 0; x < w; ++x)
                {
                    // 4 bytes/pixel; RGBA formats (28/29) need R and B swapped for BMP's BGR.
                    bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                    unsigned char b = srcRow[x * 4 + (rgba ? 2 : 0)];
                    unsigned char g = srcRow[x * 4 + 1];
                    unsigned char r = srcRow[x * 4 + (rgba ? 0 : 2)];
                    padded[x * 3 + 0] = b;
                    padded[x * 3 + 1] = g;
                    padded[x * 3 + 2] = r;
                }
                fwrite(padded.data(), 1, rowSize, f);
            }
            fclose(f);
            Log("Dumped screenshot: frame_%llu%s.bmp (%ux%u, fmt=%d)", (unsigned long long)frameIndex, rightEyeTwin ? "_R" : "", w, h, (int)desc.Format);
        }
        ctx->Unmap(staging, 0);
    }

    staging->Release();
    backbuffer->Release();
    ctx->Release();
    device->Release();
}

// ---- frame timing ----
// In VR xrWaitFrame paces every frame to the display, so the frame interval
// can't show headroom. Measure the GPU time (timestamp queries from the end of
// one Present to the start of the next) and the game's CPU time (same span on
// the CPU clock) separately; logged every 5 s as [perf].
struct GpuTimer { ID3D11Query* disjoint = nullptr; ID3D11Query* begin = nullptr; ID3D11Query* end = nullptr; bool pending = false; };
static GpuTimer g_timers[8];
static int g_openTimer = -1;
static LARGE_INTEGER g_qpf = {}, g_frameStart = {};
static double g_gpuSum = 0, g_gpuWorst = 0, g_cpuSum = 0, g_cpuWorst = 0;
static int g_gpuN = 0, g_cpuN = 0;
static double g_xrSum = 0, g_xrWorst = 0, g_presentSum = 0, g_presentWorst = 0;
static int g_blockN = 0;

// Times a blocking call (the XR frame wait/submit, the game's own Present).
struct BlockTimer
{
    LARGE_INTEGER t0;
    double& sum; double& worst;
    BlockTimer(double& s, double& w) : sum(s), worst(w) { QueryPerformanceCounter(&t0); }
    ~BlockTimer()
    {
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        sum += ms; if (ms > worst) worst = ms;
    }
};
static DWORD g_perfLog = 0;

static void TimingFrameEnd(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    if (g_frameStart.QuadPart)
    {
        double ms = (now.QuadPart - g_frameStart.QuadPart) * 1000.0 / g_qpf.QuadPart;
        g_cpuSum += ms; g_cpuN++;
        if (ms > g_cpuWorst) g_cpuWorst = ms;
    }
    if (g_openTimer >= 0)
    {
        GpuTimer& t = g_timers[g_openTimer];
        ctx->End(t.end);
        ctx->End(t.disjoint);
        ctx->Flush(); // send it now; otherwise it waits for Present (after xrWaitFrame) and counts the wait
        t.pending = true;
        g_openTimer = -1;
    }
    for (GpuTimer& t : g_timers)
    {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (ctx->GetData(t.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 b = 0, e = 0;
        if (ctx->GetData(t.begin, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(t.end, &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        t.pending = false;
        if (dj.Disjoint || !dj.Frequency || e < b) continue;
        double ms = (e - b) * 1000.0 / dj.Frequency;
        g_gpuSum += ms; g_gpuN++;
        if (ms > g_gpuWorst) g_gpuWorst = ms;
    }
    if (GetTickCount() - g_perfLog > 5000 && (g_gpuN || g_cpuN))
    {
        g_perfLog = GetTickCount();
        Log("[perf] gpu avg %.2f ms worst %.2f (%d frames) | cpu avg %.2f ms worst %.2f | xr wait+submit avg %.2f worst %.2f | present avg %.2f worst %.2f",
            g_gpuN ? g_gpuSum / g_gpuN : 0.0, g_gpuWorst, g_gpuN, g_cpuN ? g_cpuSum / g_cpuN : 0.0, g_cpuWorst,
            g_blockN ? g_xrSum / g_blockN : 0.0, g_xrWorst, g_blockN ? g_presentSum / g_blockN : 0.0, g_presentWorst);
        g_gpuSum = g_gpuWorst = g_cpuSum = g_cpuWorst = 0; g_gpuN = g_cpuN = 0;
        g_xrSum = g_xrWorst = g_presentSum = g_presentWorst = 0; g_blockN = 0;
    }
    (void)dev;
}

static void TimingFrameStart(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    QueryPerformanceCounter(&g_frameStart);
    for (int i = 0; i < 8; ++i)
    {
        GpuTimer& t = g_timers[i];
        if (t.pending) continue;
        if (!t.disjoint)
        {
            D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 };
            if (FAILED(dev->CreateQuery(&qd, &t.disjoint)) || FAILED(dev->CreateQuery(&qt, &t.begin)) ||
                FAILED(dev->CreateQuery(&qt, &t.end)))
                return;
        }
        ctx->Begin(t.disjoint);
        ctx->End(t.begin);
        g_openTimer = i;
        return;
    }
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
    uint64_t frame = g_frameCount.fetch_add(1);
    MirrorFlush("present"); // the right eye's recorded frame runs now, before the eyes are copied out
    ID3D11Device* timingDev = nullptr;
    ID3D11DeviceContext* timingCtx = nullptr;
    if (SUCCEEDED(This->GetDevice(__uuidof(ID3D11Device), (void**)&timingDev)) && timingDev)
        timingDev->GetImmediateContext(&timingCtx);
    if (timingCtx) TimingFrameEnd(timingDev, timingCtx);
    // Eye this finished frame was drawn for (2 = both: double render).
    int renderedEye = Stereo().doubleRender ? 2 : StereoCurrentEye();
    ShadowBypass bypass; // our own context calls below must not be mirrored
    g_blockN++;
    { BlockTimer bt(g_xrSum, g_xrWorst); XrSubmitFrame(This, renderedEye); }
    CamOverrideTick(XrSessionActive());
    JourneyCamTick(XrSessionActive() || g_forceGameFov);
    {
        DXGI_SWAP_CHAIN_DESC scd = {};
        This->GetDesc(&scd);
        if (scd.BufferDesc.Height) CamFindTick((float)scd.BufferDesc.Width / scd.BufferDesc.Height);
    }
    NotifyCaptureFrameBoundary();

    if (frame == 0)
        Log("First Present() call received - hook is live.");

    // F12 dumps the next two frames: consecutive frames hold both eyes under
    // alternate-eye stereo. (Periodic dumps at 7680x2160 cost ~50MB each.)
    static int dumpRemaining = 0;
    if (KeyEdge(VK_F12))
        dumpRemaining = 2;
    // Remote trigger (hotkeys need the game in focus): a file named vrmod_dump
    // next to the DLL dumps the next frame's eyes and is removed.
    if ((frame % 30) == 0)
    {
        wchar_t trig[MAX_PATH];
        swprintf_s(trig, L"%s\\vrmod_dump", g_dllDir);
        if (GetFileAttributesW(trig) != INVALID_FILE_ATTRIBUTES && DeleteFileW(trig))
        {
            dumpRemaining = 1;
            Log("[dump] requested by file");
        }
    }
    if (frame < 2 || dumpRemaining > 0)
    {
        DumpBackbufferBMP(This, frame);
        if (Stereo().doubleRender) DumpBackbufferBMP(This, frame, true);
        if (dumpRemaining > 0) dumpRemaining--;
    }

    // With a headset attached, xrWaitFrame paces us; don't also wait for the monitor.
    if (XrSessionActive()) SyncInterval = 0;
    HRESULT hr;
    { BlockTimer bt(g_presentSum, g_presentWorst); hr = g_realPresent(This, SyncInterval, Flags); }
    if (timingCtx)
    {
        TimingFrameStart(timingDev, timingCtx);
        timingCtx->Release();
    }
    if (timingDev) timingDev->Release();
    return hr;
}

void InstallHooksOnSwapChain(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!swapChain) return;
    if (HooksPassthrough())
    {
        MH_Initialize();
        InstallFakePad(g_dllDir);
        Log("[debug] passthrough: only the virtual gamepad is installed");
        return;
    }
    void** vtable = *reinterpret_cast<void***>(swapChain);

    if (!g_realPresent)
    {
        DWORD oldProtect;
        VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
        g_realPresent = reinterpret_cast<Present_t>(vtable[8]);
        vtable[8] = reinterpret_cast<void*>(&HookedPresent);
        VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);
        Log("Present() vtable slot [8] patched.");
    }

    InstallCaptureHooks(device, context);
    ShadowInstall(device, context, Stereo().doubleRender);
    if (Stereo().doubleRender)
    {
        wchar_t ini[MAX_PATH];
        swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
        MirrorInstall(device, context, ini);
    }
    InstallFakePad(g_dllDir);
    XrInit(device, g_dllDir);
    {
        wchar_t ini[MAX_PATH], buf[32];
        swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
        GetPrivateProfileStringW(L"xr", L"gameFov", L"125", buf, 32, ini);
        CamOverrideInstall((float)_wtof(buf));
        JourneyCamInstall((float)_wtof(buf), GetPrivateProfileIntW(L"xr", L"headCamera", 0, ini) != 0);
        g_forceGameFov = GetPrivateProfileIntW(L"debug", L"forceGameFov", 0, ini) != 0;
        CamOverrideSetHeadCamera(GetPrivateProfileIntW(L"xr", L"headCamera", 0, ini) != 0);
        CamOverrideSetTerrainClamp(GetPrivateProfileIntW(L"xr", L"terrainClamp", 0, ini) != 0);
    }
}
