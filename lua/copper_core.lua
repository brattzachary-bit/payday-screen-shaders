--[[
	Copper Core - the shared foundation of Copper's PAYDAY 2 series.

	Every series mod ships this same file as lua/copper_core.lua (dev/copper_core/sync.sh copies
	it). Whichever copy is newest wins at runtime, so each mod installs on its own yet all of them
	run one set of code: the theme, fades, text, settings, error handling, and the registry the
	series hub reads.

	Edit ONLY dev/copper_core/copper_core.lua, bump CORE_VERSION, then run sync.sh.
]]

local CORE_VERSION = 3

if _G.CopperCore and (_G.CopperCore.VERSION or 0) >= CORE_VERSION then
	return _G.CopperCore
end

local C = _G.CopperCore or {}
_G.CopperCore = C

C.VERSION = CORE_VERSION
C.mods = C.mods or {}          -- id -> registration (see C:register)
C.order = C.order or {}        -- ids in registration order

-- ============================================================================
-- Series-wide settings (owned by the hub, read by every mod)
-- ============================================================================

C.shared_path = (SavePath or "mods/saves/") .. "copper_series.json"
C.SHARED_DEFAULTS = {
	colorblind = false,        -- sky blue and vermilion instead of amber and red, everywhere
	text_backing = false       -- a faint strip behind overlay text, everywhere
}

C.shared = C.shared or {}

function C:load_shared()
	for k, v in pairs(self.SHARED_DEFAULTS) do
		if self.shared[k] == nil then
			self.shared[k] = v
		end
	end

	local ok, data = pcall(function()
		return io.file_is_readable(self.shared_path) and io.load_as_json(self.shared_path)
	end)

	if ok and type(data) == "table" then
		for k, v in pairs(data) do
			if self.SHARED_DEFAULTS[k] ~= nil and type(v) == type(self.SHARED_DEFAULTS[k]) then
				self.shared[k] = v
			end
		end
	end
end

function C:save_shared()
	pcall(io.save_as_json, self.shared, self.shared_path)
end

C:load_shared()

