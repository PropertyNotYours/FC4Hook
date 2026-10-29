-- FC4Hook fc4.ui demo. Copy this file into lua\autorun\ (or run it from the Console in
-- "Lua 5.4" mode with dofile) and open the F8 menu.
local w = fc4.ui.window("UI demo", { width = 340, height = 260 })
w:text("Your own window inside FC4Hook, made from Lua 5.4.")
w:separator("Player")
local status = w:text("")
w:button("Refill ammo", function()
  game.pcall("RefillPlayerAmmo", game.player())
  status:set("ammo refilled")
end)
w:same_line():button("Launch", function()
  game.pcall("CAPI_Player.SetWorldSpeed", game.player(), { 0, 0, 20 })
end)
w:checkbox("Slow motion", false, function(on)
  game.pcall("SetSlowMotion", on and 0.3 or 1.0)
end)
w:slider("Time of day", 0, 23, 12, function(h)
  game.pcall("SetTimeOfDay", math.floor(h), 0, 0)
end)
w:combo("Weather", { "clear", "storm" }, 1, function(i)
  if i == 2 then game.pcall("StartStorm", 10) else game.pcall("StopStorm", 10) end
end)

-- an overlay window: visible even with the menu closed
local hud = fc4.ui.window("Position", { overlay = true })
local pos = hud:text("")
fc4.every(0.25, function()
  local x, y, z = game.pos()
  if x then pos:set(string.format("x %.0f  y %.0f  height %.0f", x, y, z)) end
end)
