-- FC4Hook Armory (F8 > Armory): puts any weapon straight into your inventory.
--
-- Weapons are given by template (archetype) id with the game's own
--   CAPI_Pawn.SpawnWeapon(player, id)  +  CAPI_Pawn.AddWeapon(player, weapon, true)
-- The game streams weapon templates it hasn't used yet in the background, so a give that
-- isn't ready is retried every half second for ~8 s. Gives are queued and handed out one
-- at a time, so "Give whole category" doesn't stall the game.
--
-- The list itself is read by the menu from fc4hook\armory\*.txt and fc4hook\mods\*\armory.txt.

FC4Armory = FC4Armory or {}
local A = FC4Armory
A.queue = A.queue or {}
A.retryEvery = 0.5
A.maxTries = 16
A.INVALID = "18446744073709551615"

function A.status(job, s)
  ModSetState("armory." .. job.id, s)
end

function A.spawn(job)
  local me = FC4.me()
  if me == nil then return false end
  local ok, w = pcall(CAPI_Pawn.SpawnWeapon, me, job.id)
  if not ok or w == nil or tostring(w) == A.INVALID or w == 0 then return false end
  local ok2, err = pcall(CAPI_Pawn.AddWeapon, me, w, true)
  A.given = A.given or {}
  A.given[tostring(w)] = tostring(job.id)          -- weapon entity -> item id (per-weapon animations)
  if ok2 then
    A.status(job, "ok")
    ModLog("Armory: gave you " .. job.name)
  else
    A.status(job, "failed")
    ModLogWarn("Armory: AddWeapon failed for " .. job.name .. ": " .. tostring(err))
  end
  return true
end

function A.give(id, name)
  if type(CAPI_Pawn) ~= "table" or type(CAPI_Pawn.SpawnWeapon) ~= "function" then
    ModLogError("Armory: CAPI_Pawn.SpawnWeapon isn't available in this game build")
    return
  end
  local job = { id = tostring(id), name = name or tostring(id), tries = 0, nextAt = 0 }
  A.status(job, "queued")
  A.queue[#A.queue + 1] = job
end

function A.clear()
  for _, job in ipairs(A.queue) do A.status(job, "") end
  A.queue = {}
  ModLog("Armory: queue cleared")
end

function A.afterGive()
  if A.refillAfter and FC4Cheats and FC4Cheats.refill then pcall(FC4Cheats.refill) end
end

function A.step(job, now)
  if A.spawn(job) then return true end
  job.tries = job.tries + 1
  job.nextAt = now + A.retryEvery
  if job.tries == 1 then A.status(job, "loading") end
  if job.tries >= A.maxTries then
    A.status(job, "failed")
    ModLogWarn("Armory: the game never loaded " .. job.name .. " (" .. job.id .. ")." ..
      " Custom weapons need their mod files installed; DLC weapons need the DLC.")
    return true
  end
  return false
end

-- One job per tick: the first that's due. Finished jobs leave the queue.
function A.tick()
  if #A.queue == 0 then return end
  local now = os.clock()
  for i, job in ipairs(A.queue) do
    if now >= job.nextAt then
      if A.step(job, now) then
        table.remove(A.queue, i)
        if #A.queue == 0 then A.afterGive() end
      end
      ModSetState("armory.queue", string.format("%d", #A.queue))
      return
    end
  end
end

A.refillAfter = A.refillAfter ~= false
FC4Ticks.armory = A.tick
ModSetState("armory.queue", "0")
ModLog("FC4Armory loaded")
