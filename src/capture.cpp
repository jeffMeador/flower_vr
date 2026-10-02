#include "capture.h"
#include "log.h"
#include "mat4.h"
#include "stereo.h"
#include "shadow.h"
#include "mirror.h"
#include "vhook.h"
#include "game.h"
#include <cmath>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <cstring>
#include <cctype>
#include <string>

// What to do with one cbuffer variable when rendering a given eye.
// OldClip = previous frame's MVP (motion blur); with motion blur off in VR it's
// set equal to this frame's (patched) MVP, so nothing appears to move.
enum class PatchKind { Clip, OldClip, View, EyePos, LensFov, PointSize };

struct PatchVar
{
    PatchKind kind;
    UINT offset;
};

struct ShaderOffsets
{
    bool hasModel = false;   UINT modelOffset = 0;
    bool hasMVP = false;     UINT mvpOffset = 0;
    std::vector<PatchVar> patches;
    // LensDistortion_vs: fisheye re-projection of the finished frame by "fov";
    // neutralized in VR (it warps the headset image, worse at speed).
    bool hasLensR = false; UINT lensROffset = 0;
    bool hasLensMaxR = false; UINT lensMaxROffset = 0;
    UINT cbSize = 0;
    int  id = 0;
};

// CPU-side copy of what the game last wrote into a (small) constant buffer.
struct BufferShadow
{
    std::vector<uint8_t> data;
    uint64_t gen = 0;            // bumped on every game write
    bool valid = false;          // false after writes we couldn't mirror fully
    bool infoKnown = false;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;

    // What the GPU copy currently holds relative to `data`.
    uint64_t patchedGen = ~0ull;
    const ShaderOffsets* patchedLayout = nullptr;
    uint64_t patchedKey = 0;
};

static std::unordered_map<ID3D11VertexShader*, ShaderOffsets> g_offsets;
static std::unordered_map<ID3D11Resource*, BufferShadow> g_cbShadow;
static std::unordered_map<ID3D11Resource*, void*> g_activeMap;
static int g_nextShaderId = 1;

struct ContextState
{
    ID3D11VertexShader* currentVS = nullptr;
    ID3D11Resource* currentSlot0CB = nullptr;
    ID3D11PixelShader* currentPS = nullptr;
    ID3D11Resource* psCB[4] = {};
};

// Pixel-shader constants overridden while in VR, per shader: depth of field
// (blurZRanges slopes -> 0) and glare trails (glare history off).
struct PixelOverride
{
    enum Kind { DepthOfField, Trails, Fixed } kind; // Fixed: [psoverride] name=value from vrmod.ini
    UINT slot;
    UINT offset;
    float value;
};
static std::unordered_map<ID3D11PixelShader*, std::vector<PixelOverride>> g_psOverrides;

// [psoverride] in vrmod.ini: <constant name>=<float>, lower-cased names. Pins any
// pixel-shader constant while in stereo (for tuning another game's effects).
static std::unordered_map<std::string, float> LoadFixedOverrides()
{
    std::unordered_map<std::string, float> m;
    wchar_t ini[MAX_PATH];
    extern wchar_t g_dllDir[MAX_PATH];
    swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
    wchar_t buf[4096] = {};
    DWORD n = GetPrivateProfileSectionW(L"psoverride", buf, 4096, ini);
    for (const wchar_t* p = buf; p < buf + n && *p; p += wcslen(p) + 1)
    {
        const wchar_t* eq = wcschr(p, L'=');
        if (!eq) continue;
        std::string name;
        for (const wchar_t* c = p; c < eq; ++c) name += (char)towlower(*c);
        m[name] = (float)_wtof(eq + 1);
        Log("[capture] psoverride %s = %g", name.c_str(), m[name]);
    }
    return m;
}
static std::unordered_map<ID3D11DeviceContext*, ContextState> g_contextState;

static std::atomic<uint64_t> g_captureFrame{ 0 };
static ID3D11Resource* g_sceneDepth = nullptr;   // AddRef'd
static uint64_t g_sceneDepthFrame = ~0ull;

ID3D11Resource* CaptureSceneDepth()
{
    if (g_sceneDepth) g_sceneDepth->AddRef();
    return g_sceneDepth;
}
static int g_logBudget = 0;
static ID3D11DeviceContext* g_immediateCtx = nullptr;
// The game's own calls on its immediate context (not ours, not a deferred one).
static bool GameCall(ID3D11DeviceContext* ctx) { return ctx == g_immediateCtx && !ShadowBypassed(); }
static bool g_vpObservedThisFrame = false;

static std::atomic<uint64_t> g_countVSSetShader{ 0 };
static std::atomic<uint64_t> g_countVSSetCB{ 0 };
static std::atomic<uint64_t> g_countMap{ 0 };
static std::atomic<uint64_t> g_countUnmap{ 0 };
static std::atomic<uint64_t> g_countUpdateSubresource{ 0 };
static std::atomic<uint64_t> g_countDrawIndexed{ 0 };
static std::atomic<uint64_t> g_countDraw{ 0 };
static std::atomic<uint64_t> g_countDrawIndexedInstanced{ 0 };
static std::atomic<uint64_t> g_countDrawInstanced{ 0 };

static uint64_t g_mapTypeCount[6] = {};
static uint64_t g_statPatched = 0, g_statPatchCached = 0, g_statNoShadow = 0, g_statOrtho = 0, g_statNoVars = 0;

using CreateVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using VSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
using VSSetConstantBuffers_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using Map_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using Unmap_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresource_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using Draw_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

static CreateVertexShader_t g_realCreateVertexShader = nullptr;
static VSSetShader_t g_realVSSetShader = nullptr;
static VSSetConstantBuffers_t g_realVSSetConstantBuffers = nullptr;
static Map_t g_realMap = nullptr;
static Unmap_t g_realUnmap = nullptr;
static UpdateSubresource_t g_realUpdateSubresource = nullptr;
static DrawIndexed_t g_realDrawIndexed = nullptr;
static Draw_t g_realDraw = nullptr;
static DrawIndexedInstanced_t g_realDrawIndexedInstanced = nullptr;
static DrawInstanced_t g_realDrawInstanced = nullptr;

