#include "capture.h"
#include "log.h"
#include "mat4.h"
#include "stereo.h"
#include "cinema.h"
#include "shadow.h"
#include "mirror.h"
#include "vhook.h"
#include "game.h"
#include "uisign.h"
#include "journeycam.h"
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
// RayO/U/V: a full-screen pass's depth-to-world rays (projPlaneOrigin/U/V, e.g. Journey's fog).
enum class PatchKind { Clip, OldClip, View, EyePos, LensFov, PointSize, RayO, RayU, RayV };

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
    unsigned long long dxbc = 0; // DXBC checksum: identifies a game shader across runs
    bool guiImage = false;       // Journey's GuiImage: a 2D image placed by CenterPos/Orient (no camera)
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
    // Fixed: [psoverride] name=value from vrmod.ini. EyeRay: per-eye depth-to-world
    // rays (value: 0 prjPlaneOrigin, 1 prjPlaneU, 2 prjPlaneV, 3 eyePositionWS).
    enum Kind { DepthOfField, Trails, Fixed, EyeRay, Scale } kind; // Scale: multiply by value
    UINT slot;
    UINT offset;
    float value;
};
static std::unordered_map<ID3D11PixelShader*, std::vector<PixelOverride>> g_psOverrides;
static std::unordered_map<ID3D11PixelShader*, std::string> g_psNames; // constant names, for vrmod_uitrace
static std::unordered_map<ID3D11PixelShader*, std::pair<UINT, UINT>> g_psAlpha; // slot, offset of "Alpha" (GuiImage)
static std::unordered_map<ID3D11PixelShader*, std::pair<UINT, UINT>> g_psDude;  // slot, offset of "localDudePos" (Journey's player)
static float g_dudePos[3] = {};
static uint64_t g_dudeFrame = 0;

