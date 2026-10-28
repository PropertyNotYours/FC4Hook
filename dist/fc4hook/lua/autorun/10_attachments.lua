-- FC4Hook attachments (F8 > Armory > Attachments). Lua 5.4, because it works with 64-bit
-- engine pointers.
--
-- How it works (from the game's own AddWeaponAttachment code, FC64.dll patch 1.10):
--   weapon component = GetComponent(weapon entity, WeaponComponentType + 0x20)
--   data  = [component + 0xA0]
--   table = [[data + 0x68] + 0x1008], count [[data + 0x68] + 0x1010]   attachments this gun supports
--   fitted = [data + 0x468], count [data + 0x470]                     attachments it has on
--   each entry is 0x100 bytes; +0x80 = name hash, +0x88 = attachment archetype id
-- Fitting calls the same internal function the game uses (FC64+0xD93E60) with the entry;
-- if the gun's attachment limit blocks it, it calls again with the limit check off.
-- Names come from armory\attachment_names.txt (archetype id = name), from the shop data.

FC4A = FC4A or {}
local A = FC4A
local base = mem.module("FC64.dll")
local OFF = { compType = 0x8cf8c0, getComp = 0x724580, fit = 0xd93e60, release = 0x91160,
              capacity = 0xd2b490 }

function A.loadNames()
  A.names = {}
  local txt = fc4.read_file("armory/attachment_names.txt") or ""
  for id, name in txt:gmatch("(%d+)%s*=%s*([^\r\n]+)") do A.names[tonumber(id)] = name end
end

-- weapon component of the gun in your hands (or nil, reason)
function A.component()
  if not game.ready() then return nil, "load into the world first" end
  local me = game.player()
  local ok, wid = game.pcall("CAPI_Pawn.GetEquippedWeaponEntityID", me)
  wid = ok and tostring(wid) or nil
  if not wid or wid == "18446744073709551615" then return nil, "you're not holding a weapon" end
  local ent = mem.entity(wid)
  if not ent then return nil, "weapon not loaded" end
  local td = mem.call(base + OFF.compType)
  local comp = mem.call(base + OFF.getComp, ent, td + 0x20)
  if comp == 0 or not mem.readable(comp + 0xa0, 8) then return nil, "that item has no attachments" end
  return comp, wid
end

function A.tables(comp)
  local data = mem.ptr(comp + 0xa0)
  if not data or not mem.readable(data + 0x68, 8) or not mem.readable(data + 0x468, 16) then return nil end
  local props = mem.ptr(data + 0x68)
  if not props or not mem.readable(props + 0x1008, 16) then return nil end
  local t = { arr = mem.ptr(props + 0x1008), n = mem.u32(props + 0x1010),
              fitted = mem.ptr(data + 0x468), nf = mem.u32(data + 0x470) }
  if t.n > 64 or t.nf > 64 then return nil end
  return t
end

function A.isFitted(t, arch)
  for j = 0, t.nf - 1 do
    if mem.u64(t.fitted + j * 0x100 + 0x88) == arch then return true end
  end
  return false
end

function A.publish(list, status)
  fc4.set_state("att.n", tostring(#list))
  for i, e in ipairs(list) do fc4.set_state("att." .. i, e.label .. "|" .. (e.on and "1" or "0")) end
  fc4.set_state("att.status", status or "")
end

function A.scan()
  if not A.names then A.loadNames() end
  local comp, wid = A.component()
  A.list, A.comp, A.weapon = {}, nil, wid
  if not comp then A.publish({}, wid) return end
  local t = A.tables(comp)
  if not t then A.publish({}, "this weapon takes no attachments") return end
  for i = 0, t.n - 1 do
    local e = t.arr + i * 0x100
    local arch = mem.u64(e + 0x88)
    A.list[#A.list + 1] = { entry = e, arch = arch, on = A.isFitted(t, arch),
                            label = A.names[arch] or string.format("Attachment %d (%d)", i + 1, arch) }
  end
  A.comp = comp
  A.publish(A.list, #A.list == 0 and "this weapon takes no attachments" or "")
end

-- One call into the game's fit routine. limit=1 is what the game's own AddWeaponAttachment
-- passes: it refuses when the gun's attachment capacity (weapon stat 0x1D) is used up, and
-- many regular guns (AK-47, Skorpion, M133...) report a capacity of 0 there. limit=0 skips
-- that check; the routine still swaps out a fitted attachment of the same kind (sight for
-- sight) first.
function A.callFit(e, limit)
  A.out = A.out or mem.alloc(16)
  mem.set_u64(A.out, 0)
  mem.call(base + OFF.fit, A.comp, A.out, e.entry, limit, 0, 0)
  local h = mem.u64(A.out)
  local spawned = false
  if h and h ~= 0 and mem.readable(h + 8, 16) then          -- release the handle it hands back
    spawned = (mem.u64(h + 0x10) or 0) ~= 0
    local rc = mem.i32(h + 8) - 1
    mem.set_i32(h + 8, rc)
    if rc == 0 then mem.call(base + OFF.release, h) end
  end
  A.scan()
  return A.list[e.i] and A.list[e.i].on, spawned
end

-- Fit entry i (1-based) on the gun in your hands.
function A.fit(i)
  A.scan()
  local e = A.list[i]
  if not e or not A.comp then fc4.warn("Attachments: hold the gun and try again") return end
  if e.on then fc4.log("Attachments: " .. e.label .. " is already fitted") return end
  e.i = i
  local cap = mem.call(base + OFF.capacity, A.comp) or -1
  local ok, spawned = A.callFit(e, 1)
  if not ok then
    fc4.debug(string.format("Attachments: normal fit refused (capacity %d, object %s), forcing",
                            cap, spawned and "made" or "not made"))
    if A.list[i] then A.list[i].i = i; ok, spawned = A.callFit(A.list[i], 0) end
  end
  if ok then
    fc4.log("Attachments: " .. e.label .. " fitted")
  else
    fc4.warn(string.format("Attachments: %s - the game didn't fit it (capacity %d, object %s)",
                           e.label, cap, spawned and "made" or "not made"))
  end
end

-- Remove everything from the gun in your hands (the game's ClearAllWeaponAttachments).
function A.clear()
  local me = game.player()
  local _, wid = A.component()
  if not wid then fc4.warn("Attachments: hold the gun first") return end
  for slot = 0, 11 do
    local ok, id = game.pcall("CAPI_Pawn.GetWeaponIdInSlot", me, slot)
    if ok and tostring(id) == wid then
      game.call("CAPI_Player.ClearAllWeaponAttachments", me, slot)
      fc4.log("Attachments: removed all")
      A.scan()
      return
    end
  end
  fc4.warn("Attachments: couldn't find that weapon in your inventory slots")
end

-- keep the list in sync with whatever you're holding (only while the tab asks for it)
fc4.every(1, function()
  if fc4.get_state("att.watch") ~= "1" or not game.ready() then return end
  local ok, wid = game.pcall("CAPI_Pawn.GetEquippedWeaponEntityID", game.player())
  if ok and tostring(wid) ~= A.weapon then A.scan() end
end)
