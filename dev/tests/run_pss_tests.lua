-- Headless tests for Payday Screen Shaders (Lua side). Run: luajit run_pss_tests.lua
-- The native side has its own test (dev/nativebuild/test/pss_test.cpp) run under Proton.
local T = (arg and arg[0] or ""):match("^(.*)/") or "."
local ROOT = T .. "/../../"
ModPath = ROOT
SavePath = T .. "/saves/"
os.remove(SavePath .. "pss_live.txt")

local passed, failed = 0, 0
local function check(name, cond, detail)
	if cond then passed = passed + 1 print("PASS " .. name)
	else failed = failed + 1 print("FAIL " .. name .. (detail and (" -- " .. tostring(detail)) or "")) end
end

local function read(path)
	local f = io.open(path, "rb")
	if not f then return nil end
	local s = f:read("*a")
	f:close()
	return s
end

-- What the plugin would see: the same parsing rules as native/src/bridge.cpp.
local function live()
	local text = read(SavePath .. "pss_live.txt")
	if not text then return nil end
	local t = { _complete = text:match("\nend\n$") ~= nil, _text = text }
	for line in text:gmatch("[^\n]+") do
		local k, v = line:match("^(%S+) ?(.*)$")
		if k and k ~= "end" then t[k] = tonumber(v) or v end
	end
	return t
end

