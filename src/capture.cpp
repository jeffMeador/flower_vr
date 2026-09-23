#include "capture.h"
#include "log.h"
#include "mat4.h"
#include "stereo.h"
#include <MinHook.h>
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
enum class PatchKind { Clip, View, EyePos, LensFov };

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
};
static std::unordered_map<ID3D11DeviceContext*, ContextState> g_contextState;

static std::atomic<uint64_t> g_captureFrame{ 0 };
static int g_logBudget = 0;
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
            else if (n == "oldmodelviewproj" || n == "viewprojection" || n == "viewprojmtx")
                offsets.patches.push_back({ PatchKind::Clip, varDesc.StartOffset });
            else if (n == "modelview")
                offsets.patches.push_back({ PatchKind::View, varDesc.StartOffset });
            else if (n == "fov")
                offsets.patches.push_back({ PatchKind::LensFov, varDesc.StartOffset });
            else if (n == "r") { offsets.hasLensR = true; offsets.lensROffset = varDesc.StartOffset; }
            else if (n == "maxradius") { offsets.hasLensMaxR = true; offsets.lensMaxROffset = varDesc.StartOffset; }
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
        if (p.offset + (p.kind == PatchKind::EyePos ? 12u : p.kind == PatchKind::LensFov ? 4u : 64u) > offsets.cbSize)
            continue;
        // A lone "fov" without R/maxRadius isn't the lens shader.
        if (p.kind == PatchKind::LensFov && !(offsets.hasLensR && offsets.hasLensMaxR))
            continue;
        kept.push_back(p);
    }
    offsets.patches.swap(kept);

    g_offsets[shader] = offsets;
    Log("[capture] shader #%d reflected: cbSize=%u model=%d mvp=%d patches=%zu [%s]",
        offsets.id, offsets.cbSize, offsets.hasModel, offsets.hasMVP, offsets.patches.size(), names.c_str());
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
    g_countVSSetShader++;
    g_contextState[self].currentVS = shader;
    g_realVSSetShader(self, shader, instances, numInstances);
}

static void STDMETHODCALLTYPE Hook_VSSetConstantBuffers(ID3D11DeviceContext* self, UINT startSlot, UINT numBuffers, ID3D11Buffer* const* buffers)
{
    g_countVSSetCB++;
    if (startSlot <= 0 && 0 < startSlot + numBuffers && buffers)
        g_contextState[self].currentSlot0CB = buffers[0 - startSlot];
    g_realVSSetConstantBuffers(self, startSlot, numBuffers, buffers);
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
    g_countMap++;
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
    g_countUnmap++;
    auto it = g_activeMap.find(resource);
    if (it != g_activeMap.end())
    {
        RecordGameWrite(resource, it->second);
        g_activeMap.erase(it);
    }
    g_realUnmap(self, resource, sub);
}

