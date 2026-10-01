#include "mirror.h"
#include "shadow.h"
#include "capture.h"
#include "vhook.h"
#include "log.h"
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <vector>
#include <algorithm>

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_imm = nullptr;
static ID3D11DeviceContext* g_def = nullptr;
static bool g_active = false;
static bool g_outputsValid = false;
static bool g_work = false; // anything but state recorded since the last flush
static std::unordered_map<ID3D11Resource*, UINT> g_forwardMaps; // mapped dynamic buffers -> size
static std::unordered_map<UINT, uint64_t> g_fwdBySize; // DIAG: forwarded writes per buffer size since the last stats line
// Buffers whose last write on the deferred context was ours (patched right-eye
// data): after execution the game's own data must be put back.
static std::unordered_set<ID3D11Resource*> g_ourWrites;
// Plain resources the recorded work copies from: mapping one of those must
// flush first (the copy has to read the old contents). Others can be mapped freely.
static std::unordered_set<ID3D11Resource*> g_pendingSources;
// Dynamic vertex/index buffers aren't copied into the recording when no recorded
// draw has used them since the last flush: the replay then reads the game's
// current contents, which is exactly this write. (Journey rewrites ~40 MB of
// such buffers a frame; copying them all cost ~90 ms.) A buffer that is
// rewritten after a recorded draw used it forces a flush once and is copied
// from then on (g_copyAlways).
static ID3D11Resource* g_curVB[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
static ID3D11Resource* g_curIB = nullptr;
static std::unordered_set<ID3D11Resource*> g_usedSinceFlush;
static std::unordered_set<ID3D11Resource*> g_copyAlways;

static uint64_t g_statFlushes = 0, g_statFlushPresent = 0, g_statFlushMap = 0, g_statFlushOther = 0;
static uint64_t g_statRightDraws = 0, g_statForwarded = 0, g_statForwardedBytes = 0, g_statStagingSkips = 0, g_statUncopied = 0;

bool MirrorActive() { return g_active; }
ID3D11DeviceContext* MirrorContext() { return g_def; }
bool MirrorRightOutputsValid() { return g_active && g_outputsValid; }
void MirrorNoteDraw()
{
    g_work = true;
    g_statRightDraws++;
    for (ID3D11Resource* b : g_curVB) if (b) g_usedSinceFlush.insert(b);
    if (g_curIB) g_usedSinceFlush.insert(g_curIB);
}

static bool On() { return g_active && !ShadowBypassed(); }

// ---- flush ----
void MirrorFlush(const char* reason)
{
    if (!g_active || !g_work) return;
    ShadowBypass guard; // everything below is ours
    ID3D11CommandList* list = nullptr;
    HRESULT hr = g_def->FinishCommandList(TRUE, &list);
    if (FAILED(hr) || !list)
    {
        static int errs = 0;
        if (errs++ < 5) Log("[mirror] FinishCommandList failed: 0x%08X", (unsigned)hr);
        return;
    }
    g_imm->ExecuteCommandList(list, TRUE);
    list->Release();
    g_work = false;
    g_pendingSources.clear();
    g_usedSinceFlush.clear();
    for (ID3D11Resource* b : g_ourWrites) CaptureRestoreOriginal(g_imm, b);
    g_ourWrites.clear();
    g_statFlushes++;
    if (!strcmp(reason, "present")) g_statFlushPresent++;
    else if (!strcmp(reason, "map")) g_statFlushMap++;
    else g_statFlushOther++;
}

// ---- mirrored state ----
void MirrorSetShader(MirrorStage st, ID3D11DeviceChild* sh, ID3D11ClassInstance* const* inst, UINT n)
{
    if (!On()) return;
    ShadowBypass guard;
    switch (st)
    {
    case MS_VS: g_def->VSSetShader((ID3D11VertexShader*)sh, inst, n); break;
    case MS_PS: g_def->PSSetShader((ID3D11PixelShader*)sh, inst, n); break;
    case MS_GS: g_def->GSSetShader((ID3D11GeometryShader*)sh, inst, n); break;
    case MS_HS: g_def->HSSetShader((ID3D11HullShader*)sh, inst, n); break;
    case MS_DS: g_def->DSSetShader((ID3D11DomainShader*)sh, inst, n); break;
    }
}

void MirrorSetConstantBuffers(MirrorStage st, UINT start, UINT n, ID3D11Buffer* const* b)
{
    if (!On()) return;
    ShadowBypass guard;
    switch (st)
    {
    case MS_VS: g_def->VSSetConstantBuffers(start, n, b); break;
    case MS_PS: g_def->PSSetConstantBuffers(start, n, b); break;
    case MS_GS: g_def->GSSetConstantBuffers(start, n, b); break;
    case MS_HS: g_def->HSSetConstantBuffers(start, n, b); break;
    case MS_DS: g_def->DSSetConstantBuffers(start, n, b); break;
    }
}

void MirrorSetShaderResources(MirrorStage st, UINT start, UINT n, ID3D11ShaderResourceView* const* views)
{
    if (!On()) return;
    ShadowBypass guard;
    ID3D11ShaderResourceView* v[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11ShaderResourceView* twins[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    if (n > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) n = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    for (UINT i = 0; i < n; ++i)
    {
        ID3D11ShaderResourceView* orig = views ? views[i] : nullptr;
        twins[i] = ShadowTwinSRV(orig);
        v[i] = twins[i] ? twins[i] : orig;
    }
    switch (st)
    {
    case MS_VS: g_def->VSSetShaderResources(start, n, v); break;
    case MS_PS: g_def->PSSetShaderResources(start, n, v); break;
    case MS_GS: g_def->GSSetShaderResources(start, n, v); break;
    case MS_HS: g_def->HSSetShaderResources(start, n, v); break;
    case MS_DS: g_def->DSSetShaderResources(start, n, v); break;
    }
    for (UINT i = 0; i < n; ++i) if (twins[i]) twins[i]->Release();
}

// Twin outputs; the right eye only draws if every bound output has one.
static bool TwinOutputs(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
    ID3D11RenderTargetView** outRtv, ID3D11DepthStencilView** outDsv)
{
    bool any = false, missing = false;
    for (UINT i = 0; i < n && i < 8; ++i)
    {
        ID3D11RenderTargetView* v = rtvs ? rtvs[i] : nullptr;
        outRtv[i] = ShadowTwinRTV(v);
        if (v) { if (outRtv[i]) any = true; else missing = true; }
    }
    *outDsv = ShadowTwinDSV(dsv);
    if (dsv) { if (*outDsv) any = true; else missing = true; }
    return any && !missing;
}

void MirrorSetRenderTargets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    if (!On()) return;
    ShadowBypass guard;
    ID3D11RenderTargetView* t[8] = {};
    ID3D11DepthStencilView* td = nullptr;
    if (n > 8) n = 8;
    g_outputsValid = TwinOutputs(n, rtvs, dsv, t, &td);
    g_def->OMSetRenderTargets(n, t, td);
    for (UINT i = 0; i < n; ++i) if (t[i]) t[i]->Release();
    if (td) td->Release();
}

void MirrorSetRenderTargetsAndUAVs(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
    UINT uavStart, UINT numUavs, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    if (!On()) return;
    ShadowBypass guard;
    if (n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
    {
        g_def->OMSetRenderTargetsAndUnorderedAccessViews(n, nullptr, nullptr, uavStart, numUavs, uavs, counts);
        return;
    }
    ID3D11RenderTargetView* t[8] = {};
    ID3D11DepthStencilView* td = nullptr;
    if (n > 8) n = 8;
    g_outputsValid = TwinOutputs(n, rtvs, dsv, t, &td);
    g_def->OMSetRenderTargetsAndUnorderedAccessViews(n, t, td, uavStart, numUavs, uavs, counts);
    for (UINT i = 0; i < n; ++i) if (t[i]) t[i]->Release();
    if (td) td->Release();
}

void MirrorClearState()
{
    if (!On()) return;
    ShadowBypass guard;
    g_def->ClearState();
    g_outputsValid = false;
}

// ---- mirrored operations (twins where they exist) ----
void MirrorClearRTV(ID3D11RenderTargetView* v, const FLOAT color[4])
{
    if (!On()) return;
    ShadowBypass guard;
    if (ID3D11RenderTargetView* t = ShadowTwinRTV(v)) { g_def->ClearRenderTargetView(t, color); t->Release(); g_work = true; }
}

void MirrorClearDSV(ID3D11DepthStencilView* v, UINT flags, FLOAT depth, UINT8 stencil)
{
    if (!On()) return;
    ShadowBypass guard;
    if (ID3D11DepthStencilView* t = ShadowTwinDSV(v)) { g_def->ClearDepthStencilView(t, flags, depth, stencil); t->Release(); g_work = true; }
}

// Copies into a twinned resource go to its twin (from the source's twin if it
// has one). Copies between plain resources (uploads) are repeated, so the
// recorded draws see what the game had at that point. A copy of per-eye data
// into a shared resource is left alone (the right eye reads the left's, as
// before).
void MirrorCopyResource(ID3D11Resource* dst, ID3D11Resource* src)
{
    if (!On()) return;
    ShadowBypass guard;
    ID3D11Resource* td = ShadowOfResource(dst);
    ID3D11Resource* ts = ShadowOfResource(src);
    if (td) g_def->CopyResource(td, ts ? ts : src);
    else if (!ts) g_def->CopyResource(dst, src);
    if (!ts) g_pendingSources.insert(src);
    if (td || !ts) g_work = true;
    if (td) td->Release();
    if (ts) ts->Release();
}

void MirrorCopyRegion(ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ssub, const D3D11_BOX* box)
{
    if (!On()) return;
    ShadowBypass guard;
    ID3D11Resource* td = ShadowOfResource(dst);
    ID3D11Resource* ts = ShadowOfResource(src);
    if (td) g_def->CopySubresourceRegion(td, dsub, x, y, z, ts ? ts : src, ssub, box);
    else if (!ts) g_def->CopySubresourceRegion(dst, dsub, x, y, z, src, ssub, box);
    if (!ts) g_pendingSources.insert(src);
    if (td || !ts) g_work = true;
    if (td) td->Release();
    if (ts) ts->Release();
}

void MirrorResolve(ID3D11Resource* dst, UINT dsub, ID3D11Resource* src, UINT ssub, DXGI_FORMAT fmt)
{
    if (!On()) return;
    ShadowBypass guard;
    ID3D11Resource* td = ShadowOfResource(dst);
    ID3D11Resource* ts = ShadowOfResource(src);
    if (td) { g_def->ResolveSubresource(td, dsub, ts ? ts : src, ssub, fmt); g_work = true; }
    if (td) td->Release();
    if (ts) ts->Release();
}

void MirrorGenerateMips(ID3D11ShaderResourceView* v)
{
    if (!On()) return;
    ShadowBypass guard;
    if (ID3D11ShaderResourceView* t = ShadowTwinSRV(v)) { g_def->GenerateMips(t); t->Release(); g_work = true; }
}

void MirrorUpdateSubresource(ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch)
{
    if (!On()) return;
    ShadowBypass guard;
    ID3D11Resource* td = ShadowOfResource(dst);
    g_def->UpdateSubresource(td ? td : dst, sub, box, data, rowPitch, depthPitch);
    if (td) td->Release();
    else g_ourWrites.erase(dst); // the game's own data is now the last write
    g_work = true;
}

void MirrorBeforeMap(ID3D11Resource* r, D3D11_MAP type)
{
    if (!On() || !r) return;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE))
    {
        D3D11_BUFFER_DESC d = {};
        static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
        if (d.Usage == D3D11_USAGE_DYNAMIC)
        {
            bool geometryOnly = !(d.BindFlags & ~(D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER));
            if (geometryOnly && !g_copyAlways.count(r) && !g_ourWrites.count(r))
            {
                if (!g_usedSinceFlush.count(r)) { g_statUncopied++; return; }
                g_copyAlways.insert(r); // rewritten after a recorded draw used it
                MirrorFlush("rewrite");
            }
            g_forwardMaps[r] = d.ByteWidth;
            return;
        }
    }
    // Staging resources are only reachable by the recorded work through copies
    // from them; anything else mapped (e.g. a dynamic texture) may be sampled.
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC d = {}; static_cast<ID3D11Buffer*>(r)->GetDesc(&d); usage = d.Usage; }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC d = {}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&d); usage = d.Usage; }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) { D3D11_TEXTURE1D_DESC d = {}; static_cast<ID3D11Texture1D*>(r)->GetDesc(&d); usage = d.Usage; }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) { D3D11_TEXTURE3D_DESC d = {}; static_cast<ID3D11Texture3D*>(r)->GetDesc(&d); usage = d.Usage; }
    if (usage == D3D11_USAGE_STAGING && !g_pendingSources.count(r)) { g_statStagingSkips++; return; }
    MirrorFlush("map"); // recorded work must see the old contents
}

