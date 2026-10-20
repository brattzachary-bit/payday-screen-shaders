--[[
	Payday Screen Shaders
	lua/menu.lua - options pages, built by hand with MenuHelper (NewMenu / Add* / BuildMenu).

	Laid out by dev/ideas/SERIES_UI.md, like every mod in the series:
	  Enabled, Controls, one page per group of effects, Info, Reset all to defaults.
	Sliders push to the plugin as they move, so every change is visible live; each page saves
	when you leave it. Every feature page ends with Reset this page, which asks first and then
	updates the items on screen.
]]

dofile(ModPath .. "lua/core.lua")

local PSS = _G.PSS
local MAIN = "pss_menu_main"

-- Item kinds: {"toggle", key}, {"slider", key}, {"choice", key, {string ids}}, {"lut"},
-- {"divider"}, {"keybind", id}, {"button", id, callback}.
PSS.PAGES = {
	{
		id = "pss_menu_controls",
		items = {
			{"keybind", "pss_key_toggle"},
			{"keybind", "pss_key_split"},
			{"keybind", "pss_key_capture"}
		}
	},
	{
		id = "pss_menu_detail",
		reset = true,
		items = {
			{"choice", "antialiasing", {"pss_aa_off", "pss_aa_fxaa", "pss_aa_smaa", "pss_aa_smaa_t2x", "pss_aa_taa"}},
			{"slider", "aa_subpix"},
			{"slider", "aa_edge"},
			{"slider", "taa"},
			{"toggle", "taa_jitter"},
			{"divider"},
			{"slider", "sharpen"},
			{"divider"},
			{"toggle", "demoire_on"},
			{"slider", "demoire"},
			{"toggle", "detail_on"},
			{"slider", "detail"},
			{"divider"},
			{"toggle", "upscale"},
			{"slider", "upscale_scale"},
			{"slider", "upscale_sharpness"},
			{"divider"},
			{"slider", "deband"},
			{"slider", "dither"}
		}
	},
	{
		id = "pss_menu_light",
		reset = true,
		items = {
			{"slider", "exposure"},
			{"divider"},
			{"slider", "adapt"},
			{"slider", "adapt_speed"},
			{"slider", "adapt_key"},
			{"slider", "adapt_min"},
			{"slider", "adapt_max"},
			{"divider"},
			{"slider", "bloom"},
			{"slider", "bloom_threshold"},
			{"slider", "bloom_radius"},
			{"divider"},
			{"toggle", "ltm_on"},
			{"slider", "ltm"},
			{"slider", "ltm_detail"}
		}
	},
	{
		id = "pss_menu_colour",
		reset = true,
		items = {
			{"slider", "temperature"},
			{"slider", "tint"},
			{"divider"},
			{"slider", "shadows"},
			{"slider", "highlights"},
			{"slider", "lift"},
			{"slider", "gamma"},
			{"slider", "gain"},
			{"slider", "contrast"},
			{"divider"},
			{"slider", "saturation"},
			{"slider", "vibrance"},
			{"divider"},
			{"lut"},
			{"slider", "lut_mix"}
		}
	},
	{
		id = "pss_menu_depth",
		reset = true,
		items = {
			{"slider", "clarity"},
			{"divider"},
			{"toggle", "obscurance_on"},
			{"slider", "obscurance"},
			{"slider", "obscurance_radius"},
			{"divider"},
			{"toggle", "color_bloom_on"},
			{"slider", "color_bloom"},
			{"slider", "color_bloom_radius"},
			{"divider"},
			{"toggle", "cues_on"},
			{"slider", "cues"},
			{"slider", "cues_separation"},
			{"slider", "cues_depth"},
			{"slider", "cues_focus"}
		}
	},
	{
		id = "pss_menu_geometry",
		reset = true,
		items = {
			{"toggle", "depth_view"},
			{"choice", "depth_mode", {"pss_depth_auto", "pss_depth_linear", "pss_depth_projected", "pss_depth_reversed"}},
			{"slider", "depth_near"},
			{"slider", "depth_far"},
			{"divider"},
			{"toggle", "ao_on"},
			{"slider", "ao"},
			{"slider", "ao_radius"},
			{"divider"},
			{"toggle", "dof_on"},
			{"slider", "dof"},
			{"slider", "dof_aim"},
			{"slider", "dof_focus"},
			{"slider", "dof_range"},
			{"slider", "dof_near"},
			{"slider", "dof_speed"},
			{"toggle", "dof_weapon"}
		}
	},
	{
		id = "pss_menu_lighting",
		reset = true,
		items = {
			{"toggle", "il_on"},
			{"slider", "il"},
			{"slider", "il_radius"},
			{"divider"},
			{"toggle", "rtgi_on"},
			{"slider", "rtgi"},
			{"slider", "rtgi_distance"},
			{"divider"},
			{"toggle", "ssr_on"},
			{"slider", "ssr"},
			{"slider", "ssr_blur"},
			{"divider"},
			{"toggle", "temporal"}
		}
	},
	{
		id = "pss_menu_atmosphere",
		reset = true,
		items = {
			{"toggle", "fog_on"},
			{"slider", "fog"},
			{"slider", "fog_start"},
			{"slider", "fog_tint"},
			{"divider"},
			{"toggle", "hfog_on"},
			{"slider", "hfog"},
			{"slider", "hfog_height"},
			{"slider", "fog_glow"},
			{"divider"},
			{"toggle", "shafts_on"},
			{"slider", "shafts"},
			{"slider", "shafts_length"}
		}
	},
	{
		id = "pss_menu_lens",
		reset = true,
		items = {
			{"slider", "vignette"},
			{"slider", "chromatic"},
			{"slider", "grain"},
			{"divider"},
			{"toggle", "panini_on"},
			{"slider", "panini"},
			{"toggle", "spherical_on"},
			{"slider", "spherical"},
			{"divider"},
			{"toggle", "mood_hurt"},
			{"toggle", "mood_assault"},
			{"divider"},
			{"toggle", "dirt_on"},
			{"slider", "dirt"},
			{"slider", "dirt_offset"},
			{"divider"},
			{"toggle", "halation_on"},
			{"slider", "halation"},
			{"slider", "halation_threshold"},
			{"slider", "halation_tint"},
			{"slider", "halation_radius"},
			{"divider"},
			{"toggle", "surface_grain_on"},
			{"slider", "surface_grain"},
			{"slider", "surface_grain_size"},
			{"divider"},
			{"toggle", "mblur_on"},
			{"slider", "mblur"}
		}
	},
	{
		id = "pss_menu_tools",
		reset = true,
		items = {
			{"toggle", "split"},
			{"divider"},
			{"button", "pss_reload", "pss_reload"},
			{"button", "pss_status", "pss_status"},
			{"divider"},
			{"choice", "debug_view", {"pss_debug_off", "pss_debug_bloom", "pss_debug_exposure", "pss_debug_ao", "pss_debug_il", "pss_debug_rtgi", "pss_debug_ssr"}},
			{"toggle", "motion_view"},
			{"divider"},
			{"toggle", "vanilla_off"},
			{"button", "pss_vanilla_restore", "pss_vanilla_restore"},
			{"toggle", "aside_workshop"},
			{"slider", "inject_after"},
			{"toggle", "require_fullscreen"}
		}
	},
	{
		id = "pss_menu_info",
		bottom = true,
		items = {
			{"button", "pss_info_what", "pss_info_what"},
			{"button", "pss_info_others", "pss_info_others"},
			{"button", "pss_info_new", "pss_info_new"},
			{"divider"},
			{"button", "pss_info_credits", "pss_info_credits"}
		}
	}
}

