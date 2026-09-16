#include "capture.h"
#include "log.h"
#include "mat4.h"
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

struct ShaderOffsets
{
    bool hasModel = false;   UINT modelOffset = 0;
    bool hasMVP = false;     UINT mvpOffset = 0;
    bool hasEyePos = false;  UINT eyePosOffset = 0;
    UINT cbSize = 0;
    int  id = 0;
};

static std::unordered_map<ID3D11VertexShader*, ShaderOffsets> g_offsets;
static std::unordered_map<ID3D11Resource*, std::vector<uint8_t>> g_cbShadow;
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

static std::atomic<uint64_t> g_countVSSetShader{ 0 };
static std::atomic<uint64_t> g_countVSSetCB{ 0 };
static std::atomic<uint64_t> g_countMap{ 0 };
static std::atomic<uint64_t> g_countUnmap{ 0 };
static std::atomic<uint64_t> g_countUpdateSubresource{ 0 };
static std::atomic<uint64_t> g_countDrawIndexed{ 0 };
static std::atomic<uint64_t> g_countDraw{ 0 };
static std::atomic<uint64_t> g_countDrawIndexedInstanced{ 0 };
static std::atomic<uint64_t> g_countDrawInstanced{ 0 };

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

static bool NameContains(const char* name, const char* needle)
{
    std::string a(name), b(needle);
    for (auto& c : a) c = (char)tolower((unsigned char)c);
    for (auto& c : b) c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
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

            if (NameContains(varDesc.Name, "modelviewproj"))
            {
                offsets.hasMVP = true;
                offsets.mvpOffset = varDesc.StartOffset;
            }
            else if (NameContains(varDesc.Name, "model") && !NameContains(varDesc.Name, "modelview") && !NameContains(varDesc.Name, "modelit"))
            {
                offsets.hasModel = true;
                offsets.modelOffset = varDesc.StartOffset;
            }
            else if (NameContains(varDesc.Name, "eyeposition"))
            {
                offsets.hasEyePos = true;
                offsets.eyePosOffset = varDesc.StartOffset;
            }
        }
    }
    refl->Release();

    g_offsets[shader] = offsets;
    Log("[capture] shader #%d reflected: hasModel=%d(@%u) hasMVP=%d(@%u) hasEyePos=%d(@%u) cbSize=%u",
        offsets.id, offsets.hasModel, offsets.modelOffset, offsets.hasMVP, offsets.mvpOffset,
        offsets.hasEyePos, offsets.eyePosOffset, offsets.cbSize);
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

static HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT sub, D3D11_MAP mapType, UINT flags, D3D11_MAPPED_SUBRESOURCE* out)
{
    g_countMap++;
    HRESULT hr = g_realMap(self, resource, sub, mapType, flags, out);
    if (SUCCEEDED(hr) && out)
        g_activeMap[resource] = out->pData;
    return hr;
}

static void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT sub)
{
    g_countUnmap++;
    auto it = g_activeMap.find(resource);
    if (it != g_activeMap.end())
    {
        D3D11_RESOURCE_DIMENSION dim;
        resource->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
            D3D11_BUFFER_DESC desc = {};
            reinterpret_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
            if (desc.ByteWidth > 0 && desc.ByteWidth <= 4096)
            {
                auto& shadow = g_cbShadow[resource];
                shadow.resize(desc.ByteWidth);
                memcpy(shadow.data(), it->second, desc.ByteWidth);
            }
        }
        g_activeMap.erase(it);
    }
    g_realUnmap(self, resource, sub);
}

static void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, const D3D11_BOX* box, const void* src, UINT rowPitch, UINT depthPitch)
{
    g_countUpdateSubresource++;
    if (dstSub == 0 && !box && src)
    {
        D3D11_RESOURCE_DIMENSION dim;
        dst->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
            D3D11_BUFFER_DESC desc = {};
            reinterpret_cast<ID3D11Buffer*>(dst)->GetDesc(&desc);
            if (desc.ByteWidth > 0 && desc.ByteWidth <= 4096)
            {
                auto& shadow = g_cbShadow[dst];
                shadow.resize(desc.ByteWidth);
                memcpy(shadow.data(), src, desc.ByteWidth);
            }
        }
    }
    g_realUpdateSubresource(self, dst, dstSub, box, src, rowPitch, depthPitch);
}

