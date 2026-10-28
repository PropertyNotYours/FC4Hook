-- Safe way to learn what a native returns before you know its arguments.
-- Edit NAME, run it from the Scripts tab, read the result in the Console tab.
local NAME = "GetTimeOfDay"

local f = _G[NAME]
if type(f) ~= "function" then
  ModLog(NAME .. " is not a global function")
else
  local results = { pcall(f) }
  local ok = table.remove(results, 1)
  if not ok then
    ModLog(NAME .. "() raised: " .. tostring(results[1]))
  else
    local parts = {}
    for i = 1, #results do parts[#parts + 1] = type(results[i]) .. ":" .. tostring(results[i]) end
    ModLog(NAME .. "() -> " .. (#parts > 0 and table.concat(parts, ", ") or "(nothing)"))
  end
end
