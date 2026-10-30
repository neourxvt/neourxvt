-- Example config, to be put in ~/.config/nrxvt/init.lua
local opt = nrxvt.opt
opt.font = "xft:DejaVu Sans Mono:size=11,xft:Fixed:size=11,xft:Symbola:size 11"

-- Keep 50,000 lines of scrollback and disable X input methods.
opt.saveLines = 50000
opt.skipXIM = true

-- Copy and paste CLIPBOARD through native Lua callbacks.
local api = nrxvt.api
api.bind("Control-Shift-C", function(t)
	local text = api.selection(t)
	if text and #text > 0 then
		api.set_selection(t, text, true)
	end
end)
api.bind("Control-Shift-V", function(t)
	api.selection_request(t, true)
end)

-- Base16 Kokonai colors, including the extended palette entries.
opt.foreground = "#f8f8f2"
opt.background = "#272822"
opt.cursorColor = "#f8f8f2"
opt.color0 = "#272822"
opt.color1 = "#ff4535"
opt.color2 = "#a6e22e"
opt.color3 = "#fdd102"
opt.color4 = "#33b9f3"
opt.color5 = "#ae81ff"
opt.color6 = "#73e6e6"
opt.color7 = "#f8f8f2"
opt.color8 = "#99947b"
opt.color9 = "#ff4535"
opt.color10 = "#a6e22e"
opt.color11 = "#fdd102"
opt.color12 = "#33b9f3"
opt.color13 = "#ae81ff"
opt.color14 = "#73e6e6"
opt.color15 = "#f9f8f5"
opt.color16 = "#fd971f"
opt.color17 = "#f355a1"
opt.color18 = "#383830"
opt.color19 = "#49483e"
opt.color20 = "#a59f85"
opt.color21 = "#f5f4f1"
