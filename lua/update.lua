--[[
	Payday Screen Shaders
	lua/update.lua - loads the plugin on the first update, keeps its heartbeat going, and keeps
	the corner status text current.

	Loaded for both the menu and the game setup. Paused updates count too: the pause menu
	still shows the world, and a missing heartbeat would switch the shaders off behind it.

	The hooks do not agree on time. GameSetupUpdate passes game time, which stops while the game
	is paused; the paused hook passes a clock that keeps running. Throttling on a mix of the two
	left game time permanently "behind" after the first pause, so the heartbeat stopped for good
	the moment the escape menu closed. One clock, the wall timer, is used for everything.
]]

dofile(ModPath .. "lua/core.lua")
dofile(ModPath .. "lua/overlay.lua")
dofile(ModPath .. "lua/mood.lua")

local PSS = _G.PSS

local function now()
	local ok, t = pcall(function()
		return TimerManager:wall():time()
	end)

	return ok and type(t) == "number" and t or os.clock()
end

local function tick()
	local t = now()

	PSS:guard("status", function()
		PSS:poll_status(t)
	end)

	-- The mood before the heartbeat, so a change goes out with this write rather than the next.
	PSS:guard("mood", function()
		PSS.mood:update(t)
	end)

	-- Cheap, and it catches the player turning one of them back on in the video options.
	PSS:guard("vanilla settings", function()
		PSS:sync_vanilla_settings(t)
	end)

	PSS:guard("update", function()
		PSS:tick(t)
	end)

	PSS:guard("overlay", function()
		PSS.overlay:update(t)
	end)
end

-- The camera is measured as each frame starts rendering: the engine calls update, then end_update
-- (which moves the camera), then render, which runs Setup:pre_render and only then draws the world
-- (core/lib/setups/coresetup.lua). So this is exactly the camera the frame is drawn with, which
-- temporal antialiasing needs to put last frame back in the right place. The jitter is applied there
-- too (PSS:jitter_camera).
Hooks:PostHook(Setup, "pre_render", "PSS_pre_render", function()
	PSS:guard("camera", function()
		PSS:write_camera()
	end)
end)

-- The jitter goes on right after the game's viewport update places the camera (PSS:jitter_after_viewport_update).
-- managers.viewport is made after this file loads, so its update is wrapped on the first frame instead.
local function wrap_viewport_update()
	local viewports = managers and managers.viewport

	if not viewports or viewports._pss_wrapped then
		return
	end

	viewports._pss_wrapped = true
	local update = viewports.update

	viewports.update = function(self, t, dt)
		update(self, t, dt)

		PSS:guard("jitter", function()
			local camera = self:get_current_camera()

			if camera then
				PSS:jitter_after_viewport_update(camera)
			end
		end)
	end
end

Hooks:PostHook(Setup, "pre_render", "PSS_wrap_viewport_update", function()
	PSS:guard("jitter", wrap_viewport_update)
end)

-- Leaving the game. Setup:quit is the last word for every exit the game itself makes; the quit
-- dialog's yes is hooked as well so the settings file is written while the menu is still up.
if Setup and not Setup._pss_quit then
	Setup._pss_quit = true

	Hooks:PreHook(Setup, "quit", "PSS_Setup_quit", function()
		PSS:guard("vanilla settings", function()
			PSS:hand_back_vanilla("quit")
		end)
	end)
end

if MenuCallbackHandler and MenuCallbackHandler._dialog_quit_yes and not MenuCallbackHandler._pss_quit then
	MenuCallbackHandler._pss_quit = true

	Hooks:PreHook(MenuCallbackHandler, "_dialog_quit_yes", "PSS_quit_yes", function()
		PSS:guard("vanilla settings", function()
			PSS:hand_back_vanilla("quit")
		end)
	end)
end

Hooks:Add("MenuUpdate", "PSS_MenuUpdate", tick)
Hooks:Add("GameSetupUpdate", "PSS_GameSetupUpdate", tick)
Hooks:Add("GameSetupPausedUpdate", "PSS_GameSetupPausedUpdate", tick)
