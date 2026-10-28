-- FC4Hook bell tower fast travel.
--
-- Teleports you to any bell tower you've liberated (positions come from the game's
-- always-loaded tower records, so it works from anywhere on the map).
-- The stored point is the top of the tower (or a roof in a village), so you appear 12 m
-- below it and 14 m out to the side you came from, then drop the last bit, with god mode on
-- until you've landed. If that spot turns out to be underground (hillside, village) or the
-- ground hadn't loaded, you fall through, get caught, and are dropped again from higher up.
--
-- Tour mode visits every liberated tower whose reward list isn't recorded yet, waits for it
-- to load, records it (coop\tower_reward_table.txt), then brings you back where you started.

FC4Travel = FC4Travel or {}
local T = FC4Travel
-- Defaults measured from real saved spots (9 towers: 4.3-4.9 m out, 11.2-11.6 m below the top).
-- Once you've saved spots, the typical value of those is used instead.
T.offset = 4.6        -- metres out from the stored tower point
T.dropHeight = -11.3  -- metres below the stored point (it's the tower's top)
-- The game's teleport is asynchronous: it takes a moment (longer far away, while the area
-- streams in). So FC4Hook teleports ONCE, waits until you've actually arrived, and only
-- teleports again if you fell through the world (from a bit higher, at most twice).
-- Nothing ever re-teleports you every frame, and every landing has a hard time limit.
T.fallLimit = 25      -- this far below the target = fell through the world
T.landTimeout = 30    -- hard limit (seconds) for the whole landing, whatever happens
T.arriveWait = 8      -- seconds to wait for the teleport to happen before asking again
T.maxRequests = 2     -- teleport requests per jump while waiting to arrive
T.maxRetries = 2      -- re-drops after falling through
T.tourWait = 7        -- seconds to let a tower stream in
T.tourTries = 3       -- waits per tower before giving up on it

