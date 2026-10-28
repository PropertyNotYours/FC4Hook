FC4Hook Lua 5.4
===============
FC4Hook runs a stock Lua 5.4 alongside Far Cry 4's own Lua 5.1. Use it for mods that need
real 64-bit integers (ids, addresses), modern Lua, or just more room than the game's Lua
allows. It runs on the game's script thread, so calling game functions is safe and instant.

  lua\autorun\*.lua   run at startup, in name order (Developer > Lua 5.4 > Reload)
  lua\<name>.lua      modules for require("<name>")
  Console tab         switch the box to "Lua 5.4" to type commands; "=expr" prints a value

game.*  (the bridge to the game's Lua)
  game.call("CAPI_Pawn.GetPawnHealth", id)   call a game function, returns its results
  game.CAPI_Pawn.GetPawnHealth(id)           same, shorter
  game.call("Game:Method", ...)              call with the table as self
  game.pcall(name, ...)                      -> ok, results...   (no error raised)
  game.eval("return GetTimeOfDay()")         run game-Lua source
  game.has(name)   game.ready()   game.player()   game.pos([id]) -> x, y, z
  Big integers you pass are sent as decimal strings (how game functions read ids).
  Game values that can't be copied (objects, functions) come back as handles you can pass
  back to game functions; tostring() shows what they are. game.free(handle) releases one.

mem.*  (memory; plain integers, addresses can also be "FC64+2DD8958" style strings)
  mem.u8/i8/u16/i16/u32/i32/u64/i64/f32/f64/ptr(addr)       read (nil if unreadable)
  mem.set_u32(addr, v) ... mem.set_f32(addr, v)               write (false if not writable)
  mem.read(addr, "f32")  mem.write(addr, "f32", v)
  mem.module("FC64.dll") -> base, size     mem.scan("48 8B 05 ?? ?? ?? ??" [, module])
  mem.rip(addr [, dispOff, len])  mem.chain(base, off1, off2, ...)  mem.str(addr [, max])
  mem.bytes(addr, n) -> raw string   mem.write_bytes(addr, str)   mem.readable(addr, n)
  mem.entity(id) / mem.player() -> engine object address   mem.native("CAPI_Pawn.X") -> code address
  mem.alloc(n)   mem.hex(v)   mem.call(addr, a, b, c, d, e, f)   (ADVANCED: can crash the game)
  mem.vtable(obj, i) -> address of virtual function i of an engine object

fc4.*
  fc4.log/debug/warn/error(...)   print = fc4.log
  fc4.on_tick(function(dt) end) -> id      fc4.off(id)
  fc4.after(seconds, fn)   fc4.every(seconds, fn)
  fc4.on("start" | "game_lua_reset", fn)   (game_lua_reset = a new world/save was loaded)
  fc4.key_down(vk)   fc4.time()   fc4.frame()   fc4.version()
  fc4.read_file(path) / fc4.write_file(path, data [, append])   (inside fc4hook\)
  fc4.set_state(k, v) / fc4.get_state(k)   (shared with the menu, game Lua and plugins)
  fc4.menu.button("Label", fn [, "Group"])   shows in F8 > Developer > Lua 5.4
  fc4.file_id(path) -> the game's file id for a path in its data (64-bit integer)
  fc4.file_log([on]) -> on      record every file path the game asks for (dumps\file_log.txt)
  fc4.loose_status()            loose-file mods: "12 files (unchanged, reused), mounted"

Notes
  * A tick/timer that errors is switched off and logged (Console tab), the rest keep running.
  * os.exit is removed. io/os/debug are available: you're running inside the game, be careful.
