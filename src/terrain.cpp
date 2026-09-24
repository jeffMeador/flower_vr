#include "terrain.h"
#include "log.h"
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <cwctype>
#include <vector>
#include <string>

// Ground height lookup from the level's own heightmap
// (Data\Terrain\Level_N\Heightmap.pvr: PVR v3, RGBA8, 256 x 512 texels,
// height in channel 1 (G), per TerrainData.lua: gridScale 1.0, heightScale 25).
// Used to keep the VR viewpoint (which sits behind/above the game camera)
// out of the ground.

static const float kGridScale = 1.0f, kHeightScale = 25.0f;
static SRWLOCK g_lock = SRWLOCK_INIT;
static std::wstring g_pendingPath;
static std::vector<uint8_t> g_height; // G channel
static int g_w = 0, g_h = 0;
static int g_mapping = -1; // -1 = not decided; see Sample()

void TerrainNotifyFileOpened(const wchar_t* path)
{
    // Any file under ...\Data\Terrain\Level_N\ tells us the level (the game
    // loads its terrain bundle, not Heightmap.pvr itself); load that folder's
    // Heightmap.pvr ourselves.
    std::wstring p(path);
    for (auto& c : p) if (c == L'/') c = L'\\';
    std::wstring lower = p;
    for (auto& c : lower) c = towlower(c);
    size_t at = lower.find(L"\\terrain\\level_");
    if (at == std::wstring::npos) return;
    size_t end = lower.find(L'\\', at + 15);
    if (end == std::wstring::npos) return;
    std::wstring hm = p.substr(0, end) + L"\\Heightmap.pvr";
    static std::wstring last;
    if (hm == last) return;
    last = hm;
    AcquireSRWLockExclusive(&g_lock);
    g_pendingPath = hm;
    ReleaseSRWLockExclusive(&g_lock);
    Log("[terrain] level terrain folder seen: %ls", p.substr(0, end).c_str());
}

static void LoadPending()
{
    AcquireSRWLockExclusive(&g_lock);
    std::wstring path;
    path.swap(g_pendingPath);
    ReleaseSRWLockExclusive(&g_lock);
    if (path.empty()) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
    std::vector<uint8_t> file;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file.resize(size > 0 ? size : 0);
    if (size > 0) fread(file.data(), 1, size, f);
    fclose(f);
    if (file.size() < 52) return;

    uint32_t version, fmtLo, fmtHi, height, width, meta;
    memcpy(&version, &file[0], 4);
    memcpy(&fmtLo, &file[8], 4);
    memcpy(&fmtHi, &file[12], 4);
    memcpy(&height, &file[24], 4);
    memcpy(&width, &file[28], 4);
    memcpy(&meta, &file[48], 4);
    size_t off = 52 + meta;
    if (version != 0x03525650 || fmtLo != 0x61626772 || fmtHi != 0x08080808 ||
        off + (size_t)width * height * 4 > file.size())
    {
        Log("[terrain] unsupported heightmap %ls", path.c_str());
        return;
    }
    std::vector<uint8_t> h((size_t)width * height);
    for (size_t i = 0; i < h.size(); ++i) h[i] = file[off + i * 4 + 1];
    AcquireSRWLockExclusive(&g_lock);
    g_height.swap(h);
    g_w = (int)width; g_h = (int)height;
    ReleaseSRWLockExclusive(&g_lock);
    Log("[terrain] loaded %ls (%ux%u)", path.c_str(), width, height);
}

// mapping: 0 = (x, z) -> (col, row); 1 = (x, z) -> (col, rows-1-row);
//          2 = (x, z) -> (cols-1-col, row); 3 = transposed (z, x) -> (col, row)
static bool Sample(int mapping, float x, float z, float& out)
{
    if (g_height.empty()) return false;
    float u = x / kGridScale, v = z / kGridScale;
    int cols = g_w, rows = g_h;
    if (mapping == 3) { float t = u; u = v; v = t; }
    if (mapping == 1) v = rows - 1 - v;
    if (mapping == 2) u = cols - 1 - u;
    if (u < 0 || v < 0 || u > cols - 1 || v > rows - 1) return false;
    int x0 = (int)u, y0 = (int)v;
    int x1 = x0 + 1 < cols ? x0 + 1 : x0, y1 = y0 + 1 < rows ? y0 + 1 : y0;
    float fx = u - x0, fy = v - y0;
    auto H = [&](int cx, int cy) { return g_height[(size_t)cy * cols + cx] * (kHeightScale / 255.0f); };
    out = (H(x0, y0) * (1 - fx) + H(x1, y0) * fx) * (1 - fy) + (H(x0, y1) * (1 - fx) + H(x1, y1) * fx) * fy;
    return true;
}

bool TerrainHeight(float x, float z, float& out)
{
    LoadPending();
    if (g_mapping < 0) return false;
    return Sample(g_mapping, x, z, out);
}

void TerrainObserveCamera(float x, float y, float z)
{
    LoadPending();
    if (g_height.empty()) return;
    // Decide the mapping from data: the game keeps its own camera above the
    // ground, so the right mapping never puts the ground above the camera.
    static int samples = 0, below[4] = {}, inside[4] = {};
    static float minGap[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
    static DWORD lastLog = 0;
    for (int m = 0; m < 4; ++m)
    {
        float h;
        if (!Sample(m, x, z, h)) { inside[m]--; continue; }
        inside[m]++;
        float gap = y - h;
        if (gap < -0.05f) below[m]++;
        if (gap < minGap[m]) minGap[m] = gap;
    }
    samples++;
    if (GetTickCount() - lastLog > 3000)
    {
        lastLog = GetTickCount();
        float h0 = 0; Sample(0, x, z, h0);
        Log("[terrain] cam (%.1f %.1f %.1f) ground[m0]=%.2f | below: %d %d %d %d | min gap: %.2f %.2f %.2f %.2f | in-bounds: %d %d %d %d (of %d)",
            x, y, z, h0, below[0], below[1], below[2], below[3], minGap[0], minGap[1], minGap[2], minGap[3],
            inside[0], inside[1], inside[2], inside[3], samples);
    }
    if (g_mapping < 0 && samples >= 900) // ~10 s of flight
    {
        int best = -1;
        for (int m = 0; m < 4; ++m)
            if (inside[m] > samples * 0.9 && below[m] == 0 && (best < 0 || minGap[m] < minGap[best])) best = m;
        g_mapping = best >= 0 ? best : 99; // 99 = none fits: stay disabled
        Log("[terrain] mapping decided: %d%s", best, best < 0 ? " (none fits - terrain clamp disabled)" : "");
    }
}