// [psoverride] in vrmod.ini: <constant name>=<float>, lower-cased names. Pins any
// pixel-shader constant while in stereo (for tuning another game's effects).
static std::unordered_map<std::string, float> LoadFixedOverrides()
{
    std::unordered_map<std::string, float> m;
    wchar_t ini[MAX_PATH];
    extern wchar_t g_dllDir[MAX_PATH];
    swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
    if (!wcscmp(Game().name, L"Journey"))
    {
        // Journey's depth of field (blur strength from depth: nearScale/farScale)
        // smeared the intro in VR: off by default ([stereo] journeyDof=1 keeps it).
        // The heat shimmer (heatScale) stays on; [stereo] heatShimmer=0 turns it off.
        if (!GetPrivateProfileIntW(L"stereo", L"journeyDof", 0, ini)) { m["nearscale"] = 0.0f; m["farscale"] = 0.0f; }
        // (its strength is scaled in Hook_CreatePixelShader)
        Log("[capture] Journey: depth of field %s", m.count("farscale") ? "off" : "on");
    }
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
            if (n == "centerpos" && (varDesc.uFlags & D3D_SVF_USED)) offsets.guiImage = true;

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
            else if (n == "projplaneorigin" || n == "prjplaneorigin")
                offsets.patches.push_back({ PatchKind::RayO, varDesc.StartOffset });
            else if (n == "projplaneu" || n == "prjplaneu")
                offsets.patches.push_back({ PatchKind::RayU, varDesc.StartOffset });
            else if (n == "projplanev" || n == "prjplanev")
                offsets.patches.push_back({ PatchKind::RayV, varDesc.StartOffset });
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
        if (p.offset + (p.kind == PatchKind::EyePos || p.kind == PatchKind::RayO || p.kind == PatchKind::RayU || p.kind == PatchKind::RayV ? 12u : (p.kind == PatchKind::LensFov || p.kind == PatchKind::PointSize) ? 4u : 64u) > offsets.cbSize)
            continue;
        // A lone "fov" without R/maxRadius isn't the lens shader.
        if (p.kind == PatchKind::LensFov && !(offsets.hasLensR && offsets.hasLensMaxR))
            continue;
        kept.push_back(p);
    }
    offsets.patches.swap(kept);

    // DXBC checksum (bytes 4..11) identifies the game's .cso file offline.
    const uint8_t* bc = static_cast<const uint8_t*>(bytecode);
    unsigned long long sum = 0;
    if (len >= 12) memcpy(&sum, bc + 4, 8);
    offsets.dxbc = sum;
    g_offsets[shader] = offsets;
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
    int rayOff[4] = { -1, -1, -1, -1 }; UINT raySlot = 0;
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
            { std::string& names = g_psNames[*out]; if (names.size() < 160) { if (!names.empty()) names += ' '; names += vd.Name; } }
            static const std::unordered_map<std::string, float> fixed = LoadFixedOverrides();
            auto fx = fixed.find(n);
            if (fx != fixed.end())
                ov.push_back({ PixelOverride::Fixed, bd.BindPoint, vd.StartOffset, fx->second });
            if (n == "alpha") g_psAlpha[*out] = { bd.BindPoint, vd.StartOffset };
            if (n == "heatscale" && !wcscmp(Game().name, L"Journey"))
            {
                // Heat shimmer: the image is bent by a fraction of the screen,
                // and each eye's image spans ~125 degrees instead of a monitor's
                // ~45, so it bent nearly 3x as far (everything looked blurry).
                // [stereo] heatShimmer scales it: 0 off, 1 the game's, default 0.3.
                static float k = -1.0f;
                if (k < 0.0f)
                {
                    extern wchar_t g_dllDir[MAX_PATH];
                    wchar_t ini[MAX_PATH], buf[32];
                    swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
                    GetPrivateProfileStringW(L"stereo", L"heatShimmer", L"0.3", buf, 32, ini);
                    k = (float)_wtof(buf);
                    if (k < 0.0f) k = 0.0f;
                    Log("[capture] Journey heat shimmer x%.2f in VR", k);
                }
                ov.push_back({ PixelOverride::Scale, bd.BindPoint, vd.StartOffset, k });
            }
            if (n == "localdudepos") g_psDude[*out] = { bd.BindPoint, vd.StartOffset };
            if (n == "blurzranges")
            {
                // Depth of field: blur = clamp(max((Z-x)*y, (Z-z)*w)): .y is the near
                // ramp, .w the far one. Off -> both 0; on -> scaled by dofNear/dofFar.
                // (value: 0 = near entry, 1 = far entry)
                ov.push_back({ PixelOverride::DepthOfField, bd.BindPoint, vd.StartOffset + 4, 0.0f });
                ov.push_back({ PixelOverride::DepthOfField, bd.BindPoint, vd.StartOffset + 12, 1.0f });
                Log("[capture] pixel shader with depth of field: blurZRanges in slot %u @%u", bd.BindPoint, vd.StartOffset);
            }
            else if (n == "prjplaneorigin") { rayOff[0] = (int)vd.StartOffset; raySlot = bd.BindPoint; }
            else if (n == "prjplaneu") rayOff[1] = (int)vd.StartOffset;
            else if (n == "prjplanev") rayOff[2] = (int)vd.StartOffset;
            else if (n == "eyepositionws") rayOff[3] = (int)vd.StartOffset;
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
    if (rayOff[0] >= 0 && rayOff[1] >= 0 && rayOff[2] >= 0 && rayOff[3] >= 0)
    {
        // Journey's screen-space effects that rebuild world positions from depth
        // (the character's soft shadow, "Occlusion"): their rays are the game
        // camera's; each eye needs its own, or the shadow lands beside the
        // character and is cut off by the box it is drawn in.
        for (int i = 0; i < 4; ++i) ov.push_back({ PixelOverride::EyeRay, raySlot, (UINT)rayOff[i], (float)i });
        Log("[capture] pixel shader with depth-to-world rays (slot %u)", raySlot);
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
            if (p.offset + (p.kind == PatchKind::EyePos || p.kind == PatchKind::RayO || p.kind == PatchKind::RayU || p.kind == PatchKind::RayV ? 12u : (p.kind == PatchKind::LensFov || p.kind == PatchKind::PointSize) ? 4u : 64u) > patched.size()) continue;
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
            case PatchKind::RayO: case PatchKind::RayU: case PatchKind::RayV:
                break; // below, all three together
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
    // Depth-to-world rays computed in the vertex shader (Journey's fog): the
    // game camera's, so each eye's fog distances and heights were measured
    // along the wrong rays - the haze differed between the eyes.
    {
        int ro = -1, ru = -1, rv = -1, re = -1;
        for (const PatchVar& p : off.patches)
        {
            if (p.offset + 12 > orig.size()) continue;
            if (p.kind == PatchKind::RayO) ro = (int)p.offset;
            else if (p.kind == PatchKind::RayU) ru = (int)p.offset;
            else if (p.kind == PatchKind::RayV) rv = (int)p.offset;
            else if (p.kind == PatchKind::EyePos) re = (int)p.offset;
        }
        if (ro >= 0 && ru >= 0 && rv >= 0)
        {
            const float zero[3] = {};
            const float* E = re >= 0 ? reinterpret_cast<const float*>(orig.data() + re) : zero;
            float O2[3], U2[3], V2[3], E2[3];
            if (StereoEyeRays(StereoRenderEye(), reinterpret_cast<const float*>(orig.data() + ro), reinterpret_cast<const float*>(orig.data() + ru),
                              reinterpret_cast<const float*>(orig.data() + rv), E, O2, U2, V2, E2))
            {
                memcpy(patched.data() + ro, O2, 12);
                memcpy(patched.data() + ru, U2, 12);
                memcpy(patched.data() + rv, V2, 12);
                any = true;
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
    if (o.kind == PixelOverride::EyeRay) return false; // see ApplyEyeRays
    if (o.kind == PixelOverride::Fixed) { *f = o.value; return true; }
    if (o.kind == PixelOverride::Scale) { *f *= o.value; return true; }
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

// Per-eye depth-to-world rays (see PixelOverride::EyeRay), from the game's values in orig.
static bool ApplyEyeRays(int eye, const std::vector<PixelOverride>& ovs, UINT slot, const std::vector<uint8_t>& orig, std::vector<uint8_t>& data)
{
    int off[4] = { -1, -1, -1, -1 };
    for (const PixelOverride& o : ovs)
        if (o.kind == PixelOverride::EyeRay && o.slot == slot && o.offset + 12 <= orig.size()) off[(int)o.value] = (int)o.offset;
    if (off[0] < 0 || off[1] < 0 || off[2] < 0 || off[3] < 0) return false;
    const float* in[4];
    for (int i = 0; i < 4; ++i) in[i] = reinterpret_cast<const float*>(orig.data() + off[i]);
    float out[4][3];
    if (!StereoEyeRays(eye, in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3])) return false;
    for (int i = 0; i < 4; ++i) memcpy(data.data() + off[i], out[i], 12);
    return true;
}

// The bound vertex shader's patched version of a buffer the pixel shader also
// uses (same $Globals buffer), so the pixel overrides don't undo the camera patch.
static void StartFromVertexPatch(const ContextState& st, ID3D11Resource* buf, const std::vector<uint8_t>& orig, std::vector<uint8_t>& out)
{
    out = orig;
    if (buf != st.currentSlot0CB) return;
    auto off = g_offsets.find(st.currentVS);
    if (off == g_offsets.end() || off->second.patches.empty() || StereoPatchKey() == 0) return;
    std::vector<uint8_t> tmp;
    if (BuildPatched(off->second, orig, tmp)) out.swap(tmp);
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
    bool eyeRays = false;
    for (const PixelOverride& o : it->second) eyeRays |= o.kind == PixelOverride::EyeRay;
    if (eyeRays) wantKey += (StereoPatchKey() << 4) + ((uint64_t)StereoRenderEye() << 3);

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
        StartFromVertexPatch(st, buf, s.data, patched);
        bool any = false;
        for (const PixelOverride& o : it->second)
        {
            if (o.slot != slot || o.offset + 4 > patched.size()) continue;
            if (!PixelOverrideValue(o, dofOff, dofScaled, trailsOff, patched)) continue;
            any = true;
        }
        if (eyeRays && vr && ApplyEyeRays(StereoRenderEye(), it->second, slot, s.data, patched)) any = true;
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
        bool usesSlot = false;
        for (const PixelOverride& o : it->second) usesSlot |= o.slot == slot;
        if (!usesSlot) continue;
        static std::vector<uint8_t> patched;
        StartFromVertexPatch(st, buf, sh->second.data, patched);
        bool any = false;
        for (const PixelOverride& o : it->second)
        {
            if (o.slot != slot || o.offset + 4 > patched.size()) continue;
            if (!PixelOverrideValue(o, dofOff, dofScaled, trailsOff, patched)) continue;
            any = true;
        }
        if (vr && ApplyEyeRays(1, it->second, slot, sh->second.data, patched)) any = true;
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
    ID3D11DepthStencilView* dsvT = nullptr;
    self->OMGetRenderTargets(1, &rtv, &dsvT);
    ID3D11Resource* dsRes = nullptr;
    if (dsvT) { dsvT->GetResource(&dsRes); dsvT->Release(); }
    if (dsRes) dsRes->Release(); // address only
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
    auto pn = g_psNames.find(st.currentPS);
    Log("[uitrace] #%d vs %d %s rt %p%s ds %p %ux%u vp %.0f,%.0f %.0fx%.0f count %u ps %p [%s]",
        g_traceN++, vsId, cam ? "3D" : "--", res, res && res == g_backbuffer ? " (SWAPCHAIN)" : "", dsRes, w, h,
        vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, count, st.currentPS, pn != g_psNames.end() ? pn->second.c_str() : "");
    if (it != g_offsets.end())
    {
        // Every camera matrix this draw's shader has (viewprojection etc.).
        auto sh = g_cbShadow.find(st.currentSlot0CB);
        if (sh != g_cbShadow.end() && sh->second.valid)
            for (const PatchVar& p : it->second.patches)
                if (p.kind == PatchKind::Clip && p.offset + 64 <= sh->second.data.size())
                {
                    const float* m = reinterpret_cast<const float*>(sh->second.data.data() + p.offset);
                    Log("[uitrace]     clip@%u [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]", p.offset,
                        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                }
    }
    if (it != g_offsets.end() && it->second.hasMVP)
    {
        auto sh = g_cbShadow.find(st.currentSlot0CB);
        if (sh != g_cbShadow.end() && sh->second.valid && it->second.mvpOffset + 64 <= sh->second.data.size())
        {
            const float* m = reinterpret_cast<const float*>(sh->second.data.data() + it->second.mvpOffset);
            Log("[uitrace]     mvp [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
    }
    if (res) res->Release();
    if (rtv) rtv->Release();
}

// Journey's 2D layer (menu text, title) is drawn into its own target and put
// onto the swap chain in one camera-less full-screen draw after the frame's
// first draw there (the 3D scene composite). Quads drawn there with the
// scene camera (white fades, glows) are world-space and stay as they are.
// Left as it is the 2D layer lands on the same pixels of both eyes' images, which
// look in different directions - double and unreadable. Each eye draws them
// into a viewport that is the same virtual screen, kUiHalfAngle wide each way,
// straight ahead of the (head-turned) game camera. Nothing changes while the
// game's own view is narrower than that (e.g. on the desktop).
static int g_swapDraws = 0; // draws onto the swap chain this frame
static int g_swapDrawsAll = 0; // the same, counted for every game draw (automatic cinema mode)

// Journey's menu draws its text and, as a single quad, the animated "singing"
// logo with the same UI vertex shader (DXBC 657F594D94A6C203). In VR the logo
// floated oddly over the menu; [xr] hideMenuLogo=1 (default) skips that quad.
static bool HiddenMenuLogo(ID3D11DeviceContext* self)
{
    static int hide = -1;
    if (hide < 0)
    {
        extern wchar_t g_dllDir[MAX_PATH];
        wchar_t ini[MAX_PATH];
        swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
        hide =wcscmp(Game().name, L"Journey") == 0 && GetPrivateProfileIntW(L"xr", L"hideMenuLogo", 1, ini) != 0;
    }
    if (!hide) return false;
    auto off = g_offsets.find(g_contextState[self].currentVS);
    return off != g_offsets.end() && off->second.dxbc == 0x657F594D94A6C203ull;
}
static const float kUiHalfAngleTan = 0.5774f; // tan(30 deg): a 60 degree wide screen

// The sign: the 2D layer drawn as a square board standing in the room, fixed to
// the game camera's frame (not the head), menuDistance ahead, menuHeight above
// eye level, menuSize wide (meters; [xr] in vrmod.ini). Each eye draws it into
// the rectangle its four corners project to (always facing the viewer).
static float g_menuDist = 2.5f, g_menuSize = 2.5f, g_menuHeight = -0.15f;
static void LoadMenuSign()
{
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    extern wchar_t g_dllDir[MAX_PATH];
    wchar_t ini[MAX_PATH], buf[32];
    swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
    GetPrivateProfileStringW(L"xr", L"menuDistance", L"2.5", buf, 32, ini); g_menuDist = (float)_wtof(buf);
    GetPrivateProfileStringW(L"xr", L"menuSize", L"2.5", buf, 32, ini); g_menuSize = (float)_wtof(buf);
    GetPrivateProfileStringW(L"xr", L"menuHeight", L"-0.15", buf, 32, ini); g_menuHeight = (float)_wtof(buf);
    Log("[ui] menu sign: %.2f m ahead, %.2f m wide, %.2f m height", g_menuDist, g_menuSize, g_menuHeight);
    UiSignSetup(g_menuDist, g_menuSize, g_menuHeight);
}

// One eye's sign rectangle in pixels of the viewport v. False: no headset data
// (the caller falls back); *visible false: the sign is behind this eye.
static bool SignViewport(int eye, const D3D11_VIEWPORT& v, D3D11_VIEWPORT& out, bool* visible)
{
    if (!StereoHasEyePoses()) return false;
    const float h = g_menuSize * 0.5f;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    *visible = true;
    for (int c = 0; c < 4; ++c)
    {
        const float p[3] = { (c & 1) ? h : -h, g_menuHeight + ((c & 2) ? h : -h), -g_menuDist };
        float nx, ny;
        if (!StereoProjectRefPoint(eye, p, &nx, &ny)) { *visible = false; return true; }
        minX = fminf(minX, nx); maxX = fmaxf(maxX, nx); minY = fminf(minY, ny); maxY = fmaxf(maxY, ny);
    }
    out = v;
    out.TopLeftX = v.TopLeftX + (minX + 1.0f) * 0.5f * v.Width;
    out.TopLeftY = v.TopLeftY + (1.0f - maxY) * 0.5f * v.Height;
    out.Width = (maxX - minX) * 0.5f * v.Width;
    out.Height = (maxY - minY) * 0.5f * v.Height;
    return true;
}

static bool UiViewports(ID3D11DeviceContext* self, D3D11_VIEWPORT eyeVp[2], bool visible[2], D3D11_VIEWPORT& orig)
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
    // Draws with camera data are world-space quads (the intro's white fade,
    // glows, prompt icons placed in front of the camera): the stereo patch
    // already puts them right. Only camera-less draws (the 2D layer) move.
    auto off = g_offsets.find(g_contextState[self].currentVS);
    if (off != g_offsets.end() && !off->second.patches.empty()) return false;
    UINT n = 1;
    self->RSGetViewports(&n, &orig);
    if (n == 0 || orig.Width < 1) return false;
    LoadMenuSign();
    if (SignViewport(0, orig, eyeVp[0], &visible[0]) && SignViewport(1, orig, eyeVp[1], &visible[1]))
    {
        static int logged = 0;
        if (logged++ < 3)
            Log("[ui] 2D layer -> menu sign: left %.0f,%.0f %.0fx%.0f%s right %.0f,%.0f %.0fx%.0f%s",
                eyeVp[0].TopLeftX, eyeVp[0].TopLeftY, eyeVp[0].Width, eyeVp[0].Height, visible[0] ? "" : " (behind)",
                eyeVp[1].TopLeftX, eyeVp[1].TopLeftY, eyeVp[1].Width, eyeVp[1].Height, visible[1] ? "" : " (behind)");
        return true;
    }
    // No headset data (desktop): a centered virtual screen kUiHalfAngle wide.
    float xs, ys;
    if (!StereoProjection(xs, ys)) return false;
    float sx = kUiHalfAngleTan * xs, sy = kUiHalfAngleTan * ys; // virtual screen in game NDC
    if (sx >= 1.0f && sy >= 1.0f) return false;
    if (sx > 1.0f) sx = 1.0f;
    if (sy > 1.0f) sy = 1.0f;
    for (int e = 0; e < 2; ++e)
    {
        float x0, y0, x1, y1;
        if (!StereoMapNdc(e, -sx, -sy, &x0, &y0) || !StereoMapNdc(e, sx, sy, &x1, &y1)) return false;
        eyeVp[e] = orig;
        eyeVp[e].TopLeftX = orig.TopLeftX + (fminf(x0, x1) + 1.0f) * 0.5f * orig.Width;
        eyeVp[e].TopLeftY = orig.TopLeftY + (1.0f - fmaxf(y0, y1)) * 0.5f * orig.Height;
        eyeVp[e].Width = fabsf(x1 - x0) * 0.5f * orig.Width;
        eyeVp[e].Height = fabsf(y1 - y0) * 0.5f * orig.Height;
        visible[e] = true;
    }
    static int logged = 0;
    if (logged++ < 3)
        Log("[ui] 2D layer -> virtual screen: left %.0f,%.0f %.0fx%.0f right %.0f,%.0f %.0fx%.0f (of %.0fx%.0f)",
            eyeVp[0].TopLeftX, eyeVp[0].TopLeftY, eyeVp[0].Width, eyeVp[0].Height,
            eyeVp[1].TopLeftX, eyeVp[1].TopLeftY, eyeVp[1].Width, eyeVp[1].Height, orig.Width, orig.Height);
    return true;
}

// Per-eye viewports for a GuiImage draw onto the swap chain: the game screen
// mapped into each eye (StereoMapNdc), shifted to appear [xr] promptDistance
// ahead (default 4 m: about where the character stands). False: not such a draw.
// A GuiImage draw's texture size and its pixel shader's Alpha (-1: unknown).
// Journey's screen fades (to black, to white: the intro, level changes, the
// idle screen) are GuiImages too: a tiny solid texture stretched over the
// whole screen.
static void GuiImageInfo(ID3D11DeviceContext* self, UINT& w, UINT& h, float& alpha)
{
    w = h = 0; alpha = -1.0f;
    ID3D11ShaderResourceView* srv = nullptr;
    self->PSGetShaderResources(0, 1, &srv);
    if (srv)
    {
        ID3D11Resource* r = nullptr;
        srv->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim;
        if (r) { r->GetType(&dim); if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC td; static_cast<ID3D11Texture2D*>(r)->GetDesc(&td); w = td.Width; h = td.Height; } r->Release(); }
        srv->Release();
    }
    ContextState& st = g_contextState[self];
    auto a = g_psAlpha.find(st.currentPS);
    if (a == g_psAlpha.end() || a->second.first >= 4) return;
    auto sh = g_cbShadow.find(st.psCB[a->second.first]);
    if (sh == g_cbShadow.end() || !sh->second.valid || a->second.second + 4 > sh->second.data.size()) return;
    memcpy(&alpha, sh->second.data.data() + a->second.second, 4);
}
// GuiImage's vertex shader (S_GuiImage_vs), scaled by kTitleScale around the
// screen center. Same inputs and outputs, same constants (Orient, CenterPos, Bias).
static ID3D11VertexShader* GuiTitleVS(ID3D11DeviceContext* self)
{
    static ID3D11VertexShader* vs = nullptr;
    static bool tried = false;
    if (vs || tried) return vs;
    tried = true;
    static const char kSrc[] =
        "cbuffer G : register(b0) { float4 c[33]; };\n"
        "struct I { float3 p : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "O main(I i) {\n"
        "  float s, co; sincos(c[30].x, s, co);\n"
        "  float2 cp = c[31].xy; float a = c[32].x;\n"
        "  float2 off = float2(i.p.x * a - cp.x * a, i.p.y - cp.y);\n"
        "  float2 np = float2(co * off.x - s * off.y, s * off.x + co * off.y);\n"
        "  O o; o.p = float4((cp.x * a + np.x) / a * 0.85, (cp.y + np.y) * 0.85, 0, 1); o.uv = i.uv; return o;\n"
        "}\n";
    ID3DBlob* b = nullptr, * err = nullptr;
    if (FAILED(D3DCompile(kSrc, sizeof(kSrc) - 1, "guititle_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &b, &err)))
    {
        Log("[gui] title shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return nullptr;
    }
    ID3D11Device* dev = nullptr;
    self->GetDevice(&dev);
    if (dev) { dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs); dev->Release(); }
    b->Release();
    return vs;
}

// Titles in full VR (the "JOURNEY" logo as you walk): pinned in the world. When
// one appears, the game's screen is anchored as a plane kTitleDist units out
// along where the game camera looked, as big as the game's own (monitor) view
// of it there; the logo is drawn on that plane per eye. It stays put when you
// turn your head and barely moves as you walk, like a sign over the mountains.
// Its depth stays at the front: GuiImage draws are depth tested, and at 300
// units the sand hid the logo completely.
static const float kTitleDist = 300.0f;
static ID3D11VertexShader* GuiWorldVS(ID3D11DeviceContext* self)
{
    static ID3D11VertexShader* vs = nullptr;
    static bool tried = false;
    if (vs || tried) return vs;
    tried = true;
    static const char kSrc[] =
        "cbuffer G : register(b0) { float4 c[33]; };\n"
        "cbuffer W : register(b1) { row_major float4x4 M; };\n"
        "struct I { float3 p : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "O main(I i) {\n"
        "  float s, co; sincos(c[30].x, s, co);\n"
        "  float2 cp = c[31].xy; float a = c[32].x;\n"
        "  float2 off = float2(i.p.x * a - cp.x * a, i.p.y - cp.y);\n"
        "  float2 np = float2(co * off.x - s * off.y, s * off.x + co * off.y);\n"
        "  float2 ndc = float2((cp.x * a + np.x) / a, cp.y + np.y);\n"
        "  O o; o.p = mul(M, float4(ndc, 0, 1)); o.p.z = 0; o.uv = i.uv; return o;\n"
        "}\n";
    ID3DBlob* b = nullptr, * err = nullptr;
    if (FAILED(D3DCompile(kSrc, sizeof(kSrc) - 1, "guiworld_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &b, &err)))
    {
        Log("[gui] world title shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return nullptr;
    }
    ID3D11Device* dev = nullptr;
    self->GetDevice(&dev);
    if (dev) { dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs); dev->Release(); }
    b->Release();
    return vs;
}

template <class F>
static bool TitleWorldDraw(ID3D11DeviceContext* self, F&& draw)
{
    static DWORD lastSeen = 0;
    static float W[16];                      // game screen (ndc x, y) -> world, column-vector
    static ID3D11Buffer* cb[2] = {};
    const DWORD now = GetTickCount();
    ID3D11VertexShader* vs = GuiWorldVS(self);
    float vp[16], fwdNow[3];
    if (!vs || !StereoGameViewProj(vp, fwdNow)) return false;
    if (!lastSeen || now - lastSeen > 2000)
    {
        float pos[3], right[3], up[3], z[3];
        const float fov = JourneyCamGameFov();
        if (!JourneyCamGamePose(pos, right, up, z) || fov < 5.0f || fov > 170.0f) return false;
        const float sgn = (z[0] * fwdNow[0] + z[1] * fwdNow[1] + z[2] * fwdNow[2]) >= 0.0f ? 1.0f : -1.0f;
        const float half = kTitleDist * tanf(fov * 0.5f * 0.0174533f);
        const float C[3] = { pos[0] + sgn * z[0] * kTitleDist, pos[1] + sgn * z[1] * kTitleDist, pos[2] + sgn * z[2] * kTitleDist };
        const float Wm[16] = { right[0] * half, up[0] * half, 0, C[0],
                               right[1] * half, up[1] * half, 0, C[1],
                               right[2] * half, up[2] * half, 0, C[2],
                               0, 0, 0, 1 };
        memcpy(W, Wm, sizeof(W));
        Log("[gui] title pinned in the world %.0f units ahead", kTitleDist);
    }
    lastSeen = now;
    if (!cb[0])
    {
        ID3D11Device* dev = nullptr;
        self->GetDevice(&dev);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (dev) { dev->CreateBuffer(&bd, nullptr, &cb[0]); dev->CreateBuffer(&bd, nullptr, &cb[1]); dev->Release(); }
        if (!cb[0] || !cb[1]) return false;
    }
    // Per eye: K_eye * VP * W.
    float M[2][16];
    for (int e = 0; e < 2; ++e)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                float v = 0;
                for (int k = 0; k < 4; ++k) v += vp[r * 4 + k] * W[k * 4 + c];
                M[e][r * 4 + c] = v;
            }
        StereoSetRenderEye(e);
        StereoPatchClip(M[e]);
    }
    StereoSetRenderEye(0);
    auto one = [&](ID3D11DeviceContext* c, int e) {
        c->UpdateSubresource(cb[e], 0, nullptr, M[e], 0, 0);
        ID3D11VertexShader* oldVS = nullptr; c->VSGetShader(&oldVS, nullptr, nullptr);
        ID3D11Buffer* oldCB = nullptr; c->VSGetConstantBuffers(1, 1, &oldCB);
        c->VSSetShader(vs, nullptr, 0);
        c->VSSetConstantBuffers(1, 1, &cb[e]);
        draw(c);
        c->VSSetShader(oldVS, nullptr, 0);
        c->VSSetConstantBuffers(1, 1, &oldCB);
        if (oldVS) oldVS->Release();
        if (oldCB) oldCB->Release();
    };
    {
        ShadowBypass g;
        one(self, 0);
    }
    if (Stereo().doubleRender && MirrorActive() && MirrorRightOutputsValid())
    {
        ShadowBypass g;
        one(MirrorContext(), 1);
        MirrorNoteDraw();
    }
    return true;
}

static bool IsScreenFill(ID3D11DeviceContext* self)
{
    UINT w, h; float alpha;
    GuiImageInfo(self, w, h, alpha);
    return w > 0 && w <= 16 && h <= 16;
}

static bool GuiImageViewports(ID3D11DeviceContext* self, D3D11_VIEWPORT vp[2], D3D11_VIEWPORT& orig)
{
    auto off = g_offsets.find(g_contextState[self].currentVS);
    if (off == g_offsets.end() || !off->second.guiImage || !g_backbuffer || !StereoHasEyePoses()) return false;
    if (IsScreenFill(self)) return false; // a screen fade: stays full screen in both eyes
    ID3D11RenderTargetView* rtv = nullptr;
    self->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    bool swap = res == g_backbuffer;
    if (res) res->Release();
    rtv->Release();
    if (!swap) return false;
    UINT n = 1;
    self->RSGetViewports(&n, &orig);
    if (n == 0 || orig.Width < 1) return false;
    static float meters = -1.0f;
    if (meters < 0.0f)
    {
        extern wchar_t g_dllDir[MAX_PATH];
        wchar_t ini[MAX_PATH], buf[32];
        swprintf_s(ini, L"%s\\vrmod.ini", g_dllDir);
        GetPrivateProfileStringW(L"xr", L"promptDistance", L"4.0", buf, 32, ini);
        meters = (float)_wtof(buf);
        Log("[ui] tutorial prompts (GuiImage) at %.2f m", meters);
    }
    // The image is laid out for the game's own field of view; ours is wider
    // (125 deg), so map the part of our screen the game's view would cover.
    float xs, ys, s = 1.0f;
    const float gameFov = JourneyCamGameFov();
    if (gameFov > 5.0f && gameFov < 170.0f && StereoProjection(xs, ys))
    {
        s = tanf(gameFov * 0.5f * 0.0174533f) * ys;
        if (s > 1.0f) s = 1.0f;
    }
    for (int e = 0; e < 2; ++e)
    {
        float x0, y0, x1, y1;
        if (!StereoMapNdc(e, -s, -s, &x0, &y0) || !StereoMapNdc(e, s, s, &x1, &y1)) return false;
        const float shift = StereoOverlayNdcShift(e, meters);
        x0 += shift; x1 += shift;
        vp[e] = orig;
        vp[e].TopLeftX = orig.TopLeftX + (fminf(x0, x1) + 1.0f) * 0.5f * orig.Width;
        vp[e].TopLeftY = orig.TopLeftY + (1.0f - fmaxf(y0, y1)) * 0.5f * orig.Height;
        vp[e].Width = fabsf(x1 - x0) * 0.5f * orig.Width;
        vp[e].Height = fabsf(y1 - y0) * 0.5f * orig.Height;
    }
    return true;
}

// Journey's player position, from the character shaders' localDudePos.
static void NoteDudePos(ID3D11DeviceContext* self)
{
    ContextState& st = g_contextState[self];
    auto d = g_psDude.find(st.currentPS);
    if (d == g_psDude.end() || d->second.first >= 4) return;
    auto sh = g_cbShadow.find(st.psCB[d->second.first]);
    if (sh == g_cbShadow.end() || !sh->second.valid || d->second.second + 12 > sh->second.data.size()) return;
    memcpy(g_dudePos, sh->second.data.data() + d->second.second, 12);
    g_dudeFrame = g_captureFrame.load();
}
bool CapturePlayerPos(float out[3])
{
    if (!g_dudeFrame || g_captureFrame.load() - g_dudeFrame > 2) return false;
    memcpy(out, g_dudePos, sizeof(g_dudePos));
    return true;
}

template <class F>
static void StereoDraw(ID3D11DeviceContext* self, F&& draw, UINT count = 0)
{
    if (self != g_immediateCtx) { draw(self); return; } // e.g. the mirror's own deferred context
    if (!g_psDude.empty()) NoteDudePos(self);
    if (g_uiTrace) TraceDraw(self, count);
    if (count == 6 && HiddenMenuLogo(self)) return;
    if (!ShadowBypassed() && g_backbuffer && !wcscmp(Game().name, L"Journey"))
    {
        // Automatic cinema mode: the 2D layer (camera-less draws onto the swap
        // chain after the scene composite) means a menu or title is up; a
        // GuiImage (tutorial prompt) means you have control.
        auto off = g_offsets.find(g_contextState[self].currentVS);
        const bool gui = off != g_offsets.end() && off->second.guiImage;
        if (gui)
        {
            UINT w, h; float alpha;
            GuiImageInfo(self, w, h, alpha);
            // Visible images (not fades, not hidden): the tutorial prompts come
            // from a tall strip of button pictures (512x2304) and mean you have
            // control; anything else (the "JOURNEY" logo, 1024x256, in play and
            // on the idle screen) is a title card, shown on the cinema screen.
            if (w > 16 && alpha > 0.05f)
            {
                if (h >= 2 * w) CinemaNotePrompt();
                else CinemaNoteTitle();
            }
        }
        else
        {
            ID3D11RenderTargetView* rtv = nullptr;
            self->OMGetRenderTargets(1, &rtv, nullptr);
            if (rtv)
            {
                ID3D11Resource* res = nullptr;
                rtv->GetResource(&res);
                if (res == g_backbuffer && g_swapDrawsAll++ > 0 && (off == g_offsets.end() || off->second.patches.empty()))
                    CinemaNoteUiLayer();
                if (res) res->Release();
                rtv->Release();
            }
        }
    }
    D3D11_VIEWPORT uiVp[2], uiOrig;
    bool uiVis[2] = { true, true };
    if (!ShadowBypassed() && StereoPatchKey() != 0 && UiViewports(self, uiVp, uiVis, uiOrig))
    {
        // 2D overlay: each eye's virtual-screen viewport, then the game's back.
        StereoSetRenderEye(0);
        PrepareDraw(self);
        PreparePixel(self);
        // With headset data: a real panel in the room (uisign.cpp), drawn
        // instead of the game's full-screen pass. Otherwise the viewport.
        const bool panel = StereoHasEyePoses();
        {
            ShadowBypass g;
            if (!panel || !UiSignDraw(self, 0))
            {
                self->RSSetViewports(1, &uiVp[0]);
                if (uiVis[0]) draw(self);
                self->RSSetViewports(1, &uiOrig);
            }
        }
        if (Stereo().doubleRender && MirrorActive() && MirrorRightOutputsValid())
        {
            ID3D11DeviceContext* r = MirrorContext();
            StereoSetRenderEye(1);
            PrepareDrawRight();
            PreparePixelRight();
            {
                ShadowBypass g;
                if (!panel || !UiSignDraw(r, 1))
                {
                    r->RSSetViewports(1, &uiVp[1]);
                    if (uiVis[1]) draw(r);
                    r->RSSetViewports(1, &uiOrig);
                }
            }
            MirrorNoteDraw();
            StereoSetRenderEye(0);
        }
        return;
    }
    if (!ShadowBypassed() && StereoPatchKey() != 0 && !CinemaActive() && g_backbuffer && !wcscmp(Game().name, L"Journey"))
    {
        auto o = g_offsets.find(g_contextState[self].currentVS);
        if (o != g_offsets.end() && o->second.guiImage)
        {
            UINT tw, th; float ta;
            GuiImageInfo(self, tw, th, ta);
            if (tw > 16 && th < 2 * tw && TitleWorldDraw(self, draw)) return; // a title (not a fade, not a prompt)
        }
    }
    D3D11_VIEWPORT giVp[2], giOrig;
    bool giCinema = false, giTitle = false;
    if (!ShadowBypassed() && CinemaActive() && CinemaAspect() > 1.0f && !wcscmp(Game().name, L"Journey"))
    {
        // Cinema screen: it shows the middle band of the square frame, and 2D
        // images laid out for the square screen (the "JOURNEY" title sits low)
        // fell outside it. They're drawn shrunk into that band instead.
        auto o = g_offsets.find(g_contextState[self].currentVS);
        if (o != g_offsets.end() && o->second.guiImage && !IsScreenFill(self))
        {
            UINT n = 1;
            self->RSGetViewports(&n, &giOrig);
            if (n && giOrig.Width > 1)
            {
                const float k = 1.0f / CinemaAspect();
                giVp[0] = giOrig;
                giVp[0].Width = giOrig.Width * k;
                giVp[0].Height = giOrig.Height * k;
                giVp[0].TopLeftX = giOrig.TopLeftX + (giOrig.Width - giVp[0].Width) * 0.5f;
                giVp[0].TopLeftY = giOrig.TopLeftY + (giOrig.Height - giVp[0].Height) * 0.5f;
                giVp[1] = giVp[0];
                giCinema = true;
                UINT tw, th; float ta;
                GuiImageInfo(self, tw, th, ta);
                giTitle = th < 2 * tw; // not a prompt (those come from a tall strip of button pictures)
            }
        }
    }
    if (giCinema || (!ShadowBypassed() && StereoPatchKey() != 0 && GuiImageViewports(self, giVp, giOrig)))
    {
        // Journey's GuiImage (tutorial prompts): a 2D image at a game-screen
        // position, so drawn on the same pixels of both eyes' images, which
        // look in different directions - it showed double. Each eye maps the
        // game screen into its own view and adds the disparity of promptDistance.
        // Drawn smaller than on a monitor. GuiImage reads the image's shape
        // (alpha) with PointClampSampler (s8, nearest texel) and its color with
        // a bilinear one (s9): the outlines came out pixelated. Both get a
        // trilinear anisotropic sampler for these draws.
        static ID3D11SamplerState* smooth = nullptr;
        if (!smooth)
        {
            ID3D11Device* dev = nullptr;
            self->GetDevice(&dev);
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter = D3D11_FILTER_ANISOTROPIC;
            sd.MaxAnisotropy = 8;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (dev) { dev->CreateSamplerState(&sd, &smooth); dev->Release(); }
        }
        // Titles on the cinema screen: Journey lays the "JOURNEY" logo out for
        // a widescreen monitor; on its square frame the Y hung off the right
        // edge and was cut. They're drawn with a copy of the GuiImage vertex
        // shader that scales them down slightly around the screen center.
        ID3D11VertexShader* titleVS = giTitle ? GuiTitleVS(self) : nullptr;
        auto drawSmooth = [&](ID3D11DeviceContext* c) {
            ID3D11SamplerState* old[2] = {};
            ID3D11SamplerState* both[2] = { smooth, smooth };
            if (smooth) { c->PSGetSamplers(8, 2, old); c->PSSetSamplers(8, 2, both); }
            ID3D11VertexShader* oldVS = nullptr;
            if (titleVS) { c->VSGetShader(&oldVS, nullptr, nullptr); c->VSSetShader(titleVS, nullptr, 0); }
            draw(c);
            if (titleVS) { c->VSSetShader(oldVS, nullptr, 0); if (oldVS) oldVS->Release(); }
            if (smooth) { c->PSSetSamplers(8, 2, old); for (auto* o : old) if (o) o->Release(); }
        };
        StereoSetRenderEye(0);
        {
            ShadowBypass g;
            self->RSSetViewports(1, &giVp[0]);
            drawSmooth(self);
            self->RSSetViewports(1, &giOrig);
        }
        if (Stereo().doubleRender && MirrorActive() && MirrorRightOutputsValid())
        {
            ID3D11DeviceContext* r = MirrorContext();
            StereoSetRenderEye(1);
            {
                ShadowBypass g;
                r->RSSetViewports(1, &giVp[1]);
                drawSmooth(r);
                r->RSSetViewports(1, &giOrig);
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
    g_swapDrawsAll = 0;
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
