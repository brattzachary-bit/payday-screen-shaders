--[[
	Payday Screen Shaders
	lua/mood.lua - the picture answering to how the heist is going.

	Two moods, both optional and both off your own state only:

	  hurt      badly hurt, and then on the floor: the edges close in, the colour drains, the
	            picture darkens a little. It lifts as you heal.
	  assault   stealth reads a shade cooler and quieter; once it is loud, warmer and harder.

	Neither is new machinery: they lean on the grade's own knobs (vignette, saturation, exposure,
	chromatic, temperature, contrast) by lending them an offset for as long as the mood lasts.
	Your saved settings are never touched - the offset is added on the way to the plugin and eased
	in and out, so switching a mood off puts the picture straight back.

	Nothing here changes what anyone else sees, and nothing here shows you anything: your health
	is on your own HUD already, and the assault is on everyone's screen.
]]

local PSS = _G.PSS

PSS.mood = PSS.mood or {}

local M = PSS.mood

M.EASE = 1.4              -- seconds to travel the whole way in or out
M.STEP = 0.01             -- a change smaller than this is not worth a write
M.SNAP = 0.02             -- this close to where it is going, it is there

-- Each mood's pull on the grade, at full strength.
M.HURT = {
	vignette = 0.26,
	saturation = -0.34,
	exposure = -0.16,
	chromatic = 0.18
}

M.DOWNED = {
	vignette = 0.42,
	saturation = -0.62,
	exposure = -0.3,
	chromatic = 0.3
}

M.STEALTH = {
	temperature = -0.08,
	saturation = -0.08,
	contrast = 0.02
}

M.LOUD = {
	temperature = 0.1,
	saturation = 0.04,
	contrast = 0.06
}

M.value = M.value or {}

function M.number(v)
	if type(v) == "number" then
		return v
	end

	local ok, n = pcall(function()
		return Application:digest_value(v, false)
	end)

	return ok and tonumber(n) or nil
end

function M.damage()
	local ok, ext = pcall(function()
		local unit = managers.player and managers.player:player_unit()

		return unit and alive(unit) and unit:character_damage() or nil
	end)

	return ok and ext or nil
end

-- 0 while you are fine, up to 1 as you near the floor; "downed" while you are on it.
function M:hurt_state()
	local damage = self.damage()

	if not damage then
		return 0, false
	end

	local downed = false

	pcall(function()
		downed = damage._bleed_out and not damage._dead and true or false
	end)

	if downed then
		return 1, true
	end

	local share = nil

	pcall(function()
		local health = self.number(damage.get_real_health and damage:get_real_health())
		local max = self.number(damage._max_health and damage:_max_health())

		if health and max and max > 0 then
			share = math.max(math.min(health / max, 1), 0)
		end
	end)

	local low = 0.4

	if not share or share >= low then
		return 0, false
	end

	return 1 - share / low, false
end

function M.stealth()
	local ok, whisper = pcall(function()
		return managers.groupai:state():whisper_mode()
	end)

	if not ok or whisper == nil then
		return nil
	end

	return whisper and true or false
end

function M:in_heist()
	local gsm = rawget(_G, "game_state_machine")

	if not gsm then
		return false
	end

	local ok, name = pcall(gsm.current_state_name, gsm)

	return ok and type(name) == "string" and name:find("ingame", 1, true) ~= nil
end

-- Where every knob this file touches should end up, right now.
function M:targets()
	local want = {}

	if not self:in_heist() then
		return want
	end

	if PSS.settings.mood_hurt then
		local strength, downed = self:hurt_state()
		local pull = downed and self.DOWNED or self.HURT

		for key, amount in pairs(pull) do
			want[key] = (want[key] or 0) + amount * strength
		end
	end

	if PSS.settings.mood_assault then
		local stealth = self.stealth()

		if stealth ~= nil then
			for key, amount in pairs(stealth and self.STEALTH or self.LOUD) do
				want[key] = (want[key] or 0) + amount
			end
		end
	end

	return want
end

-- Eases what is being sent towards what the mood wants, and reports whether anything moved.
function M:ease(dt)
	local want = self:targets()
	local moved = false
	local step = math.min(math.max((dt or 0) / self.EASE, 0), 1)

	for key in pairs(self.HURT) do
		want[key] = want[key] or 0
	end

	for key in pairs(self.LOUD) do
		want[key] = want[key] or 0
	end

	for key, target in pairs(want) do
		local now = self.value[key] or 0
		local next_value = now + (target - now) * step

		if math.abs(target - next_value) < self.SNAP then
			next_value = target
		end

		if math.abs(next_value - now) >= self.STEP / 4 then
			moved = true
		end

		self.value[key] = next_value
	end

	return moved
end

function M:offsets()
	local out = {}

	for key, value in pairs(self.value) do
		if math.abs(value) >= self.STEP then
			out[key] = value
		end
	end

	return out
end

function M:update(t)
	local dt = self._t and (t - self._t) or 0

	self._t = t

	if dt < 0 or dt > 1 then
		dt = 0.1
	end

	local moved = self:ease(dt)

	PSS._offsets = self:offsets()

	-- A mood on the move is written at once; the heartbeat would only catch up half a second later.
	if moved then
		PSS._next = nil
	end

	return moved
end