PSS.PAGE_BY_ID = {}

for _, page in ipairs(PSS.PAGES) do
	PSS.PAGE_BY_ID[page.id] = page
end

-- The settings a page owns, for Reset this page.
function PSS:page_keys(page)
	local keys = {}

	for _, item in ipairs(page.items) do
		local kind = item[1]

		if kind == "toggle" or kind == "slider" or kind == "choice" then
			table.insert(keys, item[2])
		elseif kind == "lut" then
			table.insert(keys, "lut")
		end
	end

	return keys
end

-- Puts the current values into the built menu items, so a reset shows at once.
function PSS:sync_menu_items()
	for _, node in pairs(self._nodes or {}) do
		if node and node.item then
			for _, param in ipairs(self.PARAMS) do
				local item = node:item(param.key)
				local v = self.settings[param.key]

				if item and item.set_value then
					if param.kind == "toggle" then
						item:set_value(v and "on" or "off")
					else
						item:set_value(v)
					end
				end
			end
		end
	end

	local menus = rawget(_G, "managers") and managers.menu
	local active = menus and menus.active_menu and menus:active_menu()

	if active and active.logic and active.logic.refresh_node then
		pcall(active.logic.refresh_node, active.logic)
	end
end

local function dialog(title_id, text_id)
	QuickMenu:new(PSS:loc(title_id, title_id), PSS:loc(text_id, text_id), {
		{text = PSS:loc("dialog_ok", "OK"), is_cancel_button = true}
	}, true)
