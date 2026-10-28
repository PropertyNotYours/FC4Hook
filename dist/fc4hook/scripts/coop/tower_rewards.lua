-- Runs the co-op tower reward fix once. The logic lives in autorun\10_coop_towers.lua.
-- TOWER_MODE is set by the Co-op tab: "claim", "mark", "learn", "test" or "inspect".
if type(FC4Coop) ~= "table" or type(FC4Coop.run) ~= "function" then
  ModLog("FC4Coop isn't loaded: check scripts\\autorun\\10_coop_towers.lua, then press Reload autorun")
else
  FC4Coop.run(TOWER_MODE or "claim", false)
end
TOWER_MODE = nil
