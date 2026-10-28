-- Dumps every function the game exposes to Lua into fc4hook\dumps\natives.txt
-- Lines are "C Name" (engine/C++ native) or "L Name" (written in Lua).
-- Written in small appended chunks so one huge string never has to survive
-- the trip through the game's Lua string/concat code in one piece.

local out, seen = {}, {}

local function kind(f)
  if debug and debug.getinfo then
    local ok, info = pcall(debug.getinfo, f, "S")
    if ok and info then return info.what == "C" and "C" or "L" end
  end
  return "?"
end

local function sortedKeys(t)
  local keys = {}
  for k in pairs(t) do if type(k) == "string" then keys[#keys + 1] = k end end
  table.sort(keys)
  return keys
end

local function dump(prefix, t, depth)
  if seen[t] then return end
  seen[t] = true
  for _, k in ipairs(sortedKeys(t)) do
    local v = rawget(t, k)
    if type(v) == "function" then
      out[#out + 1] = kind(v) .. " " .. prefix .. k
    elseif type(v) == "table" and depth < 2 and k ~= "_G" and k ~= "package" then
      dump(prefix .. k .. ".", v, depth + 1)
    end
  end
end

dump("", _G, 0)

local path = "dumps/natives.txt"
ModWriteFile(path, "")                       -- truncate
local CHUNK, written, bytes = 200, 0, 0
for i = 1, #out, CHUNK do
  local last = math.min(i + CHUNK - 1, #out)
  local piece = table.concat(out, "\n", i, last) .. "\n"
  ModWriteFile(path, piece, true)             -- append
  written = last
  bytes = bytes + string.len(piece)
end

-- also a short summary of where things live, so we can sanity-check the file
local groups = {}
for _, line in ipairs(out) do
  local top = string.match(line, "^%a (%w+)[%._]") or string.match(line, "^%a (%w+)$") or "?"
  groups[top] = (groups[top] or 0) + 1
end
local names = {}
for k in pairs(groups) do names[#names + 1] = k end
table.sort(names, function(a, b) return groups[a] > groups[b] end)
local summary = {}
for i = 1, math.min(25, #names) do summary[#summary + 1] = names[i] .. "=" .. string.format("%d", groups[names[i]]) end

ModLog(string.format("Dumped %d functions, %d bytes in %d chunks -> fc4hook\\dumps\\natives.txt",
  written, bytes, math.ceil(#out / CHUNK)))
ModLog("Biggest groups: " .. table.concat(summary, ", "))
