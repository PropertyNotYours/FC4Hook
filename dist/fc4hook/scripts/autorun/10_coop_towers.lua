-- FC4Hook co-op tower reward fix (v2: instant catch-up).
--
-- Guests are told towers are unlocked but never get the rewards, because their game
-- runs RadioTower_RestoreUnlocked_NoRewards. Two ways to fix that:
--   * tower loaded nearby  -> ModGrantTowerRewards(id) replays the host's full reward step
--   * tower far away       -> grant each reward straight from the global registry with
--                             ModGrantRewardKey(rewardKey, playerId), using the tower's
--                             reward list from coop\tower_reward_table.txt
-- The table is learned automatically whenever a tower is loaded (on any PC), and the
-- same table works for every player, so it can be shared.
--
-- Towers granted the full way are remembered in coop\claimed_towers_<playerId>.txt.

FC4Coop = FC4Coop or {}
local C = FC4Coop
C.interval = 15
C.lastRun = 0
C.tableFile = "coop/tower_reward_table.txt"

-- ------------------------------------------------------------ files
function C.claimFile()
  return "coop/claimed_towers_" .. tostring(GetLocalPlayerId()) .. ".txt"
end

function C.loadSet(file)
  local set = {}
  local data = ModReadFile(file)
  if type(data) == "string" then
    for id in string.gmatch(data, "(%d+)") do set[id] = true end
  end
  return set
end

