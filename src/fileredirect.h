#pragma once

// Redirect the game's Documents\Flower\Flower.cfg to <gameDir>\vrmod_Flower.cfg
// (if that file exists), so VR can use its own resolution without touching the
// user's normal settings. Call from DllMain, before the game reads its config.
// Returns true if the redirect is active.
bool FileRedirectInstall(const wchar_t* gameDir);