end

local function confirm(title_id, text_id, on_reset)
	QuickMenu:new(PSS:loc(title_id, title_id), PSS:loc(text_id, text_id), {
		{text = PSS:loc("pss_dialog_reset", "Reset"), callback = on_reset},
		{text = PSS:loc("pss_dialog_cancel", "Cancel"), is_cancel_button = true}
	}, true)
end

Hooks:Add("LocalizationManagerPostInit", "PSS_LocalizationManagerPostInit", function(loc)
	loc:load_localization_file(PSS.loc_path)

	-- LUT names come from the files in luts/, so their strings are made here.
	local strings = {}

	for i, lut in ipairs(PSS:luts()) do
		strings["pss_lut_" .. i] = lut.name
	end

	loc:add_localized_strings(strings)
end)

Hooks:Add("MenuManagerInitialize", "PSS_MenuManagerInitialize", function()
	MenuCallbackHandler.pss_toggle = function(self, item)
		PSS:set(item:name(), item:value() == "on")
		PSS:save()
	end

	-- A slider sends every value in between while it is dragged. Land on its own steps, so the FSR
	-- and render scale buffers are not rebuilt at 1.63x, 1.61x, ... on the way from 2x to 1.5x.
	MenuCallbackHandler.pss_slider = function(self, item)
		local p = PSS.BY_KEY[item:name()]
		local v = tonumber(item:value())

		if p and v and p.step then
			v = p.min + math.floor((v - p.min) / p.step + 0.5) * p.step
		end

		PSS:set(item:name(), v)
	end

	MenuCallbackHandler.pss_choice = function(self, item)
		PSS:set(item:name(), tonumber(item:value()))
		PSS:save()
	end

	MenuCallbackHandler.pss_save = function()
		PSS:save()
	end

	MenuCallbackHandler.pss_vanilla_restore = function()
		local put_back = PSS:restore_vanilla_settings()
		local text = #put_back > 0 and (PSS:loc("pss_vanilla_restore_done", "Put back:") .. " " .. table.concat(put_back, ", "))
			or PSS:loc("pss_vanilla_restore_none", "Nothing needed putting back.")

		PSS:save()
		QuickMenu:new(PSS:loc("pss_vanilla_restore_title", "PAYDAY 2's own settings"), text, {
			{text = PSS:loc("dialog_ok", "OK"), is_cancel_button = true}
		}, true)
	end

	MenuCallbackHandler.pss_reload = function()
		PSS:reload()
	end

	MenuCallbackHandler.pss_status = function()
		QuickMenu:new(PSS:loc("pss_status_title", "Plugin status"), PSS:status_text(), {
			{text = PSS:loc("dialog_ok", "OK"), is_cancel_button = true}
		}, true)
	end

	MenuCallbackHandler.pss_info_what = function()
		dialog("pss_info_what_title", "pss_info_what_text")
	end

	MenuCallbackHandler.pss_info_others = function()
		dialog("pss_info_others_title", "pss_info_others_text")
	end

	MenuCallbackHandler.pss_info_new = function()
		dialog("pss_info_new_title", "pss_info_new_text")
	end

	MenuCallbackHandler.pss_info_credits = function()
		dialog("pss_info_credits_title", "pss_info_credits_text")
	end

	-- Reset this page: the button's name is "pss_reset_page_" .. page id.
	MenuCallbackHandler.pss_reset_page = function(self, item)
		local page_id = item and item.name and string.gsub(item:name(), "^pss_reset_page_", "")
		local page = PSS.PAGE_BY_ID[page_id]

		if not page then
			return
		end

		confirm("pss_dialog_reset_page_title", "pss_dialog_reset_page_text", function()
			PSS:reset_keys(PSS:page_keys(page))
			PSS:sync_menu_items()
		end)
	end

	-- A preset overwrites everything, so it asks first, like Reset all does.
	for _, preset in ipairs(PSS.PRESETS) do
		MenuCallbackHandler["pss_preset_" .. preset.id] = function()
			confirm("pss_preset_" .. preset.id .. "_title", "pss_dialog_preset_text", function()
				PSS:apply_preset(preset.id)
				PSS:push_all()
				PSS:save()
				PSS:sync_menu_items()
			end)
		end
	end

	MenuCallbackHandler.pss_reset_all = function()
		confirm("pss_dialog_reset_all_title", "pss_dialog_reset_all_text", function()
			PSS:reset_defaults()
			PSS:sanitize()
			PSS:push_all()
			PSS:save()
			PSS:sync_menu_items()
		end)
	end

	-- Keybinds work in the menu and in a heist.
	MenuCallbackHandler.pss_key_toggle = function()
		PSS:toggle("enabled")
		PSS:save()
	end

	MenuCallbackHandler.pss_key_split = function()
		PSS:toggle("split")
	end

	MenuCallbackHandler.pss_key_capture = function()
		PSS:capture()
	end
end)

