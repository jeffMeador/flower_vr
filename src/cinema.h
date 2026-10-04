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
// Past the title menu (control was handed to you once).
bool CinemaInGame();
float CinemaAspect(); // the screen's width / height
void CinemaToggle();
// Automatic mode inputs: the 2D layer (menu, title) was drawn / a tutorial
// prompt was drawn (you have control). CinemaUpdate once per frame.
void CinemaNoteUiLayer();
void CinemaNotePrompt();
// A camera cut to a camera this far (game units) from the player; settled:
// called again once that shot has held still (close + settled = the shot you
// play from, the end of the intro). Far cuts under a title = the idle screen.
void CinemaNoteCameraCut(float distToPlayer, bool settled);
// A title card (the "JOURNEY" logo) was drawn this frame.
void CinemaNoteTitle();
// A game level started loading (Journey: Data\...\Level_<name>).
void CinemaNoteLevel(const wchar_t* level);
// Journey, once per frame before it renders: its camera director mode, the
// camera's distance to the character, whether the camera cut this frame.
// True: hold the camera where it was (fading out to the cinema screen).
bool CinemaFrameCamera(float mode, bool haveMode, float dist, bool distKnown, bool cut);
// Pause or idle: the "JOURNEY" logo is hidden (it orbits with the camera).
bool CinemaHideTitle();
void CinemaUpdate();
// Once per Present, before the eyes go to the headset: replaces both eye
// images with the screen when cinema mode is on.
void CinemaCompose(IDXGISwapChain* swapChain, ID3D11DeviceContext* ctx);
