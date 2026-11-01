-- FC4Hook Weapon Workshop (F8 > Workshop): loading new weapons safely.
--
-- The game finds a weapon's properties in a table it builds at startup (weapon -> properties),
-- which only has the game's own weapons. FC4Hook adds the Workshop weapons to it
-- (ModWorkshopRegister) when you enter the world and before every give, and loads their
-- properties and pickup archetypes first (spawned far below the map, kept alive).
--   FC4Workshop.give(itemId, name)   FC4Workshop.preloadAll()

FC4Workshop = FC4Workshop or {}
local W = FC4Workshop
W.INVALID = "18446744073709551615"
W.loaded = W.loaded or {}      -- archetype id -> entity id holding it in memory
W.jobs = W.jobs or {}
W.byItem = W.byItem or {}

function W.refresh()
  W.byItem = {}
  local s = (type(ModWorkshopList) == "function") and ModWorkshopList() or ""
  for entry in string.gmatch(s or "", "[^;]+") do
    local item, props, pickup = string.match(entry, "(%d+),(%d+),(%d+)")
    if item then W.byItem[item] = { props = props, pickup = pickup } end
  end
end

-- one archetype into memory: spawn it 2 km below you; nil until the game has loaded it
function W.hold(id)
  if not id or id == "0" then return true end
  if W.loaded[id] then return true end
  local C = FC4Cheats
  local x, y, z = 0, 0, 0
  if C and C.pos then x, y, z = C.pos() end
  if not x then return false end
  local ok, e = pcall(SpawnEntityFromArchetype_Legacy, id, x, y, (z or 0) - 2000, 0, 0, 0)
  if ok and e ~= nil and e ~= 0 and tostring(e) ~= W.INVALID then
    W.loaded[id] = tostring(e)
    ModLog("Workshop: archetype " .. id .. " loaded")
    return true
  end
  return false
end

function W.ready(item)
  local w = W.byItem[item]
  if not w then return true end
  return W.loaded[w.props] ~= nil and (w.pickup == "0" or W.loaded[w.pickup] ~= nil)
end

-- queue: keep trying to load the parts, then run job.done
function W.when(item, done, name)
  W.jobs[#W.jobs + 1] = { item = item, done = done, name = name, first = os.clock(), at = 0 }
end

function W.register()
  if type(ModWorkshopRegister) == "function" then
    local ok, n = pcall(ModWorkshopRegister)
    return ok and tonumber(n) or -1
  end
  return -1
end

function W.give(item, name)
  item = tostring(item)
  W.refresh()
  W.register()
  if not W.byItem[item] or W.ready(item) then return W.armoryGive(item, name) end
  ModSetState("workshop.status", "loading " .. tostring(name or item) .. "...")
  W.when(item, function() W.armoryGive(item, name) ModSetState("workshop.status", "gave " .. tostring(name or item)) end, name)
end

function W.preloadAll()
  W.refresh()
  for item, _ in pairs(W.byItem) do
    if not W.ready(item) then W.when(item, nil, item) end
  end
end

function W.tick()
  local me = FC4.me and FC4.me()
  local inWorld = me ~= nil and FC4Cheats and FC4Cheats.pos and FC4Cheats.pos() ~= nil
  if not inWorld then W.inWorld = false W.loaded = {} W.regOk = false return end
  if not W.inWorld then W.inWorld = true W.regOk = false W.regTries = 0 W.regAt = 0 W.preloadAll() end
  -- the game fills its weapon table while the world loads: keep trying for a minute
  if not W.regOk and W.regTries < 30 and os.clock() >= (W.regAt or 0) then
    W.regTries = W.regTries + 1
    W.regAt = os.clock() + 2
    local n = 0
    for _ in pairs(W.byItem) do n = n + 1 end
    if n == 0 or W.register() >= n then W.regOk = true end
  end
  if #W.jobs == 0 then return end
  local now = os.clock()
  local keep = {}
  for _, j in ipairs(W.jobs) do
    if now >= j.at then
      j.at = now + 0.2
      local w = W.byItem[j.item]
      local done = not w or (W.hold(w.props) and W.hold(w.pickup))
      if done then
        if j.done then pcall(j.done) end
      elseif now - j.first > 15 then
        ModLogWarn("Workshop: " .. tostring(j.name) .. " didn't load in 15 s (use Check files in the Workshop tab)")
        ModSetState("workshop.status", tostring(j.name) .. " didn't load: use Check files")
      else
        keep[#keep + 1] = j
      end
    else
      keep[#keep + 1] = j
    end
  end
  W.jobs = keep
end

-- every give goes through here, so the Armory tab is safe too
if FC4Armory and FC4Armory.give and not W.armoryGive then
  W.armoryGive = FC4Armory.give
  FC4Armory.give = function(id, name) return W.give(id, name) end
end
W.armoryGive = W.armoryGive or function() end

FC4Ticks.workshop = W.tick
W.refresh()
ModLog("FC4Workshop loaded")
