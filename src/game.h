#pragma once
#include <string>

// Which thatgamecompany game we're loaded into (same engine: Flower, Journey),
// picked by the exe name. Everything game-specific about files lives here.
struct GameInfo
{
    const wchar_t* name;        // L"Flower", L"Journey"
    const char* nameA;          // same, for logs and OpenXR
    std::wstring settingsPath;  // the game's normal settings file (full path; empty if unknown)
    const wchar_t* settingsTail; // how the game's own open of that file ends (matched case-insensitively)
    std::wstring vrSettingsName; // VR copy next to the exe: vrmod_<name>.cfg
};

const GameInfo& Game();
