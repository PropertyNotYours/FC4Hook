-- FC4Hook Teleports (F8 > Travel): outposts, safe houses and the current objective.
-- The list is built from the game world (entities named SafeHouseEntity_...), so it works
-- on any save; teleports use the same safe single-jump landing as bell tower travel.

FC4Teleports = FC4Teleports or {}
local P = FC4Teleports
P.list = {}

-- "SafeHouseEntity_D3_Outpost_02" -> "Outpost D3 #2";  "SafeHouseEntity_Ajays_House" -> "Ajays House"
function P.label(name)
  local grid, n = string.match(name, "SafeHouseEntity_(%w%d)_[Oo]utpost_(%d+)")
  if grid then return "outpost", string.format("Outpost %s #%d", string.upper(grid), tonumber(n)) end
  local rest = string.match(name, "SafeHouseEntity_(.+)")
  if not rest then return nil end
  if string.match(rest, "^%d+$") then return "safehouse", "Safe house " .. rest end
  rest = string.gsub(rest, "_", " ")
  return "safehouse", rest
end

function P.add(e, out)
  local okn, name = pcall(CAPI_Entity.GetName, e)
  if not okn or type(name) ~= "string" then return end
  local kind, label = P.label(name)
  if not kind then return end
  local okp, p = pcall(GetEntityPositionVec3 or ModGetEntityPositionVec3, e)
  if not okp or type(p) ~= "table" or not tonumber(p[1]) then return end
  out[#out + 1] = { kind = kind, label = label, x = p[1], y = p[2], z = p[3] }
end

function P.publish()
  local rows = {}
  for i, t in ipairs(P.list) do
    rows[i] = string.format("%s|%s|%.1f|%.1f|%.1f", t.kind, t.label, t.x, t.y, t.z)
  end
  ModSetState("tp.list", table.concat(rows, "\n"))
end

function P.refresh()
  local ok, ents = pcall(GetEntitiesStartsWith, "SafeHouseEntity")
  if not ok or type(ents) ~= "table" then ModLogWarn("Teleports: can't list the world's safe houses") return end
  local out = {}
  for _, e in pairs(ents) do P.add(e, out) end
  table.sort(out, function(a, b) if a.kind ~= b.kind then return a.kind < b.kind end return a.label < b.label end)
  P.list = out
  P.publish()
  ModLog(string.format("Teleports: %d outposts and safe houses", #out))
end

function P.go(i)
  local t = P.list[tonumber(i)]
  if not t then ModLogWarn("Teleports: refresh the list first") return end
  if not FC4Travel or not FC4Travel.jump then ModLogError("Teleports: travel script not loaded") return end
  FC4Travel.home = FC4Travel.myPos()
  if FC4Travel.jump(t.x, t.y, t.z + 1.5) then ModLog("Teleports: going to " .. t.label) end
end

pcall(P.refresh)
