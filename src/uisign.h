#pragma once
#include <d3d11.h>

// Journey's 2D layer (menu, title) drawn as a real panel standing in the room
// instead of on the screen: a textured quad whose corners are projected with
// each eye's tracked pose and FOV, sampling the texture the game's full-screen
// 2D-layer draw has bound (t0). Call instead of that draw, per eye, with that
// eye's context; false if it couldn't (no headset data, setup failed) - then
// the caller draws the game's way.
bool UiSignSetup(float distance, float size, float height);
bool UiSignDraw(ID3D11DeviceContext* ctx, int eye);