static void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, const D3D11_BOX* box, const void* src, UINT rowPitch, UINT depthPitch)
{
    g_countUpdateSubresource++;
    if (dstSub == 0)
        RecordGameWrite(dst, box ? nullptr : src); // partial updates: can't mirror, mark invalid
    g_realUpdateSubresource(self, dst, dstSub, box, src, rowPitch, depthPitch);
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
    float eyeOff[3];
    bool eyePos = StereoWorldEyeOffset(eyeOff);

    // GPU copy already matches what we want?
    bool gpuIsOriginal = s.patchedGen != s.gen;
    if (key == 0 && gpuIsOriginal) return;
    if (!gpuIsOriginal && s.patchedLayout == &off && s.patchedKey == key)
    {
        g_statPatchCached++;
        return;
    }

    static std::vector<uint8_t> patched;
    patched = orig;
    bool any = false;
    if (key != 0)
    {
        for (const PatchVar& p : off.patches)
        {
            if (p.offset + (p.kind == PatchKind::EyePos ? 12u : p.kind == PatchKind::LensFov ? 4u : 64u) > patched.size()) continue;
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
            case PatchKind::View:
                StereoPatchView(f);
                any = true;
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
                float useFov = (mode == 1 && lockedFov > 0.0f) ? lockedFov : 1e-4f;
                float maxR = *reinterpret_cast<const float*>(patched.data() + off.lensMaxROffset);
                f[0] = useFov;
                *reinterpret_cast<float*>(patched.data() + off.lensROffset) = maxR / tanf(useFov * 0.5f);
                any = true;
                break;
            }
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
static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* self, UINT count, UINT start, INT base)
{
    g_countDrawIndexed++;
    PrepareDraw(self);
    g_realDrawIndexed(self, count, start, base);
}
static void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* self, UINT count, UINT start)
{
    g_countDraw++;
    PrepareDraw(self);
    g_realDraw(self, count, start);
}
static void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* self, UINT ipc, UINT ic, UINT sil, INT bvl, UINT sii)
{
    g_countDrawIndexedInstanced++;
    PrepareDraw(self);
    g_realDrawIndexedInstanced(self, ipc, ic, sil, bvl, sii);
}
static void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* self, UINT vpi, UINT ic, UINT sv, UINT si)
{
    g_countDrawInstanced++;
    PrepareDraw(self);
    g_realDrawInstanced(self, vpi, ic, sv, si);
}

void NotifyCaptureFrameBoundary()
{
    uint64_t f = g_captureFrame.fetch_add(1);
    g_vpObservedThisFrame = false;
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
    }
}

static std::unordered_set<void*> g_hookedTargets;

static bool InlineHook(void* target, void* detour, void** original, const char* name)
{
    if (g_hookedTargets.count(target))
        return true; // same underlying implementation already hooked (shared across instances)

    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
    {
        Log("[capture] MH_CreateHook FAILED for %s at %p: %s", name, target, MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED)
    {
        Log("[capture] MH_EnableHook FAILED for %s at %p: %s", name, target, MH_StatusToString(st));
        return false;
    }
    g_hookedTargets.insert(target);
    Log("[capture] inline-hooked %s at %p", name, target);
    return true;
}

void InstallCaptureHooks(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!device || !context) return;

    static bool minHookInit = false;
    if (!minHookInit)
    {
        MH_STATUS st = MH_Initialize();
        Log("[capture] MH_Initialize: %s", MH_StatusToString(st));
        minHookInit = true;
    }

    void** deviceVT = *reinterpret_cast<void***>(device);
    void** contextVT = *reinterpret_cast<void***>(context);

    InlineHook(deviceVT[12], (void*)&Hook_CreateVertexShader, (void**)&g_realCreateVertexShader, "CreateVertexShader");

    InlineHook(contextVT[7], (void*)&Hook_VSSetConstantBuffers, (void**)&g_realVSSetConstantBuffers, "VSSetConstantBuffers");
    InlineHook(contextVT[11], (void*)&Hook_VSSetShader, (void**)&g_realVSSetShader, "VSSetShader");
    InlineHook(contextVT[12], (void*)&Hook_DrawIndexed, (void**)&g_realDrawIndexed, "DrawIndexed");
    InlineHook(contextVT[13], (void*)&Hook_Draw, (void**)&g_realDraw, "Draw");
    InlineHook(contextVT[14], (void*)&Hook_Map, (void**)&g_realMap, "Map");
    InlineHook(contextVT[15], (void*)&Hook_Unmap, (void**)&g_realUnmap, "Unmap");
    InlineHook(contextVT[20], (void*)&Hook_DrawIndexedInstanced, (void**)&g_realDrawIndexedInstanced, "DrawIndexedInstanced");
    InlineHook(contextVT[21], (void*)&Hook_DrawInstanced, (void**)&g_realDrawInstanced, "DrawInstanced");
    InlineHook(contextVT[48], (void*)&Hook_UpdateSubresource, (void**)&g_realUpdateSubresource, "UpdateSubresource");
}