static void LogDrawIfBudget(ID3D11DeviceContext* self, const char* kind)
{
    ContextState& state = g_contextState[self];

    if (g_logBudget <= 0) return;
    auto offIt = g_offsets.find(state.currentVS);
    if (offIt == g_offsets.end()) return;
    auto shadowIt = g_cbShadow.find(state.currentSlot0CB);
    if (shadowIt == g_cbShadow.end()) return;

    const ShaderOffsets& off = offIt->second;
    const std::vector<uint8_t>& bytes = shadowIt->second;
    if (!off.hasMVP || off.mvpOffset + 64 > bytes.size()) return;

    Mat4 mvp;
    memcpy(&mvp, bytes.data() + off.mvpOffset, 64);

    if (off.hasModel && off.modelOffset + 64 <= bytes.size())
    {
        Mat4 model, modelInv;
        memcpy(&model, bytes.data() + off.modelOffset, 64);
        if (Mat4Inverse(model, modelInv))
        {
            Mat4 viewProj = Mat4Mul(modelInv, mvp);
            Log("[capture] %s shader#%d frame=%llu ViewProj row0=(%.3f %.3f %.3f %.3f) row3=(%.3f %.3f %.3f %.3f)",
                kind, off.id, (unsigned long long)g_captureFrame.load(),
                viewProj.m[0][0], viewProj.m[0][1], viewProj.m[0][2], viewProj.m[0][3],
                viewProj.m[3][0], viewProj.m[3][1], viewProj.m[3][2], viewProj.m[3][3]);
        }
    }
    else
    {
        Log("[capture] %s shader#%d frame=%llu (no model matrix) MVP row0=(%.3f %.3f %.3f %.3f) row3=(%.3f %.3f %.3f %.3f)",
            kind, off.id, (unsigned long long)g_captureFrame.load(),
            mvp.m[0][0], mvp.m[0][1], mvp.m[0][2], mvp.m[0][3],
            mvp.m[3][0], mvp.m[3][1], mvp.m[3][2], mvp.m[3][3]);
    }
    g_logBudget--;
}

static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* self, UINT count, UINT start, INT base)
{
    g_countDrawIndexed++;
    LogDrawIfBudget(self, "DrawIndexed");
    g_realDrawIndexed(self, count, start, base);
}
static void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* self, UINT count, UINT start)
{
    g_countDraw++;
    LogDrawIfBudget(self, "Draw");
    g_realDraw(self, count, start);
}
static void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* self, UINT ipc, UINT ic, UINT sil, INT bvl, UINT sii)
{
    g_countDrawIndexedInstanced++;
    LogDrawIfBudget(self, "DrawIndexedInstanced");
    g_realDrawIndexedInstanced(self, ipc, ic, sil, bvl, sii);
}
static void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* self, UINT vpi, UINT ic, UINT sv, UINT si)
{
    g_countDrawInstanced++;
    LogDrawIfBudget(self, "DrawInstanced");
    g_realDrawInstanced(self, vpi, ic, sv, si);
}

void NotifyCaptureFrameBoundary()
{
    uint64_t f = g_captureFrame.fetch_add(1);
    if ((f % 180) == 0)
    {
        g_logBudget = 25;
        Log("[counts] frame=%llu VSSetShader=%llu VSSetCB=%llu Map=%llu Unmap=%llu UpdateSubresource=%llu "
            "DrawIndexed=%llu Draw=%llu DrawIndexedInstanced=%llu DrawInstanced=%llu",
            (unsigned long long)f,
            (unsigned long long)g_countVSSetShader.load(), (unsigned long long)g_countVSSetCB.load(),
            (unsigned long long)g_countMap.load(), (unsigned long long)g_countUnmap.load(),
            (unsigned long long)g_countUpdateSubresource.load(),
            (unsigned long long)g_countDrawIndexed.load(), (unsigned long long)g_countDraw.load(),
            (unsigned long long)g_countDrawIndexedInstanced.load(), (unsigned long long)g_countDrawInstanced.load());
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
