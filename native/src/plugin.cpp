// SuperBLT native plugin entry points.
//
// Nothing is exchanged through the Lua C API: the released SuperBLT hands plugins no Lua
// functions. Settings arrive through the file bridge (bridge.h) instead.

#include <windows.h>

#include <cstdint>

#include "bridge.h"
#include "hooks.h"
#include "log.h"
#include "state.h"

struct lua_State;

// The thread SuperBLT loads the plugin on, which is Lua's. hooks.cpp compares the render thread with it.
DWORD g_luaThread = 0;

extern "C"
{
	__declspec(dllexport) uint64_t SBLT_API_REVISION = 1;
	__declspec(dllexport) const char* MODULE_LICENCE_DECLARATION = "This module is licenced under the GNU GPL version 2 or later, or another compatible licence";
	__declspec(dllexport) const char* MODULE_SOURCE_CODE_LOCATION = "Shipped with the mod: Payday Screen Shaders/native/src";
	__declspec(dllexport) const char* MODULE_SOURCE_CODE_REVISION = MODULE_VERSION;

	__declspec(dllexport) void SuperBLT_Plugin_Setup(void* (*)(const char*))
	{
		g_luaThread = GetCurrentThreadId();
		Log("Payday Screen Shaders native %s loaded", MODULE_VERSION);

		State::Init();

		if (!InstallHooks())
			Log("hooks could not be installed; the mod will do nothing");

		Bridge::Start("mods/saves/pss_live.txt", "mods/logs/pss_status.txt");
	}

	__declspec(dllexport) void SuperBLT_Plugin_Init_State(lua_State*)
	{
	}

	__declspec(dllexport) void SuperBLT_Plugin_Update(lua_State*)
	{
	}

	__declspec(dllexport) int SuperBLT_Plugin_PushLua(lua_State*)
	{
		return 0;
	}
}
