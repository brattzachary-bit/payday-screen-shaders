--[[
	Payday Screen Shaders
	lua/core.lua - settings, and the bridge to the native plugin.

	The shaders themselves do not run in Lua or in Diesel's post processor chain. A SuperBLT
	native plugin (native/pss.dll) hooks Direct3D 11, waits for the moment the World image is
	finished and the HUD is about to be drawn, and runs shaders/pss.hlsl there. So every effect
	lands on the game world and never on the HUD or menus.

	Live settings, camera and status use a negotiated native memory bridge on supported
	SuperBLT builds. Older native/loader combinations retain the original file bridge.
	Persistent settings and diagnostic logs still use their normal files. The native
	consumer retains the original parsers, clamping and render-time camera history.
]]

if _G.PSS then
	return
end

local PSS = {}
_G.PSS = PSS

PSS.VERSION = "0.11.0"
PSS.mod_path = ModPath
PSS.save_path = SavePath .. "pss_settings.json"
PSS.live_path = SavePath .. "pss_live.txt"      -- the plugin reads mods/saves/pss_live.txt
PSS.camera_path = SavePath .. "pss_camera.txt"  -- and this one every frame, for the camera
PSS.status_path = "mods/logs/pss_status.txt"    -- the plugin writes this once a second
PSS.loc_path = ModPath .. "loc/english.json"
PSS.dll_path = ModPath .. "native/pss.dll"
PSS.shader_path = ModPath .. "shaders/pss.hlsl"
PSS.dirt_path = ModPath .. "shaders/lens_dirt.bin" -- made by dev/nativebuild/make_dirt.py
PSS.lut_dir = ModPath .. "luts/"

