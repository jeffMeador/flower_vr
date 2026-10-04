#pragma once

// Debug tool: F7 finds the engine camera block and logs which instructions
// read/write its FOV (hardware watchpoint, ~2s). Call once per Present.
void CamFindTick(float backbufferAspect);
// vrmod_watch.txt next to the DLL ("<address> [w|rw]", address hex or exe+0xRVA)
// arms the same watchpoint on any address and logs the registers too.
void CamFindSetDir(const wchar_t* dllDir);
