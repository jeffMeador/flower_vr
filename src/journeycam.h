#pragma once

// Journey's camera: widen the main camera's field of view while in VR, so the
// game draws (and doesn't cull) everything the headset can see when you turn
// your head. Flower's equivalent is in camoverride.cpp.
// headCamera: also turn the game camera by the head (rendering and culling follow it).
bool JourneyCamInstall(float fovDegrees, bool headCamera);
void JourneyCamTick(bool enable);
float JourneyCamGameFov();
// How far the scene camera moved last frame (game units); for debug captures.
float JourneyCamSpeed(); // the game's own vertical FOV (degrees) while ours is in place; 0 if unknown // once per frame: on while a VR session is running
