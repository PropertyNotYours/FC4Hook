-- FC4Hook Cheats (F8 > Cheats). All of these call the game's own Lua functions.
--
-- Switches (god mode, unlimited ammo, slow motion, time lock) stay on until you turn
-- them off: they're re-applied every few seconds (the game resets some of them after
-- deaths, cutscenes and loads) and, with "Remember switches" on, after a restart too.
-- Saved in fc4hook\cheats.txt; saved positions in fc4hook\positions.txt.

FC4Cheats = FC4Cheats or {}
local C = FC4Cheats
C.file = "cheats.txt"
C.posFile = "positions.txt"
C.reassertEvery = 4      -- seconds between re-applying active switches
C.nextAssert = 0
C.slots = 5
-- every on/off switch (45_cheats_plus.lua adds more); saved as on_<key> in cheats.txt
C.keys = { "god", "ammo", "slowmo", "timelock" }

-- natives each feature needs (the menu hides features whose native is missing)
C.natives = {
  "EnableGodMode", "EnableUnlimitedAmmo", "RefillPlayerAmmo", "SetSlowMotion",
  "SetTimeOfDay", "GetTimeOfDay", "StartStorm", "StopStorm", "SetScriptedStormFactorOverride",
  "RemoveScriptedStormFactorOverride", "SetBlizzardActive", "UnlockAllSafeHouses",
  "SetShopItemsUnlockAll", "SetCraftingUnlockAll", "SetFastTravelEnabled",
  "GetNumberOfPlayers", "GetLocalPlayerId",
}

-- ------------------------------------------------------------ settings
function C.load()
  C.cfg = FC4.loadKV(C.file)
  C.cfg.remember = C.cfg.remember or "1"
  C.cfg.slow = C.cfg.slow or "0.3"
  C.cfg.hour = C.cfg.hour or "12"
  C.cfg.minute = C.cfg.minute or "0"
  C.on = C.on or {}
  if C.cfg.remember == "1" then
    for _, k in ipairs(C.keys) do
      if C.cfg["on_" .. k] == "1" then C.on[k] = true end
    end
  end
end

function C.save()
  for _, k in ipairs(C.keys) do
    C.cfg["on_" .. k] = C.on[k] and "1" or "0"
  end
  FC4.saveKV(C.file, C.cfg)
end

function C.publish()
  for _, k in ipairs(C.keys) do
    ModSetState("cheat." .. k, C.on[k] and "1" or "0")
  end
  ModSetState("cheat.remember", C.cfg.remember)
  ModSetState("cheat.slow", C.cfg.slow)
  ModSetState("cheat.hour", C.cfg.hour)
  ModSetState("cheat.minute", C.cfg.minute)
end

function C.publishAvail()
  for _, n in ipairs(C.natives) do
    ModSetState("avail." .. n, FC4.has(n) and "1" or "0")
  end
  local tp = FC4.has("TeleportEntityPos") or FC4.has("ModTeleportEntityPos")
  local gp = FC4.has("GetEntityPositionVec3") or FC4.has("ModGetEntityPositionVec3")
  ModSetState("avail.teleport", (tp and gp) and "1" or "0")
end

function C.isOn(k)
  return C.on ~= nil and C.on[k] == true
end

-- ------------------------------------------------------------ applying switches
function C.applyGod(v)
  if v then return FC4.call("EnableGodMode", 1) end
  return FC4.call("EnableGodMode", 0)
end

function C.applyAmmo(v)
  return FC4.call("EnableUnlimitedAmmo", v == true)
end

function C.applySlow(v)
  if v then return FC4.call("SetSlowMotion", tonumber(C.cfg.slow) or 0.3) end
  return FC4.call("SetSlowMotion", 1.0)
end

function C.applyTime()
  return FC4.call("SetTimeOfDay", tonumber(C.cfg.hour) or 12, tonumber(C.cfg.minute) or 0, 0)
end

C.appliers = { god = C.applyGod, ammo = C.applyAmmo, slowmo = C.applySlow }

function C.apply(k, v)
  if k == "timelock" then
    if v then return C.applyTime() end
    return true
  end
  local f = C.appliers[k]
  if f then return f(v) end
  return false
end

C.labels = { god = "God mode", ammo = "Unlimited ammo", slowmo = "Slow motion", timelock = "Time lock" }

function C.set(k, v)
  C.on[k] = (v == true)
  local ok = C.apply(k, C.on[k])
  ModLog("Cheats: " .. tostring(C.labels[k] or k) .. (C.on[k] and " ON" or " OFF") .. (ok and "" or " (the game refused, see log)"))
  C.save()
  C.publish()
end

function C.toggle(k)
  C.set(k, not C.isOn(k))
end

-- ------------------------------------------------------------ one-shot actions
function C.refill()
  local me = FC4.me()
  if me == nil then return end
  if FC4.call("RefillPlayerAmmo", me) then ModLog("Cheats: ammo refilled") end
end

function C.setSlow(f)
  C.cfg.slow = string.format("%.2f", tonumber(f) or 0.3)
  if C.isOn("slowmo") then C.applySlow(true) end
  C.save()
  C.publish()
end

