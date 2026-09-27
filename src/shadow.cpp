#include "shadow.h"
#include "log.h"
#include "vhook.h"
#include "mirror.h"
#include <cstring>
#include <cstdint>

// {7F3C1A2E-5B6D-4E8F-9A1B-2C3D4E5F6071}: private-data slot holding a twin.
static const GUID kTwinGuid = { 0x7f3c1a2e, 0x5b6d, 0x4e8f, { 0x9a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x60, 0x71 } };

static bool g_enabled = false;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_immediate = nullptr;
static thread_local int g_bypass = 0;

ShadowBypass::ShadowBypass() { ++g_bypass; }
ShadowBypass::~ShadowBypass() { --g_bypass; }
bool ShadowBypassed() { return g_bypass > 0; }
bool ShadowEnabled() { return g_enabled; }

// ---- real functions ----
using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateSRV_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using CreateRTV_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**);
using CreateDSV_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**);
using SetSRVs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using OMSetRTs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRTsUAVs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using ClearRTV_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using ClearDSV_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using CopyResource_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using CopyRegion_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using Resolve_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using GenerateMips_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ShaderResourceView*);
using Dispatch_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using ClearState_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);

static CreateTexture2D_t realCreateTexture2D;
static CreateSRV_t realCreateSRV;
static CreateRTV_t realCreateRTV;
static CreateDSV_t realCreateDSV;
enum Stage { VS, PS, GS, HS, DS, CS, kStages };
static SetSRVs_t realSetSRVs[kStages];
static OMSetRTs_t realOMSetRTs;
static OMSetRTsUAVs_t realOMSetRTsUAVs;
static ClearRTV_t realClearRTV;
static ClearDSV_t realClearDSV;
static CopyResource_t realCopyResource;
static CopyRegion_t realCopyRegion;
static Resolve_t realResolve;
static GenerateMips_t realGenerateMips;
static Dispatch_t realDispatch;
static ClearState_t realClearState;

// ---- stats ----
static uint64_t g_twinTextures = 0, g_twinViews = 0, g_twinFail = 0;
static uint64_t g_rightDraws = 0, g_skippedNoTwin = 0, g_mixedOutputs = 0;
static uint64_t g_mirroredClears = 0, g_mirroredCopies = 0, g_uavCalls = 0, g_dispatches = 0;

// Twin lookup: private data holding an interface is returned AddRef'd.
template <class T>
static T* Twin(ID3D11DeviceChild* o)
{
    if (!o) return nullptr;
    IUnknown* p = nullptr;
    UINT size = sizeof(p);
    if (FAILED(o->GetPrivateData(kTwinGuid, &size, &p)) || size != sizeof(p)) return nullptr;
    return static_cast<T*>(p);
}

ID3D11Resource* ShadowOfResource(ID3D11Resource* r) { return Twin<ID3D11Resource>(r); }
ID3D11RenderTargetView* ShadowTwinRTV(ID3D11RenderTargetView* v) { return Twin<ID3D11RenderTargetView>(v); }
ID3D11DepthStencilView* ShadowTwinDSV(ID3D11DepthStencilView* v) { return Twin<ID3D11DepthStencilView>(v); }
ID3D11ShaderResourceView* ShadowTwinSRV(ID3D11ShaderResourceView* v) { return Twin<ID3D11ShaderResourceView>(v); }

// The game's own calls on its immediate context (not ours, not a deferred one).
static bool GameCall(ID3D11DeviceContext* ctx) { return g_enabled && !g_bypass && ctx == g_immediate; }

static bool WantsTwin(const D3D11_TEXTURE2D_DESC& d)
{
    return (d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) && d.Usage == D3D11_USAGE_DEFAULT;
}

// Twin texture of r (AddRef'd), created on demand for render-target/depth
// textures (covers the swapchain's backbuffer, which isn't created via hooks).
static ID3D11Texture2D* EnsureTwinTexture(ID3D11Resource* r, const D3D11_SUBRESOURCE_DATA* init = nullptr)
{
    if (!r) return nullptr;
    if (ID3D11Texture2D* t = Twin<ID3D11Texture2D>(r)) return t;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
    D3D11_TEXTURE2D_DESC desc;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&desc);
    if (!WantsTwin(desc)) return nullptr;
    desc.MiscFlags &= ~(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_GDI_COMPATIBLE);

    ID3D11Texture2D* twin = nullptr;
    if (FAILED(realCreateTexture2D(g_device, &desc, init, &twin)) || !twin)
    {
        if (g_twinFail++ < 5) Log("[shadow] twin texture creation failed (%ux%u fmt %d bind 0x%X)", desc.Width, desc.Height, (int)desc.Format, desc.BindFlags);
        return nullptr;
    }
    r->SetPrivateDataInterface(kTwinGuid, twin); // original keeps the twin alive
    g_twinTextures++;
    return twin; // creation reference goes to the caller
}