static std::string Lower(const char* s)
{
    std::string a(s);
    for (auto& c : a) c = (char)tolower((unsigned char)c);
    return a;
}

static void ReflectAndCacheOffsets(ID3D11VertexShader* shader, const void* bytecode, SIZE_T len)
{
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(bytecode, len, IID_ID3D11ShaderReflection, (void**)&refl)) || !refl)
        return;

    D3D11_SHADER_DESC shaderDesc = {};
    refl->GetDesc(&shaderDesc);

    ShaderOffsets offsets;
    offsets.id = g_nextShaderId++;
    std::string names;

    for (UINT cb = 0; cb < shaderDesc.ConstantBuffers; ++cb)
    {
        ID3D11ShaderReflectionConstantBuffer* cbRefl = refl->GetConstantBufferByIndex(cb);
        D3D11_SHADER_BUFFER_DESC cbDesc = {};
        cbRefl->GetDesc(&cbDesc);

        D3D11_SHADER_INPUT_BIND_DESC bindDesc = {};
        if (FAILED(refl->GetResourceBindingDescByName(cbDesc.Name, &bindDesc)))
            continue;
        if (bindDesc.BindPoint != 0)
            continue;

        offsets.cbSize = cbDesc.Size;

        for (UINT v = 0; v < cbDesc.Variables; ++v)
        {
            ID3D11ShaderReflectionVariable* varRefl = cbRefl->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC varDesc = {};
            varRefl->GetDesc(&varDesc);
            std::string n = Lower(varDesc.Name);

            // Exact names only: substring matching confused "oldModelViewProj"
            // with "modelViewProj" (MotionBlur shaders have both).
            if (n == "modelviewproj")
            {
                offsets.hasMVP = true;
                offsets.mvpOffset = varDesc.StartOffset;
                offsets.patches.push_back({ PatchKind::Clip, varDesc.StartOffset });
            }
            else if (n == "oldmodelviewproj")
                offsets.patches.push_back({ PatchKind::OldClip, varDesc.StartOffset });
            else if (n == "viewprojection" || n == "viewprojmtx")
                offsets.patches.push_back({ PatchKind::Clip, varDesc.StartOffset });
            else if (n == "modelview")
                offsets.patches.push_back({ PatchKind::View, varDesc.StartOffset });
            else if (n == "fov")
                offsets.patches.push_back({ PatchKind::LensFov, varDesc.StartOffset });
            else if (n == "r") { offsets.hasLensR = true; offsets.lensROffset = varDesc.StartOffset; }
            else if (n == "maxradius") { offsets.hasLensMaxR = true; offsets.lensMaxROffset = varDesc.StartOffset; }
            else if (n == "pointsize")
                // FillerFlower sparkles: screen-space dots (size added in clip
                // space), which look far too big in the headset. Scaled in VR.
                offsets.patches.push_back({ PatchKind::PointSize, varDesc.StartOffset });
            else if (n == "eyepositionws")
                offsets.patches.push_back({ PatchKind::EyePos, varDesc.StartOffset });
            else if (n == "model")
            {
                offsets.hasModel = true;
                offsets.modelOffset = varDesc.StartOffset;
            }
            else
                continue;
            names += varDesc.Name;
            names += ' ';
        }
    }
    refl->Release();

    // Drop patches that don't fit in the reflected cbuffer (defensive).
    std::vector<PatchVar> kept;
    for (auto& p : offsets.patches)
    {
        if (p.offset + (p.kind == PatchKind::EyePos ? 12u : (p.kind == PatchKind::LensFov || p.kind == PatchKind::PointSize) ? 4u : 64u) > offsets.cbSize)
            continue;
        // A lone "fov" without R/maxRadius isn't the lens shader.
        if (p.kind == PatchKind::LensFov && !(offsets.hasLensR && offsets.hasLensMaxR))
            continue;
        kept.push_back(p);
    }
    offsets.patches.swap(kept);

    g_offsets[shader] = offsets;
    // DXBC checksum (bytes 4..11) identifies the game's .cso file offline.
    const uint8_t* bc = static_cast<const uint8_t*>(bytecode);
    unsigned long long sum = 0;
    if (len >= 12) memcpy(&sum, bc + 4, 8);
    Log("[capture] shader #%d reflected: cbSize=%u model=%d mvp=%d patches=%zu [%s] dxbc %016llX",
        offsets.id, offsets.cbSize, offsets.hasModel, offsets.hasMVP, offsets.patches.size(), names.c_str(), sum);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateVertexShader(ID3D11Device* self, const void* bytecode, SIZE_T len, ID3D11ClassLinkage* linkage, ID3D11VertexShader** out)
{
    HRESULT hr = g_realCreateVertexShader(self, bytecode, len, linkage, out);
    if (SUCCEEDED(hr) && out && *out)
        ReflectAndCacheOffsets(*out, bytecode, len);
    return hr;
}

static void STDMETHODCALLTYPE Hook_VSSetShader(ID3D11DeviceContext* self, ID3D11VertexShader* shader, ID3D11ClassInstance* const* instances, UINT numInstances)
{
    if (!GameCall(self)) { g_realVSSetShader(self, shader, instances, numInstances); return; }
    g_countVSSetShader++;
    g_contextState[self].currentVS = shader;
    g_realVSSetShader(self, shader, instances, numInstances);
    MirrorSetShader(MS_VS, shader, instances, numInstances);
}

static void STDMETHODCALLTYPE Hook_VSSetConstantBuffers(ID3D11DeviceContext* self, UINT startSlot, UINT numBuffers, ID3D11Buffer* const* buffers)
{
    if (!GameCall(self)) { g_realVSSetConstantBuffers(self, startSlot, numBuffers, buffers); return; }
    g_countVSSetCB++;
    if (startSlot <= 0 && 0 < startSlot + numBuffers)
        g_contextState[self].currentSlot0CB = buffers ? buffers[0 - startSlot] : nullptr;
    g_realVSSetConstantBuffers(self, startSlot, numBuffers, buffers);
    MirrorSetConstantBuffers(MS_VS, startSlot, numBuffers, buffers);
}

