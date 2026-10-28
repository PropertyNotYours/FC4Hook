-- Mem: read and write the game's memory from Lua (wraps the Mem* functions FC4Hook adds).
--
-- Far Cry 4's Lua numbers are 32-bit floats: only integers up to 16,777,216 are exact.
-- So addresses are always STRINGS, and 32/64-bit integers come back as strings too
-- (Mem.int() turns small ones into numbers for you).
--
-- Addresses can be Cheat Engine style expressions (numbers are hex, 0x optional):
--   "FC64+2DD8958"            module + offset
--   "[FC64+2DD8958]+8"        read the pointer at FC64+2DD8958, then add 8
--   "0x7FF6A1B20000"          absolute
--
-- Examples (Console tab):
--   =Mem.player()                                   -> "0x1F3A..." your pawn object
--   =Mem.read(Mem.off(Mem.player(), "0x10"), "ptr") -> a pointer inside it
--   Mem.dump(Mem.player(), 1024, "dumps/player.txt")
--   Mem.freeze("[FC64+2DD8958]+1C", "f32", 100)    -- keep a value pinned (like Cheat Engine)
--   =Mem.scan("48 8B 05 ?? ?? ?? ?? 48 8B 40 08 C3")
--
-- Types: i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 ptr bool
-- Every call returns nil if the address isn't readable/writable; Mem.err() says why.

Mem = Mem or {}
local M = Mem
M.frozen = M.frozen or {}

function M.err() return MemError() end
function M.addr(e) return MemAddress(e) end
function M.module(name) return MemModule(name or "FC64.dll") end
function M.base(name)
  local m = MemModule(name or "FC64.dll")
  return m and m.base
end

function M.read(a, t) return MemRead(a, t or "u32") end
function M.write(a, t, v) return MemWrite(a, t, v) end

-- Readers by type. Integers wider than 16 bits are strings; int()/uint() give numbers.
function M.u8(a) return MemRead(a, "u8") end
function M.i16(a) return MemRead(a, "i16") end
function M.u32(a) return MemRead(a, "u32") end
function M.i32(a) return MemRead(a, "i32") end
function M.u64(a) return MemRead(a, "u64") end
function M.f32(a) return MemRead(a, "f32") end
function M.f64(a) return MemRead(a, "f64") end
function M.ptr(a) return MemRead(a, "ptr") end
function M.bool(a) return MemRead(a, "bool") end
function M.int(a) return tonumber(MemRead(a, "i32")) end
function M.uint(a) return tonumber(MemRead(a, "u32")) end

function M.str(a, max) return MemReadString(a, max or 256) end
function M.wstr(a, max) return MemReadString(a, max or 256, "utf16") end
function M.bytes(a, n) return MemReadBytes(a, n or 16) end
function M.patch(a, hex) return MemWriteBytes(a, hex) end

function M.scan(pattern, module) return MemScan(pattern, module) end
function M.rip(a, dispOffset, len) return MemRip(a, dispOffset or 3, len or 7) end
function M.off(a, ...) return MemOffset(a, ...) end
function M.chain(base, ...) return MemPointer(base, ...) end
function M.ok(a, n) return MemReadable(a, n or 1) end
function M.dump(a, n, file) return MemDump(a, n or 256, file or "dumps/memdump.txt") end
function M.native(name) return MemNative(name) end
function M.entity(id) return MemEntity(tostring(id)) end
function M.alloc(n) return MemAlloc(n) end
function M.call(fn, a, b, c, d) return MemCall(fn, a, b, c, d) end   -- ADVANCED: can crash the game

-- Your own pawn (player object) in memory.
function M.player()
  local ok, id = pcall(GetLocalPlayerId)
  if not ok or id == nil then return nil end
  return MemEntity(tostring(id))
end

-- Freeze: re-write a value every frame. The address expression is re-evaluated each time,
-- so pointer chains keep working when the game moves things around.
function M.freeze(expr, t, v)
  M.frozen[expr] = { t = t or "u32", v = v }
  ModLog("Mem: frozen " .. expr .. " = " .. tostring(v))
end

function M.unfreeze(expr)
  if expr then M.frozen[expr] = nil else M.frozen = {} end
end

function M.tick()
  for expr, f in pairs(M.frozen) do
    if not MemWrite(expr, f.t, f.v) then f.misses = (f.misses or 0) + 1 end
  end
end

FC4Ticks.memory = M.tick
