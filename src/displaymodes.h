#pragma once

// Advertise square display modes (1440..3072) so the game accepts a square
// render resolution from vrmod_Flower.cfg. Call from DllMain.
void DisplayModesInstall(const wchar_t* dllDir);