// ---- pixel shaders: find depth-of-field constants ----
using CreatePixelShader_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using PSSetConstantBuffers_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
static CreatePixelShader_t g_realCreatePixelShader = nullptr;
static PSSetShader_t g_realPSSetShader = nullptr;
static PSSetConstantBuffers_t g_realPSSetConstantBuffers = nullptr;

static HRESULT STDMETHODCALLTYPE Hook_CreatePixelShader(ID3D11Device* self, const void* bytecode, SIZE_T len, ID3D11ClassLinkage* linkage, ID3D11PixelShader** out)
{
    HRESULT hr = g_realCreatePixelShader(self, bytecode, len, linkage, out);
    if (FAILED(hr) || !out || !*out) return hr;
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(bytecode, len, IID_ID3D11ShaderReflection, (void**)&refl)) || !refl) return hr;
    D3D11_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    std::vector<PixelOverride> ov;
    bool glareInt = false, accumInt = false;
    UINT glareSlot = 0, glareOff = 0, accumOff = 0;
    for (UINT cb = 0; cb < sd.ConstantBuffers; ++cb)
    {
        ID3D11ShaderReflectionConstantBuffer* cbr = refl->GetConstantBufferByIndex(cb);
        D3D11_SHADER_BUFFER_DESC cbd = {};
        cbr->GetDesc(&cbd);
        D3D11_SHADER_INPUT_BIND_DESC bd = {};
        if (FAILED(refl->GetResourceBindingDescByName(cbd.Name, &bd)) || bd.BindPoint >= 4) continue;
        for (UINT v = 0; v < cbd.Variables; ++v)
        {
            D3D11_SHADER_VARIABLE_DESC vd = {};
            cbr->GetVariableByIndex(v)->GetDesc(&vd);
            if (!(vd.uFlags & D3D_SVF_USED)) continue;
            std::string n = Lower(vd.Name);
            static const std::unordered_map<std::string, float> fixed = LoadFixedOverrides();
            auto fx = fixed.find(n);
            if (fx != fixed.end())
                ov.push_back({ PixelOverride::Fixed, bd.BindPoint, vd.StartOffset, fx->second });
            if (n == "blurzranges")
            {
                // Depth of field: blur = clamp(max((Z-x)*y, (Z-z)*w)): .y is the near
                // ramp, .w the far one. Off -> both 0; on -> scaled by dofNear/dofFar.
                // (value: 0 = near entry, 1 = far entry)
                ov.push_back({ PixelOverride::DepthOfField, bd.BindPoint, vd.StartOffset + 4, 0.0f });
                ov.push_back({ PixelOverride::DepthOfField, bd.BindPoint, vd.StartOffset + 12, 1.0f });
                Log("[capture] pixel shader with depth of field: blurZRanges in slot %u @%u", bd.BindPoint, vd.StartOffset);
            }
            else if (n == "glareint") { glareInt = true; glareSlot = bd.BindPoint; glareOff = vd.StartOffset; }
            else if (n == "accumint") { accumInt = true; accumOff = vd.StartOffset; }
        }
    }
    if (glareInt && accumInt)
    {
        // Glare composite: out = glare*glareInt + previousFramesGlare*accumInt
        // (0.3/0.7, i.e. the same steady brightness as glare alone). The screen-
        // space history lags whenever the view turns: in VR that showed as
        // wobbly 'water' patches during head motion. No history, full glare.
        ov.push_back({ PixelOverride::Trails, glareSlot, glareOff, 1.0f });
        ov.push_back({ PixelOverride::Trails, glareSlot, accumOff, 0.0f });
        Log("[capture] pixel shader with glare accumulation (slot %u @%u/@%u)", glareSlot, glareOff, accumOff);
    }
    if (!ov.empty()) g_psOverrides[*out] = ov;
    refl->Release();
    return hr;
}

static void STDMETHODCALLTYPE Hook_PSSetShader(ID3D11DeviceContext* self, ID3D11PixelShader* shader, ID3D11ClassInstance* const* inst, UINT n)
{
    if (!GameCall(self)) { g_realPSSetShader(self, shader, inst, n); return; }
    g_contextState[self].currentPS = shader;
    g_realPSSetShader(self, shader, inst, n);
    MirrorSetShader(MS_PS, shader, inst, n);
}

static void STDMETHODCALLTYPE Hook_PSSetConstantBuffers(ID3D11DeviceContext* self, UINT start, UINT n, ID3D11Buffer* const* buffers)
{
    if (!GameCall(self)) { g_realPSSetConstantBuffers(self, start, n, buffers); return; }
    ContextState& st = g_contextState[self];
    for (UINT i = 0; i < n; ++i)
        if (start + i < 4) st.psCB[start + i] = buffers ? buffers[i] : nullptr;
    g_realPSSetConstantBuffers(self, start, n, buffers);
    MirrorSetConstantBuffers(MS_PS, start, n, buffers);
}

static void RecordGameWrite(ID3D11Resource* resource, const void* src)
{
    D3D11_RESOURCE_DIMENSION dim;
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER)
        return;

    D3D11_BUFFER_DESC desc = {};
    reinterpret_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || desc.ByteWidth == 0 || desc.ByteWidth > 4096)
        return;

    BufferShadow& s = g_cbShadow[resource];
    s.infoKnown = true;
    s.usage = desc.Usage;
    s.gen++;
    if (src)
    {
        s.data.resize(desc.ByteWidth);
        memcpy(s.data.data(), src, desc.ByteWidth);
        s.valid = true;
    }
    else
        s.valid = false;
}

static HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT sub, D3D11_MAP mapType, UINT flags, D3D11_MAPPED_SUBRESOURCE* out)
{
    if (!GameCall(self)) return g_realMap(self, resource, sub, mapType, flags, out);
    g_countMap++;
    MirrorBeforeMap(resource, mapType);
    HRESULT hr = g_realMap(self, resource, sub, mapType, flags, out);
    if (SUCCEEDED(hr) && out && mapType != D3D11_MAP_READ)
    {
        if ((unsigned)mapType < 6) g_mapTypeCount[mapType]++;
        g_activeMap[resource] = out->pData;
    }
    return hr;
}