function C.setTime(h, m)
  C.cfg.hour = string.format("%d", tonumber(h) or 12)
  C.cfg.minute = string.format("%d", tonumber(m) or 0)
  if C.applyTime() then ModLog("Cheats: time set to " .. C.cfg.hour .. ":" .. string.format("%02d", tonumber(C.cfg.minute))) end
  C.save()
  C.publish()
end

C.simple = {
  safehouses = { "UnlockAllSafeHouses", "all safe houses unlocked" },
  shop       = { "SetShopItemsUnlockAll", "all shop items unlocked", true },
  crafting   = { "SetCraftingUnlockAll", "all crafting unlocked", true },
  fasttravel = { "SetFastTravelEnabled", "fast travel enabled", true },
  storm      = { "StartStorm", "storm rolling in", 10 },
  calm       = { "StopStorm", "storm clearing", 10 },
  blizzard   = { "SetBlizzardActive", "blizzard on", true },
  noblizzard = { "SetBlizzardActive", "blizzard off", false },
  clearstorm = { "RemoveScriptedStormFactorOverride", "weather override cleared", 10 },
}

function C.action(name)
  if name == "refill" then return C.refill() end
  if name == "fullstorm" then
    if FC4.call("SetScriptedStormFactorOverride", 1, 10, 1) then ModLog("Cheats: full storm") end
    return
  end
  local a = C.simple[name]
  if not a then ModLogWarn("Cheats: unknown action " .. tostring(name)) return end
  local ok
  if a[3] == nil then ok = FC4.call(a[1]) else ok = FC4.call(a[1], a[3]) end
  if ok then ModLog("Cheats: " .. a[2]) end
end

-- ------------------------------------------------------------ positions
function C.pos()
  local f = FC4.fn("GetEntityPositionVec3") or FC4.fn("ModGetEntityPositionVec3")
  local me = FC4.me()
  if not f or me == nil then return nil end
  local ok, p = pcall(f, me)
  if ok and type(p) == "table" and tonumber(p[1]) then return p[1], p[2], p[3] end
  return nil
end

function C.teleport(x, y, z)
  local f = FC4.fn("TeleportEntityPos") or FC4.fn("ModTeleportEntityPos")
  local me = FC4.me()
  if not f or me == nil then ModLogWarn("Cheats: teleport isn't available") return false end
  local ok, err = pcall(f, me, { x, y, z }, false)
  if not ok then ModLogWarn("Cheats: teleport failed: " .. tostring(err)) end
  return ok
end

function C.loadPositions()
  C.positions = FC4.loadKV(C.posFile)
  for i = 1, C.slots do
    ModSetState("pos." .. i, C.positions["slot" .. i] or "")
  end
end

function C.savePos(i)
  local x, y, z = C.pos()
  if not x then ModLogWarn("Cheats: can't read your position (load into the world first)") return end
  C.positions["slot" .. i] = string.format("%.2f,%.2f,%.2f", x, y, z)
  FC4.saveKV(C.posFile, C.positions)
  ModSetState("pos." .. i, C.positions["slot" .. i])
  ModLog(string.format("Cheats: saved position %d (%.0f, %.0f, height %.0f)", i, x, y, z))
end

function C.loadPos(i)
  local s = C.positions["slot" .. i]
  local x, y, z = string.match(s or "", "([-%d%.]+),([-%d%.]+),([-%d%.]+)")
  if not x then ModLog("Cheats: position " .. i .. " is empty") return end
  if C.teleport(tonumber(x), tonumber(y), tonumber(z) + 0.5) then ModLog("Cheats: teleported to position " .. i) end
end

function C.clearPos(i)
  C.positions["slot" .. i] = nil
  FC4.saveKV(C.posFile, C.positions)
  ModSetState("pos." .. i, "")
end

-- Unstick: straight up, with god mode on until the switch (or travel) says otherwise.
function C.up(m)
  local x, y, z = C.pos()
  if not x then return end
  if not C.isOn("god") then FC4.call("EnableGodMode", 1) C.godUntil = os.clock() + 20 end
  if C.teleport(x, y, z + (tonumber(m) or 30)) then ModLog("Cheats: up " .. tostring(m or 30) .. " m (god mode on for 20 s)") end
end

function C.setRemember(v)
  C.cfg.remember = v and "1" or "0"
  C.save()
  C.publish()
end

-- ------------------------------------------------------------ tick
function C.tick()
  local now = os.clock()
  if C.godUntil and now >= C.godUntil then
    C.godUntil = nil
    if not C.isOn("god") then FC4.releaseGodMode() end
  end
  if now < C.nextAssert then return end
  C.nextAssert = now + C.reassertEvery
  if C.pos() == nil then return end           -- not in the world (menus, loading)
  for k, v in pairs(C.on) do
    if v and k ~= "slowmo" and not (C.perFrame and C.perFrame[k]) then pcall(C.apply, k, true) end
  end
  if C.on.slowmo and not C.slowApplied then C.slowApplied = pcall(C.applySlow, true) end
end

C.load()
C.loadPositions()
C.slowApplied = false
C.publish()
C.publishAvail()
FC4Ticks.cheats = C.tick
ModLog("FC4Cheats loaded")
