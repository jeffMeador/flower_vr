#pragma once
#include <dxgi.h>
#include <d3d11.h>

// Cinema mode: instead of VR, the game's own flat view (its camera, its
// field of view) on a big screen in front of you, on a dark background.
// Shows exactly what a shot was framed for (Journey's intro shots show hard
// edges of the scene outside that frame in full VR). [xr] cinematicMode =
// follow (default: full VR, the game's camera turned by the head) or screen;
// CinemaToggle switches at runtime (Journey: right thumbstick click).
void CinemaInit(const wchar_t* ini);
bool CinemaActive();
float CinemaAspect(); // the screen's width / height
void CinemaToggle();
// Automatic mode inputs: the 2D layer (menu, title) was drawn / a tutorial
// prompt was drawn (you have control). CinemaUpdate once per frame.
void CinemaNoteUiLayer();
void CinemaNotePrompt();
// A camera cut to a camera this far (game units) from the player: close means
// the shot you play from (the end of the intro).
void CinemaNoteCameraCut(float distToPlayer);
void CinemaUpdate();
// Once per Present, before the eyes go to the headset: replaces both eye
// images with the screen when cinema mode is on.
void CinemaCompose(IDXGISwapChain* swapChain, ID3D11DeviceContext* ctx);
