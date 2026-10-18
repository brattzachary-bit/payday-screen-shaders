--[[
	Payday Screen Shaders
	lua/overlay.lua - small status text at the top of the screen, in the series style
	(dev/ideas/SERIES_UI.md): ivory text, amber for the one thing that needs attention, the game's
	small font at its own size, a 1 px shadow, 16 px inside the safe area, fading in over 0.15 s
	and out over 0.25 s with a 4 px slide. Nothing loops.

	  * top centre, while split view is on: SPLIT VIEW, with ORIGINAL | SHADERS either side of
	    the split line
	  * top right: SHADERS OFF for as long as they are off, SHADERS ON for a few seconds after
	    they come back on

	Drawn as GUI, which the game draws after the world, so the shaders never touch it. The font
	is checked with PackageManager first: text in a font that is not loaded is an engine crash
	pcall cannot catch.
]]

dofile(ModPath .. "lua/core.lua")

local PSS = _G.PSS

PSS.overlay = PSS.overlay or {}

local O = PSS.overlay

O.NOTICE_SECONDS = 3
O.MARGIN = 16
O.LINE_GAP = 4
O.FADE_IN = 0.15
O.FADE_OUT = 0.25
O.SLIDE = 4

function O:font()
	local menu = tweak_data and tweak_data.menu
	local font = menu and menu.pd2_small_font

	if not font or not PackageManager or not PackageManager.has then
		return nil
	end

	if not PackageManager:has(Idstring("font"), Idstring(font)) then
		return nil
	end

	return font, menu.pd2_small_font_size or 20
end

function O:ensure()
	if self._panel and alive(self._panel) then
		return true
	end

	local gui = managers and managers.gui_data
	local font, size

	if gui and gui.create_fullscreen_workspace then
		font, size = self:font()
	end

	if not font then
		return false
	end

	local ivory = Color(0.93, 0.90, 0.84)
	local amber = Color(0.95, 0.68, 0.25)
	local workspace = gui:create_fullscreen_workspace()
	local panel = workspace:panel():panel({name = "pss_overlay", layer = 2000})

	-- The safe area inside the full screen, so the text lines up with the HUD's own edges.
	local safe = gui.safe_scaled_size and gui:safe_scaled_size() or nil
	local left = (safe and safe.x or 0) + self.MARGIN
	local top = (safe and safe.y or 0) + self.MARGIN
	local width = (safe and safe.w or panel:w()) - self.MARGIN * 2
	local centre = left + width / 2

	local function label(name, text, color, align, x, y, w)
		local params = {
			text = text,
			font = font,
			font_size = size,
			align = align,
			vertical = "top",
			w = w,
			h = size + 4,
			alpha = 0,
			visible = false
		}

		params.name = name .. "_shadow"
		params.color = Color(0, 0, 0)
		params.x, params.y, params.layer = x + 1, y + 1, 0

		local shadow = panel:text(params)

		params.name = name
		params.color = color
		params.x, params.y, params.layer = x, y, 1

		return {main = panel:text(params), shadow = shadow, y = y, alpha = 0, target = 0}
	end

	local second = top + size + self.LINE_GAP

	self._labels = {
		split = label("pss_split", PSS:loc("pss_overlay_split", "SPLIT VIEW"), amber, "center", left, top, width),
		original = label("pss_original", PSS:loc("pss_overlay_original", "ORIGINAL"), ivory, "right", left, second, centre - left - 16),
		shaders = label("pss_shaders", PSS:loc("pss_overlay_shaders", "SHADERS"), ivory, "left", centre + 16, second, left + width - centre - 16),
		state = label("pss_state", "", ivory, "right", left, top, width)
	}

	self._colors = {ivory = ivory, amber = amber}
	self._shown_state = nil
	self._workspace = workspace
	self._panel = panel

	return true
end

-- Called by PSS:set, so a hotkey or menu change shows at once.
function O:changed(key)
	if key == "enabled" and PSS.settings.enabled then
		self._enabled_at = self._now or 0
	end
end

function O:set_text(label, text, color)
	label.main:set_text(text)
	label.shadow:set_text(text)

	if color then
		label.main:set_color(color)
	end
end

-- Eases one label towards its target and places it: fully shown it sits at its line, fading it
-- slides the last 4 px.
function O:animate(label, dt)
	if label.alpha ~= label.target then
		local duration = label.target > label.alpha and self.FADE_IN or self.FADE_OUT
		local step = dt / duration

		if label.target > label.alpha then
			label.alpha = math.min(label.target, label.alpha + step)
		else
			label.alpha = math.max(label.target, label.alpha - step)
		end
	end

	local shown = label.alpha > 0.001
	local offset = (1 - label.alpha) * self.SLIDE

	for _, o in ipairs({label.main, label.shadow}) do
		o:set_visible(shown)

		if o.set_alpha then
			o:set_alpha(o == label.shadow and label.alpha * 0.5 or label.alpha)
		end
	end

	if label.main.set_y then
		label.main:set_y(label.y - offset)
		label.shadow:set_y(label.y - offset + 1)
	end
end

-- Called every update.
function O:update(t)
	local dt = self._now and math.clamp(t - self._now, 0, 0.5) or 0

	self._now = t

	if not self:ensure() then
		return
	end

	local s = PSS.settings
	local L = self._labels
	local split = s.enabled == true and s.split == true

	L.split.target = split and 1 or 0
	L.original.target = L.split.target
	L.shaders.target = L.split.target

	local state

	if not s.enabled then
		state = "disabled"
	elseif self._enabled_at and t - self._enabled_at < self.NOTICE_SECONDS then
		state = "enabled"
	end

	if state ~= self._shown_state then
		self._shown_state = state

		if state == "disabled" then
			self:set_text(L.state, PSS:loc("pss_overlay_disabled", "SHADERS OFF"), self._colors.amber)
		elseif state == "enabled" then
			self:set_text(L.state, PSS:loc("pss_overlay_enabled", "SHADERS ON"), self._colors.ivory)
		end
	end

	L.state.target = state and 1 or 0

	for _, label in pairs(L) do
		self:animate(label, dt)
	end
end