Hooks:Add("MenuManagerSetupCustomMenus", "PSS_setup_menu", function()
	PSS:guard("options page", function()
		MenuHelper:NewMenu(MAIN)

		for _, page in ipairs(PSS.PAGES) do
			MenuHelper:NewMenu(page.id)
		end
	end)
end)

Hooks:Add("MenuManagerPopulateCustomMenus", "PSS_populate_menu", function()
	PSS:guard("options page", function()
		local s = PSS.settings
		local priority = 1000

		local function next_priority()
			priority = priority - 1

			return priority
		end

		local function strings(key)
			return "pss_" .. key .. "_title", "pss_" .. key .. "_desc"
		end

		local builders = {}

		function builders.toggle(menu_id, key)
			local t, d = strings(key)

			MenuHelper:AddToggle({id = key, title = t, desc = d, callback = "pss_toggle", value = s[key],
				menu_id = menu_id, priority = next_priority(), localized = true})
		end

		function builders.slider(menu_id, key)
			local t, d = strings(key)
			local p = PSS.BY_KEY[key]

			MenuHelper:AddSlider({id = key, title = t, desc = d, callback = "pss_slider", value = s[key],
				min = p.min, max = p.max, step = p.step, show_value = true,
				menu_id = menu_id, priority = next_priority(), localized = true})
		end

		function builders.choice(menu_id, key, items)
			local t, d = strings(key)
			local values = {}

			for i = 1, #items do
				values[i] = i
			end

			MenuHelper:AddMultipleChoice({id = key, title = t, desc = d, callback = "pss_choice",
				value = math.clamp(s[key], 1, #items), items = items, item_values = values,
				menu_id = menu_id, priority = next_priority(), localized = true})
		end

		function builders.lut(menu_id)
			local items = {}

			for i = 1, #PSS:luts() do
				items[i] = "pss_lut_" .. i
			end

			builders.choice(menu_id, "lut", items)
		end

		function builders.divider(menu_id, name)
			MenuHelper:AddDivider({id = name or ("pss_divider_" .. menu_id .. "_" .. priority), size = 16,
				menu_id = menu_id, priority = next_priority()})
		end

		function builders.button(menu_id, id, callback)
			MenuHelper:AddButton({id = id, title = id .. "_title", desc = id .. "_desc", callback = callback,
				menu_id = menu_id, priority = next_priority(), localized = true})
		end

		function builders.keybind(menu_id, bind_id)
			local mod = BLT and BLT.Mods and BLT.Mods.GetModOwnerOfFile and BLT.Mods:GetModOwnerOfFile(PSS.mod_path .. "mod.txt")

			if mod and BLT.Keybinds then
				BLT.Keybinds:register_keybind(mod, {
					id = bind_id,
					allow_menu = true,
					allow_game = true,
					show_in_menu = false,
					name = bind_id .. "_title",
					desc = bind_id .. "_desc",
					localize = true,
					callback = MenuCallbackHandler[bind_id]
				})
			end

			local bind = BLT and BLT.Keybinds and BLT.Keybinds.get_keybind and BLT.Keybinds:get_keybind(bind_id)
			local key = bind and bind:Key() or ""

			MenuHelper:AddKeybinding({id = bind_id, title = bind_id .. "_title", desc = bind_id .. "_desc",
				connection_name = bind_id, button = key, binding = key,
				menu_id = menu_id, priority = next_priority(), localized = true})
		end

		-- Top page: the page entries are slotted in between these when the menus are built.
		builders.toggle(MAIN, "enabled")
		builders.divider(MAIN, "pss_divider_presets")

		for _, preset in ipairs(PSS.PRESETS) do
			builders.button(MAIN, "pss_preset_" .. preset.id, "pss_preset_" .. preset.id)
		end

		builders.divider(MAIN, "pss_divider_top")
		builders.divider(MAIN, "pss_divider_bottom")
		builders.button(MAIN, "pss_reset_all", "pss_reset_all")

		for _, page in ipairs(PSS.PAGES) do
			priority = 1000

			for _, item in ipairs(page.items) do
				builders[item[1]](page.id, item[2], item[3])
			end

			if page.reset then
				builders.divider(page.id)
				MenuHelper:AddButton({id = "pss_reset_page_" .. page.id, title = "pss_reset_page_title", desc = "pss_reset_page_desc",
					callback = "pss_reset_page", menu_id = page.id, priority = next_priority(), localized = true})
			end
		end
	end)
end)

Hooks:Add("MenuManagerBuildCustomMenus", "PSS_build_menu", function(menu_manager, nodes)
	PSS:guard("options page", function()
		PSS._nodes = {}

		for _, page in ipairs(PSS.PAGES) do
			nodes[page.id] = MenuHelper:BuildMenu(page.id, {back_callback = "pss_save"})
			PSS._nodes[page.id] = nodes[page.id]
		end

		nodes[MAIN] = MenuHelper:BuildMenu(MAIN, {back_callback = "pss_save"})
		PSS._nodes[MAIN] = nodes[MAIN]

		-- Feature pages go after the top divider, one after another; Info goes after the bottom one.
		local previous = "pss_divider_top"

		for _, page in ipairs(PSS.PAGES) do
			local after = page.bottom and "pss_divider_bottom" or previous

			MenuHelper:AddMenuItem(nodes[MAIN], page.id, page.id .. "_title", page.id .. "_desc", after, "after")

			if not page.bottom then
				previous = page.id
			end
		end

		MenuHelper:AddMenuItem(nodes.blt_options, MAIN, "pss_menu_title", "pss_menu_desc")

		-- MenuHelper clones every page from the "video" node, so they all share that node name,
		-- and binding a key on a nested page then crashes the renderer. Give each page its own.
		for id, node in pairs(PSS._nodes) do
			if node and node._parameters then
				node._parameters.name = id
			end
		end
	end)
end)