static void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT sub)
{
    if (!GameCall(self)) { g_realUnmap(self, resource, sub); return; }
    g_countUnmap++;
    auto it = g_activeMap.find(resource);
    if (it != g_activeMap.end())
    {
        RecordGameWrite(resource, it->second);
        MirrorUnmap(resource, it->second); // before the real Unmap: the pointer dies with it
        g_activeMap.erase(it);
    }
    g_realUnmap(self, resource, sub);
}

static void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, const D3D11_BOX* box, const void* src, UINT rowPitch, UINT depthPitch)
{
    if (!GameCall(self)) { g_realUpdateSubresource(self, dst, dstSub, box, src, rowPitch, depthPitch); return; }
    g_countUpdateSubresource++;
    if (dstSub == 0)
        RecordGameWrite(dst, box ? nullptr : src); // partial updates: can't mirror, mark invalid
    g_realUpdateSubresource(self, dst, dstSub, box, src, rowPitch, depthPitch);
    ShadowOnUpdateSubresource(self, dst, dstSub, box, src, rowPitch, depthPitch);
}

static void WriteBuffer(ID3D11DeviceContext* ctx, ID3D11Resource* buf, const BufferShadow& s, const void* data)
{
    if (s.usage == D3D11_USAGE_DYNAMIC)
    {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(g_realMap(ctx, buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            memcpy(m.pData, data, s.data.size());
            g_realUnmap(ctx, buf, 0);
        }
    }
    else if (s.usage == D3D11_USAGE_DEFAULT)
        g_realUpdateSubresource(ctx, buf, 0, nullptr, data, 0, 0);
}

static bool BuildPatched(const ShaderOffsets& off, const std::vector<uint8_t>& orig, std::vector<uint8_t>& patched);
static void FinishPrepareDraw(ID3D11DeviceContext* self, ContextState& state, const ShaderOffsets& off, BufferShadow& s,
    const std::vector<uint8_t>& orig, const std::vector<uint8_t>& patched, bool any, bool gpuIsOriginal, uint64_t key);

// Before each draw: recover the camera once per frame, then rewrite the bound
// slot-0 cbuffer with this eye's matrices (built from the game's original data).
static void PrepareDraw(ID3D11DeviceContext* self)
{
    ContextState& state = g_contextState[self];
    auto offIt = g_offsets.find(state.currentVS);
    if (offIt == g_offsets.end()) return;
    const ShaderOffsets& off = offIt->second;
    if (off.patches.empty()) { g_statNoVars++; return; }

    auto shIt = g_cbShadow.find(state.currentSlot0CB);
    if (shIt == g_cbShadow.end() || !shIt->second.valid) { g_statNoShadow++; return; }
    BufferShadow& s = shIt->second;
    const std::vector<uint8_t>& orig = s.data;

    if (!g_vpObservedThisFrame && off.hasModel && off.hasMVP &&
        off.modelOffset + 64 <= orig.size() && off.mvpOffset + 64 <= orig.size())
    {
        Mat4 model, modelInv, mvp;
        memcpy(&model, orig.data() + off.modelOffset, 64);
        memcpy(&mvp, orig.data() + off.mvpOffset, 64);
        if (Mat4Inverse(model, modelInv) && IsPerspective(mvp))
        {
            Mat4 viewProj = Mat4Mul(mvp, modelInv); // column-vector: MVP = VP * Model
            StereoObserveViewProj(viewProj);
            g_vpObservedThisFrame = true;
            if (g_logBudget > 0)
            {
                auto dump = [](const char* name, const Mat4& m)
                {
                    Log("  %s = [%.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f]", name,
                        m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3],
                        m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3], m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
                };
                Log("[capture] shader#%d frame=%llu matrices:", off.id, (unsigned long long)g_captureFrame.load());
                dump("model", model);
                dump("mvp  ", mvp);
                dump("VP   ", viewProj);
                g_logBudget--;
            }
        }
    }

    uint64_t key = StereoPatchKey();

    // GPU copy already matches what we want?
    bool gpuIsOriginal = s.patchedGen != s.gen;
    if (key == 0 && gpuIsOriginal) return;
    if (!gpuIsOriginal && s.patchedLayout == &off && s.patchedKey == key)
    {
        g_statPatchCached++;
        return;
    }

    static std::vector<uint8_t> patched;
    bool any = key != 0 && BuildPatched(off, orig, patched);
    FinishPrepareDraw(self, state, off, s, orig, patched, any, gpuIsOriginal, key);
}