-- ============================================================================
-- Theme: the operations-room palette (Payday Modern Menu's DESIGN.md)
-- ============================================================================

C.colors = {
	charcoal = Color(0.07, 0.07, 0.075),
	warm_black = Color(0.06, 0.05, 0.045),
	ivory = Color(0.93, 0.90, 0.84),
	ivory_dim = Color(0.62, 0.60, 0.56),
	muted = Color(0.42, 0.41, 0.39),
	amber = Color(0.95, 0.68, 0.25),
	red = Color(0.78, 0.30, 0.26),
	blue = Color(0.47, 0.60, 0.74),
	green = Color(0.48, 0.72, 0.48),
	brass = Color(0.80, 0.66, 0.40),
	shadow = Color(0, 0, 0)
}

-- Okabe-Ito pair that stays apart under every common colour blindness.
C.colorblind_colors = {
	amber = Color(0.34, 0.71, 0.91),
	red = Color(0.90, 0.38, 0.00)
}

-- colorblind: nil = the series setting; true/false = a mod's own override.
function C:color(name, colorblind)
	if colorblind == nil then
		colorblind = self.shared.colorblind
	end

	if colorblind and self.colorblind_colors[name] then
		return self.colorblind_colors[name]
	end

	return self.colors[name] or self.colors.ivory
end

-- Diesel fonts are atlases baked at one size: each tier at its own native size.
function C:font(tier)
	local m = tweak_data.menu

	if tier == "large" then
		return m.pd2_large_font, m.pd2_large_font_size
	elseif tier == "medium" then
		return m.pd2_medium_font, m.pd2_medium_font_size
	elseif tier == "tiny" then
		-- The small face, drawn smaller: for a line that has to keep out of the way.
		return m.pd2_small_font, math.max(math.floor(m.pd2_small_font_size * 0.8), 8)
	end

	return m.pd2_small_font, m.pd2_small_font_size
end

C.FADE_IN = 0.15
C.FADE_OUT = 0.25
C.SLIDE = 4
C.SHADOW_ALPHA = 0.5

function C.upper(text)
	text = tostring(text or "")

	return utf8 and utf8.to_upper and utf8.to_upper(text) or string.upper(text)
end

function C.loc(id, macros)
	local ok, text = pcall(function()
		return managers.localization:text(id, macros)
	end)

	return ok and text or id
end

-- 83 -> "01:23", 3725 -> "1:02:05"
function C.clock(seconds)
	seconds = math.max(0, math.floor(tonumber(seconds) or 0))

	local h = math.floor(seconds / 3600)
	local m = math.floor(seconds / 60) % 60
	local s = seconds % 60

	if h > 0 then
		return string.format("%d:%02d:%02d", h, m, s)
	end

	return string.format("%02d:%02d", m, s)
end

-- ============================================================================
-- Settings: load, save, sanitize, reset - the same behaviour in every mod
-- ============================================================================

local Settings = {}
Settings.__index = Settings
C.Settings = Settings

-- spec: {path = file, defaults = {}, ranges = {key = {min, max}}, internal = {key = true}}
function Settings.new(spec)
	local self = setmetatable({}, Settings)

	self.path = spec.path
	self.defaults = spec.defaults or {}
	self.ranges = spec.ranges or {}
	self.internal = spec.internal or {}
	self.values = spec.values or {}

	for k, v in pairs(self.defaults) do
		if self.values[k] == nil then
			self.values[k] = v
		end
	end

	return self
end

function Settings:sanitize()
	for k, range in pairs(self.ranges) do
		if type(self.values[k]) ~= "number" then
			self.values[k] = self.defaults[k]
		end

		self.values[k] = math.min(math.max(self.values[k], range[1]), range[2])
	end
end

function Settings:load()
	local ok, data = pcall(function()
		return io.file_is_readable(self.path) and io.load_as_json(self.path)
	end)

	if ok and type(data) == "table" then
		self:apply(data)
	end

	self:sanitize()

	return self.values
end

-- Takes known keys of the right type from `data` (a save file or an imported profile).
function Settings:apply(data)
	local n = 0

	for k, v in pairs(data) do
		if self.defaults[k] ~= nil and type(self.defaults[k]) == type(v) then
			self.values[k] = v
			n = n + 1
		end
	end

	return n
end

function Settings:save()
	self:sanitize()
	pcall(io.save_as_json, self.values, self.path)
end

function Settings:reset(keys)
	if not keys then
		keys = {}

		for k in pairs(self.defaults) do
			if not self.internal[k] then
				keys[#keys + 1] = k
			end
		end
	end

	for _, k in ipairs(keys) do
		if self.defaults[k] ~= nil and not self.internal[k] then
			self.values[k] = self.defaults[k]
		end
	end

	self:save()

	return keys
end

-- The values worth sharing in a profile (everything but internal bookkeeping).
function Settings:export()
	local out = {}

	for k, v in pairs(self.values) do
		if self.defaults[k] ~= nil and not self.internal[k] then
			out[k] = v
		end
	end

	return out
end

-- ============================================================================
-- Errors: a feature that throws is switched off for the session, never every frame
-- ============================================================================

-- log_prefix "[PMH]"; broken = the mod's table of switched-off features.
function C.guard(log_prefix, broken, feature, fn, ...)
	if broken[feature] then
		return false
	end

	local ok, err = pcall(fn, ...)

	if not ok then
		broken[feature] = true
		log(string.format("%s %s failed and is off for this session: %s", log_prefix, tostring(feature), tostring(err)))
	end

	return ok, err
end

-- The series notice: "<Entry name>: <feature> switched off for this session after an error."
function C.error_notice(entry_name, feature_name)
	pcall(function()
		if managers.hud and managers.hud.show_hint then
			managers.hud:show_hint({
				text = string.format("%s: %s switched off for this session after an error. Details are in the BLT log.", entry_name, feature_name),
				time = 4
			})
		end
	end)
end

-- ============================================================================
-- Text with the series' 1 px shadow, and fades with the series' timings
-- ============================================================================

local Label = {}
Label.__index = Label
C.Label = Label

-- params: x, y, w, h, tier ("small"/"medium"/"large"), align, vertical, color, text, layer
function Label.new(panel, params)
	local font, size = C:font(params.tier)
	local self = setmetatable({}, Label)
	local base = {
		x = params.x or 0,
		y = params.y or 0,
		w = params.w or panel:w(),
		h = params.h or size + 4,
		font = font,
		font_size = size,
		align = params.align or "left",
		vertical = params.vertical or "top",
		text = params.text or "",
		wrap = false
	}
	local shadow, main = {}, {}

	for k, v in pairs(base) do
		shadow[k] = v
		main[k] = v
	end

	shadow.x = base.x + 1
	shadow.y = base.y + 1
	shadow.color = C.colors.shadow
	shadow.alpha = C.SHADOW_ALPHA
	shadow.layer = (params.layer or 1) - 1
	main.color = params.color or C:color("ivory")
	main.layer = params.layer or 1

	self._shadow = panel:text(shadow)
	self._main = panel:text(main)
	self._text = base.text

	return self
end

function Label:set_text(text)
	text = tostring(text or "")

	if text ~= self._text then
		self._text = text
		self._main:set_text(text)
		self._shadow:set_text(text)
	end
end

function Label:text()
	return self._text
end

function Label:set_color(color)
	self._main:set_color(color)
end

function Label:set_visible(visible)
	self._main:set_visible(visible)
	self._shadow:set_visible(visible)
end

function Label:set_x(x)
	self._main:set_x(x)
	self._shadow:set_x(x + 1)
end

function Label:set_y(y)
	self._main:set_y(y)
	self._shadow:set_y(y + 1)
end

function Label:text_w()
	local _, _, w = self._main:text_rect()

	return math.ceil(w or 0)
end

local Fader = {}
Fader.__index = Fader
C.Fader = Fader

-- opacity: optional function returning the owner's opacity setting (0..1)
function Fader.new(panel, slide, opacity)
	local self = setmetatable({}, Fader)

	self.panel = panel
	self.alpha = 0
	self.target = 0
	self.slide = slide or 0
	self.base_y = panel:y()
	self.opacity = opacity

	panel:set_alpha(0)
	panel:set_visible(false)

	return self
end

function Fader:show(on)
	self.target = on and 1 or 0
end

function Fader:snap(on)
	self.target = on and 1 or 0
	self.alpha = self.target
	self:_apply()
end

function Fader:_apply()
	local p = self.panel

	if not alive(p) then
		return
	end

	local o = self.opacity and self.opacity() or 1

	p:set_alpha(self.alpha * math.min(math.max(o, 0), 1))
	p:set_visible(self.alpha > 0)

	if self.slide ~= 0 then
		p:set_y(math.floor(self.base_y + (1 - self.alpha) * self.slide + 0.5))
	end
end

function Fader:update(dt)
	if self.alpha == self.target then
		return
	end

	if self.target > self.alpha then
		self.alpha = math.min(self.target, self.alpha + dt / C.FADE_IN)
	else
		self.alpha = math.max(self.target, self.alpha - dt / C.FADE_OUT)
	end

	self:_apply()
end

-- ============================================================================
-- Builds: a player's perk deck and skill split, from the outfit every player sends the lobby
-- ============================================================================

-- The five skill trees in the game's own order, with a one-letter tag each.
C.SKILL_TREES = {
	{skill = "mastermind", tag = "M"},
	{skill = "enforcer", tag = "E"},
	{skill = "technician", tag = "T"},
	{skill = "ghost", tag = "G"},
	{skill = "hoxton", tag = "F"}
}

-- outfit: NetworkPeer:blackmarket_outfit() (or anything with .skills = {skills = {...},
-- specializations = {deck, tier}}). Returns {deck_name, deck_tier, points = {mastermind = n...},
-- total} or nil.
function C.build_of_outfit(outfit)
	local skills = outfit and outfit.skills

	if type(skills) ~= "table" then
		return nil
	end

	local build = {points = {}, total = 0}
	local trees = tweak_data and tweak_data.skilltree and tweak_data.skilltree.trees or {}

	for i, points in ipairs(skills.skills or {}) do
		local tree = trees[i]
		local n = tonumber(points) or 0

		if tree and tree.skill then
			build.points[tree.skill] = (build.points[tree.skill] or 0) + n
			build.total = build.total + n
		end
	end

	local spec = skills.specializations or {}
	local deck = tonumber(spec[1])
	local deck_data = deck and tweak_data.skilltree.specializations and tweak_data.skilltree.specializations[deck]

	if deck_data then
		build.deck_name = deck_data.name_id and C.loc(deck_data.name_id) or nil
		build.deck_tier = tonumber(spec[2])
	end

	return build
end

-- "ARMORER 9/9  ·  M 12  E 30  T 8  G 0  F 21"
function C.build_line(outfit)
	local build = C.build_of_outfit(outfit)

	if not build then
		return nil
	end

	local parts = {}

	if build.deck_name then
		parts[#parts + 1] = C.upper(build.deck_name) .. (build.deck_tier and (" " .. build.deck_tier .. "/9") or "")
	end

	local trees = {}

	for _, t in ipairs(C.SKILL_TREES) do
		trees[#trees + 1] = t.tag .. " " .. tostring(build.points[t.skill] or 0)
	end

	parts[#parts + 1] = table.concat(trees, "  ")

	return table.concat(parts, "  ·  ")
end

-- ============================================================================
-- The series registry (read by the hub in Payday Modern Menu)
-- ============================================================================

--[[
	C:register({
		id = "pmh",                        -- short, unique
		name = "Modern HUD",               -- the options entry name
		version = "0.1.0",
		settings = Settings object,        -- or a plain table of values plus save = function()
		apply = function() end,            -- after settings change from outside (profile import)
		whats_new = "pmh_info_whats_new_text",
		page = "https://www.nexusmods.com/payday2/mods/..."  -- optional
	})
]]
function C:register(entry)
	if not entry or not entry.id then
		return
	end

	if not self.mods[entry.id] then
		self.order[#self.order + 1] = entry.id
	end

	self.mods[entry.id] = entry
end

function C:registered()
	local out = {}

	for _, id in ipairs(self.order) do
		out[#out + 1] = self.mods[id]
	end

	table.sort(out, function(a, b)
		return tostring(a.name) < tostring(b.name)
	end)

	return out
end

-- The values of a registered mod's settings, whichever shape it keeps them in.
local function values_of(entry)
	local s = entry.settings

	if type(s) == "table" and getmetatable(s) == Settings then
		return s:export(), s
	end

	return type(s) == "table" and s or {}, nil
end

-- ============================================================================
-- Settings profiles: every series mod's settings in one file
-- ============================================================================

C.profile_path = (SavePath or "mods/saves/") .. "copper_profile.json"

function C:export_profile(path)
	local profile = {
		series = self.VERSION,
		shared = self.shared,
		mods = {}
	}

	for _, entry in ipairs(self:registered()) do
		local values = values_of(entry)

		profile.mods[entry.id] = {version = entry.version, settings = values}
	end

	local ok = pcall(io.save_as_json, profile, path or self.profile_path)

	return ok, profile
end

-- Returns how many mods took settings from the profile.
function C:import_profile(path)
	local ok, profile = pcall(function()
		local p = path or self.profile_path

		return io.file_is_readable(p) and io.load_as_json(p)
	end)

	if not ok or type(profile) ~= "table" or type(profile.mods) ~= "table" then
		return 0
	end

	if type(profile.shared) == "table" then
		for k, v in pairs(profile.shared) do
			if self.SHARED_DEFAULTS[k] ~= nil and type(v) == type(self.SHARED_DEFAULTS[k]) then
				self.shared[k] = v
			end
		end

		self:save_shared()
	end

	local n = 0

	for id, data in pairs(profile.mods) do
		local entry = self.mods[id]

		if entry and type(data) == "table" and type(data.settings) == "table" then
			local _, settings_object = values_of(entry)

			if settings_object then
				settings_object:apply(data.settings)
				settings_object:save()
			elseif type(entry.settings) == "table" then
				for k, v in pairs(data.settings) do
					if entry.settings[k] ~= nil and type(entry.settings[k]) == type(v) then
						entry.settings[k] = v
					end
				end

				if entry.save then
					pcall(entry.save)
				end
			end

			if entry.apply then
				pcall(entry.apply)
			end

			n = n + 1
		end
	end

	return n
end

return C
