#pragma once
#include <d3d11.h>

// Batched double render. The right eye is recorded on a deferred context that
// mirrors every state change of the game's immediate context (with the
// right-eye twins substituted for render targets and shader resources) and is
// executed as one command list at Present - or earlier, when the game is
// about to change data the recorded draws still need (e.g. a staging upload).
//
// Drawing the eyes alternately per draw (ShadowBindRightEye) switches render
// targets twice per draw, which breaks every render pass: on tile-based GPUs
// (the Steam Frame's Adreno) both eyes took 13 ms of GPU time vs 4.4 ms for
// one. Recording keeps each eye's passes intact. [stereo] batch=0 restores
// the per-draw path.

enum MirrorStage { MS_VS, MS_PS, MS_GS, MS_HS, MS_DS };

void MirrorInstall(ID3D11Device* dev, ID3D11DeviceContext* imm, const wchar_t* ini);
bool MirrorActive();
ID3D11DeviceContext* MirrorContext();   // the deferred (right-eye) context
bool MirrorRightOutputsValid();         // every bound output has a twin
void MirrorNoteDraw();
void MirrorFlush(const char* reason);   // execute the recorded right-eye work now

// Called by the context hooks for the game's own calls, after they ran on the
// immediate context.
void MirrorSetShader(MirrorStage st, ID3D11DeviceChild* shader, ID3D11ClassInstance* const* inst, UINT n);
void MirrorSetConstantBuffers(MirrorStage st, UINT start, UINT n, ID3D11Buffer* const* bufs);
void MirrorSetShaderResources(MirrorStage st, UINT start, UINT n, ID3D11ShaderResourceView* const* views);
void MirrorSetRenderTargets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
void MirrorSetRenderTargetsAndUAVs(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
    UINT uavStart, UINT numUavs, ID3D11UnorderedAccessView* const* uavs, const UINT* counts);
void MirrorClearRTV(ID3D11RenderTargetView* v, const FLOAT color[4]);
void MirrorClearDSV(ID3D11DepthStencilView* v, UINT flags, FLOAT depth, UINT8 stencil);
void MirrorCopyResource(ID3D11Resource* dst, ID3D11Resource* src);
void MirrorCopyRegion(ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ssub, const D3D11_BOX* box);
void MirrorResolve(ID3D11Resource* dst, UINT dsub, ID3D11Resource* src, UINT ssub, DXGI_FORMAT fmt);
void MirrorGenerateMips(ID3D11ShaderResourceView* v);
void MirrorUpdateSubresource(ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch);
void MirrorClearState();

// Map/Unmap: writes to dynamic buffers are re-uploaded on the deferred context
// (so each recorded draw sees the data it had); any other Map flushes first.
void MirrorBeforeMap(ID3D11Resource* r, D3D11_MAP type);
void MirrorUnmap(ID3D11Resource* r, const void* data);

// Right-eye constant data written by the patcher (not by the game).
void MirrorWriteBuffer(ID3D11Resource* buf, const void* data, UINT size, D3D11_USAGE usage);

void MirrorLogStats();