void MirrorUnmap(ID3D11Resource* r, const void* data)
{
    auto it = g_forwardMaps.find(r);
    if (it == g_forwardMaps.end()) return;
    UINT size = it->second;
    g_forwardMaps.erase(it);
    if (!On() || !data) return;
    ShadowBypass guard;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_def->Map(r, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, data, size);
        g_def->Unmap(r, 0);
        g_statForwarded++;
        g_statForwardedBytes += size;
        g_fwdBySize[size]++;
    }
    g_ourWrites.erase(r);
    g_work = true;
}

void MirrorWriteBuffer(ID3D11Resource* buf, const void* data, UINT size, D3D11_USAGE usage)
{
    if (!g_active || !buf) return;
    ShadowBypass guard;
    if (usage == D3D11_USAGE_DYNAMIC)
    {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(g_def->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        memcpy(m.pData, data, size);
        g_def->Unmap(buf, 0);
    }
    else if (usage == D3D11_USAGE_DEFAULT)
        g_def->UpdateSubresource(buf, 0, nullptr, data, 0, 0);
    else
        return;
    g_ourWrites.insert(buf);
    g_work = true;
}

// ---- state setters not hooked elsewhere: mirrored as-is ----
static bool Game(ID3D11DeviceContext* c) { return g_active && c == g_imm && !ShadowBypassed(); }

using IASetInputLayout_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11InputLayout*);
using IASetVertexBuffers_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
using IASetIndexBuffer_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);
using IASetTopology_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, D3D11_PRIMITIVE_TOPOLOGY);
using SetSamplers_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
using SetCBs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using SetGS_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11GeometryShader*, ID3D11ClassInstance* const*, UINT);
using SetHS_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11HullShader*, ID3D11ClassInstance* const*, UINT);
using SetDS_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DomainShader*, ID3D11ClassInstance* const*, UINT);
using OMSetBlend_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
using OMSetDS_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using RSSetState_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RasterizerState*);
using RSSetViewports_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissors_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);

