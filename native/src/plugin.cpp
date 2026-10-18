// SuperBLT native plugin entry points.
//
// Supported loader APIs negotiate bounded in-memory camera/settings/status.
// Unsupported loaders retain the original file bridge from startup.

#include <windows.h>

#include <cstdint>
#include <cstddef>
#include <initializer_list>

#include "bridge.h"
#include "hooks.h"
#include "log.h"
#include "state.h"
#include "memory-version.h"

struct lua_State;

// The thread SuperBLT loads the plugin on, which is Lua's. hooks.cpp compares the render thread with it.
DWORD g_luaThread = 0;
namespace {
using LuaFunction = int (*)(lua_State*);
struct LuaApi {
    int (*checkstack)(lua_State*, int);
    int (*type)(lua_State*, int);
    const char* (*tolstring)(lua_State*, int, size_t*);
    void (*pushboolean)(lua_State*, int);
    void (*pushnumber)(lua_State*, double);
    void (*pushstring)(lua_State*, const char*);
    void (*createtable)(lua_State*, int, int);
    void (*pushclosure)(lua_State*, LuaFunction, int);
    void (*setfield)(lua_State*, int, const char*);
    bool (*active)(lua_State*);
} api{};
bool apiReady = false;
bool Ready(lua_State* L, int stack) { return apiReady && L && api.checkstack(L, stack) && api.active(L); }
int BeginMemory(lua_State* L) {
    if (!Ready(L, 1)) return 0;
    bool ok = false;
    try { ok = Bridge::BeginMemory(); } catch (...) {}
    api.pushboolean(L, ok); return 1;
}
int Publish(lua_State* L, bool camera) {
    if (!Ready(L, 1)) return 0;
    bool ok = false;
    if (api.type(L, 1) == 4) {
        size_t size = 0; const char* text = api.tolstring(L, 1, &size);
        try { ok = camera ? Bridge::PublishCamera(text, size) : Bridge::PublishSettings(text, size); }
        catch (...) { ok = false; }
    } else {
        if (camera) Bridge::PublishCamera(nullptr, 0); else Bridge::PublishSettings(nullptr, 0);
    }
    api.pushboolean(L, ok); return 1;
}
int Camera(lua_State* L) { return Publish(L, true); }
int PublishSettingsLua(lua_State* L) { return Publish(L, false); }
int StatusText(lua_State* L) {
    if (!Ready(L, 1)) return 0;
    char text[32768]{};
    try { Bridge::CopyStatusText(text, sizeof(text)); } catch (...) { text[0] = 0; }
    api.pushstring(L, text); return 1;
}
void Num(lua_State* L, const char* name, uint64_t value) { api.pushnumber(L, static_cast<double>(value)); api.setfield(L, -2, name); }
void Flag(lua_State* L, const char* name, bool value) { api.pushboolean(L, value); api.setfield(L, -2, name); }
int BridgeStatusLua(lua_State* L) {
    if (!Ready(L, 4)) return 0;
    const auto s = Bridge::GetBridgeStatus();
    api.createtable(L, 0, 16);
    Flag(L, "memory_mode", s.memory); Flag(L, "pending", s.pending);
    Num(L, "published_camera", s.publishedCamera); Num(L, "consumed_camera", s.consumedCamera);
    Num(L, "applied_camera", s.appliedCamera); Num(L, "published_settings", s.publishedSettings);
    Num(L, "consumed_settings", s.consumedSettings);
    Num(L, "rejected_camera", s.rejectedCamera); Num(L, "rejected_settings", s.rejectedSettings);
    Num(L, "rejected", s.rejectedCamera + s.rejectedSettings);
    Num(L, "disk_camera_reads", s.diskCamera); Num(L, "disk_settings_reads", s.diskSettings);
    api.pushstring(L, s.memory ? "memory" : s.pending ? "pending" : "file"); api.setfield(L, -2, "transport");
    api.pushstring(L, MODULE_VERSION); api.setfield(L, -2, "version"); return 1;
}
// Stack room is guaranteed anyway: a C function called from Lua always has LUA_MINSTACK (20) free
// slots, and nothing here needs more than four at once. So the check is a courtesy, taken from
// whichever name the loader knows (SuperBLT on Linux/Proton 2026-10 offers luaL_checkstack only),
// or skipped.
void (*luaLCheckstack)(lua_State*, int, const char*) = nullptr;
int CheckstackViaAux(lua_State* L, int n) { luaLCheckstack(L, n, "Payday Screen Shaders"); return 1; }
int CheckstackAssumed(lua_State*, int) { return 1; }

void ResolveApi(void* (*resolve)(const char*)) {
    api = {}; apiReady = false;
    if (!resolve) { Log("loader gave no function lookup; live updates stay on files"); return; }
    // Name the first function the loader cannot provide: that is the whole diagnosis when a
    // SuperBLT build leaves the mod on the file bridge.
#define RESOLVE(member, name) api.member = reinterpret_cast<decltype(api.member)>(resolve(name)); \
    if (!api.member) { Log("loader has no %s; live updates stay on files", name); return; }
    api.checkstack = reinterpret_cast<decltype(api.checkstack)>(resolve("lua_checkstack"));
    if (!api.checkstack) {
        luaLCheckstack = reinterpret_cast<decltype(luaLCheckstack)>(resolve("luaL_checkstack"));
        api.checkstack = luaLCheckstack ? CheckstackViaAux : CheckstackAssumed;
        Log("loader has no lua_checkstack; using %s", luaLCheckstack ? "luaL_checkstack" : "the guaranteed C call stack room");
    }
    RESOLVE(type, "lua_type"); RESOLVE(tolstring, "lua_tolstring");
    RESOLVE(pushboolean, "lua_pushboolean"); RESOLVE(pushnumber, "lua_pushnumber"); RESOLVE(pushstring, "lua_pushstring");
    RESOLVE(createtable, "lua_createtable"); RESOLVE(pushclosure, "lua_pushcclosure"); RESOLVE(setfield, "lua_setfield");
    RESOLVE(active, "is_active_state");
#undef RESOLVE
    apiReady = true;
}
}

