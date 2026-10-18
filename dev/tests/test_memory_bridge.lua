-- Memory IPC integration: mocked native API, actual Lua payload generation and history.
-- From AndreikaRPM's fork (dev/lua_tests/test_memory_bridge.lua), kept as the memory bridge's own test.
-- Run: luajit dev/tests/test_memory_bridge.lua (from anywhere).
local T = (arg and arg[0] or ""):match("^(.*)/") or "."
ModPath, SavePath, log = T .. "/../../", T .. "/saves/", function() end
io.file_is_readable = function() return true end
io.load_as_json = function() return nil end
io.save_as_json = function() end
math.clamp = function(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local opened, writes, pushed = {}, {}, {}
local status = "\nCamera jitter: allowed\nFrames 3  processed 2\n"
io.open = function(path, mode)
 opened[#opened+1] = {path,mode}
 return {write=function(_,text) writes[path]=text end,read=function() return status end,close=function() end}
end
local attempts, accepted, throw_publish, fail_begin = 0, true, false, false
local api = {capability="memory-v1",begin_memory=function() attempts=attempts+1; return not fail_begin end,
 publish_camera=function(text) pushed.camera=text; if throw_publish then error("reject") end; return accepted end,
 publish_settings=function(text) pushed.settings=text; if throw_publish then error("reject") end; return accepted end,
 status_text=function() if throw_publish then error("no status") end; return status end,
 bridge_status=function() return {} end}
blt={load_native=function() return true,api end}
dofile(ModPath .. "lua/core.lua")
local p=PSS
p._luts={{path="custom LUT"}}
assert(p:native() and p._native_api==api and p._ipc_mode=="memory" and attempts==1)
p:write_bridge();assert(#opened==0 and pushed.settings:match("\nend\n$"))
local settings=pushed.settings
local line=0;p.camera_line=function() line=line+1; return "camera "..line.."\n" end
p.wants_camera=function() return true end
for i=1,7 do p:write_camera() end
assert(#opened==0 and pushed.camera=="camera 4\ncamera 5\ncamera 6\ncamera 7\n")
local camera=pushed.camera
p:poll_status(1);assert(p._jitter_allowed and not p._plugin_working)
status="\nCamera jitter: allowed\nFrames 4  processed 3\n";p:poll_status(2);assert(p._plugin_working)
status="\nCamera jitter: denied\nFrames 5  processed 4\n";p:poll_status(3);assert(not p._jitter_allowed)
assert(p:status_text()==status and #opened==0)
accepted=false;p:write_bridge();p:write_camera();assert(#opened==0)
throw_publish=true;p:write_bridge();p:write_camera();p:poll_status(4);assert(not p._jitter_allowed and #opened==0)
throw_publish=false;accepted=true
-- Supported-but-pending startup retries, never reads/replays stale files.
p._native=nil;p._native_api=nil;p._ipc_mode=nil;fail_begin=true
assert(p:native() and p._ipc_mode=="memory_failed")
p:write_bridge();p:write_camera();assert(p:read_status()==nil and #opened==0)
fail_begin=false;assert(p:native() and p._ipc_mode=="memory");p:write_bridge();assert(#opened==0)
-- Old/partial capabilities use the legacy path from startup, exact same payloads.
for _,cap in ipairs({false,{capability="memory-v1",begin_memory=api.begin_memory},{capability="other"}}) do
 p._native=nil;p._native_api=nil;p._ipc_mode=nil;blt.load_native=function() return true,cap end
 assert(p:native() and p._ipc_mode=="file")
 p:write_bridge();assert(writes[p.live_path]==settings)
 line=3;p._camera_lines={};for i=1,4 do p:write_camera() end;assert(writes[p.camera_path]==camera)
 assert(p:read_status()==status)
end
-- A resident DLL can withhold the table from a coroutine/later Lua caller.
local now, loads, table_ready, throw_lookup = 0, 0, false, false
TimerManager={wall=function()return {time=function()return now end}end}
local function new_client()
 p._native=nil;p._native_api=nil;p._ipc_mode=nil;p._api_lookup_pending=nil;p._api_retry_at=nil
end
blt.load_native=function()
 loads=loads+1
 if throw_lookup then error("injected lookup error")end
 local co, is_main = coroutine.running()
 if (co and not is_main) or not table_ready then return "Already loaded"end
 return "Already loaded",api
end
new_client();table_ready=true
local co=coroutine.create(function()p:write_bridge()end)
assert(coroutine.resume(co));assert(p._ipc_mode=="file" and p._api_lookup_pending and loads==1)
local opened_before=#opened
now=0.5;p:write_bridge();assert(loads==1 and #opened==opened_before+1)
now=1.1;throw_lookup=true;p:write_bridge();assert(loads==2 and p._api_lookup_pending and p._ipc_mode=="file")
now=2.2;throw_lookup=false;opened_before=#opened;p:write_bridge()
assert(loads==3 and p._ipc_mode=="memory" and not p._api_lookup_pending and #opened==opened_before)
-- Supported publication failure cannot return to file mode or restart lookup.
accepted=false;p:write_bridge();assert(loads==3 and #opened==opened_before and p._ipc_mode=="memory");accepted=true
-- Real old loaders continue to work, with bounded lookup overhead.
new_client();loads=0;now=0;table_ready=false
for i=1,30 do p:write_bridge()end
assert(loads==1 and p._ipc_mode=="file")
now=1;p:write_bridge();now=2;p:write_bridge();assert(loads==3 and p._ipc_mode=="file")
now=4;table_ready=true
co=coroutine.create(function()p:native()end);assert(coroutine.resume(co));assert(loads==3)
opened_before=#opened;p:write_bridge();assert(loads==4 and p._ipc_mode=="memory" and #opened==opened_before)
-- A backwards clock step cannot postpone recovery indefinitely.
new_client();now=10;table_ready=false;p:write_bridge();table_ready=true;now=0
opened_before=#opened;p:write_bridge();assert(p._ipc_mode=="memory" and #opened==opened_before)
print("PASS later-state API recovery, coroutine/main-state handoff, retry throttling, transient errors and legacy compatibility")

print("PASS memory forwarding, exact camera/settings payloads, no-file memory mode, status/jitter, rejection, retry, legacy capabilities")