function C.saveSet(file, set)
  local ids = {}
  for id in pairs(set) do ids[#ids + 1] = id end
  table.sort(ids)
  ModWriteFile(file, table.concat(ids, "\n") .. "\n")
end

-- tower id -> "k1,k2,..."
function C.loadTable()
  local t = {}
  local data = ModReadFile(C.tableFile)
  if type(data) == "string" then
    for id, keys in string.gmatch(data, "(%d+)=([%d,]*)") do t[id] = keys end
  end
  return t
end

function C.saveTable(t)
  local rows = {}
  for id, keys in pairs(t) do rows[#rows + 1] = id .. "=" .. keys end
  table.sort(rows)
  ModWriteFile(C.tableFile, table.concat(rows, "\n") .. "\n")
end

-- ------------------------------------------------------------ game state
function C.unlockedTowers()
  local list = {}
  local raw = ModGetUnlockedTowers()
  if type(raw) == "string" then
    for id, key in string.gmatch(raw, "(%d+):(%d+);") do list[#list + 1] = { id = id, key = key } end
  end
  return list
end

-- Guest = in a session with 2+ players and not the player in slot 0 (the host).
function C.isGuest()
  local okN, n = pcall(GetNumberOfPlayers)
  if not okN or (tonumber(n) or 0) < 2 then return false end
  local okH, host = pcall(GetPlayerInSlot, 0)
  return okH and tostring(host) ~= tostring(GetLocalPlayerId())
end

-- Record reward lists of every unlocked tower that is loaded right now.
function C.learn(towers, t)
  local added = 0
  for _, tw in ipairs(towers) do
    if t[tw.id] == nil then
      local keys = ModTowerRewardKeys(tw.id)
      if type(keys) == "string" then t[tw.id] = keys; added = added + 1 end
    end
  end
  if added > 0 then C.saveTable(t) end
  return added
end

-- Grant one tower's rewards from the table. Returns number granted, or nil if unknown.
function C.grantFromTable(keys, me, s)
  local granted = 0
  for k in string.gmatch(keys, "(%d+)") do
    local r = ModGrantRewardKey(k, me)
    if r == "ok" then granted = granted + 1
    elseif r ~= "owned" then
      s.failed = s.failed + 1
      ModLogDebug("  reward " .. k .. ": " .. tostring(r))
    end
  end
  return granted
end

-- ------------------------------------------------------------ modes
function C.handleTower(mode, tw, ctx, s)
  local id = tw.id
  if mode == "mark" then
    if not ctx.claimed[id] then ctx.claimed[id] = true; s.done = s.done + 1 end
    return false
  end
  if mode == "claim" and ctx.claimed[id] then
    s.skipped = s.skipped + 1
    return false
  end
  local r = ModGrantTowerRewards(id)
  if r == "ok" then
    ctx.claimed[id] = true
    s.done = s.done + 1
    ModLog("Tower rewards granted for tower " .. id .. " (full)")
    return mode == "test"
  end
  if r == "entity has no tower component" then
    ctx.claimed[id] = true
    s.notTower = s.notTower + 1
  elseif r == "tower entity not loaded" then
    C.handleFar(mode, tw, ctx, s)
  else
    s.failed = s.failed + 1
    ModLogDebug("  tower " .. id .. ": " .. tostring(r))
  end
  return false
end

-- Tower not loaded: grant from the reward table if we know its list.
function C.handleFar(mode, tw, ctx, s)
  local keys = ctx.rewards[tw.id]
  if mode == "test" or keys == nil then
    s.notLoaded = s.notLoaded + 1
    return
  end
  local n = C.grantFromTable(keys, ctx.me, s)
  if n > 0 then
    s.done = s.done + 1
    ModLog("Tower rewards granted for tower " .. tw.id .. " (" .. string.format("%d", n) .. " from table)")
  else
    s.skipped = s.skipped + 1
  end
end

function C.report(mode, total, s)
  local verb = "granted"
  if mode == "mark" then verb = "marked" end
  local text = string.format("Towers (%s): %d unlocked, %d %s, %d already had everything", mode, total, s.done, verb, s.skipped)
  text = text .. string.format(", %d far away with no reward list yet, %d not towers, %d failed", s.notLoaded, s.notTower, s.failed)
  ModLog(text)
end

function C.inspect(towers)
  ModWriteFile("dumps/tower_inspect.txt", "")
  local loaded = 0
  for _, tw in ipairs(towers) do
    if ModInspectTower(tw.id, tw.key) == "loaded" then loaded = loaded + 1 end
  end
  ModLog(string.format("Inspected %d towers (%d loaded nearby) -> dumps\\tower_inspect.txt", #towers, loaded))
end

function C.run(mode, quiet)
  local towers = C.unlockedTowers()
  local ctx = { claimed = C.loadSet(C.claimFile()), rewards = C.loadTable(), me = tostring(GetLocalPlayerId()) }
  local learned = C.learn(towers, ctx.rewards)
  if learned > 0 then ModLog(string.format("Learned reward lists for %d towers -> coop\\tower_reward_table.txt", learned)) end
  if mode == "inspect" then C.inspect(towers) return end
  if mode == "learn" then
    if quiet then return end
    local n = 0
    for _ in pairs(ctx.rewards) do n = n + 1 end
    ModLog(string.format("Reward table now has %d of %d unlocked towers", n, #towers))
    return
  end
  local s = { done = 0, skipped = 0, notLoaded = 0, notTower = 0, failed = 0 }
  for _, tw in ipairs(towers) do
    if C.handleTower(mode, tw, ctx, s) then break end
  end
  if mode ~= "test" then C.saveSet(C.claimFile(), ctx.claimed) end
  if not quiet or s.done > 0 or s.failed > 0 then C.report(mode, #towers, s) end
end

-- ------------------------------------------------------------ auto mode
-- Everyone learns reward lists as they pass towers; guests also auto-claim.
function C.tickFn()
  local now = os.clock()
  if now - C.lastRun < C.interval then return end
  C.lastRun = now
  local mode = "learn"
  if C.isGuest() then mode = "claim" end
  local ok, err = pcall(C.run, mode, true)
  if not ok then ModLogWarn("coop tower auto " .. mode .. " error: " .. tostring(err)) end
end
C.prevTick = nil
-- Shared tick registry (safe to reload; other autorun scripts add their own entries).
FC4Ticks = FC4Ticks or {}
FC4Ticks.coop = C.tickFn

ModLog("FC4Coop tower fix v2 loaded (learns reward lists; guests auto-claim every 15s)")
