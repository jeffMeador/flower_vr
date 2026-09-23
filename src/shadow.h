#pragma once
#include <d3d11.h>

// Phase 4: render both eyes every frame ("double render").
//
// Every render-target / depth texture the game creates gets a twin (the
// right-eye copy), and so does every view of it; twins hang off the originals
// as D3D private data, so they die with them. Each draw call runs twice:
// once as the game issued it (left eye), once with every bound render target,
// depth buffer and shader resource swapped to its twin (right eye). Clears,
// copies, resolves and mip generation are mirrored onto the twins, so the
// game's whole post-processing chain runs per eye. The right eye's final image
// ends up in the backbuffer's twin.

// Enabled by vrmod.ini [stereo] render=double. Must be called right after the
// device is created (before the game creates its render targets).
void ShadowInstall(ID3D11Device* device, ID3D11DeviceContext* context, bool enabled);
bool ShadowEnabled();

// Bind the right-eye twins of everything currently bound (outputs + shader
// resources). Returns false if no bound output has a twin (then the caller
// must not issue the right-eye draw: it would draw into the left target).
bool ShadowBindRightEye(ID3D11DeviceContext* ctx);
// Restore the game's own bindings after the right-eye draw.
void ShadowRestore(ID3D11DeviceContext* ctx);

// Twin of a resource (AddRef'd) or null.
ID3D11Resource* ShadowOfResource(ID3D11Resource* r);

// Mirror a whole-resource/region UpdateSubresource onto the twin.
void ShadowOnUpdateSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* src, UINT rowPitch, UINT depthPitch);

// Our own context calls (XR copies, frame dumps) must not be mirrored.
struct ShadowBypass { ShadowBypass(); ~ShadowBypass(); };
bool ShadowBypassed();

void ShadowLogStats();