function log(s) LOG = LOG or {} LOG[#LOG + 1] = s end
math.clamp = function(v, a, b) return v < a and a or v > b and b or v end
io.file_is_readable = function(p) return p:match("pss%.dll$") ~= nil or p:match("%.json$") and SAVED_FILE ~= nil end
io.load_as_json = function() return SAVED_FILE end
io.save_as_json = function(d) SAVED = d end
file = { GetFiles = function(dir)
	local list = {}
	local p = io.popen('ls "' .. dir .. '"')
	for name in p:lines() do list[#list + 1] = name end
	p:close()
	return list
end }
HOOKS = {}
POSTHOOKS = {}
Hooks = { Add = function(_, n, id, f) HOOKS[n] = f end, PostHook = function(_, cls, name, id, f) POSTHOOKS[name] = f end, PreHook = function(_, cls, name, id, f) POSTHOOKS["pre_" .. name] = f end }
Setup = Setup or {}

-- SuperBLT 4.0.2-beta: load_native works but hands nothing back (the plugin has no Lua API).
local LOADS = 0
blt = { load_native = function(path) LOADS = LOADS + 1 return LOADS == 1 and true or "Already loaded" end }

-- The game has these before any mod file is read.
Idstring = Idstring or function(s) return s end
alive = alive or function(o) return o ~= nil and o ~= false end
CoreEnvironmentControllerManager = CoreEnvironmentControllerManager or { refresh_render_settings = function() end }

dofile(ROOT .. "lua/core.lua")
local PSS = _G.PSS
local LOC = require_loc and require_loc() or (function()
	local text = read(ROOT .. "loc/english.json")
	local t = {}
	for k, v in text:gmatch('"([%w_]+)": "(.-)"') do t[k] = v end
	return t
end)()
check("core loads", PSS ~= nil)
PSS.status_path = SavePath .. "pss_status.txt"

-- ---- the three parameter lists must agree: Lua, C++ and HLSL ----
local cpp = read(ROOT .. "native/src/params.cpp")
local native = {}
local native_order = {}
for name, def, lo, hi in cpp:gmatch('{"([%w_]+)", ([%-%d%.]+)f, ([%-%d%.]+)f, ([%-%d%.]+)f}') do
	native[name] = { def = tonumber(def), min = tonumber(lo), max = tonumber(hi) }
	native_order[#native_order + 1] = name
end
check("params.cpp parsed", #native_order == 104, #native_order)

local header = read(ROOT .. "native/src/params.h")
local enum_order = {}
for name in header:gmatch("\n%s*P_([A-Z0-9_]+)[%s,=]") do
	if name ~= "COUNT" then enum_order[#enum_order + 1] = name:lower() end
end
local same_order = #enum_order == #native_order
for i = 1, #enum_order do same_order = same_order and enum_order[i] == native_order[i] end
check("params.h enum order matches the params.cpp table", same_order)

for _, param in ipairs(PSS.PARAMS) do
	if param.kind ~= "lut" and not param.lua_only then
		local n = native[param.key]
		check("native knows '" .. param.key .. "'", n ~= nil)
		if n and param.kind == "slider" and param.key == "upscale_scale" then
			check("'upscale_scale' runs below 1x in Lua only, where it becomes the render scale", param.min == 0.5 and n.min == 1 and n.max == param.max)
		elseif n and param.kind == "slider" then
			check("'" .. param.key .. "' has the same range in Lua and native", n.min == param.min and n.max == param.max,
				string.format("lua %s..%s native %s..%s", param.min, param.max, n.min, n.max))
		end
	end
end

local hlsl = read(ROOT .. "shaders/pss.hlsl")
local hlsl_ok, hlsl_detail = true, nil
for i, name in ipairs(native_order) do
	local slot = string.format("P%%[%d%%]%%.%s", math.floor((i - 1) / 4), ({ "x", "y", "z", "w" })[(i - 1) % 4 + 1])
	local define = hlsl:match("#define%s+" .. name:upper() .. "%s+(P%[%d+%]%.[xyzw])")
	if define then
		if not define:match("^" .. slot .. "$") then
			hlsl_ok, hlsl_detail = false, name .. " is " .. define .. ", expected slot " .. (i - 1)
		end
	elseif name ~= "inject_after" and name ~= "require_fullscreen" and not name:match("^unused_") then
		hlsl_ok, hlsl_detail = false, name .. " has no #define in pss.hlsl"
	end
end
check("every shader #define reads the constant buffer slot params.h gives it", hlsl_ok, hlsl_detail)

for entry in ("VS_Fullscreen PS_FXAA PS_Luma PS_Adapt PS_BloomPrefilter PS_BloomDown PS_BloomUp PS_Grade PS_Final PS_WideDown PS_TintPrefilter PS_HaloPrefilter PS_LightInfo PS_Easu PS_Ao PS_AoBlur PS_SmaaEdges PS_SmaaWeights PS_SmaaBlend PS_Focus PS_DofPrepare PS_DofBlur PS_DofFill PS_DofComposite PS_Il PS_Rtgi PS_Ssr PS_DepthBlur PS_MotionInput PS_MotionSearch PS_Temporal PS_MotionBlur PS_ShaftMask PS_Shafts PS_Grain PS_Downsample PS_LinearDepth PS_SmaaT2x PS_Taa PS_TaaSync"):gmatch("%S+") do
	check("shader entry point " .. entry .. " exists", hlsl:match("[%s]" .. entry .. "%(") ~= nil)
	check("the plugin compiles entry point " .. entry, read(ROOT .. "native/src/effects.cpp"):match('"' .. entry .. '"') ~= nil)
end

for _, param in ipairs(PSS.PARAMS) do
	if param.gate then
		check("'" .. param.key .. "' is switched by a Lua-only toggle", PSS.BY_KEY[param.gate] and PSS.BY_KEY[param.gate].lua_only)
	end
end
-- The colour-only shape effects are still there, but with real occlusion and haze shipping on, they
-- matter only where there is no depth: obscurance steps aside entirely, and the cues stay gentle.
check("the colour-only shape effects are still on for when there is no depth", PSS.BY_KEY.obscurance_on.default and PSS.BY_KEY.cues_on.default and PSS.BY_KEY.cues.default <= 0.35)

-- 0.3.0: lens dirt is on but gentle; halation and both projections ship off.
check("lens dirt is available but off in the shipped look", PSS.BY_KEY.dirt_on.default == false and PSS.BY_KEY.dirt.min == 0)
check("halation ships off, and the projections are gone for good", PSS.BY_KEY.halation_on.default == false and PSS.BY_KEY.spherical_on == nil and PSS.BY_KEY.panini_on == nil)
local dirt_blob = read(ROOT .. "shaders/lens_dirt.bin")
check("the lens dirt texture ships with the mod", dirt_blob ~= nil and dirt_blob:sub(1, 4) == "PSSD" and #dirt_blob > 100000, dirt_blob and #dirt_blob)

-- 0.4.0: FSR 1.0 upscaling. The whole stage ships off, EASU feeds RCAS, the final pass fits the
-- result back to the screen, and the existing CAS pass must not sharpen the same frame twice.
local effects_cpp = read(ROOT .. "native/src/effects.cpp")
check("FSR upscaling ships on in the Enhanced look", PSS.BY_KEY.upscale.default == true and PSS.BY_KEY.upscale_scale.default >= 1.25)
check("the upscale factor runs 0.5x to 2x, in steps of 0.05 (below 1x it is the render scale cleanup)", PSS.BY_KEY.upscale_scale.min == 0.5 and PSS.BY_KEY.upscale_scale.max == 2 and PSS.BY_KEY.upscale_scale.step == 0.05)
check("EASU and RCAS have their own sharpness, separate from CAS", PSS.BY_KEY.upscale_sharpness ~= nil and PSS.BY_KEY.sharpen.default == 0.45)
check("the final pass resolves the upscaled buffer instead of point sampling it", hlsl:match("float3 ResolveUpscaled%(") ~= nil and hlsl:match("upscaled %? ResolveUpscaled%(uv%)") ~= nil)
check("the CAS pass is bypassed while FSR upscaling is on", hlsl:match("SHARPEN > 0%.001 && !upscaled") ~= nil)
check("RCAS is applied at screen size, in the final pass", hlsl:match("PS_Final") ~= nil and hlsl:match("Rcas") ~= nil)
check("the shader is told whether the larger buffer really exists", effects_cpp:match("frame%[8 %+ P_UPSCALE%] = upscale") ~= nil)
check("switching the stage off gives the video memory back", effects_cpp:match("ReleaseUpscale%(%)") ~= nil)

-- ---- depth (0.5.0) ----
-- ---- SMAA and the new depth of field ----
local smaa = read(ROOT .. "shaders/smaa.hlsl")
check("SMAA ships with its MIT notice intact", smaa ~= nil and smaa:match("Jorge Jimenez") ~= nil and smaa:match("Permission is hereby granted") ~= nil)
check("SMAA is pasted in by the plugin, not left to Wine's include handling", hlsl:match('#include "smaa%.hlsl"') ~= nil and effects_cpp:match('#include \\"') ~= nil)
check("SMAA's lookup textures ship with the mod", (function()
	local area, search = read(ROOT .. "shaders/smaa_area.bin"), read(ROOT .. "shaders/smaa_search.bin")
	return area ~= nil and search ~= nil and area:sub(1, 4) == "PSSS" and #area == 16 + 160 * 560 * 2 and #search == 16 + 64 * 16
end)())
check("SMAA T2x is the default antialiasing, one choice among Off, FXAA, SMAA, SMAA T2x and TAA", PSS.BY_KEY.antialiasing.default == 4 and PSS.BY_KEY.antialiasing.options == 5 and PSS.BY_KEY.aa_mode.options == 3)
check("SMAA edge detection reads gamma encoded colour on any target", hlsl:match("#define SMAASamplePoint%(tex, coord%) float4%(ReadGamma") ~= nil)
check("depth of field focuses on the crosshair by default", PSS.BY_KEY.dof_focus.default == 0 and hlsl:match("kFocusTaps") ~= nil)
check("depth of field keeps your weapon sharp by default", PSS.BY_KEY.dof_weapon.default == true and hlsl:match("kWeaponDistance") ~= nil)
check("refocusing eases in log space", hlsl:match("exp%(lerp%(log%(") ~= nil)
check("the old grade-pass depth of field is gone", hlsl:match("fades into the quarter") == nil and effects_cpp:match("cues || dof") == nil)

-- ---- lighting, motion, atmosphere ----

check("temporal accumulation is on by default, for whichever of those gets switched on", PSS.BY_KEY.temporal.default == true)
check("the camera line is only written while something needs it", (function()
	for _, key in ipairs({"ao_on", "il_on", "rtgi_on", "ssr_on", "hfog_on", "taa_on"}) do
		local found = false
		for _, user in ipairs(PSS.CAMERA_USERS) do found = found or user == key end
		if not found then return false, key end
	end
	local saved = {}
	for _, key in ipairs({"ao_on", "il_on", "rtgi_on", "ssr_on", "hfog_on", "taa_on", "dof_on"}) do saved[key] = PSS.settings[key]; PSS.settings[key] = false end
	local mode = PSS.settings.aa_mode
	PSS.settings.aa_mode = 2
	local idle = PSS:wants_camera()
	PSS.settings.ssr_on = true
	local busy = PSS:wants_camera()
	PSS.settings.ssr_on = false
	PSS.settings.dof_on = true
	local focusing = PSS:wants_camera()
	PSS.settings.dof_on = false
	PSS.settings.aa_mode = 3
	local t2x = PSS:wants_camera()
	PSS.settings.aa_mode = mode
	for key, value in pairs(saved) do PSS.settings[key] = value end
	return idle == false and busy == true and focusing == true and t2x == true
end)())
check("the plugin reads the camera right before each frame, on the render thread", read(ROOT .. "native/src/bridge.cpp"):match("PollCameraNow") ~= nil)
check("a camera file left behind never keeps the camera valid: only a changed file counts", read(ROOT .. "native/src/bridge.cpp"):match("sameTime") ~= nil)

-- The projection is measured through a mocked world_to_screen with a known field of view: the line
-- written must give those tangents back, whatever the aspect.
check("the camera's projection is measured, not assumed", (function()
	local vec = {}
	vec.__index = vec
	local function v(x, y, z) return setmetatable({x = x, y = y, z = z}, vec) end
	vec.__add = function(a, b) return v(a.x + b.x, a.y + b.y, a.z + b.z) end
	vec.__mul = function(a, k) return v(a.x * k, a.y * k, a.z * k) end
	vec.__sub = function(a, b) return v(a.x - b.x, a.y - b.y, a.z - b.z) end
	local tan_x, tan_y, w, h = 0.9, 0.39, 1280, 720
	local rot = {x = function() return v(1, 0, 0) end, y = function() return v(0, 1, 0) end, z = function() return v(0, 0, 1) end,
		pitch = function() return 12.5 end, roll = function() return -3 end, yaw = function() return 0 end}
	local cam = {position = function() return v(100, 200, 350) end, rotation = function() return rot end}
	local workspace = {
		world_to_screen = function(_, _, p)
			local rel = v(p.x - 100, p.y - 200, p.z - 350)
			return v(w / 2 + rel.x / rel.y / tan_x * w / 2, h / 2 - rel.z / rel.y / tan_y * h / 2, 0)
		end,
		panel = function() return {w = function() return w end, h = function() return h end} end,
	}
	local saved = managers
	local steelsight = true
	local player = {movement = function() return {current_state = function() return {in_steelsight = function() return steelsight end} end} end}
	managers = {viewport = {get_current_camera = function() return cam end}, hud = {_workspace = workspace},
		player = {player_unit = function() return player end}}
	local line = PSS:camera_line()
	steelsight = false
	local lowered = PSS:camera_line()
	managers = saved
	if not line then return false, "no line" end
	-- tangents, pitch, roll, height, aiming, then the jitter, the frame, the position, the axes, the jitter state and the game state
	local tx, ty, pitch, roll, height, aim = line:match("^(%S+) (%S+) (%S+) (%S+) (%S+) (%S+) ")
	tx, ty, pitch, roll, height, aim = tonumber(tx), tonumber(ty), tonumber(pitch), tonumber(roll), tonumber(height), tonumber(aim)
	local lowered_aim = tonumber(lowered:match("^%S+ %S+ %S+ %S+ %S+ (%S+) "))
	local axes = 0
	for _ in line:gmatch("%S+") do axes = axes + 1 end
	return math.abs(tx - tan_x) < 0.001 and math.abs(ty - tan_y) < 0.001 and pitch == 12.5 and roll == -3 and height == 350
		and aim == 1 and lowered_aim == 0 and axes == 12 + 18 + 2, line
end)())
check("the camera line is skipped in menus with no world camera", (function()
	local saved = managers
	managers = {viewport = {get_current_camera = function() return nil end}}
	local line = PSS:camera_line()
	managers = saved
	return line == nil
end)())
check("the lighting and atmosphere pages exist, each with its own reset", (function()
	local menu = read(ROOT .. "lua/menu.lua")
	return menu:match('id = "pss_menu_lighting",%s*reset = true') ~= nil and menu:match('id = "pss_menu_atmosphere",%s*reset = true') ~= nil
end)())
check("light shafts, fog glow and lens dirt share one light measurement", effects_cpp:match("bool lightInfo = dirt || shafts || fogGlow") ~= nil)
check("the lighting buffers are only made while something uses them", effects_cpp:match("EnsureAdvanced%(desc%.Width, desc%.Height%)") ~= nil and effects_cpp:match("ReleaseAdvanced%(%);") ~= nil)
check("history is only trusted where the depth agrees", hlsl:match("abs%(depthNow %- depthThen%)") ~= nil)
check("height fog needs the camera, and stops without it", hlsl:match("HFOG > 0%.0 && CAMERA_VALID > 0%.5") ~= nil)

-- The enhanced defaults: what an improved PAYDAY 2 looks like, aimed at PAYDAY 3.
check("the enhanced defaults switch on occlusion, bounce light, ray traced lighting, reflections, haze, shafts and tone mapping", (function()
	for _, key in ipairs({"ao_on", "il_on", "rtgi_on", "ssr_on", "fog_on", "shafts_on", "ltm_on", "dof_on", "temporal",
		"upscale", "demoire_on", "detail_on", "mblur_on"}) do
		if PSS.BY_KEY[key].default ~= true then return false, key end
	end
	return true
end)())
check("depth of field is subtle from the hip and stronger while aiming", PSS.BY_KEY.dof_aim.default > PSS.BY_KEY.dof.default and PSS.BY_KEY.dof.default <= 0.15)
check("the lens eases between the two sizes rather than switching", hlsl:match("lerp%(DOF, DOF_AIM, saturate%(aim%)%)") ~= nil)
check("moire reduction only touches detail that alternates, not real edges", hlsl:match("alternating %* 4%.0 %- across %* 0%.8") ~= nil)
check("texture detail amplifies what is there, held back at edges", hlsl:match("g %+= fine %* DETAIL") ~= nil and hlsl:match("1%.0 %- edge %* 0%.75") ~= nil)
check("surface grain travels with the surfaces, and is re-seeded where it cannot", hlsl:match("float4 PS_Grain%(") ~= nil and hlsl:match("Tex0%.SampleLevel%(Linear, before, 0%)%.r") ~= nil)
check("the shader is only told about grain once a buffer holds it", effects_cpp:match("frame%[8 %+ P_SURFACE_GRAIN%] = surfaceGrain") ~= nil)
check("rays start where the indirect light's reach ends, so bounce light is not counted twice", hlsl:match("float start = IL > 0%.0 %? min%(IL_RADIUS") ~= nil)

-- Temporal antialiasing: the jitter, as the fork built it (0.6.x): a Halton cycle of half a pixel
-- for TAA, two opposite quarter pixels for SMAA T2x, and never a move unless the plugin says the
-- frames really show it.
check("TAA and its jitter ship on, with SMAA T2x doing the work until the jitter is confirmed", PSS.BY_KEY.taa_on.default == true and PSS.BY_KEY.taa_jitter.default == true and PSS.BY_KEY.render_scale.default == 1)
check("the camera is not jittered unless the plugin confirms the frames show it", (function()
	PSS.settings.enabled, PSS.settings.aa, PSS.settings.aa_mode, PSS.settings.taa_on, PSS.settings.taa_jitter = true, true, 2, true, true
	PSS._jitter_allowed = false
	local j = PSS:next_jitter()
	local idle = j.x == 0 and j.y == 0 and PSS._jitter_state == 2
	PSS._jitter_allowed = true
	j = PSS:next_jitter()
	local moving = (j.x ~= 0 or j.y ~= 0) and PSS._jitter_state == 0
	return idle and moving
end)())
check("that confirmation comes from the plugin's own status line", (function()
	local core = read(ROOT .. "lua/core.lua")
	local bridge = read(ROOT .. "native/src/bridge.cpp")
	return core:match("Camera jitter: allowed") ~= nil and bridge:match("Camera jitter: ") ~= nil
end)())
check("the jitter stays within half a pixel and walks a spread of positions", (function()
	PSS.settings.aa_mode, PSS.settings.taa_on, PSS.settings.taa_jitter, PSS._jitter_allowed = 2, true, true, true
	local seen, biggest, spread = {}, 0, 0
	for _ = 1, 8 do
		local j = PSS:next_jitter()
		local key = string.format("%.3f,%.3f", j.x, j.y)
		if seen[key] then return false, "repeats within one cycle" end
		seen[key] = true
		biggest = math.max(biggest, math.abs(j.x), math.abs(j.y))
		spread = spread + math.abs(j.x)
	end
	return biggest <= 0.5 and biggest > 0.1 and spread > 0.5, string.format("biggest %.3f spread %.3f", biggest, spread)
end)())
check("SMAA T2x jitters a quarter pixel one way and then the other", (function()
	PSS.settings.aa, PSS.settings.aa_mode, PSS.settings.taa_on, PSS.settings.taa_jitter, PSS._jitter_allowed = true, 3, false, true, true
	local a = PSS:next_jitter(); local ax, ay = a.x, a.y
	local b = PSS:next_jitter()
	return PSS:t2x() and math.abs(ax) == 0.25 and math.abs(ay) == 0.25 and b.x == -ax and b.y == -ay
end)())
check("with no antialiasing there is no jitter", (function()
	PSS.settings.aa, PSS.settings.taa_on = false, false
	local j = PSS:next_jitter()
	PSS.settings.aa, PSS.settings.aa_mode, PSS.settings.taa_on = true, 3, true
	return j.x == 0 and j.y == 0 and PSS._jitter_state == 1
end)())
check("the jitter is applied after the viewport update, and the heist update measures the camera every frame", (function()
	local update = read(ROOT .. "lua/update.lua")
	return update:match("jitter_after_viewport_update") ~= nil and update:match('PostHook%(Setup, "pre_render"') ~= nil and update:match("PSS:write_camera%(%)") ~= nil
end)())
-- Panini and everything else happen in the final pass; the accumulation must come before them, or
-- the warp would feed back into its own history.
-- The temporal passes run on the graded frame (so the grade's own dither is averaged and then
-- redone in the final pass), and before the final pass, which fits the result to the screen.
check("the temporal passes run after the grade and before the final pass", (function()
	local grade = effects_cpp:find("Pass(ctx, m_psGrade", 1, true)
	local t2x = effects_cpp:find("Pass(ctx, m_psSmaaT2x", 1, true)
	local taa = effects_cpp:find("Pass(ctx, m_psTaa,", 1, true)
	local final = effects_cpp:find("Pass(ctx, m_psFinal", 1, true)
	return grade and t2x and taa and final and grade < t2x and t2x < taa and taa < final
end)())
check("render scale shrinks the frame and rebuilds it with EASU", effects_cpp:match("m_psDownsample.-m_psEasu, m_rescaled") ~= nil)
check("the smaller buffers are given back when the render scale returns to 1", effects_cpp:match("ReleaseScaled%(%)") ~= nil)

-- The game's own versions of what this mod does are turned off while it is on. Light adaptation is
-- not one of them: this mod has no eye adaptation, and forcing it off only left the game dark.
local function fake_user(store)
	return {
		user = {get_setting = function(_, k) return store[k] end, set_setting = function(_, k, v) store[k] = v end},
		savefile = {save_setting = function() store._disk_writes = (store._disk_writes or 0) + 1 end}
	}
end

check("the game's own antialiasing, occlusion and depth of field are the ones turned off", (function()
	local keys = {}
	for _, e in ipairs(PSS.VANILLA_OFF) do keys[e.key] = e.off end
	return keys.video_aa == "off" and keys.video_ao == "off" and keys.dof_setting == "none"
		and keys.video_color_grading == nil and keys.video_anti_alias == nil
end)())
check("light adaptation is left alone", (function()
	for _, e in ipairs(PSS.VANILLA_OFF) do
		if e.key == "light_adaption" then return false end
	end
	return true
end)())
check("they are forced off, and put back when the switch is turned off", (function()
	local store = {video_aa = "fxaa", video_ao = "ssao", light_adaption = true, dof_setting = "standard"}
	local saved, native = managers, PSS._native
	managers = fake_user(store)
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native, PSS._plugin_working = true, true, true, true
	PSS._vanilla_saved, PSS._vanilla_repaired = nil, PSS.REPAIR_GENERATION
	PSS:sync_vanilla_settings()
	local forced = store.video_aa == "off" and store.video_ao == "off" and store.dof_setting == "none"
	PSS.settings.vanilla_off = false
	PSS:sync_vanilla_settings()
	local restored = store.video_aa == "fxaa" and store.video_ao == "ssao" and store.dof_setting == "standard"
	PSS.settings.vanilla_off, managers, PSS._native = true, saved, native
	return forced and restored, string.format("forced %s restored %s", tostring(forced), tostring(restored))
end)())
check("the player's own values are written to the save file, not just held in memory", (function()
	local store = {video_aa = "fxaa", video_ao = "ssao", dof_setting = "standard"}
	local saved, native, was = managers, PSS._native, SAVED
	managers = fake_user(store)
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native, PSS._plugin_working = true, true, true, true
	PSS._vanilla_saved, PSS._vanilla_repaired = nil, PSS.REPAIR_GENERATION
	PSS:sync_vanilla_settings()
	-- A later session reads them back, so removing the mod or a crash cannot strand the settings.
	local written = SAVED and SAVED.vanilla_saved and SAVED.vanilla_saved.video_aa == "fxaa"
		and SAVED.vanilla_saved.dof_setting == "standard"
	managers, PSS._native, SAVED = saved, native, was
	return written
end)())
check("a restart still knows what to give back", (function()
	local was_file, was_saved = SAVED_FILE, PSS._vanilla_saved
	SAVED_FILE = {vanilla_saved = {video_aa = "fxaa"}, vanilla_repaired = true}
	PSS:load()
	local kept = PSS._vanilla_saved and PSS._vanilla_saved.video_aa == "fxaa" and PSS._vanilla_repaired == 1
	SAVED_FILE = was_file
	PSS:load()
	PSS._vanilla_saved = was_saved
	return kept
end)())
check("nothing is turned off when the plugin did not load", (function()
	local store = {video_aa = "fxaa", video_ao = "ssao", dof_setting = "standard"}
	local saved, native = managers, PSS._native
	managers = fake_user(store)
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native = true, true, false
	PSS._vanilla_saved, PSS._vanilla_repaired = nil, PSS.REPAIR_GENERATION
	PSS:sync_vanilla_settings()
	local untouched = store.video_aa == "fxaa" and store.video_ao == "ssao" and store.dof_setting == "standard"
	managers, PSS._native = saved, native
	return untouched
end)())
check("light adaptation an older build turned off is given back once", (function()
	local store = {light_adaption = false, video_aa = "off", video_ao = "off", dof_setting = "none"}
	local saved, native = managers, PSS._native
	managers = fake_user(store)
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native, PSS._plugin_working = true, true, true, true
	PSS._vanilla_saved, PSS._vanilla_repaired = nil, 0
	PSS:sync_vanilla_settings()
	local back = store.light_adaption == true
	-- And only once: turning it off again is the player's choice from then on.
	store.light_adaption = false
	PSS:sync_vanilla_settings()
	local left_alone = store.light_adaption == false
	managers, PSS._native = saved, native
	return back and left_alone, string.format("back %s left alone %s", tostring(back), tostring(left_alone))
end)())
-- 0.5.1 marked its repair done in this mod's own file at once, but the game writes its settings to
-- disk later - and 0.5.1 crashed on leaving a heist. The repair runs again, and is saved this time.
check("a repair a crash threw away is run again, and written to disk", (function()
	local was_file = SAVED_FILE
	SAVED_FILE = {vanilla_repaired = true}   -- what 0.5.1 to 0.5.3 wrote
	PSS:load()
	local legacy = PSS._vanilla_repaired == 1
	local store = {light_adaption = false}
	local saved, native = managers, PSS._native
	managers = fake_user(store)
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native, PSS._plugin_working = true, true, true, true
	PSS._vanilla_saved = nil
	PSS:sync_vanilla_settings(0)
	local back = store.light_adaption == true and (store._disk_writes or 0) >= 1
	local done = PSS._vanilla_repaired == PSS.REPAIR_GENERATION
	managers, PSS._native = saved, native
	SAVED_FILE = was_file
	PSS:load()
	return legacy and back and done, string.format("legacy %s back %s done %s", tostring(legacy), tostring(back), tostring(done))
end)())
-- Removing the mod must never leave the game's own options switched off (0.5.6).
check("the game's own settings are only taken away while the plugin really draws", (function()
	local store = { video_aa = "fxaa", video_ao = "ssao", dof_setting = "standard", light_adaption = true }
	local saved, native, working = managers, PSS._native, PSS._plugin_working
	managers = fake_user(store)
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native, PSS._plugin_working = true, true, true, false
	PSS._vanilla_saved, PSS._vanilla_repaired = nil, PSS.REPAIR_GENERATION
	PSS:sync_vanilla_settings(0)
	local left_alone = store.video_aa == "fxaa" and store.video_ao == "ssao"
	managers, PSS._native, PSS._plugin_working = saved, native, working
	return left_alone
end)())
check("quitting gives them back, and keeps the preference for next time", (function()
	local store = { video_aa = "fxaa", video_ao = "ssao", dof_setting = "standard", light_adaption = true }
	local saved, native, working = managers, PSS._native, PSS._plugin_working
	managers = fake_user(store)
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native, PSS._plugin_working = true, true, true, true
	PSS._vanilla_saved, PSS._vanilla_repaired = nil, PSS.REPAIR_GENERATION
	PSS:sync_vanilla_settings(0)
	local forced = store.video_aa == "off" and store.video_ao == "off"
	local put_back = PSS:hand_back_vanilla("test")
	local back = store.video_aa == "fxaa" and store.video_ao == "ssao" and store.dof_setting == "standard"
	local kept = PSS.settings.vanilla_off == true and #put_back == 3
	local written = (store._disk_writes or 0) >= 3
	managers, PSS._native, PSS._plugin_working = saved, native, working
	return forced and back and kept and written, string.format("forced %s back %s kept %s written %s", tostring(forced), tostring(back), tostring(kept), tostring(written))
end)())
check("with nothing of the player's held, quitting changes nothing", (function()
	local store = { video_aa = "fxaa" }
	local saved = managers
	managers = fake_user(store)
	PSS._vanilla_saved = {}
	local put_back = PSS:hand_back_vanilla("test")
	managers = saved
	return #put_back == 0 and store.video_aa == "fxaa"
end)())
check("the plugin counts as working only once its frame count rises", (function()
	local path = PSS.status_path
	local tmp = SavePath .. "pss_status_test.txt"
	PSS.status_path = tmp
	PSS._plugin_working, PSS._last_processed, PSS._status_checked = false, nil, nil
	local f = io.open(tmp, "w") f:write("Frames 10  processed 4\n") f:close()
	PSS:poll_status(100)
	local after_first = PSS._plugin_working
	f = io.open(tmp, "w") f:write("Frames 20  processed 9\n") f:close()
	PSS:poll_status(200)
	local after_rise = PSS._plugin_working
	os.remove(tmp)
	PSS.status_path = path
	return after_first ~= true and after_rise == true, tostring(after_first) .. " then " .. tostring(after_rise)
end)())
check("every change to the game's settings is written to disk straight away", (function()
	local store = {video_aa = "fxaa", video_ao = "ssao", dof_setting = "standard"}
	local saved, native = managers, PSS._native
	managers = fake_user(store)
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native, PSS._plugin_working = true, true, true, true
	PSS._vanilla_saved, PSS._vanilla_repaired = nil, PSS.REPAIR_GENERATION
	PSS:sync_vanilla_settings(0)
	local forced = store._disk_writes == 3
	PSS.settings.vanilla_off = false
	PSS:sync_vanilla_settings(0)
	local restored = store._disk_writes == 6
	PSS.settings.vanilla_off, managers, PSS._native = true, saved, native
	return forced and restored, string.format("writes %s", tostring(store._disk_writes))
end)())
check("nothing in the mod writes a game setting without saving it", not read(ROOT .. "lua/core.lua"):find("managers.user:set_setting(entry", 1, true)
	and select(2, read(ROOT .. "lua/core.lua"):gsub("managers%.user:set_setting", "")) == 1)
check("a first install has nothing to repair", (function()
	local was_file = SAVED_FILE
	SAVED_FILE = nil
	PSS:load()
	local skipped = PSS._vanilla_repaired == PSS.REPAIR_GENERATION
	SAVED_FILE = was_file
	PSS:load()
	return skipped
end)())
-- Eye adaptation is the game's. It adapts in HDR, before tone mapping; this mod's own works on the
-- finished 8-bit frame and can only trim. Taking the game's away left everything darker, and
-- reaching into the viewport to do it crashed the game on the way out of a heist.
check("the game's eye adaptation is left running: the viewport is never touched", (function()
	local calls, saved, native = 0, managers, PSS._native
	local vp = {set_post_processor_effect = function() calls = calls + 1 end}
	local store = {light_adaption = true, video_aa = "fxaa", video_ao = "ssao", dof_setting = "standard"}
	managers = fake_user(store)
	managers.environment_controller = {_vp = {vp = function() return vp end}}
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native, PSS._plugin_working = true, true, true, true
	PSS._vanilla_saved, PSS._vanilla_repaired = nil, PSS.REPAIR_GENERATION
	for i = 1, 120 do PSS:sync_vanilla_settings(i / 60) end
	local untouched = calls == 0 and store.light_adaption == true
	PSS.settings.vanilla_off = false
	PSS:sync_vanilla_settings(3)
	managers, PSS._native = saved, native
	PSS.settings.vanilla_off = true
	return untouched and calls == 0, string.format("calls %d", calls)
end)())
check("nothing in the mod names the HDR post processor any more", not read(ROOT .. "lua/core.lua"):find("no_light_adaption", 1, true)
	and not read(ROOT .. "lua/core.lua"):find("set_post_processor_effect", 1, true))
check("the mod's own adaptation measures the frame before its own exposure, so the two cannot fight",
	effects_cpp:match("Pass%(ctx, m_psLuma, m_lum%.rtv, m_lum%.w, m_lum%.h, w, h, &current, 1%)") ~= nil)
check("one button hands back everything this mod ever changed", (function()
	local store = {video_aa = "off", video_ao = "off", dof_setting = "none", light_adaption = false}
	local saved, native = managers, PSS._native
	managers = fake_user(store)
	PSS.settings.vanilla_off, PSS.settings.enabled, PSS._native, PSS._plugin_working = true, true, true, true
	PSS._vanilla_saved = {video_aa = "fxaa", video_ao = "ssao", dof_setting = "standard"}
	PSS._vanilla_repaired = PSS.REPAIR_GENERATION
	local put_back = PSS:restore_vanilla_settings()
	-- Light adaptation too: it lives on the Advanced Video page and some builds do not show it,
	-- so a player who met the 0.5.0 bug may have nothing else to click.
	local all = store.video_aa == "fxaa" and store.video_ao == "ssao" and store.dof_setting == "standard"
		and store.light_adaption == true and #put_back == 4
	-- And it turns the switch off, so nothing forces them straight back again.
	local stays = PSS.settings.vanilla_off == false
	PSS.settings.vanilla_off, managers, PSS._native = true, saved, native
	return all and stays, string.format("all %s stays %s put back %d", tostring(all), tostring(stays), #put_back)
end)())
check("the button is on the tools page and has its text", read(ROOT .. "lua/menu.lua"):match('"pss_vanilla_restore"') ~= nil
	and LOC.pss_vanilla_restore_title ~= nil and LOC.pss_vanilla_restore_desc ~= nil and LOC.pss_vanilla_restore_none ~= nil)
check("the button says so when there was nothing to put back", (function()
	local store = {video_aa = "fxaa", light_adaption = true}
	local saved = managers
	managers = fake_user(store)
	PSS._vanilla_saved = {}
	local put_back = PSS:restore_vanilla_settings()
	PSS.settings.vanilla_off, managers = true, saved
	return #put_back == 0
end)())
check("nothing is touched while the mod is switched off", (function()
	local store = {video_aa = "fxaa", video_ao = "ssao", light_adaption = true, dof_setting = "standard"}
	local saved, native = managers, PSS._native
	managers = fake_user(store)
	PSS.settings.enabled, PSS._native = false, true
	PSS._vanilla_saved, PSS._vanilla_repaired = nil, PSS.REPAIR_GENERATION
	PSS:sync_vanilla_settings()
	PSS.settings.enabled = true
	managers, PSS._native = saved, native
	return store.video_aa == "fxaa" and store.video_ao == "ssao"
end)())
check("the update hook keeps them off if they are turned back on in the video options", read(ROOT .. "lua/update.lua"):match("sync_vanilla_settings%(t%)") ~= nil)
-- Security camera feeds: an in-world monitor the game grades grey on purpose. The chain steps aside
-- while one is up, straight away, and without touching the player's own setting.
-- The weapon workshop: the game draws the weapon in a room of its own, the chain has nothing to add
-- to a menu, and the renderer crashed on that screen after a game update. So it steps aside there.
do
	_G.managers = rawget(_G, "managers") or {}

	local saved_scene = managers.menu_scene and managers.menu_scene._current_scene_template

	managers.menu_scene = managers.menu_scene or {}
	managers.menu_scene._current_scene_template = "blackmarket_crafting"
	check("the chain steps aside in the weapon workshop", PSS:stepped_aside() == true)
	PSS.settings.aside_workshop = false
	check("unless you would rather have them there", PSS:stepped_aside() == false)
	PSS.settings.aside_workshop = true
	managers.menu_scene._current_scene_template = "blackmarket"
	check("anywhere else in the menus it runs as usual", PSS:stepped_aside() == false)
	managers.menu_scene._current_scene_template = saved_scene
	PSS._aside = nil
end

check("the chain steps aside on a security camera feed, at once", (function()
	local saved_gsm = rawget(_G, "game_state_machine")
	-- Leave the heartbeat exactly as found: later checks count its writes from the start.
	local tick0, next0, file0 = PSS._tick, PSS._next, read(SavePath .. "pss_live.txt")
	local state = "ingame_standard"
	game_state_machine = {current_state_name = function() return state end}
	PSS.settings.enabled = true
	PSS._aside, PSS._next = nil, nil
	PSS:tick(100)
	local before = live()
	-- Well inside the heartbeat's half-second throttle: the switch still goes out now.
	state = "ingame_access_camera"
	PSS:tick(100.05)
	local on_feed = live()
	state = "ingame_standard"
	PSS:tick(100.1)
	local after = live()
	game_state_machine = saved_gsm
	PSS._aside, PSS._next, PSS._tick = nil, next0, tick0
	if file0 then local f = io.open(SavePath .. "pss_live.txt", "w") f:write(file0) f:close() else os.remove(SavePath .. "pss_live.txt") end
	local ok = before.enabled == 1 and on_feed.enabled == 0 and after.enabled == 1 and PSS.settings.enabled == true
	return ok, string.format("before %s feed %s after %s", tostring(before.enabled), tostring(on_feed.enabled), tostring(after.enabled))
end)())
check("no game state machine (the menus) is not a camera feed", (function()
	local saved_gsm = rawget(_G, "game_state_machine")
	game_state_machine = nil
	local aside = PSS:stepped_aside()
	game_state_machine = saved_gsm
	return aside == false
end)())

-- Presets: one click back down for a weaker machine, or up to the full look.
check("there are four presets, and Enhanced is the shipped defaults", #PSS.PRESETS == 4 and PSS.PRESETS[1].id == "enhanced" and PSS.PRESETS[4].id == "andreika" and next(PSS.PRESETS[1].values) == nil)
check("each preset has a menu button and its own text", (function()
	local menu = read(ROOT .. "lua/menu.lua")
	for _, preset in ipairs(PSS.PRESETS) do
		if not menu:match('pss_preset_" %.%. preset%.id') then return false, "no buttons" end
		if not LOC["pss_preset_" .. preset.id .. "_title"] then return false, preset.id end
	end
	return true
end)())
check("applying a preset starts from the defaults, so it is the same look however you got there", (function()
	PSS.settings.ao_on = false
	PSS.settings.vignette = 0.9
	PSS:apply_preset("performance")
	local performance = PSS.settings.ao_on == true and PSS.settings.il_on == false and PSS.settings.vignette == PSS.BY_KEY.vignette.default
	PSS:apply_preset("clean")
	local clean = PSS.settings.ao_on == false and PSS.settings.bloom == 0 and PSS.settings.aa == true
	PSS:apply_preset("enhanced")
	local enhanced = PSS.settings.rtgi_on == true and PSS.settings.ao_on == true and PSS.settings.vignette == PSS.BY_KEY.vignette.default
	return performance and clean and enhanced
end)())
check("the diagnostics and the effects the author left off stay off", (function()
	for _, key in ipairs({"hfog_on", "halation_on", "motion_view", "depth_view", "split"}) do
		if PSS.BY_KEY[key].default ~= false then return false, key end
	end
	-- Film grain stays off: the surface grain does that job without crawling over the screen.
	return PSS.BY_KEY.grain.default == 0
end)())
check("obscurance steps aside for real ambient occlusion, in the shader and the plugin", hlsl:match("OBSCURANCE > 0%.0 && !%(HaveDepth%(%) && AO > 0%.0%)") ~= nil and effects_cpp:match("P_OBSCURANCE%] > 0%.001f && !%(haveDepth && p%[P_AO%] > 0%.001f%)") ~= nil)
check("temporal effects use interleaved gradient noise", hlsl:match("float InterleavedGradient%(") ~= nil and hlsl:match("5%.588238 %* frame") ~= nil)
check("occlusion keeps light in bright creases (multi-bounce)", hlsl:match("2%.0404 %* albedo") ~= nil)
check("bloom's prefilter is a Karis average", hlsl:match("1%.0 / %(1%.0 %+ LumaLinear%(a%)%)") ~= nil)
check("reflections guess smoothness from texture detail", hlsl:match("float rough = saturate%(spread") ~= nil)
check("a closed lens skips the depth of field passes", effects_cpp:match("lensShut") ~= nil)
check("each depth effect is switched by its own Lua-only toggle", PSS.BY_KEY.ao.gate == "ao_on" and PSS.BY_KEY.fog.gate == "fog_on" and PSS.BY_KEY.dof.gate == "dof_on")
check("the depth buffer is copied, not viewed in place", effects_cpp:match("CopyResource%(m_depthCopy%.tex, depth%)") ~= nil)
check("a missing depth target is reported to the shader as -1", effects_cpp:match("frame%[8 %+ P_DEPTH_MODE%] = haveDepth %? p%[P_DEPTH_MODE%] : %-1%.0f") ~= nil)
check("every depth effect tests HaveDepth() first", (function()
	for _, block in ipairs({"AO > 0.0", "FOG > 0.0"}) do
		if not hlsl:match("HaveDepth%(%) && " .. block:gsub("([%.%(%)])", "%%%1")) then return false, block end
	end
	-- Depth of field is its own pass chain, and every pass of it bails out without depth.
	local _, bails = hlsl:gsub("!HaveDepth%(%) || !DofOn%(%)", "")
	return bails >= 3, bails .. " depth of field passes check for depth"
end)())
check("ambient occlusion is GTAO: horizon angles integrated over slices", hlsl:match("float4 PS_Ao%(") ~= nil and hlsl:match("acos%(horizons%[0%]%)") ~= nil and hlsl:match("kAoTaps") == nil)
check("the hook only accepts a full screen single channel float target as depth", (function()
	local hooks = read(ROOT .. "native/src/hooks.cpp")
	return hooks:match("DXGI_FORMAT_R32_FLOAT") ~= nil and hooks:match("td%.Width == g_backbufferW") ~= nil
end)())
check("the depth copy is handed to the effects", read(ROOT .. "native/src/hooks.cpp"):match("fresh, g_frame%.depthTex%)") ~= nil)
check("the plugin says why depth is unavailable", read(ROOT .. "native/src/bridge.cpp"):match("Depth target: ") ~= nil)
check("a depth page exists with its own reset", (function()
	local menu = read(ROOT .. "lua/menu.lua")
	return menu:match('id = "pss_menu_geometry"') ~= nil and menu:match('id = "pss_menu_geometry",%s*reset = true') ~= nil
end)())
check("the depth view is its own toggle, not a mode in the debug list", (function()
	local menu = read(ROOT .. "lua/menu.lua")
	-- It shares a list with the exposure view, which tints the whole screen red; telling those
	-- two apart by eye is impossible, so they must not live in the same control.
	return menu:match('{"toggle", "depth_view"}') ~= nil and menu:match("pss_debug_depth") == nil
end)())
check("the no-depth warning is a pattern no effect can produce, not a colour tint", hlsl:match("DEPTH_VIEW > 0%.5") ~= nil and hlsl:match("float3%(check, 0%.0, check%)") ~= nil)

-- ---- settings ----
check("defaults load", PSS.settings.enabled == true and PSS.settings.sharpen == 0.45)

PSS.settings.sharpen = 99
PSS.settings.gamma = "nonsense"
PSS.settings.debug_view = 7.4
PSS:sanitize()
check("sliders clamp to their range", PSS.settings.sharpen == 1)
check("wrong types fall back to the default", PSS.settings.gamma == 1)
check("choices round and clamp", PSS.settings.debug_view == 7)
PSS:reset_defaults()

SAVED_FILE = { sharpen = 0.8, enabled = "yes", unknown_key = 3, lut = 3 }
PSS:load()
check("saved values load", PSS.settings.sharpen == 0.8)
check("saved values of the wrong type are ignored", PSS.settings.enabled == true)
check("unknown saved keys are dropped", PSS.settings.unknown_key == nil)

-- Version 1 files carry inject_after = 1 from the rule the real game disproved.
SAVED_FILE = { inject_after = 1, sharpen = 0.6 }
PSS:load()
check("an old settings file loses its stale shader position", PSS.settings.inject_after == 0)
check("but keeps everything else", PSS.settings.sharpen == 0.6)

SAVED_FILE = { inject_after = 3, settings_version = 2 }
PSS:load()
check("a current settings file keeps a chosen shader position", PSS.settings.inject_after == 3)

PSS:save()
check("saving records the settings version", SAVED and SAVED.settings_version == PSS.SETTINGS_VERSION)
check("and every setting", SAVED and SAVED.inject_after == 3)

SAVED_FILE = { sharpen = 0.8, enabled = "yes", lut = 3 }
PSS:load()
SAVED_FILE = nil

-- ---- LUTs ----
local luts = PSS:luts()
check("LUT list starts with none", luts[1].name == "None" and luts[1].path == "")
check("LUTs are found in luts/", #luts == 6, #luts)
check("LUT names drop the ordering number", luts[2].name == "Neutral" and luts[3].name == "Ops Room", luts[2] and luts[2].name)

-- ---- the file bridge to the plugin ----
check("nothing is written before the first update", live() == nil)

PSS:tick(0)
local L = live()
check("the plugin loads on the first update", LOADS == 1)
check("the first update writes the bridge file", L ~= nil)
check("the file ends with the end marker the plugin waits for", L and L._complete)
check("the shader path is sent", L and L.shader_path == ROOT .. "shaders/pss.hlsl")
check("toggles are sent as 1/0", L and L.enabled == 1 and L.split == 0)
check("sliders are sent as numbers", L and L.sharpen == 0.8)
check("choices are sent zero-based", L and L.debug_view == 0)
check("the LUT is sent as a path", L and L.lut_path == luts[3].path, L and L.lut_path)
check("the lens dirt texture is sent as a path", L and L.dirt_path == ROOT .. "shaders/lens_dirt.bin", L and L.dirt_path)
check("the heartbeat is sent", L and L.tick == 1)

local every = true
for _, param in ipairs(PSS.PARAMS) do
	if param.kind ~= "lut" and not param.lua_only and L[param.key] == nil then every = false end
	if param.lua_only and L[param.key] ~= nil then every = false end
end
check("every parameter is in the file, and the Lua-only switches are not", every)
check("a switched-on effect sends its strength", L.obscurance == PSS.settings.obscurance and L.cues == PSS.settings.cues)
PSS:set("obscurance_on", false)
check("switching an effect off sends strength 0", live().obscurance == 0 and PSS.settings.obscurance > 0)
PSS:set("obscurance_on", true)
check("and switching it back on sends the strength again", live().obscurance == PSS.settings.obscurance)

PSS:tick(0.2)
check("updates are throttled", live().tick == 1)
PSS:tick(0.6)
check("the heartbeat repeats", live().tick == 2)
check("the plugin is loaded only once", LOADS == 1)

PSS:set("split", true)
check("changing a setting writes it at once", live().split == 1)
PSS:set("lut", 1)
check("choosing no LUT sends an empty path", live().lut_path == "")

PSS:capture()
check("capture bumps its counter", live().capture == 1)
PSS:reload()
check("reload bumps its counter", live().reload == 1)
check("and requests do not fake a heartbeat", live().tick == 2)

do
	local f = io.open(PSS.status_path, "w")
	f:write("Plugin 0.1.0  hooks yes  present hook yes\nFrames 10  processed 9\n")
	f:close()
end
check("status text is the plugin's status file", PSS:status_text():match("processed 9") ~= nil)
os.remove(PSS.status_path)
check("a missing status file is explained", PSS:status_text():match("not written") ~= nil)

-- ---- a missing or broken plugin must not break the game ----
PSS._native = nil
local before = live().tick
blt.load_native = function() error("Failed to load module: ERR126") end
check("a plugin that fails to load gives false, not an error", PSS:native() == false)
check("and says why in the log", LOG[#LOG]:match("ERR126") ~= nil, LOG[#LOG])
PSS:tick(10)
check("updates without a plugin write nothing", live().tick == before)
check("status explains a missing plugin", PSS:status_text():match("not loaded") ~= nil)

-- ---- menu: pages, strings, order and resets (dev/ideas/SERIES_UI.md) ----
local strings = read(ROOT .. "loc/english.json")
local ITEMS, BY_MENU, MENU_ITEMS = {}, {}, {}
local function record(kind)
	return function(_, p)
		local entry = { kind = kind, p = p }
		ITEMS[#ITEMS + 1] = entry
		BY_MENU[p.menu_id] = BY_MENU[p.menu_id] or {}
		table.insert(BY_MENU[p.menu_id], entry)
	end
end
local function node_for(menu_id)
	local node = { _parameters = { name = "video" }, _items = {}, _set = {} }
	for _, entry in ipairs(BY_MENU[menu_id] or {}) do
		node._items[#node._items + 1] = { _parameters = { name = entry.p.id } }
	end
	function node:item(name)
		for _, entry in ipairs(BY_MENU[menu_id] or {}) do
			if entry.p.id == name then
				return { set_value = function(_, v) node._set[name] = v end }
			end
		end
	end
	return node
end
MenuHelper = { NewMenu = function() end, AddToggle = record("toggle"), AddSlider = record("slider"),
	AddMultipleChoice = record("choice"), AddDivider = record("divider"), AddButton = record("button"),
	AddKeybinding = record("keybind"), BuildMenu = function(_, id) return node_for(id) end,
	AddMenuItem = function(_, parent, child, title, desc, position, sub) MENU_ITEMS[#MENU_ITEMS + 1] = { parent = parent, child = child, title = title, desc = desc, position = position, sub = sub } end }
MenuCallbackHandler = {}
local DIALOGS = {}
QuickMenu = { new = function(_, title, text, options) DIALOGS[#DIALOGS + 1] = { title = title, text = text, options = options } return {} end }
BLT = nil
dofile(ROOT .. "lua/menu.lua")
HOOKS.MenuManagerInitialize()
HOOKS.MenuManagerSetupCustomMenus()
HOOKS.MenuManagerPopulateCustomMenus()
local NODES = { blt_options = { _items = {} } }
HOOKS.MenuManagerBuildCustomMenus(nil, NODES)

local function has_string(key) return strings:match('"' .. key:gsub("%p", "%%%0") .. '"') ~= nil end
local shown, missing = {}, {}
for _, item in ipairs(ITEMS) do
	local p = item.p
	if item.kind ~= "divider" then
		shown[p.id] = true
		for _, key in ipairs({ p.title, p.desc }) do
			if not has_string(key) then missing[#missing + 1] = key end
		end
		if item.kind == "button" or item.kind == "toggle" or item.kind == "slider" or item.kind == "choice" then
			check("menu callback " .. tostring(p.callback) .. " exists", MenuCallbackHandler[p.callback] ~= nil)
		end
		if item.kind == "choice" and p.id == "debug_view" then
			for _, str in ipairs(p.items) do
				if not has_string(str) then missing[#missing + 1] = str end
			end
		end
	end
end
for _, entry in ipairs(MENU_ITEMS) do
	for _, key in ipairs({ entry.title, entry.desc }) do
		if not has_string(key) then missing[#missing + 1] = key end
	end
end
check("every menu string exists in english.json", #missing == 0, table.concat(missing, ", "))

-- Antialiasing is one choice on the page; aa, aa_mode and taa_on are what it sets. The FSR factor
-- below 1x is the render scale.
local represented = {aa = "antialiasing", aa_mode = "antialiasing", taa_on = "antialiasing", render_scale = "upscale_scale"}
for _, param in ipairs(PSS.PARAMS) do
	check("setting '" .. param.key .. "' is on an options page", shown[param.key] == true or shown[represented[param.key] or ""] == true)
end

-- Top page order: Enabled first, feature pages after the top divider in order, Info after the bottom divider, Reset all last.
local main = BY_MENU.pss_menu_main
check("top page starts with Enabled", main[1].p.id == "enabled" and main[1].p.title == "pss_enabled_title")
check("top page ends with Reset all to defaults", main[#main].p.id == "pss_reset_all")
check("the entry in Mod Options is Screen Shaders", MENU_ITEMS[#MENU_ITEMS].parent == NODES.blt_options and MENU_ITEMS[#MENU_ITEMS].title == "pss_menu_title")
check("Controls is the first page, straight after the top divider", MENU_ITEMS[1].child == "pss_menu_controls" and MENU_ITEMS[1].position == "pss_divider_top" and MENU_ITEMS[1].sub == "after")
local chain_ok = true
for i = 2, #PSS.PAGES - 1 do
	local e = MENU_ITEMS[i]
	if e.child ~= PSS.PAGES[i].id or e.position ~= PSS.PAGES[i - 1].id then chain_ok = false end
end
check("feature pages follow one another in order", chain_ok)
local info = MENU_ITEMS[#PSS.PAGES]
check("Info sits after the bottom divider", info.child == "pss_menu_info" and info.position == "pss_divider_bottom")

local keys_ok = true
for menu_id, entries in pairs(BY_MENU) do
	for _, entry in ipairs(entries) do
		if entry.kind == "keybind" and menu_id ~= "pss_menu_controls" then keys_ok = false end
	end
end
check("every key lives on the Controls page and nowhere else", keys_ok and #BY_MENU.pss_menu_controls == 3)

local resets_ok = true
for _, page in ipairs(PSS.PAGES) do
	local entries = BY_MENU[page.id]
	local last = entries[#entries].p.id
	if page.reset and last ~= "pss_reset_page_" .. page.id then resets_ok = false end
	if not page.reset and last:match("^pss_reset_page_") then resets_ok = false end
end
check("every feature page ends with Reset this page", resets_ok)

local renamed = true
for _, page in ipairs(PSS.PAGES) do
	if NODES[page.id]._parameters.name ~= page.id then renamed = false end
end
check("every page gets its own node name (the nested keybind crash)", renamed and NODES.pss_menu_main._parameters.name == "pss_menu_main")

-- wording rules
local long_titles, long_descs = {}, {}
for key, value in strings:gmatch('"([%w_]+)":%s*"(.-)",?\n') do
	if key:match("_title$") then
		local words = 0
		for _ in value:gmatch("%S+") do words = words + 1 end
		if words > 4 then long_titles[#long_titles + 1] = value end
	elseif key:match("_desc$") and #value > 200 then
		long_descs[#long_descs + 1] = key
	end
end
check("titles are four words at most", #long_titles == 0, table.concat(long_titles, " | "))
check("descriptions are 200 characters at most", #long_descs == 0, table.concat(long_descs, ", "))

-- Reset this page: asks first, resets only its page, shows the new values.
PSS:set("obscurance", 0.9)
PSS:set("exposure", 1.5)
DIALOGS = {}
MenuCallbackHandler.pss_reset_page(nil, { name = function() return "pss_reset_page_pss_menu_depth" end })
check("Reset this page asks first", #DIALOGS == 1 and PSS.settings.obscurance == 0.9 and DIALOGS[1].options[2].is_cancel_button)
DIALOGS[1].options[1].callback()
check("Reset this page resets that page", PSS.settings.obscurance == PSS.BY_KEY.obscurance.default)
check("and leaves other pages alone", PSS.settings.exposure == 1.5)
check("and shows the new value on the page", NODES.pss_menu_depth._set.obscurance == PSS.BY_KEY.obscurance.default)
check("toggles are shown as on/off", NODES.pss_menu_depth._set.cues_on == "on")
DIALOGS = {}
MenuCallbackHandler.pss_reset_all()
check("Reset all asks first", #DIALOGS == 1 and PSS.settings.exposure == 1.5)
DIALOGS[1].options[1].callback()
check("Reset all resets everything", PSS.settings.exposure == PSS.BY_KEY.exposure.default)
DIALOGS = {}
for _, id in ipairs({ "pss_info_what", "pss_info_others", "pss_info_new", "pss_info_credits" }) do
	MenuCallbackHandler[id]()
end
check("every Info button opens a dialog", #DIALOGS == 4)

local LOC = {}
HOOKS.LocalizationManagerPostInit({ load_localization_file = function() end, add_localized_strings = function(_, t) for k, v in pairs(t) do LOC[k] = v end end })
check("LUT names are registered as strings", LOC.pss_lut_1 == "None" and LOC.pss_lut_3 == "Ops Room")

-- slider callbacks write live
PSS._native = true
MenuCallbackHandler.pss_slider(nil, { name = function() return "bloom" end, value = function() return "0.9" end })
check("moving a slider writes the value live", live().bloom == 0.9)

-- an error in a guarded feature says so on screen, once
local HINTS = {}
managers = { hud = { show_hint = function(_, p) HINTS[#HINTS + 1] = p.text end } }
PSS:guard("overlay_test", function() error("boom") end)
PSS:guard("overlay_test", function() error("boom") end)
check("a failing feature shows one notice in the series wording", #HINTS == 1 and HINTS[1]:match("^Screen Shaders: .* switched off for this session after an error%. Details are in the BLT log%.$") ~= nil, HINTS[1])
managers = nil

-- ---- corner status text ----
Color = function(r, g, b) return { r = r, g = g, b = b } end
local TEXTS = {}
local function gui_object()
	local o = { _visible = true, _text = "" }
	function o:panel() return gui_object() end
	function o:text(p)
		local t = gui_object()
		t._visible = p.visible ~= false
		t._text = p.text or ""
		t._params = { font_size = p.font_size, color = p.color, x = p.x, y = p.y }
		TEXTS[p.name] = t
		return t
	end
	function o:set_visible(v) self._visible = v end
	function o:visible() return self._visible end
	function o:set_text(s) self._text = s end
	function o:set_color(c) self._color = c end
	function o:set_alpha(a) self._alpha = a end
	function o:set_y(y) self._y = y end
	function o:w() return 1920 end
	function o:h() return 1080 end
	return o
end
local FONT_LOADED = false
PackageManager = { has = function() return FONT_LOADED end }
tweak_data = { menu = { pd2_small_font = "fonts/font_small_mf", pd2_small_font_size = 20 } }
managers = { gui_data = { create_fullscreen_workspace = function() return { panel = function() return gui_object() end } end, safe_scaled_size = function() return { x = 96, y = 54, w = 1728, h = 972 } end } }
dofile(ROOT .. "lua/overlay.lua")
PSS._native = true
PSS.settings.enabled = true
PSS.settings.split = false

PSS.overlay:update(1)
check("no text is made while the font is not loaded (that crash is uncatchable)", next(TEXTS) == nil)
FONT_LOADED = true
PSS.overlay:update(2)
check("the corner text is created once the font is loaded", TEXTS.pss_state ~= nil)
check("text uses the small font at its own size", TEXTS.pss_state._params.font_size == 20)
check("text sits 16 px inside the safe area", TEXTS.pss_state._params.x == 96 + 16 and TEXTS.pss_state._params.y == 54 + 16)
check("every label has a 1 px shadow", TEXTS.pss_state_shadow ~= nil and TEXTS.pss_state_shadow._params.x == TEXTS.pss_state._params.x + 1)
check("labels use the series ivory", TEXTS.pss_state._params.color.r == 0.93 and TEXTS.pss_state._params.color.g == 0.90)
check("nothing shows with shaders on and split off", TEXTS.pss_state and not TEXTS.pss_state:visible() and not TEXTS.pss_split:visible())

PSS:set("split", true)
PSS.overlay:update(2.1)
check("split view shows its label", TEXTS.pss_split:visible() and TEXTS.pss_split._text:match("SPLIT") ~= nil)
check("labels fade in rather than pop", TEXTS.pss_split._alpha > 0 and TEXTS.pss_split._alpha < 1, TEXTS.pss_split._alpha)
PSS.overlay:update(2.3)
check("and settle fully shown", TEXTS.pss_split._alpha == 1 and TEXTS.pss_split._y == TEXTS.pss_split._params.y)
check("and names both halves", TEXTS.pss_original:visible() and TEXTS.pss_shaders:visible())

PSS:set("enabled", false)
PSS.overlay:update(2.4)
check("disabled shaders say so", TEXTS.pss_state:visible() and TEXTS.pss_state._text:match("OFF") ~= nil)
check("off is the one amber label", TEXTS.pss_state._color and TEXTS.pss_state._color.r == 0.95)
PSS.overlay:update(2.8)
check("split labels fade out while shaders are off", not TEXTS.pss_split:visible())
PSS.overlay:update(20)
check("the disabled notice stays up", TEXTS.pss_state:visible())

PSS:set("enabled", true)
PSS.overlay:update(20.1)
check("enabling shows a notice", TEXTS.pss_state:visible() and TEXTS.pss_state._text:match("ON$") ~= nil)
PSS.overlay:update(24)
check("which goes away after a few seconds", not TEXTS.pss_state:visible())
check("while split view keeps its label", TEXTS.pss_split:visible())

-- update hooks, including the paused game
HOOKS = {}
dofile(ROOT .. "lua/update.lua")
check("menu, game and paused updates all keep the heartbeat", HOOKS.MenuUpdate and HOOKS.GameSetupUpdate and HOOKS.GameSetupPausedUpdate)

-- The game update passes game time, which stops during a pause; the paused update does not.
-- Mixing them once stopped the heartbeat for good after the first escape menu.
local WALL = 1000
TimerManager = { wall = function() return { time = function() return WALL end } end }
PSS._native = true
PSS._next = nil
HOOKS.GameSetupUpdate(50, 0.016)
local t0 = live().tick
WALL = WALL + 0.6
HOOKS.GameSetupPausedUpdate(5000, 0.016)
check("the paused update keeps the heartbeat", live().tick == t0 + 1)
WALL = WALL + 0.6
HOOKS.GameSetupUpdate(51, 0.016)
check("and it keeps going after unpausing", live().tick == t0 + 2)
WALL = WALL + 0.2
HOOKS.GameSetupUpdate(52, 0.016)
check("while still throttled to twice a second", live().tick == t0 + 2)

PSS._next = 5000
PSS:tick(60)
check("a clock that jumps backwards does not stall the heartbeat", live().tick == t0 + 3)

-- ---- moods: the grade lent an offset while you are hurt, or while it is loud ----
local saved_managers = managers
local HURT = {health = 20, max = 20}
local WHISPER = true
local hurt_ext = {
	get_real_health = function() return HURT.health end,
	_max_health = function() return HURT.max end
}
local hurt_unit = {character_damage = function() return hurt_ext end}
managers = {
	player = {player_unit = function() return hurt_unit end},
	groupai = {state = function() return {whisper_mode = function() return WHISPER end} end}
}
game_state_machine = {current_state_name = function() return "ingame_standard" end}
Application = Application or {digest_value = function(_, v) return v end}

local base_vignette = PSS.settings.vignette
local function mood_frames(n)
	for _ = 1, (n or 1) do
		PSS.mood:update((PSS.mood._t or 0) + 0.25)
	end
end
PSS.settings.mood_hurt = true
PSS.settings.mood_assault = false
PSS.mood.value = {}
PSS.mood._t = nil
mood_frames(2)
check("mood: at full health the grade is the one you set", (PSS._offsets.vignette or 0) == 0 and PSS:live_value({key = "vignette", kind = "slider", min = 0, max = 1}) == base_vignette)
HURT.health = 3
mood_frames(12)
check("mood: badly hurt, the edges close in and the colour drains", PSS._offsets.vignette > 0.1 and PSS._offsets.saturation < -0.1, tostring(PSS._offsets.vignette))
check("mood: your saved settings are not touched", PSS.settings.vignette == base_vignette)
PSS:write_bridge()
check("mood: and it is the offset picture the plugin is sent", live().vignette > base_vignette)
hurt_ext._bleed_out = true
mood_frames(20)
check("mood: on the floor, further still", PSS._offsets.vignette >= 0.4 - 0.02)
check("mood: and never past the knob's own range", PSS:live_value({key = "vignette", kind = "slider", min = 0, max = 1}) <= 1)
hurt_ext._bleed_out = nil
HURT.health = 20
mood_frames(20)
check("mood: healing puts the picture back", (PSS._offsets.vignette or 0) == 0 and (PSS._offsets.saturation or 0) == 0)
PSS.settings.mood_assault = true
WHISPER = true
mood_frames(20)
local cool = PSS._offsets.temperature
WHISPER = false
mood_frames(20)
check("mood: stealth reads cooler than loud", cool < 0 and PSS._offsets.temperature > cool)
PSS.settings.mood_assault = false
PSS.settings.mood_hurt = false
mood_frames(20)
check("mood: both switched off, nothing is lent", next(PSS._offsets) == nil)
managers = saved_managers
game_state_machine = nil

os.remove(SavePath .. "pss_live.txt")
do
	-- The version in the code and the one BLT reads must not drift apart.
	local vf = io.open(ModPath .. "mod.txt", "r")
	local vt = vf:read("*a")
	vf:close()
	check("the version is the same in the code and in mod.txt", vt:find('"version": "' .. PSS.VERSION .. '"', 1, true) ~= nil, PSS.VERSION)
end

print(string.format("\n%d passed, %d failed", passed, failed))
os.exit(failed == 0 and 0 or 1)