-- Names match native/src/params.h. kind: toggle (sent as 1/0), slider, choice (index,
-- sent as index - 1). These defaults are the look the mod ships with: an enhanced PAYDAY 2 aimed
-- at PAYDAY 3's - real contact shadows and bounce light, reflective floors, a little distance haze,
-- light shafts, gentle bloom and the grade without a LUT, with temporal antialiasing. Since 0.5.28 they
-- are the author's own saved settings as played. Vignette, depth of field, chromatic aberration,
-- motion blur, height fog and the other lens effects are a matter of taste and stay off.
--   lua_only - a switch the plugin never sees
--   gate     - the switch that turns this strength on; when it is off the plugin is sent 0
PSS.PARAMS = {
	{key = "enabled", kind = "toggle", default = true},
	-- Steps the chain aside in the weapon workshop (see stepped_aside).
	{key = "aside_workshop", kind = "toggle", default = true, lua_only = true},
	{key = "split", kind = "toggle", default = false},

	-- What does the antialiasing, as one choice: 1 off, 2 FXAA, 3 SMAA, 4 SMAA T2x, 5 TAA (SMAA until
	-- TAA can run). The plugin still gets it as aa, aa_mode and TAA on or off, which follow it (sanitize).
	-- SMAA T2x by default since 0.7.0, the author's choice in the game; a saved choice is kept.
	{key = "antialiasing", kind = "choice", default = 4, options = 5, lua_only = true},
	{key = "aa", kind = "toggle", default = true},
	{key = "aa_mode", kind = "choice", default = 2, options = 3}, -- 1 FXAA, 2 SMAA, 3 SMAA T2x
	{key = "aa_subpix", kind = "slider", default = 0.7, min = 0, max = 1, step = 0.05},
	{key = "aa_edge", kind = "slider", default = 0.17, min = 0.063, max = 0.333, step = 0.01},
	-- Temporal antialiasing, on since 0.5.27, once its camera jitter was seen working in a heist (0.5.25:
	-- the plugin saw it in every checked frame). Things that move by themselves can leave faint trails.
	{key = "taa_on", kind = "toggle", default = true, lua_only = true},
	{key = "taa", kind = "slider", default = 1, min = 0, max = 1, step = 0.05, gate = "taa_on"},
	-- The camera jitter that gives it new sample positions. Without it only your own movement does.
	{key = "taa_jitter", kind = "toggle", default = true, lua_only = true},
	-- Not in the menu since 0.5.29: worked out from the FSR factor below 1x (native_value).
	{key = "render_scale", kind = "slider", default = 1, min = 0.5, max = 1, step = 0.05},

	{key = "demoire_on", kind = "toggle", default = true, lua_only = true},
	{key = "demoire", kind = "slider", default = 0.4, min = 0, max = 1, step = 0.05, gate = "demoire_on"},
	{key = "detail_on", kind = "toggle", default = true, lua_only = true},
	{key = "detail", kind = "slider", default = 0.25, min = 0, max = 1, step = 0.05, gate = "detail_on"},
	{key = "surface_grain_on", kind = "toggle", default = false, lua_only = true},
	{key = "surface_grain", kind = "slider", default = 0.2, min = 0, max = 1, step = 0.05, gate = "surface_grain_on"},
	{key = "surface_grain_size", kind = "slider", default = 0.4, min = 0, max = 1, step = 0.05},
	{key = "sharpen", kind = "slider", default = 0.35, min = 0, max = 1, step = 0.05},
	-- FSR 1.0: the finished frame is redrawn larger along its edges (EASU), fitted back to the screen
	-- and sharpened there (RCAS). It cannot recover detail the game never drew; what it gives is
	-- edge-directed resampling and sharpening that judges its own limit. Ships off since 0.5.28 (the
	-- author's settings); 1.5x when switched on.
	{key = "upscale", kind = "toggle", default = false},
	-- One factor for both ways: above 1x the frame is redrawn larger and fitted back; below 1x it is
	-- shrunk and rebuilt at screen size (sent to the plugin as render_scale, see native_value).
	{key = "upscale_scale", kind = "slider", default = 1.5, min = 0.5, max = 2, step = 0.05},
	{key = "upscale_sharpness", kind = "slider", default = 0.45, min = 0, max = 1, step = 0.05},
	{key = "deband", kind = "slider", default = 0.5, min = 0, max = 1, step = 0.05},
	{key = "dither", kind = "slider", default = 1, min = 0, max = 1, step = 0.05},

	{key = "bloom", kind = "slider", default = 0.7, min = 0, max = 2, step = 0.05},
	{key = "bloom_threshold", kind = "slider", default = 0.75, min = 0, max = 2, step = 0.05},
	{key = "bloom_radius", kind = "slider", default = 1.2, min = 0.25, max = 3, step = 0.05},
	{key = "bloom_tint", kind = "slider", default = 0.25, min = 0, max = 1, step = 0.05},

	{key = "adapt", kind = "slider", default = 0.35, min = 0, max = 1, step = 0.05},
	{key = "adapt_speed", kind = "slider", default = 1.5, min = 0.05, max = 10, step = 0.05},
	{key = "adapt_min", kind = "slider", default = -0.75, min = -4, max = 0, step = 0.05},
	{key = "adapt_max", kind = "slider", default = 1.25, min = 0, max = 4, step = 0.05},
	{key = "adapt_key", kind = "slider", default = 0.14, min = 0.02, max = 0.5, step = 0.01},

	{key = "exposure", kind = "slider", default = 0, min = -3, max = 3, step = 0.05},
	{key = "temperature", kind = "slider", default = 0, min = -1, max = 1, step = 0.05},
	{key = "tint", kind = "slider", default = 0, min = -1, max = 1, step = 0.05},
	{key = "clarity", kind = "slider", default = 0.15, min = -0.5, max = 1, step = 0.05},
	{key = "shadows", kind = "slider", default = 0.25, min = -0.5, max = 1, step = 0.05},
	{key = "highlights", kind = "slider", default = 0.05, min = -0.5, max = 1, step = 0.05},
	{key = "lift", kind = "slider", default = 0, min = -0.2, max = 0.3, step = 0.01},
	{key = "gamma", kind = "slider", default = 1, min = 0.5, max = 2, step = 0.05},
	{key = "gain", kind = "slider", default = 1, min = 0.5, max = 1.5, step = 0.05},
	{key = "contrast", kind = "slider", default = 0.5, min = -1, max = 1, step = 0.02},
	{key = "saturation", kind = "slider", default = 1, min = 0, max = 2, step = 0.05},
	{key = "vibrance", kind = "slider", default = 1, min = -1, max = 1, step = 0.05},

	{key = "lut", kind = "lut", default = 5}, -- 1 is no LUT, then luts/ in name order: 6 is Film Stock
	{key = "lut_mix", kind = "slider", default = 0.5, min = 0, max = 1, step = 0.05},

	-- Moods (lua/mood.lua): an offset lent to the grade while you are hurt, or while it is loud.
	{key = "mood_hurt", kind = "toggle", default = true, lua_only = true},
	{key = "mood_assault", kind = "toggle", default = false, lua_only = true},

	{key = "vignette", kind = "slider", default = 0.32, min = 0, max = 1, step = 0.02},
	{key = "chromatic", kind = "slider", default = 0.4, min = 0, max = 1, step = 0.05},
	{key = "grain", kind = "slider", default = 0, min = 0, max = 1, step = 0.05},

	-- Projections of the finished frame (PS_Final), from the real field of view. Off: they move the
	-- picture under the HUD, which is drawn afterwards and does not follow.
	{key = "panini_on", kind = "toggle", default = false, lua_only = true},
	{key = "panini", kind = "slider", default = 0.45, min = 0, max = 1, step = 0.05, gate = "panini_on"},
	{key = "spherical_on", kind = "toggle", default = false, lua_only = true},
	{key = "spherical", kind = "slider", default = 0.3, min = 0, max = 1, step = 0.05, gate = "spherical_on"},

	{key = "color_bloom_on", kind = "toggle", default = false, lua_only = true},
	{key = "color_bloom", kind = "slider", default = 0.35, min = 0, max = 1, step = 0.05, gate = "color_bloom_on"},
	{key = "color_bloom_radius", kind = "slider", default = 1, min = 0.5, max = 2, step = 0.05},
	{key = "cues_on", kind = "toggle", default = true, lua_only = true},
	{key = "cues", kind = "slider", default = 0.3, min = 0, max = 1, step = 0.05, gate = "cues_on"},

	{key = "dirt_on", kind = "toggle", default = false, lua_only = true},
	{key = "dirt", kind = "slider", default = 1, min = 0, max = 1, step = 0.05, gate = "dirt_on"},
	{key = "dirt_offset", kind = "slider", default = 0.5, min = 0, max = 1, step = 0.05},
	{key = "flare_on", kind = "toggle", default = true, lua_only = true},
	{key = "flare", kind = "slider", default = 0.35, min = 0, max = 1, step = 0.05, gate = "flare_on"},
	{key = "flare_length", kind = "slider", default = 1, min = 0.25, max = 2, step = 0.05},

	-- Depth. The plugin finds the game's own depth target among Diesel's render targets and copies
	-- it; when there is nothing to copy it tells the shader so, and every one of these does
	-- nothing at all. Show depth buffer and the depth effects' debug views show whether that buffer
	-- reads right on a given machine.
	{key = "depth_mode", kind = "choice", default = 1, options = 4},
	{key = "depth_near", kind = "slider", default = 10, min = 1, max = 200, step = 1},
	{key = "depth_far", kind = "slider", default = 20000, min = 1000, max = 100000, step = 500},
	{key = "ao_on", kind = "toggle", default = true, lua_only = true},
	{key = "ao", kind = "slider", default = 1, min = 0, max = 1, step = 0.05, gate = "ao_on"},
	{key = "ao_radius", kind = "slider", default = 200, min = 10, max = 200, step = 5},
	{key = "fog_on", kind = "toggle", default = true, lua_only = true},
	{key = "fog", kind = "slider", default = 0.2, min = 0, max = 1, step = 0.05, gate = "fog_on"},
	{key = "fog_start", kind = "slider", default = 0.25, min = 0, max = 1, step = 0.05},
	{key = "fog_tint", kind = "slider", default = 0.5, min = 0, max = 1, step = 0.05},
	{key = "dof_on", kind = "toggle", default = true, lua_only = true},
	{key = "dof", kind = "slider", default = 0.1, min = 0, max = 1, step = 0.05, gate = "dof_on"},
	{key = "dof_aim", kind = "slider", default = 0.5, min = 0, max = 1, step = 0.05, gate = "dof_on"},
	{key = "dof_focus", kind = "slider", default = 0, min = 0, max = 1, step = 0.05},
	{key = "dof_range", kind = "slider", default = 0.9, min = 0.05, max = 1, step = 0.05},
	{key = "dof_speed", kind = "slider", default = 6, min = 0.5, max = 10, step = 0.5},
	{key = "dof_near", kind = "slider", default = 0.3, min = 0, max = 1, step = 0.05},
	{key = "dof_weapon", kind = "toggle", default = true}, -- 1 always, 2 only while aiming down sights
	{key = "depth_view", kind = "toggle", default = false},


	-- Lighting from the depth buffer. The most expensive things in the mod: Plugin status shows
	-- their GPU time.
	{key = "il_on", kind = "toggle", default = true, lua_only = true},
	{key = "il", kind = "slider", default = 0.75, min = 0, max = 1, step = 0.05, gate = "il_on"},
	{key = "il_radius", kind = "slider", default = 200, min = 50, max = 500, step = 10},
	{key = "rtgi_on", kind = "toggle", default = true, lua_only = true},
	{key = "rtgi", kind = "slider", default = 0.75, min = 0, max = 1, step = 0.05, gate = "rtgi_on"},
	{key = "rtgi_distance", kind = "slider", default = 500, min = 100, max = 2000, step = 50},
	{key = "ssr_on", kind = "toggle", default = true, lua_only = true},
	{key = "ssr", kind = "slider", default = 0.2, min = 0, max = 1, step = 0.05, gate = "ssr_on"},
	{key = "ssr_blur", kind = "slider", default = 1, min = 0, max = 1, step = 0.05},
	{key = "temporal", kind = "toggle", default = true},

	-- Atmosphere
	{key = "hfog_on", kind = "toggle", default = false, lua_only = true},
	{key = "hfog", kind = "slider", default = 0.3, min = 0, max = 1, step = 0.05, gate = "hfog_on"},
	{key = "hfog_height", kind = "slider", default = 200, min = 20, max = 1000, step = 10},
	{key = "fog_glow", kind = "slider", default = 0.35, min = 0, max = 1, step = 0.05},
	{key = "shafts_on", kind = "toggle", default = true, lua_only = true},
	{key = "shafts", kind = "slider", default = 0.4, min = 0, max = 1, step = 0.05, gate = "shafts_on"},
	{key = "shafts_length", kind = "slider", default = 0.5, min = 0.1, max = 1, step = 0.05},

	{key = "ltm_on", kind = "toggle", default = true, lua_only = true},
	{key = "ltm", kind = "slider", default = 0.35, min = 0, max = 1, step = 0.05, gate = "ltm_on"},
	{key = "ltm_detail", kind = "slider", default = 0.3, min = 0, max = 1, step = 0.05},
	{key = "mblur_on", kind = "toggle", default = true, lua_only = true},
	{key = "mblur", kind = "slider", default = 0.5, min = 0, max = 1, step = 0.05, gate = "mblur_on"},
	{key = "motion_view", kind = "toggle", default = false},
	{key = "vanilla_off", kind = "toggle", default = true, lua_only = true},

	{key = "debug_view", kind = "choice", default = 1, options = 7},
	{key = "inject_after", kind = "slider", default = 0, min = 0, max = 16, step = 1},
	{key = "require_fullscreen", kind = "toggle", default = true}
}

-- One-click looks. Enhanced is what the mod ships as; the other two are the way back for a weaker
-- machine or for someone who only wants the cleanup. Applying one resets everything to the defaults
-- first, so a preset is always the same look however it was reached. Keys are left alone.
PSS.PRESETS = {
	{
		id = "enhanced",
		values = {}
	},
	{
		id = "performance",
		values = {il_on = false, rtgi_on = false, ssr_on = false, shafts_on = false, upscale = false, ao = 0.5, surface_grain_on = false, antialiasing = 3, render_scale = 1}
	},
	{
		id = "clean",
		values = {
			ao_on = false, il_on = false, rtgi_on = false, ssr_on = false, fog_on = false, shafts_on = false,
			ltm_on = false, dof_on = false, dirt_on = false, flare_on = false, color_bloom_on = false,
			cues_on = false, detail_on = false, surface_grain_on = false, render_scale = 1, bloom = 0, clarity = 0, shadows = 0, highlights = 0,
			contrast = 0, vibrance = 0, saturation = 1, lut_mix = 0, vignette = 0, adapt = 0, sharpen = 0.2
		}
	},
	{
		-- AndreikaRPM's own look, as the fork shipped it from 0.6 to 0.7: no vignette or chromatic aberration,
		-- a gentler bloom, Film Stock at a quarter, and no depth of field or motion blur.
		id = "andreika",
		values = {
			upscale = false, upscale_scale = 1.5, bloom = 0.3, vibrance = 0.5, lut = 6, lut_mix = 0.25,
			vignette = 0, chromatic = 0, dof_on = false, mblur_on = false, flare_on = false, bloom_tint = 0
		}
	}}

function PSS:apply_preset(id)
	for _, preset in ipairs(self.PRESETS) do
		if preset.id == id then
			self:reset_defaults()

			for key, value in pairs(preset.values) do
				self.settings[key] = value
			end

			self:sanitize()

			return true
		end
	end

	return false
end

PSS.BY_KEY = {}

for _, param in ipairs(PSS.PARAMS) do
	PSS.BY_KEY[param.key] = param
end

PSS.settings = {}

-- Counters the plugin watches for changes: tick is the heartbeat, the others are requests.
PSS._tick = 0
PSS._reload = 0
PSS._capture = 0

function PSS:log(fmt, ...)
	local ok, msg = pcall(string.format, "[PSS] " .. tostring(fmt), ...)

	log(ok and msg or ("[PSS] " .. tostring(fmt)))
end

-- One throw disables that feature for the session rather than repeating the fault, with one
-- log line and a short notice on screen.
PSS._broken = {}

PSS.FEATURE_NAMES = {
	["options page"] = "The options page",
	update = "The shader heartbeat",
	overlay = "The status text",
	jitter = "Camera jitter"
}

function PSS:loc(id, fallback, macros)
	local localization = rawget(_G, "managers") and managers.localization

	if localization and localization.exists and localization:exists(id) then
		return localization:text(id, macros)
	end

	return fallback
end

function PSS:guard(feature, fn, ...)
	if self._broken[feature] then
		return
	end

	local ok, err = pcall(fn, ...)

	if not ok then
		self._broken[feature] = true

		self:log("%s failed and is off for this session: %s", feature, tostring(err))
		pcall(self.notice_broken, self, feature)
	end
end

function PSS:notice_broken(feature)
	local name = self.FEATURE_NAMES[feature] or tostring(feature)
	local text = self:loc("pss_error_hint", "Screen Shaders: " .. name .. " switched off for this session after an error. Details are in the BLT log.", {FEATURE = name})
	local hud = rawget(_G, "managers") and managers.hud

	if hud and hud.show_hint then
		hud:show_hint({text = text, time = 4})
	end
end

function PSS:reset_defaults()
	for _, param in ipairs(self.PARAMS) do
		self.settings[param.key] = param.default
	end
end

-- Resets just these settings (one options page) and sends them to the plugin.
function PSS:reset_keys(keys)
	for _, key in ipairs(keys) do
		local param = self.BY_KEY[key]

		if param then
			self.settings[key] = param.default
		end
	end

	self:sanitize()
	self:push_all()
	self:save()
end

function PSS:sanitize()
	for _, param in ipairs(self.PARAMS) do
		local v = self.settings[param.key]

		if param.kind == "toggle" then
			if type(v) ~= "boolean" then
				self.settings[param.key] = param.default
			end
		else
			if type(v) ~= "number" or v ~= v then
				v = param.default
			end

			if param.kind == "slider" then
				v = math.clamp(v, param.min, param.max)
			else
				local count = param.kind == "lut" and #self:luts() or param.options

				v = math.clamp(math.floor(v + 0.5), 1, math.max(count, 1))
			end

			self.settings[param.key] = v
		end
	end

	-- The one Antialiasing choice decides the three values the plugin and the camera code read.
	local method = self.settings.antialiasing
	self.settings.aa = method ~= 1
	self.settings.aa_mode = ({2, 1, 2, 3, 2})[method]
	self.settings.taa_on = method == 5
end

-- Antialiasing was three settings to 0.6.10: on or off, the method, and TAA on top, which ran instead
-- of the method (SMAA T2x instead of TAA). An older file gets the one choice that matches what ran.
function PSS:antialiasing_from(aa, mode, taa)
	if aa and mode == 3 then
		return 4
	elseif taa then
		return 5
	elseif not aa then
		return 1
	end

	return mode == 1 and 2 or 3
end

-- Values whose meaning changed, and the settings version that changed it: files older than that
-- lose just that value. 2: inject_after = 1 meant "after the first back buffer draw", a rule the
-- game proved wrong. 3: Upscale sharpness moved to screen size, where 0.45 looks like the old 0.7.
-- 4: SMAA became the default once its blending was fixed (0.5.17); FXAA had been chosen over the
-- broken one. 5: TAA rebuilt (strength 0.9, jitter on). 6: TAA on by default once it worked in the game.
PSS.SETTINGS_VERSION = 6
PSS.RESET_IN = {inject_after = 2, upscale_sharpness = 3, aa_mode = 4, taa = 5, taa_jitter = 5, taa_on = 6}

function PSS:load()
	self:reset_defaults()

	-- Not settings the player edits: what PAYDAY 2's own video options held before this mod turned
	-- them off, and whether the one-time light adaptation repair has run.
	self._vanilla_saved = {}
	self._vanilla_repaired = 0

	local existing = io.file_is_readable(self.save_path)
	local data = existing and io.load_as_json(self.save_path)

	-- A first install has no older version behind it, so there is nothing to repair and a player
	-- who plays without light adaptation keeps playing without it.
	self._vanilla_repaired = existing and 0 or self.REPAIR_GENERATION

	if type(data) == "table" then
		if type(data.vanilla_saved) == "table" then
			self._vanilla_saved = data.vanilla_saved
		end

		local generation = data.vanilla_repaired

		self._vanilla_repaired = generation == true and 1 or tonumber(generation) or 0

		local version = tonumber(data.settings_version) or 1

		for key, v in pairs(data) do
			local param = self.BY_KEY[key]

			if param and type(v) == type(param.default) and version >= (self.RESET_IN[key] or 0) then
				self.settings[key] = v
			end
		end
	end

	if type(data) == "table" and data.antialiasing == nil then
		self.settings.antialiasing = self:antialiasing_from(self.settings.aa, self.settings.aa_mode, self.settings.taa_on)
	end

	-- Render scale cleanup became the FSR factor below 1x (0.5.29).
	if self.settings.render_scale < 1 then
		self.settings.upscale = true
		self.settings.upscale_scale = self.settings.render_scale
		self.settings.render_scale = 1
	end

	self:sanitize()
end

function PSS:save()
	self:sanitize()

	local data = {settings_version = self.SETTINGS_VERSION}

	for key, v in pairs(self.settings) do
		data[key] = v
	end

	data.vanilla_saved = self._vanilla_saved or {}
	data.vanilla_repaired = self._vanilla_repaired or 0

	io.save_as_json(data, self.save_path)
end

-- LUTs are whatever .cube files sit in luts/. Index 1 is always "none".
function PSS:luts()
	if self._luts then
		return self._luts
	end

	local list = {{name = "None", path = ""}}
	local files = file and file.GetFiles and file.GetFiles(self.lut_dir)

	if type(files) == "table" then
		local names = {}

		for _, name in ipairs(files) do
			if name:lower():match("%.cube$") then
				table.insert(names, name)
			end
		end

		table.sort(names)

		for _, name in ipairs(names) do
			local label = name:gsub("%.[Cc][Uu][Bb][Ee]$", ""):gsub("^%d+%s+", "")

			table.insert(list, {name = label, path = self.lut_dir .. name})
		end
	end

	self._luts = list

	return list
end

-- Loads once and negotiates live transport. A temporarily inactive Lua state
-- may retry its supported handshake; it never replays old files while pending.
local function native_retry_time()
	local wall = TimerManager and TimerManager.wall and TimerManager:wall()
	return wall and wall:time() or os.clock()
end

-- Missing tables can be temporary even when the DLL is already resident.
function PSS:_accept_native_api(api)
	if type(api) ~= "table" or api.capability ~= "memory-v1" or
		type(api.begin_memory) ~= "function" or type(api.publish_camera) ~= "function" or
		type(api.publish_settings) ~= "function" or type(api.status_text) ~= "function" or
		type(api.bridge_status) ~= "function" then return false end
	self._native_api, self._api_lookup_pending, self._api_retry_at = api, nil, nil
	local called, ready = pcall(api.begin_memory)
	self._ipc_mode = called and ready == true and "memory" or "memory_failed"
	return true
end

-- Only from the main thread: a coroutine's state gets no table from the loader. Lua 5.1 says
-- "nil" for the main thread, 5.2-style builds say "thread, true"; both are read here.
local function in_coroutine()
	local co, is_main = coroutine.running()

	return co ~= nil and not is_main
end

function PSS:_retry_native_api()
	if in_coroutine() or not blt or not blt.load_native then return end
	local now = native_retry_time()
	local remaining = (self._api_retry_at or now) - now
	if remaining > 0 and remaining <= 1 then return end
	self._api_retry_at = now + 1
	local ok, _, api = pcall(blt.load_native, self.dll_path)
	if ok then self:_accept_native_api(api) end
end

function PSS:native()
	if self._native ~= nil then
		if self._native and self._api_lookup_pending then
			self:_retry_native_api()
		elseif self._ipc_mode == "memory_failed" and self._native_api then
			local called, ready = pcall(self._native_api.begin_memory)
			if called and ready == true then self._ipc_mode = "memory" end
		end
		return self._native
	end

	self._native = false

	if not blt or not blt.load_native then
		self:log("this SuperBLT cannot load native plugins; the mod does nothing")
		return false
	end

	if not io.file_is_readable(self.dll_path) then
		self:log("native plugin missing at %s", self.dll_path)
		return false
	end

	local ok, result, api = pcall(blt.load_native, self.dll_path)

	if not ok then
		self:log("native plugin failed to load: %s", tostring(result))
		return false
	end

	self._native = true
	self._ipc_mode = "file"
	if not self:_accept_native_api(api) then
		self._api_lookup_pending, self._api_retry_at = true, native_retry_time() + 1
	end
	self:log("native plugin loaded (%s), live transport %s", tostring(result), self._ipc_mode)

	return true
end

-- What the plugin is sent for a parameter: the saved setting, plus whatever a mood is lending it
-- right now (lua/mood.lua), kept inside the parameter's own range. The setting itself never moves.
function PSS:live_value(param)
	local v = self.settings[param.key]
	local offset = self._offsets and self._offsets[param.key]

	if not offset or param.kind ~= "slider" or type(v) ~= "number" then
		return v
	end

	v = v + offset

	if param.min then
		v = math.max(v, param.min)
	end

	if param.max then
		v = math.min(v, param.max)
	end

	return v
end

-- Memory is selected once for this session. A rejected packet never revives
-- a stale file from a previous session. Unsupported native builds keep files.
function PSS:publish_memory(kind, text)
	if self._ipc_mode == "memory" then
		local called, accepted = pcall(self._native_api["publish_" .. kind], text)
		if not called or accepted ~= true then
			if not self._ipc_rejected then
				self._ipc_rejected = true
				self:log("native %s packet rejected; retaining the previous native state", kind)
			end
		end
		return true
	end
	return self._ipc_mode == "memory_failed"
end

function PSS:read_status()
	if not self:native() then return nil end
	if self._ipc_mode == "memory" then
		local ok, text = pcall(self._native_api.status_text)
		return ok and type(text) == "string" and text or nil
	end
	if self._ipc_mode == "memory_failed" then return nil end
	local file = io.open(self.status_path, "r")
	if not file then return nil end
	local text = file:read("*a"); file:close(); return text
end

function PSS:native_value(param, v)
	if param.gate and not self.settings[param.gate] then
		return 0
	end

	-- The FSR factor: above 1x the plugin upscales, below 1x it does the render scale cleanup.
	local scale = self.settings.upscale_scale

	-- A projection reads from the larger frame, so it switches FSR on by itself (above 1x).
	if param.key == "upscale" then
		return (self.settings.upscale or self.settings.panini_on or self.settings.spherical_on) and scale > 1.001 and 1 or 0
	elseif param.key == "render_scale" then
		return self.settings.upscale and math.min(scale, 1) or 1
	end

	if param.kind == "toggle" then
		return v and 1 or 0
	elseif param.kind == "choice" then
		return v - 1
	end

	return v
end

-- Looking through a security camera is looking at an in-world monitor, not through your own eyes.
-- The game grades the feed grey on purpose (IngameAccessCamera:at_enter sets "color_sin") and frames
-- it with its own noise and overlay. Eye adaptation, depth of field, the vignette and the grade
-- are all built for the player's own view, and laid over the feed they only darken it.
-- So the chain steps aside while the feed is up. The player's own setting is never touched.
PSS.STEP_ASIDE_STATES = {ingame_access_camera = true}

-- The weapon workshop: the game draws your weapon in a little room of its own, and the chain has
-- nothing to add to a menu. It also keeps the shaders away from a screen where the renderer has
-- crashed (a game update changed that screen), so the workshop is left to the game.
PSS.STEP_ASIDE_SCENES = {
	blackmarket_crafting = true,
	blackmarket_weapon_color = true,
	blackmarket_customize = true
}

function PSS:scene_template()
	local scene = managers and managers.menu_scene

	return scene and scene._current_scene_template or nil
end

function PSS:stepped_aside()
	if self.STEP_ASIDE_STATES[self:game_state()] == true then
		return true
	end

	if self.settings.aside_workshop then
		local template = self:scene_template()

		if template and self.STEP_ASIDE_SCENES[template] then
			return true
		end
	end

	return false
end

-- The game state's name (ingame_standard, ingame_bleed_out ...), or "none".
function PSS:game_state()
	local gsm = rawget(_G, "game_state_machine")

	if not gsm then
		return "none"
	end

	local ok, name = pcall(gsm.current_state_name, gsm)

	return ok and type(name) == "string" and name or "none"
end

-- Writes every setting at once. The trailing "end" line is how the plugin tells a finished
-- file from one it caught halfway through being written.
function PSS:write_bridge()
	if not self:native() then
		return
	end

	local lut = self:luts()[self.settings.lut]
	local lines = {
		"shader_path " .. self.shader_path,
		"lut_path " .. (lut and lut.path or ""),
		"dirt_path " .. self.dirt_path
	}

	for _, param in ipairs(self.PARAMS) do
		if param.kind ~= "lut" and not param.lua_only then
			local v = self:live_value(param)

			if param.key == "enabled" and self._aside then
				v = false
			end

			table.insert(lines, param.key .. " " .. tostring(self:native_value(param, v)))
		end
	end

	table.insert(lines, "tick " .. self._tick)
	table.insert(lines, "reload " .. self._reload)
	table.insert(lines, "capture " .. self._capture)
	table.insert(lines, "end")
	table.insert(lines, "")

	local text = table.concat(lines, "\n")
	if self:publish_memory("settings", text) then return end
	local f = io.open(self.live_path, "w")

	if f then
		f:write(text)
		f:close()
	end
end

function PSS:push(key)
	self:write_bridge()
end

function PSS:push_all()
	self:write_bridge()
end

-- Called from the menu and game update hooks. Each write bumps the tick, which the plugin
-- treats as a heartbeat: when updates stop (loading screens), it stops processing frames.
function PSS:tick(t)
	-- Going onto a camera feed, or back off it, is sent at once rather than on the next heartbeat.
	local aside = self:stepped_aside()

	if aside ~= (self._aside or false) then
		self._aside = aside
		self._next = nil
	end

	-- More than a second "early" means the clock changed under us, not that we are early; a
	-- throttle that trusted it would silence the heartbeat until that clock caught up.
	if self._next and t < self._next and self._next - t <= 1 then
		return
	end

	self._next = t + 0.5
	self._tick = self._tick + 1
	self:write_bridge()
end

function PSS:set(key, v)
	self.settings[key] = v
	self:sanitize()
	self:push(key)

	if self.overlay and self.overlay.changed then
		self.overlay:changed(key)
	end
end

function PSS:toggle(key)
	self:set(key, not self.settings[key])
end

function PSS:reload()
	self._luts = nil
	self._reload = self._reload + 1
	self:sanitize()
	self:write_bridge()
end

function PSS:capture()
	self._capture = self._capture + 1
	self:write_bridge()
	self:log("draw capture requested: mods/logs/pss_capture.txt")
end

-- The effects that work in view space: they turn a depth into a position, so they need the
-- camera's projection, and height fog also needs which way is up.
PSS.CAMERA_USERS = {"ao_on", "il_on", "rtgi_on", "ssr_on", "hfog_on", "taa_on"}

function PSS:wants_camera()
	if not self.settings.enabled then
		return false
	end

	-- Depth of field changes its blur while you aim, so it needs to know when you are aiming.
	if self.settings.dof_on then
		return true
	end

	if self:t2x() then
		return true
	end

	for _, key in ipairs(self.CAMERA_USERS) do
		if self.settings[key] then
			return true
		end
	end

	return false
end

-- Sub-pixel camera jitter for temporal antialiasing. An 8 frame Halton (2, 3) cycle, half a pixel either
-- way: well spread positions, so a handful of frames cover the whole pixel.
PSS.jitter = {x = 0, y = 0, index = 0}

local function halton(index, base)
	local result, fraction, i = 0, 1, index

	while i > 0 do
		fraction = fraction / base
		result = result + fraction * (i % base)
		i = math.floor(i / base)
	end

	return result
end

function PSS:screen_size()
	local ok, resolution = pcall(function()
		return RenderSettings.resolution
	end)

	if ok and resolution and resolution.x and resolution.x > 1 then
		return resolution.x, resolution.y
	end

	return 1920, 1080
end

-- The jitter for the frame about to be drawn, in pixels. Zero unless the plugin says it allows it:
-- a jittered frame that nothing puts back would shake.
-- Why Lua is or is not jittering, sent to the plugin for its status file: 0 jittering, 1 off in the
-- options, 2 waiting for the plugin to allow it, 3 stopped (its code failed), 4 the game moved the camera
-- after the turn, before the frame was drawn.
PSS._jitter_state = 1

-- SMAA T2x (Antialiasing method 3) instead of temporal antialiasing: its own two jitter positions, a
-- quarter pixel either way along a diagonal, as smaa.hlsl gives them (@SUBSAMPLE_INDICES; y up there and
-- here). Of both diagonals, each with either subsample set first, this one with set 1 for the right
-- position scored best in the harness, standing still and turning (dev/NOTES.md).
function PSS:t2x()
	return self.settings.enabled and self.settings.aa and self.settings.aa_mode == 3
end

PSS.T2X_JITTER = {{x = 0.25, y = -0.25}, {x = -0.25, y = 0.25}}

function PSS:next_jitter()
	local j = self.jitter
	local t2x = self:t2x()
	local wanted = self.settings.enabled and (self.settings.taa_on or t2x) and self.settings.taa_jitter

	if not wanted or not self._jitter_allowed then
		j.x, j.y = 0, 0
		self._jitter_state = wanted and 2 or 1

		return j
	end

	self._jitter_state = 0

	if t2x then
		j.index = j.index % 2 + 1
		j.x, j.y = self.T2X_JITTER[j.index].x, self.T2X_JITTER[j.index].y

		return j
	end

	j.index = j.index % 8 + 1
	j.x = halton(j.index, 2) - 0.5
	j.y = halton(j.index, 3) - 0.5

	return j
end

-- Turns the camera by this frame's jitter, right after the game's viewport update has placed it.
--
-- Where, measured in the game: 0.5.23 turned the camera in Setup:pre_render and put it back in
-- Setup:render; 0.5.24 left the turn on until the game moved the camera (it does, every frame). Neither
-- turn reached a frame (the plugin compares the frames' depth with both cameras). Yet the camera read
-- in pre_render is the one the frame is drawn with. So the game takes its copy of the camera for drawing
-- after it places the camera and before pre_render; this turns it as soon as it is placed. pre_render
-- then checks the turn is still there, and the plugin checks the frames show it.
--
-- Aiming does not use this camera object (PlayerCamera keeps its own rotation), so a shot is never
-- nudged.
function PSS:jitter_after_viewport_update(camera)
	local rotation = camera:rotation()
	local last = self._jitter_frame

	-- Still turned from earlier (the game did not place it again): start from the camera before the turn.
	if last and last.camera == camera and self:vector_gap(rotation:y(), last.jittered:y()) < 1e-6 and self:vector_gap(rotation:z(), last.jittered:z()) < 1e-6 then
		rotation = last.rotation
	end

	self._jitter_frame = nil
	local j = self:next_jitter()

	if j.x == 0 and j.y == 0 or not self._camera_tan_x then
		if last and rotation == last.rotation then
			camera:set_rotation(rotation)
		end

		return
	end

	local w, h = self:screen_size()
	local a = j.x * 2 * self._camera_tan_x / w
	local b = j.y * 2 * self._camera_tan_y / h
	local jittered = Rotation()
	mrotation.set_look_at(jittered, (rotation:y() + rotation:x() * a + rotation:z() * b):normalized(), rotation:z() - rotation:y() * b)
	camera:set_rotation(jittered)

	if not self._jitter_logged then
		self._jitter_logged = true
		self:log("first jitter, after the viewport update: %.2f x %.2f pixels, %.5f degrees", j.x, j.y, math.deg(self:vector_gap(jittered:y(), rotation:y())))
	end

	self._jitter_frame = {camera = camera, rotation = last and rotation == last.rotation and rotation or Rotation(rotation:yaw(), rotation:pitch(), rotation:roll()), jittered = jittered, x = j.x, y = j.y}
end

-- For pre_render: the camera as the game placed it, the camera the frame is drawn with, and the jitter
-- in it. If the game moved the camera after the turn, there is no turn in the frame.
function PSS:frame_cameras(camera)
	local rotation = camera:rotation()
	local f = self._jitter_frame

	if not f or f.camera ~= camera then
		return rotation, rotation, 0, 0
	end

	if self:vector_gap(rotation:y(), f.jittered:y()) < 1e-6 and self:vector_gap(rotation:z(), f.jittered:z()) < 1e-6 then
		return f.rotation, f.jittered, f.x, f.y
	end

	self._jitter_frame = nil
	self._jitter_state = 4

	if not self._jitter_moved_logged then
		self._jitter_moved_logged = true
		self:log("the game moved the camera after the jitter, before pre_render")
	end

	return rotation, rotation, 0, 0
end

function PSS:vector_gap(a, b)
	local x, y, z = a.x - b.x, a.y - b.y, a.z - b.z

	return math.sqrt(x * x + y * y + z * z)
end

-- The projection is MEASURED rather than taken from a field of view number: points ten metres
-- ahead and ten metres to either side and above and below are projected through the HUD's own
-- world_to_screen, and how far apart they land gives the tangent of half the field of view on each
-- axis directly. That is right whether the game's FOV setting is horizontal or vertical, for any
-- aspect ratio, and through zooming sights. Both sides, not one, which cancels a turned camera. One
-- capture still moved with the jitter (by its own size in tangents); in a heist that hardly happens
-- (0.5.35: 126 of 29,221 frame pairs looked like it, and the depth could not tell them apart).
function PSS:camera_line()
	local viewport = managers and managers.viewport
	local camera = viewport and viewport:get_current_camera()
	local workspace = managers and managers.hud and managers.hud._workspace

	if not camera or not workspace then
		return nil
	end

	local position = camera:position()
	local rotation, drawn, jitter_x, jitter_y = self:frame_cameras(camera)
	local ahead = position + rotation:y() * 1000
	local right = workspace:world_to_screen(camera, ahead + rotation:x() * 1000)
	local left = workspace:world_to_screen(camera, ahead - rotation:x() * 1000)
	local above = workspace:world_to_screen(camera, ahead + rotation:z() * 1000)
	local below = workspace:world_to_screen(camera, ahead - rotation:z() * 1000)
	local panel = workspace:panel()
	local dx = math.abs(right.x - left.x) * 0.5
	local dy = math.abs(above.y - below.y) * 0.5

	-- Written this way round so a camera that is not a number (seen while loading) is left out too.
	if not (dx >= 1 and dy >= 1) then
		return nil
	end

	self._camera_tan_x = panel:w() * 0.5 / dx
	self._camera_tan_y = panel:h() * 0.5 / dy
	self._render_frame = ((self._render_frame or 0) + 1) % 1000000

	-- Then the pose, for temporal antialiasing: where the camera is and its right, up and forward axes
	-- (rotation x, z and y in Diesel), each turned to point the way it does on screen as measured above.
	-- First as the frame is drawn, jitter included, then without it.
	local flip_x = right.x >= left.x and 1 or -1
	local flip_z = above.y <= below.y and 1 or -1

	if self._broken.jitter then
		self._jitter_state = 3
	end

	local axes = {}

	for _, r in ipairs({drawn, rotation}) do
		for _, v in ipairs({r:x() * flip_x, r:z() * flip_z, r:y()}) do
			table.insert(axes, string.format("%.6f %.6f %.6f", v.x, v.y, v.z))
		end
	end

	return string.format("%.5f %.5f %.3f %.3f %.2f %d %.4f %.4f %d %.3f %.3f %.3f %s %d %s\n",
		self._camera_tan_x, self._camera_tan_y, rotation:pitch(), rotation:roll(), position.z, self:aiming() and 1 or 0,
		jitter_x, jitter_y, self._render_frame, position.x, position.y, position.z, table.concat(axes, " "), self._jitter_state, self:game_state())
end

-- Aiming down sights, from the player's own movement state. False in menus, when downed, or with
-- no player at all.
function PSS:aiming()
	local player = managers and managers.player and managers.player:player_unit()

	if not player or (alive and not alive(player)) then
		return false
	end

	local movement = player:movement()
	local state = movement and movement:current_state()

	return state and state.in_steelsight and state:in_steelsight() and true or false
end

function PSS:write_camera()
	if not self._native or not self:wants_camera() then
		return
	end

	local line = self:camera_line()

	if not line then
		return
	end

	-- The last four lines, oldest first. PAYDAY 2 draws on a thread of its own, so by the time the
	-- plugin runs on a frame, Lua may already have written the next frame's line; the plugin measures
	-- which one the frame was really drawn with.
	local lines = self._camera_lines or {}
	self._camera_lines = lines
	table.insert(lines, line)

	if #lines > 4 then
		table.remove(lines, 1)
	end

	local text = table.concat(lines)
	if self:publish_memory("camera", text) then return end
	local f = io.open(self.camera_path, "w")

	if f then
		f:write(text)
		f:close()
	end
end

-- PAYDAY 2's own versions of things this mod now does. Left on, they stack: two antialiasers soften
-- each other, the game's occlusion darkens the same creases ours already has, and its eye adaptation
-- fights ours for the exposure. They are turned off while this mod is on, and put back as they were
-- if the switch below is turned off.
--
-- Deliberately NOT touched: video_color_grading (its default is the game's own look, not "off", and
-- this mod's colours were tuned with it on) and video_anti_alias (only read to fill in the menu, and
-- it needs a restart, so forcing it would change a setting without changing the picture).
-- PAYDAY 2's own versions of effects this mod does itself.
--
-- Light adaptation is deliberately not in this list, and is not switched off any other way
-- either. The game adapts in HDR, before its tone mapping; this mod's own adaptation works on the
-- finished 8-bit frame, so it can trim the exposure but cannot lift a dark room without banding and
-- without dragging up shadows the tone mapper has already crushed. Taking the game's away left
-- everything darker (0.5.0 by writing the setting, 0.5.1-0.5.2 at runtime). The two do not fight:
-- this mod measures the frame before its own exposure, so it never sees its own correction.
PSS.VANILLA_OFF = {
	{key = "video_aa", off = "off", what = "antialiasing"},
	{key = "video_ao", off = "off", what = "ambient occlusion"},
	{key = "dof_setting", off = "none", what = "depth of field"}
}

function PSS:vanilla_setting(key)
	local ok, value = pcall(function()
		return managers.user:get_setting(key)
	end)

	return ok, value
end

-- Changes one of PAYDAY 2's own settings AND writes the game's settings file straight away.
-- set_setting only changes the value in memory; the game writes it to disk later, when its options
-- menu closes or the game exits cleanly. 0.5.1's repair recorded itself as done in this mod's file
-- at once, and a crash before the game saved - which 0.5.1 had, on leaving a heist - threw the
-- repair away while this mod went on believing it had worked.
function PSS:set_game_setting(key, value)
	local ok = pcall(function()
		managers.user:set_setting(key, value)
	end)

	if ok then
		pcall(function()
			managers.savefile:save_setting(true)
		end)
	end

	return ok
end

-- Once per save file: hand back the light adaptation an older build turned off and could not
-- undo. Someone who had chosen to play without it has it switched on this one time; that is the
-- cost of getting everyone else out of a game that is dark for no reason.
PSS.REPAIR_GENERATION = 2

function PSS:repair_vanilla_settings()
	if self._vanilla_repaired >= self.REPAIR_GENERATION then
		return
	end

	local ok, current = self:vanilla_setting("light_adaption")

	if not ok then
		return
	end

	self._vanilla_repaired = self.REPAIR_GENERATION
	self:log("PAYDAY 2's light adaptation is %s", current == false and "OFF" or "on")

	if current == false then
		self:set_game_setting("light_adaption", true)

		self:log("put PAYDAY 2's light adaptation back on; a build before 0.5.1 turned it off and could not undo it")
	end

	self:save()
end

-- Everything this mod ever changed in PAYDAY 2's own options, handed back on demand. It exists
-- because light adaptation lives on the Advanced Video page and some builds do not show it at
-- all, which leaves a player who has met the 0.5.0 bug with nothing to click. Returns the list of
-- what it put back, for the dialog.
function PSS:restore_vanilla_settings()
	if not managers or not managers.user or not managers.user.set_setting then
		return {}
	end

	local put_back = {}

	self.settings.vanilla_off = false
	self._vanilla_saved = self._vanilla_saved or {}

	for _, entry in ipairs(self.VANILLA_OFF) do
		local saved = self._vanilla_saved[entry.key]

		if saved ~= nil then
			self._vanilla_saved[entry.key] = nil

			self:set_game_setting(entry.key, saved)

			put_back[#put_back + 1] = entry.what
		end
	end

	-- The game's own default, which is what an older build of this mod took away.
	local ok, current = self:vanilla_setting("light_adaption")

	if ok and current == false then
		self:set_game_setting("light_adaption", true)

		put_back[#put_back + 1] = "light adaptation"
	end

	self:save()

	return put_back
end

-- Quitting: the game's own antialiasing, ambient occlusion and depth of field go back to what the
-- player had before this mod touched them, and the game's settings file is written with those
-- values. The preference is kept, so they are taken off again next time the plugin runs.
--
-- This is what makes the mod removable. Before 0.8.0 the values were only handed back while the mod
-- was running, so deleting the mod with them forced off left the game with no antialiasing and no
-- ambient occlusion and nothing to put them back - exactly what players reported.
function PSS:hand_back_vanilla(reason)
	if not managers or not managers.user or not managers.user.set_setting then
		return {}
	end

	self._vanilla_saved = self._vanilla_saved or {}

	local put_back = {}

	for _, entry in ipairs(self.VANILLA_OFF) do
		local saved = self._vanilla_saved[entry.key]

		if saved ~= nil then
			self._vanilla_saved[entry.key] = nil

			self:set_game_setting(entry.key, saved)

			put_back[#put_back + 1] = entry.what
		end
	end

	if #put_back > 0 then
		self:log("%s: gave PAYDAY 2's own %s back", tostring(reason or "quit"), table.concat(put_back, ", "))
		self:save()
	end

	return put_back
end

function PSS:sync_vanilla_settings(t)
	if not managers or not managers.user or not managers.user.get_setting then
		return
	end

	self._vanilla_saved = self._vanilla_saved or {}

	self:repair_vanilla_settings()

	-- Only once the plugin has really processed a frame. Loaded is not enough: a plugin whose hooks
	-- see no draws would take the game's antialiasing and occlusion away and give nothing back.
	local should_force = self.settings.enabled and self.settings.vanilla_off and self:native() and self._plugin_working
	local changed = false

	for _, entry in ipairs(self.VANILLA_OFF) do
		local ok, current = self:vanilla_setting(entry.key)

		if ok then
			if should_force then
				if current ~= entry.off then
					-- Written to this mod's save file, not just held in memory, so the player's
					-- own choice survives a restart, a crash and removing the mod.
					if self._vanilla_saved[entry.key] == nil then
						self._vanilla_saved[entry.key] = current
						changed = true
					end

					self:set_game_setting(entry.key, entry.off)

					self:log("turned PAYDAY 2's own %s off; this mod does it instead", entry.what)
				end
			elseif self._vanilla_saved[entry.key] ~= nil then
				local saved = self._vanilla_saved[entry.key]
				self._vanilla_saved[entry.key] = nil
				changed = true

				self:set_game_setting(entry.key, saved)

				self:log("gave PAYDAY 2's own %s back", entry.what)
			end
		end
	end

	if changed then
		self:save()
	end
end

-- The plugin rewrites its status once a second. Two lines matter here: whether the camera may be
-- jittered, and whether the shaders have really processed a frame (loaded is not the same as working).
function PSS:poll_status(t)
	if self._status_checked and t - self._status_checked < 1 then
		return
	end

	self._status_checked = t

	local text = self:read_status()
	if not text then
		self._jitter_allowed = false
		return
	end
	self._jitter_allowed = text:match("\nCamera jitter: allowed") ~= nil

	-- Only a rising count counts: the file left from the last session can be read before the plugin
	-- rewrites it, and its number never moves.
	-- ponytail: once true it stays true for the session; reset it if the plugin can stop mid-session
	local processed = tonumber(text:match("processed (%d+)")) or 0

	if self._last_processed and processed > self._last_processed then
		self._plugin_working = true
	end

	self._last_processed = processed
end

function PSS:status_text()
	if not self:native() then
		return "The native plugin is not loaded. See the BLT log for why."
	end

	return self:read_status() or "The plugin has not written its status yet. Try again in a second."
end

PSS:load()

-- Copper Core: the series' shared foundation, and this mod's entry in the series hub
-- (Payday Modern Menu > Series). See dev/copper_core/.
do
	local ok, Core = pcall(dofile, ModPath .. "lua/copper_core.lua")

	if ok and Core and Core.register then
		Core:register({
			id = "pss",
			name = "Screen Shaders",
			version = PSS.VERSION,
			settings = PSS.settings,
			save = function()
				PSS:save()
			end,
			apply = nil,
			whats_new = "pss_info_new_text"
		})
	end
end