// This eye's version of a slot-0 constant buffer (camera matrices, eye
// position, lens). False if nothing needed patching.
static bool BuildPatched(const ShaderOffsets& off, const std::vector<uint8_t>& orig, std::vector<uint8_t>& patched)
{
    float eyeOff[3];
    bool eyePos = StereoWorldEyeOffset(eyeOff);
    patched = orig;
    bool any = false;
    {
        for (const PatchVar& p : off.patches)
        {
            if (p.offset + (p.kind == PatchKind::EyePos ? 12u : (p.kind == PatchKind::LensFov || p.kind == PatchKind::PointSize) ? 4u : 64u) > patched.size()) continue;
            float* f = reinterpret_cast<float*>(patched.data() + p.offset);
            switch (p.kind)
            {
            case PatchKind::Clip:
            {
                Mat4 m; memcpy(&m, f, 64);
                if (!IsPerspective(m)) { g_statOrtho++; break; }
                StereoPatchClip(f);
                any = true;
                break;
            }
            case PatchKind::OldClip:
            {
                Mat4 m; memcpy(&m, f, 64);
                if (!IsPerspective(m)) { g_statOrtho++; break; }
                if (!Stereo().motionBlur && off.hasMVP && off.mvpOffset + 64 <= patched.size())
                {
                    // Previous-frame MVP := this frame's MVP for this eye -> no motion blur.
                    memcpy(f, orig.data() + off.mvpOffset, 64);
                }
                StereoPatchClip(f);
                any = true;
                break;
            }
            case PatchKind::View:
                StereoPatchView(f);
                any = true;
                break;
            case PatchKind::PointSize:
                if (Stereo().sparkleSize != 1.0f) { f[0] *= Stereo().sparkleSize; any = true; }
                break;
            case PatchKind::EyePos:
                if (eyePos) { f[0] += eyeOff[0]; f[1] += eyeOff[1]; f[2] += eyeOff[2]; any = true; }
                break;
            case PatchKind::LensFov:
            {
                // Fisheye mapping uv = 0.5 + xy * R*tan(r/maxR * fov/2)/r.
                //  game:  leave it (magnified at rest, shrinks as fov grows with speed)
                //  fixed: lock fov at its value when "fixed" was selected (F2)
                //  off:   fov -> 0 with R = maxR/tan(fov/2) is the identity
                if (off.lensROffset + 4 > patched.size() || off.lensMaxROffset + 4 > patched.size()) break;
                static float lockedFov = 0.0f;
                static DWORD lastLog = 0;
                if (GetTickCount() - lastLog > 3000)
                {
                    lastLog = GetTickCount();
                    Log("[capture] lens: game fov %.1f deg (mode %d, locked %.1f)", f[0] * 57.2958f, Stereo().lensMode, lockedFov * 57.2958f);
                }
                if (Stereo().lensMode == 1 && (Stereo().lensLockPending || lockedFov == 0.0f) && f[0] > 0.01f)
                {
                    lockedFov = f[0];
                    Stereo().lensLockPending = false;
                    Log("[capture] lens: locked at %.1f deg", lockedFov * 57.2958f);
                }                int mode = Stereo().lensMode;
                if (mode == 0) break;
                // "Off" uses a small but not tiny fov: at 1e-4 rad the GPU's sincos
                // has an absolute error that is a large *relative* error, so each
                // grid column of the pass was displaced differently - fixed
                // blurry vertical bands that warped the view during head motion.
                // At 0.02 rad the math is accurate and the remaining fisheye is
                // ~(0.01)^2/3 relative, under 0.1 px.
                float useFov = (mode == 1 && lockedFov > 0.0f) ? lockedFov : 0.02f;
                float maxR = *reinterpret_cast<const float*>(patched.data() + off.lensMaxROffset);
                f[0] = useFov;
                *reinterpret_cast<float*>(patched.data() + off.lensROffset) = maxR / tanf(useFov * 0.5f);
                any = true;
                break;
            }
            }
        }
    }
    return any;
}

static void FinishPrepareDraw(ID3D11DeviceContext* self, ContextState& state, const ShaderOffsets& off, BufferShadow& s,
    const std::vector<uint8_t>& orig, const std::vector<uint8_t>& patched, bool any, bool gpuIsOriginal, uint64_t key)
{

    // Scene depth for the headset: the depth buffer of the frame's first 3D
    // draw (left eye; the right eye's is its twin).
    if (any && off.hasMVP && g_sceneDepthFrame != g_captureFrame.load())
    {
        ID3D11DepthStencilView* dsv = nullptr;
        self->OMGetRenderTargets(0, nullptr, &dsv);
        if (dsv)
        {
            ID3D11Resource* res = nullptr;
            dsv->GetResource(&res);
            if (g_sceneDepth) g_sceneDepth->Release();
            g_sceneDepth = res; // keeps the reference from GetResource
            dsv->Release();
            g_sceneDepthFrame = g_captureFrame.load();
        }
    }

    // One-time diagnostics: which depth buffers do 3D draws use?
    if (any && off.hasMVP)
    {
        static int logged = 0;
        static ID3D11Resource* seen[4] = {};
        if (logged < 4)
        {
            ID3D11DepthStencilView* dsv = nullptr;
            self->OMGetRenderTargets(0, nullptr, &dsv);
            if (dsv)
            {
                ID3D11Resource* res = nullptr;
                dsv->GetResource(&res);
                bool known = false;
                for (int i = 0; i < logged; ++i) known |= seen[i] == res;
                if (!known && res)
                {
                    seen[logged++] = res;
                    D3D11_TEXTURE2D_DESC td = {};
                    static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
                    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
                    dsv->GetDesc(&dd);
                    Log("[capture] 3D draws use depth buffer %p: %ux%u fmt %d (view fmt %d) samples %u bind 0x%X",
                        res, td.Width, td.Height, (int)td.Format, (int)dd.Format, td.SampleDesc.Count, td.BindFlags);
                }
                if (res) res->Release();
                dsv->Release();
            }
        }
    }

    if (!any && gpuIsOriginal) return;
    WriteBuffer(self, state.currentSlot0CB, s, any ? patched.data() : orig.data());
    s.patchedGen = any ? s.gen : ~0ull;
    s.patchedLayout = &off;
    s.patchedKey = key;
    g_statPatched++;
}
// Applies one override to a copy of the buffer (true if it changed something).
static bool PixelOverrideValue(const PixelOverride& o, bool dofOff, bool dofScaled, bool trailsOff, std::vector<uint8_t>& data)
{
    float* f = reinterpret_cast<float*>(data.data() + o.offset);
    if (o.kind == PixelOverride::Fixed) { *f = o.value; return true; }
    if (o.kind == PixelOverride::Trails)
    {
        if (!trailsOff) return false;
        *f = o.value;
        return true;
    }
    if (dofOff) { *f = 0.0f; return true; }
    if (dofScaled) { *f *= o.value == 0.0f ? Stereo().dofNear : Stereo().dofFar; return true; }
    return false;
}

