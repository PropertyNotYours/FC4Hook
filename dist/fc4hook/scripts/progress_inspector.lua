-- Snapshot of this player's progress as the game sees it (v2.2).
-- Run it on BOTH PCs during the same co-op session (host and guest), then compare.
-- Output: fc4hook\dumps\progress_<host|guest|solo>.txt
--
-- Note: FC4's Lua build has a much lower per-function register limit than stock
-- Lua 5.1, so every section lives in its own small function.

local P = { lines = {} }

function P.out(fmt, ...)
  P.lines[#P.lines + 1] = string.format(fmt, ...)
end

function P.try(f, a, b)
  if type(f) ~= "function" then return "n/a" end
  local ok, r1, r2 = pcall(f, a, b)
  if not ok then return "ERR:" .. tostring(r1) end
  if r2 ~= nil then return tostring(r1) .. "," .. tostring(r2) end
  return tostring(r1)
end

function P.num(s)
  return string.format("%d", tonumber(s) or -1)
end

function P.header()
  P.me = GetLocalPlayerId()
  P.nplayers = tonumber(P.try(GetNumberOfPlayers)) or 0
  local isServer = P.try(Game and Game.IsServer)
  if P.nplayers < 2 then P.role = "solo"
  elseif isServer == "true" then P.role = "host"
  else P.role = "guest" end
  P.out("== FC4Hook progress snapshot v2.2 (%s) %s", P.role, os.date())
  P.out("me=%s players=%d Game.IsServer=%s mode=%s", tostring(P.me), P.nplayers, isServer, P.try(GetCurrentGameModeName))
  for s = 0, 3 do
    P.out("  slot %d -> %s", s, P.try(GetPlayerInSlot, s))
  end
end

function P.stat(label, f)
  P.out("  %-14s %s", label, P.num(P.try(f, P.me)))
end

function P.stats()
  P.out("")
  P.out("== Player stats (local)")
  P.stat("money", CAPI_Player.GetPlayerMoney)
  P.stat("karmaLevel", CAPI_Player.GetPlayerKarmaLevel)
  P.stat("karmaPoints", CAPI_Player.GetPlayerTotalKarmaPoints)
  P.stat("weaponSlots", CAPI_Player.GetNumWeaponSlotsUnlocked)
  P.stat("medpacks", CAPI_Player.GetPlayerMedPackCount)
  P.stat("masks", GetOpenWorldDemonicMaskCollected)
  P.stat("posters", GetOpenWorldPropagandaPosterCollected)
  P.stat("letters", GetOpenWorldLieutenantLetterCollected)
  P.stat("thangkas", GetOpenWorldThangkaCollected)
  P.stat("propaganda", GetNumberOfDestroyedPropaganda)
end

-- ---------------------------------------------------------------- missions
P.cands = {}

function P.addName(s)
  local u = string.upper(s)
  if string.len(u) < 48 and string.find(u, "^[A-Z][A-Z0-9]*_[A-Z0-9_]+$") then P.cands[u] = true end
end

function P.isMissionish(u)
  return string.find(u, "_MAIN$") or string.find(u, "_BRIEF$") or string.find(u, "^MIS_")
      or string.find(u, "^GIV_") or string.find(u, "^SQ") or string.find(u, "^ULC_")
end

function P.namesFromEntity(ent)
  local name = string.lower(tostring(CAPI_Entity.GetName(ent) or "")) .. "\\"
  local rel = ""
  for s in string.gmatch(name, "(.-)\\") do rel = s end
  P.addName(string.match(rel, "([^.]*)") or rel)
end

function P.namesFromFunction(f)
  local ok, bc = pcall(string.dump, f)
  if not ok or not bc then return end
  for s in string.gmatch(bc, "[%a][%w_]+") do
    if P.isMissionish(string.upper(s)) then P.addName(s) end
  end
end

function P.collectMissionNames()
  for _, ent in pairs(GetEntitiesStartsWith("DominoOmniEntity") or {}) do P.namesFromEntity(ent) end
  if type(Boxes) ~= "table" then return end
  for _, box in pairs(Boxes) do
    if type(box) == "table" then
      for _, f in pairs(box) do
        if type(f) == "function" then P.namesFromFunction(f) end
      end
    end
  end
end

function P.missionState(mm, name)
  local okId, id = pcall(function() return mm:GetMissionIdFromScriptName(name) end)
  if not okId or id == nil then return nil end
  local okSt, st = pcall(function() return mm:GetMissionState(id) end)
  if not okSt then return "ERR:" .. tostring(st) end
  return tostring(st)
end

function P.missions()
  P.out("")
  P.out("== Missions known to CMissionManager")
  P.collectMissionNames()
  local mm = CMissionManager.GetInstance()
  local rows, counts, n = {}, {}, 0
  for name in pairs(P.cands) do
    n = n + 1
    local st = P.missionState(mm, name)
    if st and st ~= "Invalid" then
      rows[#rows + 1] = string.format("  %-40s %s", name, st)
      counts[st] = (counts[st] or 0) + 1
    end
  end
  table.sort(rows)
  for _, l in ipairs(rows) do P.out("%s", l) end
  local summary = {}
  for st, c in pairs(counts) do summary[#summary + 1] = st .. "=" .. string.format("%d", c) end
  P.out("  (%d candidates checked; valid: %s)", n, table.concat(summary, " "))
  P.nMissions = #rows
end

-- ------------------------------------------------------------------ towers
function P.towerLine(e, n)
  local u = P.try(IsRadioTowerUnlocked, e)
  local f = P.try(CAPI_Entity.IsRadioTowerFinished, e)
  P.out("  %-50s unlocked=%s finished=%s", n, u, f)
end

function P.isTowerish(low)
  if string.find(low, "bush") then return false end
  return string.find(low, "tower") or string.find(low, "radio") or string.find(low, "bell")
end

function P.towers()
  P.out("")
  P.out("== Entities whose name mentions tower / radio / bell")
  local total, shown = 0, 0
  for _, e in pairs(GetEntitiesStartsWith("") or {}) do
    total = total + 1
    local n = tostring(CAPI_Entity.GetName(e) or "")
    if P.isTowerish(string.lower(n)) then
      shown = shown + 1
      if shown <= 80 then P.towerLine(e, n) end
    end
  end
  P.out("  (%d matching of %d entities scanned)", shown, total)
  P.nTowers = shown
end

function P.unlockedTowers()
  P.out("")
  P.out("== Unlocked towers (read from the open-world manager)")
  if type(ModGetUnlockedTowers) ~= "function" then
    P.out("  needs FC4Hook v0.3.4+")
    return
  end
  local raw = ModGetUnlockedTowers()
  if type(raw) ~= "string" then
    P.out("  could not read the tower list (see fc4hook.log)")
    return
  end
  local list = {}
  for id, key in string.gmatch(raw, "(%d+):(%d+);") do list[#list + 1] = { id = id, key = key } end
  local good = 0
  for i, t in ipairs(list) do
    local check = P.try(IsRadioTowerUnlocked, t.id)
    if check == "true" then good = good + 1 end
    P.out("  %2d  id=%-22s key=%-22s IsRadioTowerUnlocked=%s", i, t.id, t.key, check)
  end
  P.out("  (%d towers listed, %d confirmed by IsRadioTowerUnlocked)", #list, good)
  P.out("  IsRadioTowerUnlocked native at %s", tostring(ModNativeAddress("IsRadioTowerUnlocked")))
  P.nTowers = #list
end

-- -------------------------------------------------------------------- main
function P.section(name, f)
  local ok, err = pcall(f)
  if not ok then P.out("!! section %s failed: %s", name, tostring(err)) end
end

P.section("header", P.header)
P.section("stats", P.stats)
P.section("missions", P.missions)
P.section("towers", P.towers)
P.section("unlockedTowers", P.unlockedTowers)

local role = P.role or "unknown"
ModWriteFile("dumps/progress_" .. role .. ".txt", table.concat(P.lines, "\n") .. "\n")
ModLog("Progress snapshot v2.2 (" .. role .. "): " .. string.format("%d", P.nMissions or 0) .. " missions, "
  .. string.format("%d", P.nTowers or 0) .. " tower-ish entities -> fc4hook\\dumps\\progress_" .. role .. ".txt")
