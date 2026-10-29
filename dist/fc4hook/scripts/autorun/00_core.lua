-- FC4Hook core helpers. Everything in scripts\autorun\ runs (in name order) when the game's
-- Lua comes up, again after each world load, and when you press Reload scripts.
--
-- Functions FC4Hook adds to the game's Lua:
--   ModLog(...), ModLogDebug(...), ModLogWarn(...), ModLogError(...)   -> fc4hook.log + console
--   ModWriteFile(path, text [, append]), ModReadFile(path), ModFileExists(path)   (under fc4hook\)
--   ModSetState(key, value), ModGetState(key)   share values with the menu / C++ plugins
--   ModVersion()                                "1.3.0"
--   ModArchiveStatus()                          state of data_win32\fc4hook_mods.dat
--   ModTeleportEntityPos / ModGetEntityPositionVec3   game natives FC4Hook digs out
--   ...and the tower helpers used by the Co-op and Travel tabs.
-- Everything else (CAPI_Pawn.*, SetTimeOfDay, ...) is the game's own API:
-- turn on DeveloperMode in fc4hook.ini and use Developer > Dump natives to list it.
--
-- NOTE: Far Cry 4's Lua has a much smaller per-function register limit than stock
-- Lua 5.1. Keep functions short with few locals, or the chunk fails to load.

FC4 = FC4 or {}

-- Is a global (or "Table.Field") a callable game function?
function FC4.fn(name)
  local t, k = string.match(name, "^([%w_]+)%.([%w_]+)$")
  if t then
    local tbl = _G[t]
    if type(tbl) == "table" and type(tbl[k]) == "function" then return tbl[k] end
    return nil
  end
  local f = _G[name]
  if type(f) == "function" then return f end
  return nil
end

function FC4.has(name)
  return FC4.fn(name) ~= nil
end

-- Calls a game native safely. Returns ok, result (ok=false if missing or it raised).
function FC4.call(name, ...)
  local f = FC4.fn(name)
  if not f then
    ModLogDebug(name .. " is not available in this game build")
    return false, "missing"
  end
  local ok, r = pcall(f, ...)
  if not ok then ModLogWarn(name .. " failed: " .. tostring(r)) end
  return ok, r
end

function FC4.me()
  local ok, id = pcall(GetLocalPlayerId)
  if ok then return id end
  return nil
end

-- key=value text files under fc4hook\ (used for settings that survive restarts)
function FC4.loadKV(path)
  local t = {}
  local data = ModReadFile(path)
  if type(data) ~= "string" then return t end
  for k, v in string.gmatch(data, "([%w_%.]+)=([^\r\n]*)") do t[k] = v end
  return t
end

function FC4.saveKV(path, t)
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = k end
  table.sort(keys)
  local out = {}
  for _, k in ipairs(keys) do out[#out + 1] = k .. "=" .. tostring(t[k]) end
  ModWriteFile(path, table.concat(out, "\n") .. "\n")
end

-- One OnModTick for everyone: modules put a function in FC4Ticks.<name>.
-- A module whose tick errors is logged and switched off (the others keep running).
FC4Ticks = FC4Ticks or {}
function FC4RunTicks()
  for name, f in pairs(FC4Ticks) do
    local ok, err = pcall(f)
    if not ok then
      ModLogError("tick '" .. tostring(name) .. "' disabled after error: " .. tostring(err))
      FC4Ticks[name] = nil
    end
  end
end
OnModTick = FC4RunTicks

-- Travel/tour code turns god mode off after landing; this keeps it on when the
-- God mode cheat is switched on.
function FC4.releaseGodMode()
  if FC4Cheats and FC4Cheats.isOn and FC4Cheats.isOn("god") then return end
  pcall(EnableGodMode, 0)
end

ModLog("FC4Hook " .. tostring(ModVersion and ModVersion() or "?") .. " scripts loading (" .. tostring(_VERSION) .. ")")

-- ------------------------------------------------------------ Lua 5.4 bridge
-- FC4Hook's Lua 5.4 runtime calls game functions through these. Values travel as
-- source text; anything that isn't a plain value (userdata, functions, deep tables)
-- stays here in FC4Bridge.refs and the 5.4 side gets a handle to it.
FC4Bridge = { refs = {}, n = 0 }
local B = FC4Bridge

function B.ref(v)
  B.n = B.n + 1
  B.refs[B.n] = v
  return string.format("R(%d,%q)", B.n, tostring(v))
end

function B.num(v)
  if v ~= v then return "0/0" end
  if v == math.huge then return "1/0" end
  if v == -math.huge then return "-1/0" end
  return string.format("%.9g", v)
end

function B.tab(t, d)
  local out, c = {}, 0
  for k, v in pairs(t) do
    c = c + 1
    if c > 256 or type(k) == "table" then return B.ref(t) end
    out[c] = "[" .. B.ser(k, d + 1) .. "]=" .. B.ser(v, d + 1)
  end
  return "{" .. table.concat(out, ",") .. "}"
end

function B.ser(v, d)
  local t = type(v)
  if t == "nil" then return "nil" end
  if t == "boolean" then return v and "true" or "false" end
  if t == "number" then return B.num(v) end
  if t == "string" then return string.format("%q", v) end
  if t == "table" and d < 4 then return B.tab(v, d) end
  return B.ref(v)
end

function B.pack(...)
  local out = {}
  for i = 1, select("#", ...) do out[i] = B.ser((select(i, ...)), 0) end
  return "return " .. table.concat(out, ",")
end

-- "CAPI_Pawn.SpawnWeapon" -> the function (any depth)
function B.get(path)
  local v = _G
  for part in string.gmatch(path, "[^%.]+") do
    if type(v) ~= "table" then return nil end
    v = v[part]
  end
  return v
end

function B.call(f, ...)
  if type(f) ~= "function" then return "return false,\"not a function\"" end
  return B.pack(pcall(f, ...))
end

function B.free(n) B.refs[n] = nil end
