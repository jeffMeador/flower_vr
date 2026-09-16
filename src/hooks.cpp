#include "hooks.h"
#include "log.h"
#include <cstdio>
#include <atomic>
#include <vector>

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

static Present_t g_realPresent = nullptr;
static std::atomic<uint64_t> g_frameCount{ 0 };
static wchar_t g_dllDir[MAX_PATH] = {};

void SetHooksDllDir(const wchar_t* dir)
{
    wcscpy_s(g_dllDir, dir);
}

// Minimal 32bpp BMP writer for verifying frames visually without extra deps.
static void DumpBackbufferBMP(IDXGISwapChain* swapChain, uint64_t frameIndex)
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

    D3D11_TEXTURE2D_DESC desc = {};
    backbuffer->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)) || !staging)
    {
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
        swprintf_s(path, L"%s\\frame_%llu.bmp", g_dllDir, (unsigned long long)frameIndex);
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
                    // Assume BGRA8 or RGBA8 (both 4 bytes/pixel); write as BGR.
                    unsigned char b = srcRow[x * 4 + 0];
                    unsigned char g = srcRow[x * 4 + 1];
                    unsigned char r = srcRow[x * 4 + 2];
                    padded[x * 3 + 0] = b;
                    padded[x * 3 + 1] = g;
                    padded[x * 3 + 2] = r;
                }
                fwrite(padded.data(), 1, rowSize, f);
            }
            fclose(f);
            Log("Dumped screenshot: frame_%llu.bmp (%ux%u, fmt=%d)", (unsigned long long)frameIndex, w, h, (int)desc.Format);
        }
        ctx->Unmap(staging, 0);
    }

    staging->Release();
    backbuffer->Release();
    ctx->Release();
    device->Release();
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
    uint64_t frame = g_frameCount.fetch_add(1);

    if (frame == 0)
        Log("First Present() call received - hook is live.");

    // Dump a handful of early frames, then one every ~5 seconds (assuming ~60fps).
    if (frame < 5 || (frame % 300) == 0)
        DumpBackbufferBMP(This, frame);

    return g_realPresent(This, SyncInterval, Flags);
}

void InstallHooksOnSwapChain(IDXGISwapChain* swapChain, ID3D11Device* /*device*/, ID3D11DeviceContext* /*context*/)
{
    if (!swapChain) return;
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
}