-- ------------------------------------------------------------ helpers
function T.towers()
  local list = {}
  local raw = ModTowerPositions and ModTowerPositions()
  if type(raw) == "string" then
    for id, x, y, z in string.gmatch(raw, "(%d+):([-%d%.]+),([-%d%.]+),([-%d%.]+);") do
      list[#list + 1] = { id = id, x = tonumber(x), y = tonumber(y), z = tonumber(z) }
    end
  end
  return list
end

function T.find(id)
  for _, tw in ipairs(T.towers()) do
    if tw.id == id then return tw end
  end
  return nil
end

function T.teleportFn()
  if type(TeleportEntityPos) == "function" then return TeleportEntityPos end
  if type(ModTeleportEntityPos) == "function" then return ModTeleportEntityPos end
  return nil
end

function T.myPos()
  local f = GetEntityPositionVec3 or ModGetEntityPositionVec3
  if type(f) ~= "function" then return nil end
  local ok, p = pcall(f, GetLocalPlayerId())
  if ok and type(p) == "table" and tonumber(p[1]) then return { x = p[1], y = p[2], z = p[3] } end
  return nil
end

function T.rawJump(x, y, z)
  local tp = T.teleportFn()
  if not tp then
    ModLog("Travel: teleport native not found (see fc4hook.log)")
    return false
  end
  local ok, err = pcall(tp, GetLocalPlayerId(), { x, y, z }, false)
  if not ok then ModLog("Travel: teleport failed: " .. tostring(err)) end
  return ok
end

-- Jump to exactly (x, y, z) and watch the landing.
function T.jump(x, y, z)
  local from = T.myPos()
  pcall(EnableGodMode, 1)
  if not T.rawJump(x, y, z) then return false end
  local now = os.clock()
  T.landing = { x = x, y = y, z = z, from = from, t0 = now, lastTp = now, requests = 1,
                retries = 0, next = 0, still = 0 }
  return true
end

function T.loading()
  if type(IsLoadPending) ~= "function" then return false end
  local ok, v = pcall(IsLoadPending)
  return ok and (v == true or v == 1)
end

function T.near(p, l)
  local dx, dy = p.x - l.x, p.y - l.y
  return dx * dx + dy * dy < 25 and math.abs(p.z - l.z) < 40
end

-- Before arriving: just wait. Ask once more if nothing happened for a while.
function T.waitArrival(l, p, now)
  if T.near(p, l) then
    l.arrived = now
    l.lastZ = nil
    l.still = 0
    return
  end
  if T.loading() or now - l.lastTp < T.arriveWait then return end
  if l.requests >= T.maxRequests then T.landed("Travel: the game didn't move you; try again in a moment") return end
  l.requests = l.requests + 1
  l.lastTp = now
  T.rawJump(l.x, l.y, l.z)
end

-- Out of retries: one teleport back to where the jump started, then stop for good.
function T.giveUp(l)
  T.landing = nil
  pcall(EnableGodMode, 1)
  if l.from and not l.wentBack then
    T.rawJump(l.from.x, l.from.y, l.from.z + 0.5)
    local now = os.clock()
    T.landing = { x = l.from.x, y = l.from.y, z = l.from.z + 0.5, t0 = now, lastTp = now, requests = 1,
                  retries = T.maxRetries, next = 0, still = 0, wentBack = true }
    ModLog("Travel: that area wouldn't load, sent you back to where you were")
    return
  end
  ModLog("Travel: couldn't land safely, god mode stays on. Use Cheats > Unstick or a safe house")
end

-- Fell through (ground hadn't loaded): drop again from a little higher, then give up safely.
function T.fellThrough(l, now)
  if l.retries >= T.maxRetries then T.giveUp(l) return end
  l.retries = l.retries + 1
  l.z = l.z + 4
  l.arrived = nil
  l.lastTp = now
  l.requests = 1
  ModLog("Travel: the ground hadn't loaded yet, dropping you again")
  T.rawJump(l.x, l.y, l.z)
end

function T.cancel()
  if not T.landing and not T.tour then ModLog("Travel: nothing to cancel") return end
  T.tour = nil
  T.landing = nil
  FC4.releaseGodMode()
  ModLog("Travel: cancelled")
end

-- Drop point beside a tower, on the side the player is coming from.
function T.dropPoint(tw, off)
  local dx, dy = 1, 0
  local me = T.myPos()
  if me then
    local ax, ay = me.x - tw.x, me.y - tw.y
    local len = math.sqrt(ax * ax + ay * ay)
    if len > 1 then dx, dy = ax / len, ay / len end
  end
  return tw.x + dx * off, tw.y + dy * off
end

-- ------------------------------------------------------------ saved spawn spots
-- coop\tower_spawns.txt: "towerId=dx,dy,dz" = where to put you, relative to the tower's
-- stored point. Recorded with scripts\save_tower_spawn.lua while standing at the spot.
T.spawnFile = "coop/tower_spawns.txt"

function T.loadSpawns()
  local t = {}
  local data = ModReadFile(T.spawnFile)
  if type(data) == "string" then
    for id, a, b, c in string.gmatch(data, "(%d+)=([-%d%.]+),([-%d%.]+),([-%d%.]+)") do
      t[id] = { dx = tonumber(a), dy = tonumber(b), dz = tonumber(c) }
    end
  end
  return t
end

function T.saveSpawns(t)
  local rows = {}
  for id, o in pairs(t) do rows[#rows + 1] = string.format("%s=%.2f,%.2f,%.2f", id, o.dx, o.dy, o.dz) end
  table.sort(rows)
  ModWriteFile(T.spawnFile, table.concat(rows, "\n") .. "\n")
end

function T.median(v)
  table.sort(v)
  return v[math.floor((#v + 1) / 2)]
end

-- Learn the default drop from your saved spots: typical height below the top and distance out.
function T.defaults(spawns)
  local dz, dist = {}, {}
  for _, o in pairs(spawns) do
    dz[#dz + 1] = o.dz
    dist[#dist + 1] = math.sqrt(o.dx * o.dx + o.dy * o.dy)
  end
  if #dz == 0 then return T.offset, T.dropHeight end
  return T.median(dist), T.median(dz) + 0.5
end

function T.nearest()
  local p = T.myPos()
  if not p then return nil end
  local best, bestD, idx
  for i, tw in ipairs(T.towers()) do
    local d = math.sqrt((tw.x - p.x) ^ 2 + (tw.y - p.y) ^ 2)
    if not bestD or d < bestD then best, bestD, idx = tw, d, i end
  end
  return best, bestD, p, idx
end

function T.saveSpot()
  local tw, d, p, idx = T.nearest()
  if not tw then ModLog("Spawn spot: no liberated towers / no position") return end
  if d > 80 then ModLog(string.format("Spawn spot: nearest liberated tower is %.0f m away, get closer (within 80 m)", d)) return end
  local t = T.loadSpawns()
  t[tw.id] = { dx = p.x - tw.x, dy = p.y - tw.y, dz = p.z - tw.z }
  T.saveSpawns(t)
  local o = t[tw.id]
  ModLog(string.format("Spawn spot saved for tower %d: %.1f m out from the top, %.1f m %s it",
    idx, math.sqrt(o.dx * o.dx + o.dy * o.dy), math.abs(o.dz), o.dz < 0 and "below" or "above"))
end

function T.forgetSpot()
  local tw, d, _, idx = T.nearest()
  if not tw or d > 80 then ModLog("Spawn spot: not near a liberated tower") return end
  local t = T.loadSpawns()
  t[tw.id] = nil
  T.saveSpawns(t)
  ModLog(string.format("Spawn spot for tower %d forgotten", idx))
end

function T.jumpToTower(tw)
  local spawns = T.loadSpawns()
  local o = spawns[tw.id]
  if o then return T.jump(tw.x + o.dx, tw.y + o.dy, tw.z + o.dz + 0.5) end
  local off, dz = T.defaults(spawns)
  local x, y = T.dropPoint(tw, off)
  return T.jump(x, y, tw.z + dz)
end

function T.landed(msg)
  T.landing = nil
  if not T.tour then FC4.releaseGodMode() end
  if msg then ModLog(msg) end
end

function T.watchLanding(now)
  local l = T.landing
  if now - l.t0 > T.landTimeout then T.landed("Travel: landing timed out, god mode off") return end
  if now < l.next then return end
  l.next = now + 0.25
  local p = T.myPos()
  if not p then return end
  if not l.arrived then T.waitArrival(l, p, now) return end
  if p.z < l.z - T.fallLimit then T.fellThrough(l, now) return end
  if l.lastZ and math.abs(p.z - l.lastZ) < 0.05 then l.still = l.still + 1 else l.still = 0 end
  l.lastZ = p.z
  if l.still >= 6 and now - l.arrived > 1 then T.landed() end
end

-- ------------------------------------------------------------ menu actions
function T.refresh()
  local list = T.towers()
  ModLog(string.format("Travel: %d liberated bell towers available", #list))
end

function T.go(id)
  local tw = T.find(id)
  if not tw then ModLog("Travel: tower " .. tostring(id) .. " isn't liberated / has no position") return end
  T.home = T.home or T.myPos()
  if T.jumpToTower(tw) then
    ModLog(string.format("Travel: jumped to tower %s (%.0f, %.0f, %.0f)", id, tw.x, tw.y, tw.z))
  end
end

function T.back()
  local h = T.home
  if not h then ModLog("Travel: no saved position yet") return end
  if T.jump(h.x, h.y, h.z + 0.5) then
    ModLog("Travel: back where you were")
    T.home = nil
  end
end

function T.logPos()
  local p = T.myPos()
  if p then ModLog(string.format("Travel: you are at %.1f, %.1f, %.1f", p.x, p.y, p.z))
  else ModLog("Travel: position native not found") end
end

-- ------------------------------------------------------------ tour
function T.tourStart()
  if not FC4Coop or not FC4Coop.loadTable then ModLog("Tour: co-op tower script not loaded") return end
  local known = FC4Coop.loadTable()
  local todo = {}
  for _, tw in ipairs(T.towers()) do
    if known[tw.id] == nil then todo[#todo + 1] = tw end
  end
  if #todo == 0 then ModLog("Tour: every liberated tower is already in the reward table") return end
  T.tour = { list = todo, i = 0, start = T.myPos(), learned = 0, skipped = 0 }
  ModLog(string.format("Tour: visiting %d towers (about %d s). Stop it from the Travel tab.", #todo, #todo * T.tourWait))
  T.tourNext()
end

function T.tourNext()
  local t = T.tour
  t.i = t.i + 1
  t.tries = 0
  local tw = t.list[t.i]
  if not tw then T.tourEnd() return end
  T.jumpToTower(tw)
  t.nextAt = os.clock() + T.tourWait
end

function T.tourCheck()
  local t = T.tour
  local tw = t.list[t.i]
  pcall(FC4Coop.run, "learn", true)
  if FC4Coop.loadTable()[tw.id] ~= nil then
    t.learned = t.learned + 1
    ModLog(string.format("Tour: %d/%d recorded tower %s", t.i, #t.list, tw.id))
    T.tourNext()
    return
  end
  t.tries = t.tries + 1
  if t.tries >= T.tourTries then
    t.skipped = t.skipped + 1
    ModLog(string.format("Tour: %d/%d tower %s didn't load, skipping", t.i, #t.list, tw.id))
    T.tourNext()
    return
  end
  t.nextAt = os.clock() + T.tourWait
end

function T.tourEnd()
  local t = T.tour
  T.tour = nil
  if t.start then T.jump(t.start.x, t.start.y, t.start.z + 0.5)
  else FC4.releaseGodMode() end
  ModLog(string.format("Tour finished: %d recorded, %d skipped", t.learned, t.skipped))
end

function T.tourStop()
  if not T.tour then ModLog("Tour: not running") return end
  T.tour.i = #T.tour.list
  T.tourEnd()
end

-- ------------------------------------------------------------ tick
function T.tick()
  local now = os.clock()
  if T.tour and T.tour.nextAt and now >= T.tour.nextAt then
    T.tour.nextAt = nil
    local ok, err = pcall(T.tourCheck)
    if not ok then ModLog("Tour error: " .. tostring(err)); T.tour = nil end
  end
  if T.landing then
    local ok, err = pcall(T.watchLanding, now)
    if not ok then T.landed("Travel landing error: " .. tostring(err)) end
  end
end

-- Shared tick registry (safe to reload).
FC4Ticks = FC4Ticks or {}
FC4Ticks.travel = T.tick

pcall(ModTowerPositions)
ModLog("FC4Travel loaded (Travel tab: jump to liberated bell towers)")
