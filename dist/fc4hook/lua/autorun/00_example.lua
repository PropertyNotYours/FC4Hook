-- Example FC4Hook Lua 5.4 script. Delete or edit freely; see ..\README.txt for the API.

fc4.menu.button("Log my health and position", function()
  if not game.ready() then fc4.warn("load into the world first") return end
  local me = game.player()
  local hp = game.CAPI_Pawn.GetPawnHealth(me)
  local x, y, z = game.pos(me)
  fc4.log(string.format("health %.0f at %.1f, %.1f, %.1f   pawn object %s", hp, x, y, z, mem.hex(mem.player() or 0)))
end, "Examples")

fc4.menu.button("Print FC64.dll base", function()
  local base, size = mem.module("FC64.dll")
  fc4.log("FC64.dll at " .. mem.hex(base or 0) .. ", " .. (size or 0) // 1024 // 1024 .. " MB")
end, "Examples")
