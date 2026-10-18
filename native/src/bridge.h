#pragma once

// Settings reach the plugin through a file, not the Lua C API.
//
// SuperBLT 4.0.2-beta (the current release) gives native plugins no Lua functions at all: its
// lookup only knows the few engine signatures it scans for, and LuaJIT's FFI is compiled out.
// So lua/core.lua writes "key value" lines ending in "end" to mods/saves/pss_live.txt, and a
// thread here polls it. The plugin writes mods/logs/pss_status.txt back for the status dialog.
namespace Bridge
{
	// Reads the file if it changed since the last call. Returns true when settings were applied.
	bool Poll(const char* path);

	void WriteStatus(const char* path);

	// Reads mods/saves/pss_camera.txt if Lua wrote a new line since the last call.
	void PollCamera(const char* path);
	// The same read, from the render thread, right before the shaders run: Lua writes the camera as
	// the frame starts rendering, so this is the camera that frame was drawn with.
	void PollCameraNow();

	void Start(const char* livePath, const char* statusPath);
}
