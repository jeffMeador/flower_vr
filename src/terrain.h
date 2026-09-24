#pragma once

// Called by the file redirect hook for every file the game opens; remembers
// the level's Heightmap.pvr so it can be loaded.
void TerrainNotifyFileOpened(const wchar_t* path);

// Feed the game camera position each frame (world units). Used to work out
// how the heightmap lines up with the world (camera never below ground).
void TerrainObserveCamera(float x, float y, float z);

// Ground height at world (x, z); false until the heightmap is loaded and its
// alignment is known.
bool TerrainHeight(float x, float z, float& out);
