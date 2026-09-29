#pragma once

// First run without settings files (e.g. the README's copy step was skipped):
//  - vrmod.ini missing -> written with the recommended settings;
//  - vrmod_Flower.cfg missing -> made from the player's normal Flower.cfg
//    (Documents\Flower) with a square (the ini's maxSquare), 4x MSAA, windowed screen, so
//    the game doesn't render a widescreen/fullscreen image into VR.
// Existing files are never touched. Call from DllMain before the config is read.
void EnsureDefaultSettings(const wchar_t* dllDir);
