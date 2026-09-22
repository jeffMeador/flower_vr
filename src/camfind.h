#pragma once

// Debug tool: F7 finds the engine camera block and logs which instructions
// read/write its FOV (hardware watchpoint, ~2s). Call once per Present.
void CamFindTick(float backbufferAspect);