// Pixel-shader constant overrides while in VR (depth of field, glare trails):
// write the bound buffer with the overridden values. Eye-independent.
static void PreparePixel(ID3D11DeviceContext* self)
{
    ContextState& st = g_contextState[self];
    auto it = g_psOverrides.find(st.currentPS);
    if (it == g_psOverrides.end()) return;
    bool vr = StereoPatchKey() != 0;
    bool dofOff = vr && !Stereo().depthOfField;
    bool dofScaled = vr && Stereo().depthOfField && (Stereo().dofNear != 1.0f || Stereo().dofFar != 1.0f);
    bool trailsOff = vr && !Stereo().motionBlur;
    // Cache tag: this shader's override list (unique address).
    const ShaderOffsets* tag = reinterpret_cast<const ShaderOffsets*>(&it->second);
    uint64_t wantKey = 1 + (dofOff ? 1 : 0) + (trailsOff ? 2 : 0) + (dofScaled ? 4 : 0);

    for (UINT slot = 0; slot < 4; ++slot)
    {
        bool usesSlot = false;
        for (const PixelOverride& o : it->second) usesSlot |= o.slot == slot;
        if (!usesSlot) continue;
        ID3D11Resource* buf = st.psCB[slot];
        auto sh = g_cbShadow.find(buf);
        if (sh == g_cbShadow.end() || !sh->second.valid) continue;
        BufferShadow& s = sh->second;
        bool ours = s.patchedGen == s.gen && s.patchedLayout == tag;
        if (ours && s.patchedKey == wantKey) continue; // already as wanted

        static std::vector<uint8_t> patched;
        patched = s.data;
        bool any = false;
        for (const PixelOverride& o : it->second)
        {
            if (o.slot != slot || o.offset + 4 > patched.size()) continue;
            if (!PixelOverrideValue(o, dofOff, dofScaled, trailsOff, patched)) continue;
            any = true;
        }
        if (!any && !ours) continue; // GPU already holds the game's values
        WriteBuffer(self, buf, s, patched.data());
        s.patchedGen = any ? s.gen : ~0ull;
        s.patchedLayout = tag;
        s.patchedKey = wantKey;
    }
}

// Batched double render: this draw's right-eye constants, written on the
// mirror's deferred context (the immediate context keeps the left eye's).
static void PrepareDrawRight()
{
    ContextState& state = g_contextState[g_immediateCtx];
    auto offIt = g_offsets.find(state.currentVS);
    if (offIt == g_offsets.end() || offIt->second.patches.empty()) return;
    auto shIt = g_cbShadow.find(state.currentSlot0CB);
    if (shIt == g_cbShadow.end() || !shIt->second.valid || StereoPatchKey() == 0) return;
    BufferShadow& s = shIt->second;
    static std::vector<uint8_t> patched;
    if (BuildPatched(offIt->second, s.data, patched))
        MirrorWriteBuffer(state.currentSlot0CB, patched.data(), (UINT)patched.size(), s.usage);
}

static void PreparePixelRight()
{
    ContextState& st = g_contextState[g_immediateCtx];
    auto it = g_psOverrides.find(st.currentPS);
    if (it == g_psOverrides.end()) return;
    bool vr = StereoPatchKey() != 0;
    bool dofOff = vr && !Stereo().depthOfField;
    bool dofScaled = vr && Stereo().depthOfField && (Stereo().dofNear != 1.0f || Stereo().dofFar != 1.0f);
    bool trailsOff = vr && !Stereo().motionBlur;
    for (UINT slot = 0; slot < 4; ++slot)
    {
        ID3D11Resource* buf = st.psCB[slot];
        auto sh = g_cbShadow.find(buf);
        if (sh == g_cbShadow.end() || !sh->second.valid) continue;
        static std::vector<uint8_t> patched;
        patched = sh->second.data;
        bool any = false;
        for (const PixelOverride& o : it->second)
        {
            if (o.slot != slot || o.offset + 4 > patched.size()) continue;
            if (!PixelOverrideValue(o, dofOff, dofScaled, trailsOff, patched)) continue;
            any = true;
        }
        if (any) MirrorWriteBuffer(buf, patched.data(), (UINT)patched.size(), sh->second.usage);
    }
}

// After the mirror's command list ran, a buffer holds whatever the right eye
// last wrote: put the game's own (latest) data back.
void CaptureRestoreOriginal(ID3D11DeviceContext* ctx, ID3D11Resource* buf)
{
    auto it = g_cbShadow.find(buf);
    if (it == g_cbShadow.end()) return;
    BufferShadow& s = it->second;
    if (s.valid)
    {
        WriteBuffer(ctx, buf, s, s.data.data());
        s.patchedGen = ~0ull; // GPU holds the original
    }
    else
    {
        s.patchedGen = s.gen; // unknown: force the next patch to rewrite
        s.patchedLayout = nullptr;
    }
}

// Issue one game draw: patched for the current eye (alternate-eye mode), or
// twice - left eye into the game's targets, right eye into their twins.
extern wchar_t g_dllDir[MAX_PATH];

// Debug: vertex shader ids listed in vrmod_skipvs.txt (next to the DLL) are
// not drawn - to find out which shader draws what. Re-read twice a second.
static std::unordered_set<int> g_skipVS;
static bool SkippedShader(ID3D11DeviceContext* self)
{
    static DWORD lastRead = 0;
    if (GetTickCount() - lastRead > 500)
    {
        lastRead = GetTickCount();
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\vrmod_skipvs.txt", g_dllDir);
        std::unordered_set<int> ids;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"r") == 0 && f)
        {
            int id;
            while (fscanf_s(f, "%d", &id) == 1) ids.insert(id);
            fclose(f);
        }
        if (ids != g_skipVS) { g_skipVS.swap(ids); Log("[capture] skipping %zu vertex shader(s)", g_skipVS.size()); }
    }
    if (g_skipVS.empty()) return false;
    auto it = g_offsets.find(g_contextState[self].currentVS);
    return it != g_offsets.end() && g_skipVS.count(it->second.id);
}

// UI trace (diagnostic): a file named vrmod_uitrace next to the DLL logs every
// draw of the next frame - shader ids, whether it carries camera data, its
// render target (is it the swap chain?), viewport and vertex count - to tell
// 2D overlay draws from the 3D scene and post passes. The file is removed.
static ID3D11Resource* g_backbuffer = nullptr; // not AddRef'd; compared by address only
static bool g_uiTrace = false;
static int g_traceN = 0;
void CaptureSetBackbuffer(ID3D11Resource* bb) { g_backbuffer = bb; }

