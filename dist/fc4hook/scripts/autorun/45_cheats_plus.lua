-- FC4Hook Cheats, part 2: infinite magazine, no fall damage, speed, fly/noclip, money/XP,
-- launch, objective teleport, outfit. Argument lists come from the game's own code
-- (decompiled natives), e.g. SetWeaponAmmoClip(entity, [weaponName], count).
--
-- Fly/noclip tuning lives in fc4hook\cheats.txt (written when you change them):
--   fly_speed, fly_fast (with Shift), fly_yaw (which angle is yaw: 1-3), fly_yaw_sign (1/-1),
--   fly_yaw_offset (degrees), fly_units (auto/deg/rad)

local C = FC4Cheats
if not C then ModLogError("45_cheats_plus: 40_cheats.lua must load first") return end

C.keys[#C.keys + 1] = "mag"
C.keys[#C.keys + 1] = "nofall"
C.keys[#C.keys + 1] = "speed"
C.keys[#C.keys + 1] = "fly"
C.keys[#C.keys + 1] = "noclip"
C.perFrame = { mag = true, speed = true, fly = true, noclip = true }
C.labels.mag = "Infinite magazine"
C.labels.nofall = "No fall damage"
C.labels.speed = "Speed boost"
C.labels.fly = "Fly"
C.labels.noclip = "Noclip"

C.natives[#C.natives + 1] = "CAPI_Pawn.SetWeaponAmmoClip"
C.natives[#C.natives + 1] = "CAPI_Pawn.GetCurrentWeaponCurrentClipAmmo"
C.natives[#C.natives + 1] = "CAPI_Player.SetLandingDamageEnabled"
C.natives[#C.natives + 1] = "CAPI_Entity.SetEntityPositionVec3"
C.natives[#C.natives + 1] = "CAPI_Entity.SetGravity"
C.natives[#C.natives + 1] = "CAPI_Entity.SetImmuneToCollisions"
C.natives[#C.natives + 1] = "CAPI_Entity.GetEntityAngles"
C.natives[#C.natives + 1] = "CAPI_Player.SetPlayerMoney"
C.natives[#C.natives + 1] = "CAPI_Player.AddPlayerMoney"
C.natives[#C.natives + 1] = "CAPI_Player.GivePlayerXP"
C.natives[#C.natives + 1] = "CAPI_Pawn.GiveSkillPoints"
C.natives[#C.natives + 1] = "CAPI_Player.SetWorldSpeed"
C.natives[#C.natives + 1] = "CAPI_Player.EquipCostume"
C.natives[#C.natives + 1] = "Game.TeleportToCurrentObjective"
C.natives[#C.natives + 1] = "CAPI_Weapon.ForceFullAmmoInClip"

C.GRAVITY = -9.81

function C.defaults2()
  local d = { fly_freeze = "0", speedx = "2.0", fly_speed = "12", fly_fast = "40", fly_yaw = "3", fly_yaw_sign = "1",
              fly_yaw_offset = "0", fly_units = "auto" }
  for k, v in pairs(d) do C.cfg[k] = C.cfg[k] or v end
  if C.cfg.remember == "1" then
    for _, k in ipairs({ "mag", "nofall", "speed" }) do
      if C.cfg["on_" .. k] == "1" then C.on[k] = true end
    end
  end
end

-- ------------------------------------------------------------ simple switches
function C.applyNoFall(v)
  local me = FC4.me()
  if me == nil then return false end
  return FC4.call("CAPI_Player.SetLandingDamageEnabled", me, not v)
end
C.appliers.nofall = C.applyNoFall
function C.perFrameApplier() return true end          -- handled every frame in C.tick2
for k in pairs(C.perFrame) do C.appliers[k] = C.perFrameApplier end

-- Infinite magazine: keep the clip full (checked every frame). Not the same as unlimited
-- ammo: you never reload, reserve ammo is untouched.
-- CAPI_Weapon.ForceFullAmmoInClip takes the WEAPON's entity id and fills its clip to max;
-- SetWeaponAmmoClip(pawn, weaponName, count) is the fallback ("" = the current weapon; a
-- number in the 2nd place would be read as a weapon name).
function C.magTick(me)
  local okc, clip = pcall(CAPI_Pawn.GetCurrentWeaponCurrentClipAmmo, me)
  local okm, max = pcall(CAPI_Pawn.GetCurrentWeaponMaxClipAmmo, me)
  if not okc or not okm or type(clip) ~= "number" or type(max) ~= "number" then return end
  if max <= 0 or clip >= max then return end
  local okw, wid = pcall(CAPI_Pawn.GetEquippedWeaponEntityID, me)
  if okw and wid ~= nil and pcall(CAPI_Weapon.ForceFullAmmoInClip, wid) then
    local ok2, now = pcall(CAPI_Pawn.GetCurrentWeaponCurrentClipAmmo, me)
    if ok2 and type(now) == "number" and now >= max then return end
  end
  pcall(CAPI_Pawn.SetWeaponAmmoClip, me, "", max)
end

-- ------------------------------------------------------------ movement helpers
function C.setPos(me, x, y, z)
  return pcall(CAPI_Entity.SetEntityPositionVec3, me, { x, y, z })
end

function C.yaw(me)
  local ok, a = pcall(CAPI_Entity.GetEntityAngles, me)
  if not ok or type(a) ~= "table" then return nil end
  local v = tonumber(a[tonumber(C.cfg.fly_yaw) or 3]) or 0
  if math.abs(v) > 6.4 then C.degrees = true end
  local units = C.cfg.fly_units
  if units == "deg" or (units == "auto" and C.degrees) then v = math.rad(v) end
  return v * (tonumber(C.cfg.fly_yaw_sign) or 1) + math.rad(tonumber(C.cfg.fly_yaw_offset) or 0)
end

-- Speed boost: adds (factor - 1) x the distance the game moved you this frame.
function C.speedTick(me, x, y, z)
  local l = C.lastPos
  C.lastPos = { x, y, z }
  if not l then return end
  local dx, dy = x - l[1], y - l[2]
  local d2 = dx * dx + dy * dy
  if d2 < 0.0001 or d2 > 25 then return end            -- standing still, or a teleport/vehicle jump
  local k = (tonumber(C.cfg.speedx) or 2) - 1
  C.setPos(me, x + dx * k, y + dy * k, z)
  C.lastPos = { x + dx * k, y + dy * k, z }
end

-- ------------------------------------------------------------ fly / noclip
function C.flyInput()
  local f, r, u = 0, 0, 0
  if ModKeyDown(87) then f = f + 1 end        -- W
  if ModKeyDown(83) then f = f - 1 end        -- S
  if ModKeyDown(68) then r = r + 1 end        -- D
  if ModKeyDown(65) then r = r - 1 end        -- A
  if ModKeyDown(32) then u = u + 1 end        -- Space
  if ModKeyDown(17) then u = u - 1 end        -- Ctrl
  return f, r, u
end

-- The game thinks a flying player is falling (falling animation, camera pulled down).
-- While flying FC4Hook zeroes your speed every frame. "Freeze body" (fly_freeze=1) sets your
-- animation speed to 0 so your arms don't flail; it's applied live and re-applied every
-- second while flying (the game can reset it), and always undone when you land.
-- (An earlier "steady camera" option used LockBeautifier; that only blocks scripted camera
-- changes, not the engine's falling camera, so it was removed.)
-- Space/Ctrl are kept from the game while flying (DirectInput filter in fc4hook.asi).
function C.flyGround(me)
  pcall(CAPI_Player.SetWorldSpeed, me, { 0, 0, 0 })
end

function C.setFrozen(me, on)
  local ok, err = pcall(CAPI_Entity.SetAnimSpeedScale, me, on and 0 or 1)
  if not ok then ModLogWarn("Fly: SetAnimSpeedScale failed: " .. tostring(err)) end
  C.frozen = on and ok or nil
end

-- called every frame while flying, and once when you stop
function C.flyPose(me, flying)
  local want = flying and C.cfg.fly_freeze == "1"
  if want then
    local now = os.clock()
    if not C.frozen or now - (C.frozenAt or 0) > 1 then C.setFrozen(me, true) C.frozenAt = now end
  elseif C.frozen then
    C.setFrozen(me, false)
  end
end

function C.flyStart(me)
  local x, y, z = C.pos()
  if not x then return false end
  C.fly = { x = x, y = y, z = z, t = os.clock() }
  ModSetState("cheat.flying", "1")
  pcall(CAPI_Entity.SetGravity, me, { 0, 0, 0 })
  local ok, a = pcall(CAPI_Entity.GetEntityAngles, me)
  if ok and type(a) == "table" then ModLog(string.format("Fly: angles %.2f %.2f %.2f", a[1] or 0, a[2] or 0, a[3] or 0)) end
  return true
end

function C.flyStop(me)
  C.fly = nil
  ModSetState("cheat.flying", "0")
  pcall(CAPI_Entity.SetGravity, me, { 0, 0, C.GRAVITY })
  C.flyPose(me, false)
end

function C.flyDelta(dt)
  local f, r, u = C.flyInput()
  local speed = tonumber(ModKeyDown(16) and C.cfg.fly_fast or C.cfg.fly_speed) or 12
  local yaw = C.yaw(FC4.me()) or 0
  local fx, fy = -math.sin(yaw), math.cos(yaw)       -- Dunia: +Y forward at yaw 0
  local k = speed * dt
  return (fx * f + fy * r) * k, (fy * f - fx * r) * k, u * k
end

function C.flyStep(me)
  local s = C.fly
  local now = os.clock()
  local dt = math.min(now - s.t, 0.1)
  s.t = now
  local dx, dy, dz = C.flyDelta(dt)
  s.x, s.y, s.z = s.x + dx, s.y + dy, s.z + dz
  C.setPos(me, s.x, s.y, s.z)
  C.flyGround(me)
  C.flyPose(me, true)
  C.flyCheck(me, s)
end

-- Safety: if the game puts you somewhere else (cutscene, death, the game moving you), follow it
-- instead of fighting; only give up if that keeps happening for 5 seconds.
function C.flyCheck(me, s)
  local x, y, z = C.pos()
  if not x then return end
  local d = math.abs(x - s.x) + math.abs(y - s.y) + math.abs(z - s.z)
  if d < 15 then s.missSince = nil return end
  s.x, s.y, s.z = x, y, z
  local now = os.clock()
  s.missSince = s.missSince or now
  if now - s.missSince < 5 then return end
  ModLogWarn("Fly: the game keeps moving you, fly switched off")
  C.on.fly = nil
  C.on.noclip = nil
  C.flyStop(me)
  C.applyNoclip(me, false)
  C.publish()
end

function C.applyNoclip(me, v)
  pcall(CAPI_Entity.SetImmuneToCollisions, me, v == true)
end

function C.moveTick(me)
  local flying = C.on.fly or C.on.noclip
  if flying and not C.fly then
    if not C.flyStart(me) then return end
    if C.on.noclip then C.applyNoclip(me, true) end
  elseif not flying and C.fly then
    C.flyStop(me)
    C.applyNoclip(me, false)
  end
  if C.fly then C.flyStep(me) return end
  if C.on.speed then
    local x, y, z = C.pos()
    if x then C.speedTick(me, x, y, z) end
  else
    C.lastPos = nil
  end
end

-- noclip implies fly; switching noclip off while fly stays on keeps flying with collisions
local baseSet = C.set
function C.set(k, v)
  baseSet(k, v)
  local me = FC4.me()
  if k == "noclip" and me ~= nil and C.fly then C.applyNoclip(me, v == true) end
  if (k == "fly" or k == "noclip") and not v and me ~= nil and not (C.on.fly or C.on.noclip) and C.fly then
    C.flyStop(me)
    C.applyNoclip(me, false)
  end
end

function C.tick2()
  local me = FC4.me()
  if me == nil then return end
  if C.on.mag then C.magTick(me) end
  C.moveTick(me)
end

-- ------------------------------------------------------------ actions
function C.money(mode, n)
  local me = FC4.me()
  n = tonumber(n) or 0
  if me == nil or n < 0 then return end
  if mode == "set" then
    if FC4.call("CAPI_Player.SetPlayerMoney", me, n) then ModLog("Cheats: money set to " .. string.format("%d", n)) end
  elseif FC4.call("CAPI_Player.AddPlayerMoney", me, n) then
    ModLog("Cheats: +" .. string.format("%d", n) .. " money")
  end
end

function C.xp(n)
  local me = FC4.me()
  if me ~= nil and FC4.call("CAPI_Player.GivePlayerXP", me, tonumber(n) or 0, true) then
    ModLog("Cheats: +" .. string.format("%d", tonumber(n) or 0) .. " XP")
  end
end

function C.skill(n)
  local me = FC4.me()
  if me ~= nil and FC4.call("CAPI_Pawn.GiveSkillPoints", me, tonumber(n) or 1) then
    ModLog("Cheats: +" .. string.format("%d", tonumber(n) or 1) .. " skill points")
  end
end

-- Launch: set your velocity straight up (and keep your horizontal speed at zero)
function C.launch(v)
  local me = FC4.me()
  if me ~= nil and FC4.call("CAPI_Player.SetWorldSpeed", me, { 0, 0, tonumber(v) or 25 }) then ModLog("Cheats: wheee") end
end

function C.objective()
  local f = FC4.fn("Game.TeleportToCurrentObjective")
  if not f then ModLogWarn("Cheats: objective teleport isn't available") return end
  local ok, err = pcall(f)
  if not ok then ok, err = pcall(f, Game) end
  if ok then ModLog("Cheats: teleporting to the current objective") else ModLogWarn("Cheats: objective teleport failed: " .. tostring(err)) end
end

function C.costume(v)
  local me = FC4.me()
  if me == nil then return end
  local ok, r = FC4.call("CAPI_Player.EquipCostume", me, v == true)
  ModLog("Cheats: outfit " .. (v and "on" or "off") .. " -> " .. tostring(r))
end

function C.setCfg(k, v)
  C.cfg[k] = tostring(v)
  C.save()
  C.publish()
  ModSetState("cheat." .. k, tostring(v))
end

function C.publish2()
  for _, k in ipairs({ "speedx", "fly_speed", "fly_fast", "fly_freeze" }) do ModSetState("cheat." .. k, C.cfg[k]) end
end

C.defaults2()
C.publish()
C.publish2()
C.publishAvail()
FC4Ticks.cheats2 = C.tick2
ModLog("FC4Cheats+ loaded")
