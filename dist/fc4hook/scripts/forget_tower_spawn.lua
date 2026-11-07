-- Removes the saved arrival spot of the nearest liberated tower.
if FC4Travel and FC4Travel.forgetSpot then FC4Travel.forgetSpot()
else ModLog("Tower travel script isn't loaded: Developer > Lua -> Reload autorun") end