static void TraceDraw(ID3D11DeviceContext* self, UINT count)
{
    ID3D11RenderTargetView* rtv = nullptr;
    self->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Resource* res = nullptr;
    UINT w = 0, h = 0;
    if (rtv)
    {
        rtv->GetResource(&res);
        D3D11_RESOURCE_DIMENSION dim;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC d; static_cast<ID3D11Texture2D*>(res)->GetDesc(&d); w = d.Width; h = d.Height; }
    }
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    self->RSGetViewports(&nvp, &vp);
    ContextState& st = g_contextState[self];
    auto it = g_offsets.find(st.currentVS);
    int vsId = it != g_offsets.end() ? it->second.id : -1;
    bool cam = it != g_offsets.end() && !it->second.patches.empty();
    Log("[uitrace] #%d vs %d %s rt %p%s %ux%u vp %.0f,%.0f %.0fx%.0f count %u ps %p",
        g_traceN++, vsId, cam ? "3D" : "--", res, res && res == g_backbuffer ? " (SWAPCHAIN)" : "", w, h,
        vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, count, st.currentPS);
    if (res) res->Release();
    if (rtv) rtv->Release();
}

// Journey's 2D overlays (menu text, title, tutorial prompts) are drawn onto
// the swap chain after the frame's first draw there (the 3D scene composite):
// the UI layer in one full-screen draw, the prompts as flat-projected quads.
// Left as they are they land on the same pixels of both eyes' images, which
// look in different directions - double and unreadable. Each eye draws them
// into a viewport that is the same virtual screen, kUiHalfAngle wide each way,
// straight ahead of the (head-turned) game camera. Nothing changes while the
// game's own view is narrower than that (e.g. on the desktop).
static int g_swapDraws = 0; // draws onto the swap chain this frame
static const float kUiHalfAngleTan = 0.5774f; // tan(30 deg): a 60 degree wide screen

static bool UiViewports(ID3D11DeviceContext* self, D3D11_VIEWPORT eyeVp[2], D3D11_VIEWPORT& orig)
{
    if (!g_backbuffer || wcscmp(Game().name, L"Journey") != 0) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    self->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    bool swap = res == g_backbuffer;
    if (res) res->Release();
    rtv->Release();
    if (!swap || g_swapDraws++ == 0) return false; // the first one is the scene composite
    float xs, ys;
    if (!StereoProjection(xs, ys)) return false;
    float sx = kUiHalfAngleTan * xs, sy = kUiHalfAngleTan * ys; // virtual screen in game NDC
    if (sx >= 1.0f && sy >= 1.0f) return false;
    if (sx > 1.0f) sx = 1.0f;
    if (sy > 1.0f) sy = 1.0f;
    UINT n = 1;
    self->RSGetViewports(&n, &orig);
    if (n == 0 || orig.Width < 1) return false;
    for (int e = 0; e < 2; ++e)
    {
        float x0, y0, x1, y1;
        if (!StereoMapNdc(e, -sx, -sy, &x0, &y0) || !StereoMapNdc(e, sx, sy, &x1, &y1)) return false;
        eyeVp[e] = orig;
        eyeVp[e].TopLeftX = orig.TopLeftX + (fminf(x0, x1) + 1.0f) * 0.5f * orig.Width;
        eyeVp[e].TopLeftY = orig.TopLeftY + (1.0f - fmaxf(y0, y1)) * 0.5f * orig.Height;
        eyeVp[e].Width = fabsf(x1 - x0) * 0.5f * orig.Width;
        eyeVp[e].Height = fabsf(y1 - y0) * 0.5f * orig.Height;
    }
    static int logged = 0;
    if (logged++ < 3)
        Log("[ui] 2D overlay -> virtual screen: left %.0f,%.0f %.0fx%.0f right %.0f,%.0f %.0fx%.0f (of %.0fx%.0f)",
            eyeVp[0].TopLeftX, eyeVp[0].TopLeftY, eyeVp[0].Width, eyeVp[0].Height,
            eyeVp[1].TopLeftX, eyeVp[1].TopLeftY, eyeVp[1].Width, eyeVp[1].Height, orig.Width, orig.Height);
    return true;
}

template <class F>
static void StereoDraw(ID3D11DeviceContext* self, F&& draw, UINT count = 0)
{
    if (self != g_immediateCtx) { draw(self); return; } // e.g. the mirror's own deferred context
    if (g_uiTrace) TraceDraw(self, count);
    D3D11_VIEWPORT uiVp[2], uiOrig;
    if (!ShadowBypassed() && StereoPatchKey() != 0 && UiViewports(self, uiVp, uiOrig))
    {
        // 2D overlay: each eye's virtual-screen viewport, then the game's back.
        StereoSetRenderEye(0);
        PrepareDraw(self);
        PreparePixel(self);
        {
            ShadowBypass g;
            self->RSSetViewports(1, &uiVp[0]);
            draw(self);
            self->RSSetViewports(1, &uiOrig);
        }
        if (Stereo().doubleRender && MirrorActive() && MirrorRightOutputsValid())
        {
            ID3D11DeviceContext* r = MirrorContext();
            StereoSetRenderEye(1);
            PrepareDrawRight();
            PreparePixelRight();
            {
                ShadowBypass g;
                r->RSSetViewports(1, &uiVp[1]);
                draw(r);
                r->RSSetViewports(1, &uiOrig);
            }
            MirrorNoteDraw();
            StereoSetRenderEye(0);
        }
        return;
    }
    if (SkippedShader(self)) return;
    if (!Stereo().doubleRender || ShadowBypassed())
    {
        PrepareDraw(self);
        PreparePixel(self);
        draw(self);
        return;
    }
    StereoSetRenderEye(0);
    PrepareDraw(self);
    PreparePixel(self);
    draw(self);
    if (MirrorActive())
    {
        // Batched: record the right eye's draw; it runs with the rest at Present.
        if (MirrorRightOutputsValid())
        {
            StereoSetRenderEye(1);
            PrepareDrawRight();
            PreparePixelRight();
            draw(MirrorContext());
            MirrorNoteDraw();
            StereoSetRenderEye(0);
        }
        return;
    }
    if (ShadowBindRightEye(self))
    {
        StereoSetRenderEye(1);
        PrepareDraw(self);
        PreparePixel(self);
        draw(self);
        ShadowRestore(self);
        StereoSetRenderEye(0);
    }
}