extern "C"
{
	__declspec(dllexport) uint64_t SBLT_API_REVISION = 1;
	__declspec(dllexport) const char* MODULE_LICENCE_DECLARATION = "This module is licenced under the GNU GPL version 2 or later, or another compatible licence";
	__declspec(dllexport) const char* MODULE_SOURCE_CODE_LOCATION = "Shipped with the mod: Payday Screen Shaders/native/src";
	__declspec(dllexport) const char* MODULE_SOURCE_CODE_REVISION = MODULE_VERSION;

	__declspec(dllexport) void SuperBLT_Plugin_Setup(void* (*resolve)(const char*))
	{
		g_luaThread = GetCurrentThreadId();
		Log("Payday Screen Shaders native %s loaded", MODULE_VERSION);

		State::Init();
		ResolveApi(resolve);
		Bridge::ConfigureMemoryCapability(apiReady);

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

	__declspec(dllexport) int SuperBLT_Plugin_PushLua(lua_State* L)
	{
		if (!Ready(L, 4)) {
			// No capability table can be delivered to this startup state. Permit
			// the legacy bridge only if memory has never been enabled. Once
			// negotiated, a later inactive state can never revive stale files.
			Bridge::ConfigureMemoryCapability(false);
			return 0;
		}
		// A healthy retry can upgrade startup file fallback to memory.
		// ConfigureMemoryCapability fences both consumers and keeps memory sticky.
		Bridge::ConfigureMemoryCapability(true);
		api.createtable(L, 0, 6);
		struct Export { const char* name; LuaFunction function; };
		for (const auto& e : {Export{"begin_memory", BeginMemory}, Export{"publish_camera", Camera},
		                     Export{"publish_settings", PublishSettingsLua}, Export{"status_text", StatusText}, Export{"bridge_status", BridgeStatusLua}}) {
			api.pushclosure(L, e.function, 0); api.setfield(L, -2, e.name);
		}
		api.pushstring(L, "memory-v1"); api.setfield(L, -2, "capability");
		return 1;
	}
}
