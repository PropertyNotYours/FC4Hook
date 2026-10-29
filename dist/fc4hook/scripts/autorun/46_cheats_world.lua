-- FC4Hook Cheats, part 3: FOV, HUD, karma, vehicles. Argument lists from the game's own code:
--   CAPI_Player.SetPlayerFOV(player, degrees, blendSeconds, true)   override the camera FOV
--   CAPI_Player.SetPlayerFOV(player, 0, blendSeconds, false)        release it (game FOV again)
--   CAPI_Player.SetPlayerNearFOV(...)   same for the weapon / arms ("near") camera
--   SetHudVisible(bool)   CAPI_Player.GivePlayerKarma(player, n)   GetPlayerKarmaLevel(player)
--   CAPI_Pawn.GetPawnVehicle(pawn) -> vehicle   CAPI_Vehicle.Get/SetVehicleSpeed(vehicle, km/h)
--     (SetVehicleSpeed keeps the direction you're moving in, so it's only used while moving)
-- Settings live in cheats.txt: fov, fov_near, vboostx, vmax.

local C = FC4Cheats
if not C then ModLogError("46_cheats_world: 40_cheats.lua must load first") return end

C.keys[#C.keys + 1] = "fov"
C.keys[#C.keys + 1] = "fovnear"
C.keys[#C.keys + 1] = "hud"
C.keys[#C.keys + 1] = "vboost"
C.labels.fov = "FOV override"
C.labels.fovnear = "Weapon FOV override"
C.labels.hud = "Hide HUD"
C.labels.vboost = "Vehicle speed boost"
-- handled by tick3 (FOV is re-checked once a second, boost every frame), not the 4 s reassert.
C.perFrame.fov = true
C.perFrame.fovnear = true
C.perFrame.hud = true
C.perFrame.vboost = true

for _, n in ipairs({ "CAPI_Player.SetPlayerFOV", "CAPI_Player.SetPlayerNearFOV", "CAPI_Player.GetPlayerFOV",
                     "SetHudVisible", "CAPI_Player.GivePlayerKarma", "CAPI_Player.GetPlayerKarmaLevel",
                     "CAPI_Pawn.GetPawnVehicle", "CAPI_Vehicle.GetVehicleSpeed", "CAPI_Vehicle.SetVehicleSpeed" }) do
  C.natives[#C.natives + 1] = n
end

C.INVALID = "18446744073709551615"

function C.defaults3()
  local d = { fov = "90", fov_near = "70", vboostx = "2.0", vmax = "250" }
  for k, v in pairs(d) do C.cfg[k] = C.cfg[k] or v end
  if C.cfg.remember == "1" then
    for _, k in ipairs({ "fov", "fovnear", "hud", "vboost" }) do
      if C.cfg["on_" .. k] == "1" then C.on[k] = true end
    end
  end
end

-- ------------------------------------------------------------ FOV
-- k = "fov" or "fovnear". Release first, then set: never stack two overrides.
function C.fovFn(k)
  return k == "fov" and "CAPI_Player.SetPlayerFOV" or "CAPI_Player.SetPlayerNearFOV"
end

function C.fovApply(k, on)
  local me = FC4.me()
  local f = FC4.fn(C.fovFn(k))
  if me == nil or not f then return false end
  pcall(f, me, 0, on and 0 or 0.25, false)          -- off: blend back over 0.25 s
  if not on then return true end
  local deg = tonumber(C.cfg[k == "fov" and "fov" or "fov_near"]) or 90
  local ok, err = pcall(f, me, deg, 0.25, true)
  if not ok then ModLogWarn("Cheats: " .. C.fovFn(k) .. " failed: " .. tostring(err)) end
  return ok
end

function C.setFov(k, deg)
  C.cfg[k == "fov" and "fov" or "fov_near"] = string.format("%.0f", tonumber(deg) or 90)
  C.save()
  C.publish3()
  if C.on[k] then C.fovApply(k, true) end
end

function C.gameFov()
  local me = FC4.me()
  local ok, v = pcall(CAPI_Player.GetPlayerFOV, me)
  if not ok or type(v) ~= "number" then return end
  if v < 4 then v = math.deg(v) end
  ModSetState("cheat.fov_game", string.format("%.0f", v))
end

-- ------------------------------------------------------------ HUD
function C.hudApply(hide)
  return FC4.call("SetHudVisible", not hide)
end

-- ------------------------------------------------------------ karma
function C.karma(n)
  local me = FC4.me()
  if me ~= nil and FC4.call("CAPI_Player.GivePlayerKarma", me, tonumber(n) or 100) then
    ModLog("Cheats: +" .. string.format("%d", tonumber(n) or 100) .. " karma")
  end
  C.karmaInfo()
end

function C.karmaInfo()
  local me = FC4.me()
  if me == nil then return end
  local ok, lv = pcall(CAPI_Player.GetPlayerKarmaLevel, me)
  local ok2, pts = pcall(CAPI_Player.GetPlayerTotalKarmaPoints, me)
  ModSetState("cheat.karma", (ok and tostring(lv) or "?") .. "|" .. (ok2 and tostring(pts) or "?"))
end

-- ------------------------------------------------------------ vehicles
function C.vehicle(me)
  local ok, v = pcall(CAPI_Pawn.GetPawnVehicle, me)
  if not ok or v == nil or v == 0 or tostring(v) == C.INVALID then return nil end
  return v
end

function C.vspeed(v)
  local ok, s = pcall(CAPI_Vehicle.GetVehicleSpeed, v)
  if ok and type(s) == "number" and s == s then return s end
  return nil
end

-- Boost: while the throttle is held (W, or the right trigger), push the vehicle's speed up
-- by (factor - 1) x 25 km/h per second on top of what the engine does, up to vmax km/h.
-- Let go, brake or reverse (S / left trigger) and the game drives normally.
function C.throttle()
  if ModKeyDown(83) then return false end                 -- S: braking / reversing
  if ModKeyDown(87) then return true end                  -- W
  local ok, rt = pcall(function() return FC4.padTrigger and FC4.padTrigger("right") end)
  return ok and type(rt) == "number" and rt > 0.3
end

function C.vboostTick(me)
  local now = os.clock()
  local dt = C.vtime and math.min(now - C.vtime, 0.1) or 0
  C.vtime = now
  local v = C.vehicle(me)
  local s = v and C.vspeed(v)
  if not s or s < 3 or dt <= 0 or not C.throttle() then return end
  local vmax = tonumber(C.cfg.vmax) or 250
  if s >= vmax then return end
  local k = (tonumber(C.cfg.vboostx) or 2) - 1
  local new = math.min(s + k * 25 * dt, vmax)
  if new > s then pcall(CAPI_Vehicle.SetVehicleSpeed, v, new) end
end

function C.brake()
  local me = FC4.me()
  local v = me and C.vehicle(me)
  if not v then ModLog("Cheats: brake - you're not in a vehicle") return end
  local s = C.vspeed(v)
  if not s or s < 0.5 then return end          -- standing still: nothing to stop (and no direction)
  if pcall(CAPI_Vehicle.SetVehicleSpeed, v, 0) then ModLog("Cheats: brake!") end
end

function C.setVehicleCfg(k, val)
  C.cfg[k] = string.format("%.2f", tonumber(val) or 1)
  C.save()
  C.publish3()
end

-- ------------------------------------------------------------ switches + ticks
C.appliers.fov = function(v) return C.fovApply("fov", v == true) end
C.appliers.fovnear = function(v) return C.fovApply("fovnear", v == true) end
C.appliers.hud = function(v) return C.hudApply(v == true) end
C.appliers.vboost = function() C.vtime = nil return true end

-- FOV/HUD switches go back on when you enter the world (after a load); vehicle boost every frame.
function C.tick3()
  local me = FC4.me()
  local inWorld = me ~= nil and C.pos() ~= nil
  if not inWorld then C.worldApplied = false return end
  if not C.worldApplied then
    C.worldApplied = true
    if C.on.fov then C.fovApply("fov", true) end
    if C.on.fovnear then C.fovApply("fovnear", true) end
    if C.on.hud then C.hudApply(true) end
    C.karmaInfo()
    C.gameFov()
  end
  if C.on.vboost then C.vboostTick(me) end
  C.fovKeep()
end

-- The game sets its own FOV again (aiming, vehicles, cutscenes...). Once a second, if the
-- camera isn't at our FOV any more, put the override back.
function C.fovKeep()
  local now = os.clock()
  if C.fovNext and now < C.fovNext then return end
  C.fovNext = now + 1
  for _, k in ipairs({ "fov", "fovnear" }) do
    if C.on[k] then
      local want = tonumber(C.cfg[k == "fov" and "fov" or "fov_near"]) or 90
      local cur
      if k == "fov" then
        local ok, v = pcall(CAPI_Player.GetPlayerFOV, FC4.me())
        if ok and type(v) == "number" then cur = v < 4 and math.deg(v) or v end
      end
      if cur == nil or math.abs(cur - want) > 1 then
        local f = FC4.fn(C.fovFn(k))
        if f then pcall(f, FC4.me(), want, 0.1, true) end
      end
    end
  end
end

function C.publish3()
  for _, k in ipairs({ "fov", "fov_near", "vboostx", "vmax" }) do ModSetState("cheat." .. k, C.cfg[k]) end
end

C.defaults3()
C.publish()
C.publish3()
C.publishAvail()
FC4Ticks.cheats3 = C.tick3
ModLog("FC4Cheats (FOV/HUD/vehicles) loaded")