static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* self, UINT count, UINT start, INT base)
{
    g_countDrawIndexed++;
    StereoDraw(self, [&](ID3D11DeviceContext* c) { if (c == self) g_realDrawIndexed(c, count, start, base); else c->DrawIndexed(count, start, base); }, count);
}
static void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* self, UINT count, UINT start)
{
    g_countDraw++;
    StereoDraw(self, [&](ID3D11DeviceContext* c) { if (c == self) g_realDraw(c, count, start); else c->Draw(count, start); }, count);
}
static void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* self, UINT ipc, UINT ic, UINT sil, INT bvl, UINT sii)
{
    g_countDrawIndexedInstanced++;
    StereoDraw(self, [&](ID3D11DeviceContext* c) { if (c == self) g_realDrawIndexedInstanced(c, ipc, ic, sil, bvl, sii); else c->DrawIndexedInstanced(ipc, ic, sil, bvl, sii); }, ipc * ic);
}
static void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* self, UINT vpi, UINT ic, UINT sv, UINT si)
{
    g_countDrawInstanced++;
    StereoDraw(self, [&](ID3D11DeviceContext* c) { if (c == self) g_realDrawInstanced(c, vpi, ic, sv, si); else c->DrawInstanced(vpi, ic, sv, si); }, vpi * ic);
}
void NotifyCaptureFrameBoundary()
{
    uint64_t f = g_captureFrame.fetch_add(1);
    g_vpObservedThisFrame = false;
    g_swapDraws = 0;
    if (g_uiTrace) { g_uiTrace = false; Log("[uitrace] end of frame (%d draws)", g_traceN); }
    if ((f % 30) == 0)
    {
        extern wchar_t g_dllDir[MAX_PATH];
        wchar_t trig[MAX_PATH];
        swprintf_s(trig, L"%s\\vrmod_uitrace", g_dllDir);
        if (GetFileAttributesW(trig) != INVALID_FILE_ATTRIBUTES && DeleteFileW(trig)) { g_uiTrace = true; g_traceN = 0; Log("[uitrace] tracing the next frame"); }
    }
    StereoFrameBoundary();

    if ((f % 180) == 0)
    {
        g_logBudget = 3;
        Log("[counts] frame=%llu VSSetShader=%llu VSSetCB=%llu Map=%llu Unmap=%llu UpdateSubresource=%llu "
            "DrawIndexed=%llu Draw=%llu DrawIndexedInstanced=%llu DrawInstanced=%llu",
            (unsigned long long)f,
            (unsigned long long)g_countVSSetShader.load(), (unsigned long long)g_countVSSetCB.load(),
            (unsigned long long)g_countMap.load(), (unsigned long long)g_countUnmap.load(),
            (unsigned long long)g_countUpdateSubresource.load(),
            (unsigned long long)g_countDrawIndexed.load(), (unsigned long long)g_countDraw.load(),
            (unsigned long long)g_countDrawIndexedInstanced.load(), (unsigned long long)g_countDrawInstanced.load());
        Log("[stereo] eye=%d patched=%llu cached=%llu noShadow=%llu orthoSkipped=%llu noVars=%llu "
            "mapTypes(W=%llu RW=%llu DISCARD=%llu NOOVERWRITE=%llu)",
            StereoCurrentEye(),
            (unsigned long long)g_statPatched, (unsigned long long)g_statPatchCached,
            (unsigned long long)g_statNoShadow, (unsigned long long)g_statOrtho, (unsigned long long)g_statNoVars,
            (unsigned long long)g_mapTypeCount[D3D11_MAP_WRITE], (unsigned long long)g_mapTypeCount[D3D11_MAP_READ_WRITE],
            (unsigned long long)g_mapTypeCount[D3D11_MAP_WRITE_DISCARD], (unsigned long long)g_mapTypeCount[D3D11_MAP_WRITE_NO_OVERWRITE]);
        ShadowLogStats();
        MirrorLogStats();
    }
}

void InstallCaptureHooks(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!device || !context) return;
    g_immediateCtx = context;

    static bool minHookInit = false;
    if (!minHookInit)
    {
        MH_STATUS st = MH_Initialize();
        Log("[capture] MH_Initialize: %s", MH_StatusToString(st));
        minHookInit = true;
    }

    void** deviceVT = *reinterpret_cast<void***>(device);
    void** contextVT = *reinterpret_cast<void***>(context);

    HookMethod(deviceVT, 12, (void*)&Hook_CreateVertexShader, (void**)&g_realCreateVertexShader, "CreateVertexShader");
    HookMethod(deviceVT, 15, (void*)&Hook_CreatePixelShader, (void**)&g_realCreatePixelShader, "CreatePixelShader");
    HookMethod(contextVT, 9, (void*)&Hook_PSSetShader, (void**)&g_realPSSetShader, "PSSetShader");
    HookMethod(contextVT, 16, (void*)&Hook_PSSetConstantBuffers, (void**)&g_realPSSetConstantBuffers, "PSSetConstantBuffers");

    HookMethod(contextVT, 7, (void*)&Hook_VSSetConstantBuffers, (void**)&g_realVSSetConstantBuffers, "VSSetConstantBuffers");
    HookMethod(contextVT, 11, (void*)&Hook_VSSetShader, (void**)&g_realVSSetShader, "VSSetShader");
    HookMethod(contextVT, 12, (void*)&Hook_DrawIndexed, (void**)&g_realDrawIndexed, "DrawIndexed");
    HookMethod(contextVT, 13, (void*)&Hook_Draw, (void**)&g_realDraw, "Draw");
    HookMethod(contextVT, 14, (void*)&Hook_Map, (void**)&g_realMap, "Map");
    HookMethod(contextVT, 15, (void*)&Hook_Unmap, (void**)&g_realUnmap, "Unmap");
    HookMethod(contextVT, 20, (void*)&Hook_DrawIndexedInstanced, (void**)&g_realDrawIndexedInstanced, "DrawIndexedInstanced");
    HookMethod(contextVT, 21, (void*)&Hook_DrawInstanced, (void**)&g_realDrawInstanced, "DrawInstanced");
    HookMethod(contextVT, 48, (void*)&Hook_UpdateSubresource, (void**)&g_realUpdateSubresource, "UpdateSubresource");
}