// ---- device hooks: create twins alongside the game's resources/views ----
static HRESULT STDMETHODCALLTYPE Hook_CreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out)
{
    HRESULT hr = realCreateTexture2D(self, desc, init, out);
    if (SUCCEEDED(hr) && out && *out && g_enabled && !g_bypass && desc && WantsTwin(*desc))
    {
        if (ID3D11Texture2D* t = EnsureTwinTexture(*out, init)) t->Release();
    }
    return hr;
}

template <class View, class Desc, class Fn>
static void AttachTwinView(ID3D11Device* self, ID3D11Resource* res, const Desc* desc, View* view, Fn create)
{
    ID3D11Texture2D* twinTex = EnsureTwinTexture(res);
    if (!twinTex) return;
    View* twinView = nullptr;
    if (SUCCEEDED(create(self, twinTex, desc, &twinView)) && twinView)
    {
        view->SetPrivateDataInterface(kTwinGuid, twinView);
        twinView->Release();
        g_twinViews++;
    }
    twinTex->Release();
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSRV(ID3D11Device* self, ID3D11Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out)
{
    HRESULT hr = realCreateSRV(self, res, desc, out);
    if (SUCCEEDED(hr) && out && *out && g_enabled && !g_bypass)
        AttachTwinView(self, res, desc, *out, realCreateSRV);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateRTV(ID3D11Device* self, ID3D11Resource* res, const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** out)
{
    HRESULT hr = realCreateRTV(self, res, desc, out);
    if (SUCCEEDED(hr) && out && *out && g_enabled && !g_bypass)
        AttachTwinView(self, res, desc, *out, realCreateRTV);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateDSV(ID3D11Device* self, ID3D11Resource* res, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, ID3D11DepthStencilView** out)
{
    HRESULT hr = realCreateDSV(self, res, desc, out);
    if (SUCCEEDED(hr) && out && *out && g_enabled && !g_bypass)
        AttachTwinView(self, res, desc, *out, realCreateDSV);
    return hr;
}

// ---- binding state of the immediate context ----
static const UINT kMaxSRV = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; // 128

struct StageSRVs
{
    ID3D11ShaderResourceView* orig[kMaxSRV] = {};
    ID3D11ShaderResourceView* twin[kMaxSRV] = {}; // AddRef'd, null if none
    UINT count = 0;   // highest bound slot + 1
    UINT twins = 0;   // number of slots with a twin
};

struct Bindings
{
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11RenderTargetView* twinRtv[8] = {}; // AddRef'd
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11DepthStencilView* twinDsv = nullptr; // AddRef'd
    UINT numRtv = 0;
    StageSRVs srv[kStages];
};
static Bindings g_b;

template <class T> static void Replace(T*& slot, T* v) { if (slot) slot->Release(); slot = v; }

static void TrackOutputs(UINT num, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    for (UINT i = 0; i < 8; ++i)
    {
        ID3D11RenderTargetView* v = (rtvs && i < num) ? rtvs[i] : nullptr;
        g_b.rtv[i] = v;
        Replace(g_b.twinRtv[i], Twin<ID3D11RenderTargetView>(v));
    }
    g_b.numRtv = num;
    g_b.dsv = dsv;
    Replace(g_b.twinDsv, Twin<ID3D11DepthStencilView>(dsv));
}

static void TrackSRVs(Stage st, UINT start, UINT n, ID3D11ShaderResourceView* const* views)
{
    StageSRVs& s = g_b.srv[st];
    for (UINT i = 0; i < n && start + i < kMaxSRV; ++i)
    {
        UINT slot = start + i;
        ID3D11ShaderResourceView* v = views ? views[i] : nullptr;
        if (s.twin[slot]) s.twins--;
        s.orig[slot] = v;
        Replace(s.twin[slot], Twin<ID3D11ShaderResourceView>(v));
        if (s.twin[slot]) s.twins++;
    }
    UINT c = 0;
    for (UINT i = 0; i < kMaxSRV; ++i) if (s.orig[i]) c = i + 1;
    s.count = c;
}

static void OnOMSetRTs(ID3D11DeviceContext* ctx, UINT num, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    if (ctx == g_immediate && !g_bypass && g_enabled) TrackOutputs(num, rtvs, dsv);
}

static void STDMETHODCALLTYPE Hook_OMSetRTs(ID3D11DeviceContext* ctx, UINT num, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    OnOMSetRTs(ctx, num, rtvs, dsv);
    realOMSetRTs(ctx, num, rtvs, dsv);
    if (GameCall(ctx)) MirrorSetRenderTargets(num, rtvs, dsv);
}
static void STDMETHODCALLTYPE Hook_OMSetRTsUAVs(ID3D11DeviceContext* ctx, UINT num, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
    UINT uavStart, UINT numUavs, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    if (num != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) OnOMSetRTs(ctx, num, rtvs, dsv);
    if (numUavs && numUavs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS && uavs && uavs[0]) g_uavCalls++;
    realOMSetRTsUAVs(ctx, num, rtvs, dsv, uavStart, numUavs, uavs, counts);
    if (GameCall(ctx)) MirrorSetRenderTargetsAndUAVs(num, rtvs, dsv, uavStart, numUavs, uavs, counts);
}

#define SRV_HOOK(ST, MIRROR) \
static void STDMETHODCALLTYPE Hook_SetSRVs_##ST(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11ShaderResourceView* const* v) \
{ \
    if (ctx == g_immediate && !g_bypass && g_enabled) TrackSRVs(ST, start, n, v); \
    realSetSRVs[ST](ctx, start, n, v); \
    if (MIRROR && GameCall(ctx)) MirrorSetShaderResources((MirrorStage)ST, start, n, v); \
}
// (compute isn't mirrored: the game dispatches nothing, and a dispatch flushes)
SRV_HOOK(VS, true) SRV_HOOK(PS, true) SRV_HOOK(GS, true) SRV_HOOK(HS, true) SRV_HOOK(DS, true) SRV_HOOK(CS, false)

static void STDMETHODCALLTYPE Hook_ClearState(ID3D11DeviceContext* ctx)
{
    if (ctx == g_immediate && !g_bypass && g_enabled)
    {
        TrackOutputs(0, nullptr, nullptr);
        for (int st = 0; st < kStages; ++st) TrackSRVs((Stage)st, 0, kMaxSRV, nullptr);
    }
    realClearState(ctx);
    if (GameCall(ctx)) MirrorClearState();
}

// ---- mirrored operations ----
static void STDMETHODCALLTYPE Hook_ClearRTV(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* v, const FLOAT color[4])
{
    realClearRTV(ctx, v, color);
    if (GameCall(ctx) && MirrorActive()) { MirrorClearRTV(v, color); return; }
    if (GameCall(ctx))
        if (ID3D11RenderTargetView* t = Twin<ID3D11RenderTargetView>(v)) { realClearRTV(ctx, t, color); t->Release(); g_mirroredClears++; }
}
static void STDMETHODCALLTYPE Hook_ClearDSV(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* v, UINT flags, FLOAT depth, UINT8 stencil)
{
    realClearDSV(ctx, v, flags, depth, stencil);
    if (GameCall(ctx) && MirrorActive()) { MirrorClearDSV(v, flags, depth, stencil); return; }
    if (GameCall(ctx))
        if (ID3D11DepthStencilView* t = Twin<ID3D11DepthStencilView>(v)) { realClearDSV(ctx, t, flags, depth, stencil); t->Release(); g_mirroredClears++; }
}
static void STDMETHODCALLTYPE Hook_CopyResource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
{
    realCopyResource(ctx, dst, src);
    if (!GameCall(ctx)) return;
    if (MirrorActive()) { MirrorCopyResource(dst, src); return; }
    if (ID3D11Resource* td = Twin<ID3D11Resource>(dst))
    {
        ID3D11Resource* ts = Twin<ID3D11Resource>(src);
        realCopyResource(ctx, td, ts ? ts : src);
        if (ts) ts->Release();
        td->Release();
        g_mirroredCopies++;
    }
}
static void STDMETHODCALLTYPE Hook_CopyRegion(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ssub, const D3D11_BOX* box)
{
    realCopyRegion(ctx, dst, dsub, x, y, z, src, ssub, box);
    if (!GameCall(ctx)) return;
    if (MirrorActive()) { MirrorCopyRegion(dst, dsub, x, y, z, src, ssub, box); return; }
    if (ID3D11Resource* td = Twin<ID3D11Resource>(dst))
    {
        ID3D11Resource* ts = Twin<ID3D11Resource>(src);
        realCopyRegion(ctx, td, dsub, x, y, z, ts ? ts : src, ssub, box);
        if (ts) ts->Release();
        td->Release();
        g_mirroredCopies++;
    }
}
static void STDMETHODCALLTYPE Hook_Resolve(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dsub, ID3D11Resource* src, UINT ssub, DXGI_FORMAT fmt)
{
    realResolve(ctx, dst, dsub, src, ssub, fmt);
    if (!GameCall(ctx)) return;
    if (MirrorActive()) { MirrorResolve(dst, dsub, src, ssub, fmt); return; }
    if (ID3D11Resource* td = Twin<ID3D11Resource>(dst))
    {
        ID3D11Resource* ts = Twin<ID3D11Resource>(src);
        realResolve(ctx, td, dsub, ts ? ts : src, ssub, fmt);
        if (ts) ts->Release();
        td->Release();
        g_mirroredCopies++;
    }
}
static void STDMETHODCALLTYPE Hook_GenerateMips(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* v)
{
    realGenerateMips(ctx, v);
    if (GameCall(ctx) && MirrorActive()) { MirrorGenerateMips(v); return; }
    if (GameCall(ctx))
        if (ID3D11ShaderResourceView* t = Twin<ID3D11ShaderResourceView>(v)) { realGenerateMips(ctx, t); t->Release(); }
}
static void STDMETHODCALLTYPE Hook_Dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z)
{
    g_dispatches++;
    if (GameCall(ctx)) MirrorFlush("dispatch"); // compute isn't mirrored; recorded draws go first
    realDispatch(ctx, x, y, z);
}

void ShadowOnUpdateSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* src, UINT rowPitch, UINT depthPitch)
{
    if (!GameCall(ctx)) return;
    if (MirrorActive()) { MirrorUpdateSubresource(dst, sub, box, src, rowPitch, depthPitch); return; }
    if (ID3D11Resource* td = Twin<ID3D11Resource>(dst))
    {
        ShadowBypass guard;
        ctx->UpdateSubresource(td, sub, box, src, rowPitch, depthPitch);
        td->Release();
    }
}

// ---- right-eye pass ----
static bool g_rightBound = false;

bool ShadowBindRightEye(ID3D11DeviceContext* ctx)
{
    if (!g_enabled || ctx != g_immediate) return false;
    // Every bound output needs a twin, or the right eye would draw over the left.
    bool any = false, missing = false;
    for (UINT i = 0; i < g_b.numRtv && i < 8; ++i)
    {
        if (!g_b.rtv[i]) continue;
        if (g_b.twinRtv[i]) any = true; else missing = true;
    }
    if (g_b.dsv) { if (g_b.twinDsv) any = true; else missing = true; }
    if (!any || missing)
    {
        if (any && missing) g_mixedOutputs++; else g_skippedNoTwin++;
        return false;
    }

    ID3D11RenderTargetView* rtvs[8];
    for (UINT i = 0; i < 8; ++i) rtvs[i] = g_b.twinRtv[i] ? g_b.twinRtv[i] : g_b.rtv[i];
    realOMSetRTs(ctx, g_b.numRtv, rtvs, g_b.twinDsv ? g_b.twinDsv : g_b.dsv);
    for (int st = 0; st < kStages; ++st)
    {
        StageSRVs& s = g_b.srv[st];
        if (!s.twins || !realSetSRVs[st]) continue;
        ID3D11ShaderResourceView* v[kMaxSRV];
        for (UINT i = 0; i < s.count; ++i) v[i] = s.twin[i] ? s.twin[i] : s.orig[i];
        realSetSRVs[st](ctx, 0, s.count, v);
    }
    g_rightBound = true;
    g_rightDraws++;
    return true;
}

void ShadowRestore(ID3D11DeviceContext* ctx)
{
    if (!g_rightBound) return;
    g_rightBound = false;
    realOMSetRTs(ctx, g_b.numRtv, g_b.rtv, g_b.dsv);
    for (int st = 0; st < kStages; ++st)
    {
        StageSRVs& s = g_b.srv[st];
        if (!s.twins || !realSetSRVs[st]) continue;
        realSetSRVs[st](ctx, 0, s.count, s.orig);
    }
}

void ShadowLogStats()
{
    if (!g_enabled) return;
    Log("[shadow] twins: %llu textures %llu views (%llu failed); right draws %llu, skipped (no twin) %llu, mixed outputs %llu; "
        "mirrored clears %llu copies %llu; UAV binds %llu dispatches %llu",
        g_twinTextures, g_twinViews, g_twinFail, g_rightDraws, g_skippedNoTwin, g_mixedOutputs,
        g_mirroredClears, g_mirroredCopies, g_uavCalls, g_dispatches);
}

// ---- install ----
void ShadowInstall(ID3D11Device* device, ID3D11DeviceContext* context, bool enabled)
{
    g_enabled = enabled;
    if (!enabled) { Log("[shadow] double render disabled (alternate-eye mode)"); return; }
    g_device = device;
    g_immediate = context;

    void** dv = *reinterpret_cast<void***>(device);
    void** cv = *reinterpret_cast<void***>(context);
    HookMethod(dv, 5, (void*)&Hook_CreateTexture2D, (void**)&realCreateTexture2D, "CreateTexture2D");
    HookMethod(dv, 7, (void*)&Hook_CreateSRV, (void**)&realCreateSRV, "CreateShaderResourceView");
    HookMethod(dv, 9, (void*)&Hook_CreateRTV, (void**)&realCreateRTV, "CreateRenderTargetView");
    HookMethod(dv, 10, (void*)&Hook_CreateDSV, (void**)&realCreateDSV, "CreateDepthStencilView");

    HookMethod(cv, 8, (void*)&Hook_SetSRVs_PS, (void**)&realSetSRVs[PS], "PSSetShaderResources");
    HookMethod(cv, 25, (void*)&Hook_SetSRVs_VS, (void**)&realSetSRVs[VS], "VSSetShaderResources");
    HookMethod(cv, 31, (void*)&Hook_SetSRVs_GS, (void**)&realSetSRVs[GS], "GSSetShaderResources");
    HookMethod(cv, 59, (void*)&Hook_SetSRVs_HS, (void**)&realSetSRVs[HS], "HSSetShaderResources");
    HookMethod(cv, 63, (void*)&Hook_SetSRVs_DS, (void**)&realSetSRVs[DS], "DSSetShaderResources");
    HookMethod(cv, 67, (void*)&Hook_SetSRVs_CS, (void**)&realSetSRVs[CS], "CSSetShaderResources");
    HookMethod(cv, 33, (void*)&Hook_OMSetRTs, (void**)&realOMSetRTs, "OMSetRenderTargets");
    HookMethod(cv, 34, (void*)&Hook_OMSetRTsUAVs, (void**)&realOMSetRTsUAVs, "OMSetRenderTargetsAndUnorderedAccessViews");
    HookMethod(cv, 41, (void*)&Hook_Dispatch, (void**)&realDispatch, "Dispatch");
    HookMethod(cv, 46, (void*)&Hook_CopyRegion, (void**)&realCopyRegion, "CopySubresourceRegion");
    HookMethod(cv, 47, (void*)&Hook_CopyResource, (void**)&realCopyResource, "CopyResource");
    HookMethod(cv, 50, (void*)&Hook_ClearRTV, (void**)&realClearRTV, "ClearRenderTargetView");
    HookMethod(cv, 53, (void*)&Hook_ClearDSV, (void**)&realClearDSV, "ClearDepthStencilView");
    HookMethod(cv, 54, (void*)&Hook_GenerateMips, (void**)&realGenerateMips, "GenerateMips");
    HookMethod(cv, 57, (void*)&Hook_Resolve, (void**)&realResolve, "ResolveSubresource");
    HookMethod(cv, 110, (void*)&Hook_ClearState, (void**)&realClearState, "ClearState");
    Log("[shadow] double render enabled; hooks installed");
}
