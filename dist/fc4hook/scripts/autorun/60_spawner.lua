-- FC4Hook Spawner + Animation tools (F8 > Developer > Spawner / Animation).
--
-- Spawning uses the game's own factory, so the engine builds the object from its archetype
-- with its native classes and components:
--   SpawnEntityFromArchetype_Legacy(archetypeId, x, y, z, rx, ry, rz) -> entity id
-- Any archetype works: weapons (ids in armory\weapons.txt), props, vehicles, animals, NPCs,
-- and NEW archetypes a mod adds as  files\entityarchetypeslibrary\<id>.ark.fcb  (loose files,
-- see fc4hook\mods\README.txt). The game loads archetypes in the background the first time,
-- so a spawn that isn't ready yet is retried every half second for ~8 s.
--   CAPI_Entity.RemoveEntity(id) removes what you spawned.
--
-- Animation goes through the game's animation system (no skeleton hacks):
--   CAPI_Entity.AttachAnimationPackage(entity, path) / DetachAnimationPackage(entity, path)
--     adds an animation package (also one shipped as a loose file) to an entity
--   CAPI_Pawn.PlayAnimation(pawn, path, layer, loop) / ForcePlayAnimation(...)
--   CAPI_Player.StartPlayAnim(player, name, b1, b2) -> ok / StopPlayAnim(player)
--   CAPI_Pawn.GetCurrentAnimName(pawn, layer) -> name
-- Lua:  FC4Spawn.spawn("7602852352217")  FC4Spawn.removeAll()
--       FC4Anim.play("me", "path", 0, false)   FC4Anim.attach("last", "path")

FC4Spawn = FC4Spawn or {}
local S = FC4Spawn
S.list = S.list or {}
S.queue = S.queue or {}
S.INVALID = "18446744073709551615"
S.dist = S.dist or 4

-- a point S.dist metres in front of you, at your feet
function S.front()
  local me = FC4.me()
  local C = FC4Cheats
  if me == nil or not C or not C.pos then return nil end
  local x, y, z = C.pos()
  if not x then return nil end
  local yaw = (C.yaw and C.yaw(me)) or 0
  return x - math.sin(yaw) * S.dist, y + math.cos(yaw) * S.dist, z
end

function S.publish(msg)
  ModSetState("spawn.n", tostring(#S.list))
  ModSetState("spawn.last", tostring(S.list[#S.list] or ""))
  if msg then ModSetState("spawn.status", msg) end
end

function S.try(job)
  local ok, e = pcall(SpawnEntityFromArchetype_Legacy, job.id, job.x, job.y, job.z, 0, 0, 0)
  if not ok then
    S.publish("failed: " .. tostring(e))
    ModLogWarn("Spawner: " .. tostring(e))
    return true
  end
  if e == nil or tostring(e) == S.INVALID or e == 0 then return false end
  S.list[#S.list + 1] = tostring(e)
  S.publish("spawned " .. job.name .. " (entity " .. tostring(e) .. ")")
  ModLog("Spawner: spawned " .. job.name .. " -> entity " .. tostring(e))
  return true
end

function S.spawn(id, name)
  if type(SpawnEntityFromArchetype_Legacy) ~= "function" then
    S.publish("SpawnEntityFromArchetype_Legacy isn't available") return
  end
  local x, y, z = S.front()
  if not x then S.publish("load into the world first") return end
  local job = { id = tostring(id), name = name or tostring(id), x = x, y = y, z = z, tries = 0, at = 0 }
  S.queue[#S.queue + 1] = job
  S.publish("spawning " .. job.name .. "...")
end

function S.step(job, now)
  if S.try(job) then return true end
  job.tries = job.tries + 1
  job.at = now + 0.5
  if job.tries < 16 then return false end
  S.publish("gave up: archetype " .. job.id .. " didn't load (wrong id, or its files are missing)")
  ModLogWarn("Spawner: archetype " .. job.id .. " never loaded")
  return true
end

function S.tick()
  local job = S.queue[1]
  if not job then return end
  local now = os.clock()
  if now < job.at then return end
  if S.step(job, now) then table.remove(S.queue, 1) end
end

function S.remove(id)
  return pcall(CAPI_Entity.RemoveEntity, id)
end

function S.removeLast()
  local id = table.remove(S.list)
  if id then S.remove(id) end
  S.publish(id and ("removed " .. id) or "nothing to remove")
end

function S.removeAll()
  local n = #S.list
  for i = n, 1, -1 do S.remove(S.list[i]) end
  S.list = {}
  S.publish("removed " .. n .. " spawned entities")
end

-- ------------------------------------------------------------ animation
FC4Anim = FC4Anim or {}
local N = FC4Anim

-- "me" = you, "last" = the last thing you spawned, anything else = an entity id
function N.target(t)
  if t == nil or t == "" or t == "me" then return FC4.me() end
  if t == "last" then return S.list[#S.list] end
  return t
end

function N.result(what, ok, r)
  local msg = what .. (ok and " -> ok" or (" failed: " .. tostring(r)))
  if ok and r ~= nil then msg = what .. " -> " .. tostring(r) end
  ModSetState("anim.status", msg)
  ModLog("Animation: " .. msg)
end

function N.attach(t, path)
  N.result("attach " .. path, pcall(CAPI_Entity.AttachAnimationPackage, N.target(t), path))
end

function N.detach(t, path)
  N.result("detach " .. path, pcall(CAPI_Entity.DetachAnimationPackage, N.target(t), path))
end

function N.play(t, path, layer, loop, force)
  local f = force and CAPI_Pawn.ForcePlayAnimation or CAPI_Pawn.PlayAnimation
  N.result((force and "force play " or "play ") .. path, pcall(f, N.target(t), path, layer or 0, loop == true))
end

function N.start(name, b1, b2)
  N.result("StartPlayAnim " .. name, pcall(CAPI_Player.StartPlayAnim, FC4.me(), name, b1 == true, b2 == true))
end

function N.stop()
  N.result("StopPlayAnim", pcall(CAPI_Player.StopPlayAnim, FC4.me()))
end

function N.current(t, layer)
  local ok, r = pcall(CAPI_Pawn.GetCurrentAnimName, N.target(t), layer or 0)
  ModSetState("anim.current", ok and tostring(r) or ("? " .. tostring(r)))
end

FC4Ticks.spawner = S.tick
S.publish()
ModLog("FC4Spawn/FC4Anim loaded")