static IASetInputLayout_t realIASetInputLayout;
static IASetVertexBuffers_t realIASetVertexBuffers;
static IASetIndexBuffer_t realIASetIndexBuffer;
static IASetTopology_t realIASetPrimitiveTopology;
static SetSamplers_t realVSSetSamplers, realPSSetSamplers, realGSSetSamplers, realHSSetSamplers, realDSSetSamplers;
static SetCBs_t realGSSetCBs, realHSSetCBs, realDSSetCBs;
static SetGS_t realGSSetShader;
static SetHS_t realHSSetShader;
static SetDS_t realDSSetShader;
static OMSetBlend_t realOMSetBlendState;
static OMSetDS_t realOMSetDepthStencilState;
static RSSetState_t realRSSetState;
static RSSetViewports_t realRSSetViewports;
static RSSetScissors_t realRSSetScissorRects;

static void STDMETHODCALLTYPE Hook_IASetInputLayout(ID3D11DeviceContext* ctx, ID3D11InputLayout* l)
{
    realIASetInputLayout(ctx, l);
    if (Game(ctx)) { ShadowBypass g; g_def->IASetInputLayout(l); }
}
static void STDMETHODCALLTYPE Hook_IASetVertexBuffers(ID3D11DeviceContext* ctx, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st, const UINT* o)
{
    realIASetVertexBuffers(ctx, s, n, b, st, o);
    if (ctx == g_imm) for (UINT i = 0; i < n && s + i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++i) g_curVB[s + i] = b ? b[i] : nullptr;
    if (Game(ctx)) { ShadowBypass g; g_def->IASetVertexBuffers(s, n, b, st, o); }
}
static void STDMETHODCALLTYPE Hook_IASetIndexBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* b, DXGI_FORMAT f, UINT o)
{
    realIASetIndexBuffer(ctx, b, f, o);
    if (ctx == g_imm) g_curIB = b;
    if (Game(ctx)) { ShadowBypass g; g_def->IASetIndexBuffer(b, f, o); }
}
static void STDMETHODCALLTYPE Hook_IASetPrimitiveTopology(ID3D11DeviceContext* ctx, D3D11_PRIMITIVE_TOPOLOGY t)
{
    realIASetPrimitiveTopology(ctx, t);
    if (Game(ctx)) { ShadowBypass g; g_def->IASetPrimitiveTopology(t); }
}
#define SAMPLER_HOOK(ST) \
static void STDMETHODCALLTYPE Hook_##ST##SetSamplers(ID3D11DeviceContext* ctx, UINT s, UINT n, ID3D11SamplerState* const* v) \
{ \
    real##ST##SetSamplers(ctx, s, n, v); \
    if (Game(ctx)) { ShadowBypass g; g_def->ST##SetSamplers(s, n, v); } \
}
SAMPLER_HOOK(VS) SAMPLER_HOOK(PS) SAMPLER_HOOK(GS) SAMPLER_HOOK(HS) SAMPLER_HOOK(DS)
#define CB_HOOK(ST) \
static void STDMETHODCALLTYPE Hook_##ST##SetCBs(ID3D11DeviceContext* ctx, UINT s, UINT n, ID3D11Buffer* const* b) \
{ \
    real##ST##SetCBs(ctx, s, n, b); \
    if (Game(ctx)) { ShadowBypass g; g_def->ST##SetConstantBuffers(s, n, b); } \
}
CB_HOOK(GS) CB_HOOK(HS) CB_HOOK(DS)
static void STDMETHODCALLTYPE Hook_GSSetShader(ID3D11DeviceContext* ctx, ID3D11GeometryShader* sh, ID3D11ClassInstance* const* i, UINT n)
{
    realGSSetShader(ctx, sh, i, n);
    if (Game(ctx)) { ShadowBypass g; g_def->GSSetShader(sh, i, n); }
}
static void STDMETHODCALLTYPE Hook_HSSetShader(ID3D11DeviceContext* ctx, ID3D11HullShader* sh, ID3D11ClassInstance* const* i, UINT n)
{
    realHSSetShader(ctx, sh, i, n);
    if (Game(ctx)) { ShadowBypass g; g_def->HSSetShader(sh, i, n); }
}
static void STDMETHODCALLTYPE Hook_DSSetShader(ID3D11DeviceContext* ctx, ID3D11DomainShader* sh, ID3D11ClassInstance* const* i, UINT n)
{
    realDSSetShader(ctx, sh, i, n);
    if (Game(ctx)) { ShadowBypass g; g_def->DSSetShader(sh, i, n); }
}
static void STDMETHODCALLTYPE Hook_OMSetBlendState(ID3D11DeviceContext* ctx, ID3D11BlendState* s, const FLOAT f[4], UINT m)
{
    realOMSetBlendState(ctx, s, f, m);
    if (Game(ctx)) { ShadowBypass g; g_def->OMSetBlendState(s, f, m); }
}
static void STDMETHODCALLTYPE Hook_OMSetDepthStencilState(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* s, UINT r)
{
    realOMSetDepthStencilState(ctx, s, r);
    if (Game(ctx)) { ShadowBypass g; g_def->OMSetDepthStencilState(s, r); }
}
static void STDMETHODCALLTYPE Hook_RSSetState(ID3D11DeviceContext* ctx, ID3D11RasterizerState* s)
{
    realRSSetState(ctx, s);
    if (Game(ctx)) { ShadowBypass g; g_def->RSSetState(s); }
}
static void STDMETHODCALLTYPE Hook_RSSetViewports(ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* v)
{
    realRSSetViewports(ctx, n, v);
    if (Game(ctx)) { ShadowBypass g; g_def->RSSetViewports(n, v); }
}
static void STDMETHODCALLTYPE Hook_RSSetScissorRects(ID3D11DeviceContext* ctx, UINT n, const D3D11_RECT* r)
{
    realRSSetScissorRects(ctx, n, r);
    if (Game(ctx)) { ShadowBypass g; g_def->RSSetScissorRects(n, r); }
}

void MirrorLogStats()
{
    if (!g_active) return;
    Log("[mirror] flushes %llu (present %llu, map %llu, other %llu); right draws %llu; forwarded buffer writes %llu (%llu KB), not needed %llu; staging maps without flush %llu",
        g_statFlushes, g_statFlushPresent, g_statFlushMap, g_statFlushOther, g_statRightDraws,
        g_statForwarded, g_statForwardedBytes / 1024, g_statUncopied, g_statStagingSkips);
    // DIAG: the buffer sizes that cost the most since the last line
    std::vector<std::pair<uint64_t, UINT>> top;
    for (auto& kv : g_fwdBySize) top.push_back({ kv.second * kv.first, kv.first });
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 6; ++i)
        Log("[mirror]   forwarded %u-byte buffer x%llu (%llu KB)", top[i].second,
            (unsigned long long)g_fwdBySize[top[i].second], (unsigned long long)(top[i].first / 1024));
    g_fwdBySize.clear();
}

void MirrorInstall(ID3D11Device* dev, ID3D11DeviceContext* imm, const wchar_t* ini)
{
    if (!GetPrivateProfileIntW(L"stereo", L"batch", 1, ini))
    {
        Log("[mirror] off ([stereo] batch=0): per-draw double render");
        return;
    }
    g_dev = dev;
    g_imm = imm;
    HRESULT hr = dev->CreateDeferredContext(0, &g_def);
    if (FAILED(hr) || !g_def)
    {
        Log("[mirror] CreateDeferredContext failed (0x%08X): per-draw double render", (unsigned)hr);
        return;
    }
    void** cv = *reinterpret_cast<void***>(imm);
    bool ok = true;
    ok &= HookMethod(cv, 17, (void*)&Hook_IASetInputLayout, (void**)&realIASetInputLayout, "IASetInputLayout");
    ok &= HookMethod(cv, 18, (void*)&Hook_IASetVertexBuffers, (void**)&realIASetVertexBuffers, "IASetVertexBuffers");
    ok &= HookMethod(cv, 19, (void*)&Hook_IASetIndexBuffer, (void**)&realIASetIndexBuffer, "IASetIndexBuffer");
    ok &= HookMethod(cv, 24, (void*)&Hook_IASetPrimitiveTopology, (void**)&realIASetPrimitiveTopology, "IASetPrimitiveTopology");
    ok &= HookMethod(cv, 26, (void*)&Hook_VSSetSamplers, (void**)&realVSSetSamplers, "VSSetSamplers");
    ok &= HookMethod(cv, 10, (void*)&Hook_PSSetSamplers, (void**)&realPSSetSamplers, "PSSetSamplers");
    ok &= HookMethod(cv, 32, (void*)&Hook_GSSetSamplers, (void**)&realGSSetSamplers, "GSSetSamplers");
    ok &= HookMethod(cv, 61, (void*)&Hook_HSSetSamplers, (void**)&realHSSetSamplers, "HSSetSamplers");
    ok &= HookMethod(cv, 65, (void*)&Hook_DSSetSamplers, (void**)&realDSSetSamplers, "DSSetSamplers");
    ok &= HookMethod(cv, 22, (void*)&Hook_GSSetCBs, (void**)&realGSSetCBs, "GSSetConstantBuffers");
    ok &= HookMethod(cv, 62, (void*)&Hook_HSSetCBs, (void**)&realHSSetCBs, "HSSetConstantBuffers");
    ok &= HookMethod(cv, 66, (void*)&Hook_DSSetCBs, (void**)&realDSSetCBs, "DSSetConstantBuffers");
    ok &= HookMethod(cv, 23, (void*)&Hook_GSSetShader, (void**)&realGSSetShader, "GSSetShader");
    ok &= HookMethod(cv, 60, (void*)&Hook_HSSetShader, (void**)&realHSSetShader, "HSSetShader");
    ok &= HookMethod(cv, 64, (void*)&Hook_DSSetShader, (void**)&realDSSetShader, "DSSetShader");
    ok &= HookMethod(cv, 35, (void*)&Hook_OMSetBlendState, (void**)&realOMSetBlendState, "OMSetBlendState");
    ok &= HookMethod(cv, 36, (void*)&Hook_OMSetDepthStencilState, (void**)&realOMSetDepthStencilState, "OMSetDepthStencilState");
    ok &= HookMethod(cv, 43, (void*)&Hook_RSSetState, (void**)&realRSSetState, "RSSetState");
    ok &= HookMethod(cv, 44, (void*)&Hook_RSSetViewports, (void**)&realRSSetViewports, "RSSetViewports");
    ok &= HookMethod(cv, 45, (void*)&Hook_RSSetScissorRects, (void**)&realRSSetScissorRects, "RSSetScissorRects");
    if (!ok)
    {
        Log("[mirror] some state hooks failed: per-draw double render");
        return; // the hooks that did install only mirror while g_active
    }
    g_active = true;
    Log("[mirror] batched double render: right eye recorded on deferred context %p", g_def);
}
