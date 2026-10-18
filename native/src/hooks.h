#pragma once

// Patches the DXGI swap chain vtable through a throwaway device (shared by every swap chain), then
// the game's own immediate context on its first present, and runs the effects at the
// moment the world is finished and the HUD is about to be drawn.
bool InstallHooks();
