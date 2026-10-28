-- Stand exactly where you'd like to arrive at a bell tower, then run this.
-- It saves that spot for the nearest liberated tower (coop\tower_spawns.txt).
-- Towers without a saved spot use the typical height/distance of the ones you've saved.
if FC4Travel and FC4Travel.saveSpot then FC4Travel.saveSpot()
else ModLog("Tower travel script isn't loaded: Scripts tab -> Reload autorun") end
