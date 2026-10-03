#include "uisign.h"
#include "stereo.h"
#include "log.h"
#include <d3dcompiler.h>
#include <cstring>

// Panel corners in the headset's reference space (meters, OpenXR axes: the
// game camera looks along -z), set from [xr] menuDistance/menuSize/menuHeight.
static float g_dist = 2.5f, g_half = 1.25f, g_height = -0.15f;
static bool g_failed = false;
static ID3D11Device* g_dev = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_ps = nullptr;
static ID3D11InputLayout* g_layout = nullptr;
static ID3D11Buffer* g_vb[2] = {};
static ID3D11SamplerState* g_sampler = nullptr;
static ID3D11RasterizerState* g_raster = nullptr;

struct Vertex { float pos[4]; float uv[2]; };

static const char kVS[] =
    "struct I { float4 p : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "O main(I i) { O o; o.p = i.p; o.uv = i.uv; return o; }\n";
static const char kPS[] =
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
    "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t0.Sample(s0, uv); }\n";

bool UiSignSetup(float distance, float size, float height)
{
    g_dist = distance;
    g_half = size * 0.5f;
    g_height = height;
    return true;
}

static bool Create(ID3D11DeviceContext* ctx)
{
    if (g_vs) return true;
    if (g_failed) return false;
    g_failed = true; // until everything below succeeds
    ctx->GetDevice(&g_dev);
    if (!g_dev) return false;
    ID3DBlob* vsb = nullptr, * psb = nullptr, * err = nullptr;
    if (FAILED(D3DCompile(kVS, sizeof(kVS) - 1, "uisign_vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsb, &err)))
    {
        Log("[uisign] VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    if (FAILED(D3DCompile(kPS, sizeof(kPS) - 1, "uisign_ps", nullptr, nullptr, "main", "ps_4_0", 0, 0, &psb, &err)))
    {
        Log("[uisign] PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        vsb->Release();
        return false;
    }
    const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    bool ok = SUCCEEDED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps)) &&
              SUCCEEDED(g_dev->CreateInputLayout(el, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout));
    vsb->Release();
    psb->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Vertex) * 4;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ok = ok && SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_vb[0])) && SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_vb[1]));
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(g_dev->CreateSamplerState(&sd, &g_sampler));
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(g_dev->CreateRasterizerState(&rd, &g_raster));
    if (!ok) { Log("[uisign] setup failed"); return false; }
    g_failed = false;
    Log("[uisign] menu panel ready: %.2f m ahead, %.2f m wide, %.2f m height", g_dist, g_half * 2.0f, g_height);
    return true;
}

// A textured square panel in the reference space: dist ahead, half-size half,
// center height above eye level. srv: the texture to show (null: whatever the
// game has bound at t0). opaque: no blending (the cinema screen).
static bool DrawPanel(ID3D11DeviceContext* ctx, int eye, float dist, float half, float height,
                      ID3D11ShaderResourceView* srv, bool opaque)
{
    if (!StereoHasEyePoses() || !Create(ctx)) return false;

    // Corners: top-left, top-right, bottom-left, bottom-right (a triangle strip).
    const float xs[4] = { -half, half, -half, half };
    const float ys[4] = { height + half, height + half, height - half, height - half };
    const float us[4] = { 0, 1, 0, 1 }, vs[4] = { 0, 0, 1, 1 };
    Vertex v[4];
    for (int i = 0; i < 4; ++i)
    {
        const float p[3] = { xs[i], ys[i], -dist };
        if (!StereoRefPointToClip(eye, p, v[i].pos)) return false;
        v[i].uv[0] = us[i];
        v[i].uv[1] = vs[i];
    }
    ID3D11Buffer* vb = g_vb[eye ? 1 : 0];
    ctx->UpdateSubresource(vb, 0, nullptr, v, 0, 0);

    // Save what we change, draw, put it back.
    ID3D11InputLayout* oldLayout = nullptr; ctx->IAGetInputLayout(&oldLayout);
    ID3D11Buffer* oldVB = nullptr; UINT oldStride = 0, oldOffset = 0; ctx->IAGetVertexBuffers(0, 1, &oldVB, &oldStride, &oldOffset);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo; ctx->IAGetPrimitiveTopology(&oldTopo);
    ID3D11VertexShader* oldVS = nullptr; ctx->VSGetShader(&oldVS, nullptr, nullptr);
    ID3D11PixelShader* oldPS = nullptr; ctx->PSGetShader(&oldPS, nullptr, nullptr);
    ID3D11SamplerState* oldSampler = nullptr; ctx->PSGetSamplers(0, 1, &oldSampler);
    ID3D11RasterizerState* oldRaster = nullptr; ctx->RSGetState(&oldRaster);
    ID3D11ShaderResourceView* oldSrv = nullptr; ctx->PSGetShaderResources(0, 1, &oldSrv);
    ID3D11BlendState* oldBlend = nullptr; float oldFactor[4]; UINT oldMask = 0; ctx->OMGetBlendState(&oldBlend, oldFactor, &oldMask);

    const UINT stride = sizeof(Vertex), offset = 0;
    ctx->IASetInputLayout(g_layout);
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &g_sampler);
    ctx->RSSetState(g_raster);
    if (srv) ctx->PSSetShaderResources(0, 1, &srv);
    if (opaque) ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->Draw(4, 0);

    ctx->IASetInputLayout(oldLayout);
    ctx->IASetVertexBuffers(0, 1, &oldVB, &oldStride, &oldOffset);
    ctx->IASetPrimitiveTopology(oldTopo);
    ctx->VSSetShader(oldVS, nullptr, 0);
    ctx->PSSetShader(oldPS, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &oldSampler);
    ctx->RSSetState(oldRaster);
    if (srv) ctx->PSSetShaderResources(0, 1, &oldSrv);
    if (opaque) ctx->OMSetBlendState(oldBlend, oldFactor, oldMask);
    if (oldLayout) oldLayout->Release();
    if (oldVB) oldVB->Release();
    if (oldVS) oldVS->Release();
    if (oldPS) oldPS->Release();
    if (oldSampler) oldSampler->Release();
    if (oldRaster) oldRaster->Release();
    if (oldSrv) oldSrv->Release();
    if (oldBlend) oldBlend->Release();
    return true;
}

bool UiSignDraw(ID3D11DeviceContext* ctx, int eye)
{
    return DrawPanel(ctx, eye, g_dist, g_half, g_height, nullptr, false);
}

bool UiPanelDraw(ID3D11DeviceContext* ctx, int eye, ID3D11ShaderResourceView* srv, float dist, float size, float height)
{
    return DrawPanel(ctx, eye, dist, size * 0.5f, height, srv, true);
}
