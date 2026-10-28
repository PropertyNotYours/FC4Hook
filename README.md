# FC4Hook 1.2.0 — source code

FC4Hook is a ScriptHook-style mod framework and mod menu for Far Cry 4 (PC, x64). It's an
`.asi` that Ultimate ASI Loader (as `winmm.dll`) loads into the game. It finds the game's
built-in Lua 5.1 by signature, runs Lua scripts and C++ plugins on the game's script
thread, and draws an ImGui menu (F8) through a D3D11 `Present` hook.

FC4Hook ships no custom content. Weapon/content mods are separate packages that FC4Hook
loads (see "Mods" below).

## Layout
```
src/main.cpp        startup, default ini, heartbeat
src/util.cpp        config, logger (levels, rotation), crash reports, pattern scanner, MinHook
src/game.cpp        Lua bridge, built-in Lua functions, state store, hidden natives,
                    bell tower data, mod archive mount, custom string loader, file trace
src/overlay.cpp     ImGui menu: Cheats / Armory / Travel / Co-op / Console / Settings /
                    Developer / About, hotkeys
src/lua54.cpp       embedded Lua 5.4 runtime + game bridge (game.*, mem.*, fc4.*)
src/memory.cpp      memory binder: Mem* Lua functions, address expressions, Memory tab helpers
src/plugins.cpp     loads fc4hook\plugins\*.dll
src/core.h          internal declarations, version
include/fc4hook.h   public C API for plugins (API v3)
examples/           example C++ plugin
external/           MinHook, Dear ImGui, Lua 5.4.8 (third-party, unmodified; Lua is compiled as C++)
dist/fc4hook/       the runtime folder that goes in Far Cry 4\bin\
package/            README / CHANGELOG / LICENSES / Nexus page for the release zip
```

## Runtime folder (`Far Cry 4\bin\fc4hook\`)
```
fc4hook.ini                 settings (written with comments on first start)
fc4hook.log, .prev.log      this and the previous session's log
crashes\                    crash reports
armory\weapons.txt          built-in weapon list (from the game's ShoppingItems.ndb)
armory\custom.txt           user weapons;  armory\favourites.txt  favourites
mods\<Mod>\armory.txt       weapons added by mod packages
scripts\autorun\*.lua       00_core (helpers, tick dispatcher), 05_memory (Mem.*), 10_coop_towers,
                            20_tower_travel, 30_armory, 40_cheats
scripts\*.lua               developer scripts (natives dump, probes, progress snapshot)
plugins\*.dll               C++ plugins
cheats.txt, positions.txt   cheat switches / saved positions (written by 40_cheats.lua)
```

## Lua functions FC4Hook adds
| Function | What it does |
|---|---|
| `ModLog(...)`, `ModLogDebug/Warn/Error(...)` | write to `fc4hook.log` and the console at that level |
| `ModWriteFile(path, text [, append])`, `ModReadFile(path)`, `ModFileExists(path)` | files under `bin\fc4hook\` |
| `ModSetState(key, value)`, `ModGetState(key)` | key/value store shared with the menu and plugins |
| `ModVersion()` | `"1.2.0"` |
| `ModArchiveStatus()`, `ModLoadStrings(path)` | mod archive state / add a strings.bin |
| `ModLooseStatus()` | loose-file mods status |
| `ModFileId(path)` | the game's file id (CRC-64 of the lower-case path) as a decimal string |
| `ModArchetypeFile(id)` | `"entityarchetypeslibrary\\<id>.ark.fcb"` |
| `ModFileLog([on])` | switch the file request log on/off, returns its state |
| `FC4Spawn.*`, `FC4Anim.*` | spawner and animation helpers (`60_spawner.lua`) |
| `ModNativeAddress(name)` | where a native lives, e.g. `"FC64+0x18bad60"` |
| `ModTeleportEntityPos`, `ModGetEntityPositionVec3` | natives the game registers in another state |
| `ModGetUnlockedTowers`, `ModTowerPositions`, `ModInspectTower` | bell tower data |
| `ModGrantTowerRewards`, `ModTowerRewardKeys`, `ModRewardState`, `ModGrantRewardKey` | co-op tower rewards |
| `ModCopyArchiveRange`, `ModDumpStrings` | read-only research helpers |
| `OnModTick()` | called about once per frame; use `FC4Ticks.<name> = fn` instead of replacing it |

## Memory binder (`Mem*`, wrapped as `Mem.*` in `05_memory.lua`)
FC4's Lua numbers are 32-bit floats, so **addresses and 32/64-bit integers are strings**.
Addresses accept Cheat Engine expressions (numbers are hex): `"FC64+2DD8958"`,
`"[FC64+2DD8958]+8"`, `"0x7FF6A1B20000"`. Failed calls return `nil`; `MemError()` says why.
Every access is checked with VirtualQuery first, so bad addresses don't crash the game.

| Function | What it does |
|---|---|
| `MemAddress(expr)` | resolve an expression to `"0x..."` |
| `MemModule(name)` | `{ base, size, path }` of a loaded module |
| `MemRead(addr, type)` / `MemWrite(addr, type, value)` | types `i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 ptr bool` |
| `MemReadString(addr [, max [, "utf16"]])`, `MemReadBytes(addr, n)`, `MemWriteBytes(addr, "90 90")` | strings / raw bytes |
| `MemScan(pattern [, module])`, `MemRip(addr [, dispOff, len])` | IDA-style signature scan, RIP-relative resolve |
| `MemOffset(addr, off...)`, `MemPointer(base, off...)` | 64-bit math; pointer chains (`[[base]+o1]+o2`) |
| `MemReadable(addr [, n])`, `MemDump(addr, n, file)` | check / hex dump to a file |
| `MemNative(name)` | address of a game Lua function's C code |
| `MemEntity(id)` | engine object of an entity id (`Mem.player()` = your pawn) |
| `MemAlloc(size)` | scratch read/write memory |
| `MemCall(addr, a1..a4)` | call native code with up to 4 integer/pointer args (**can crash**) |

`Mem.freeze(expr, type, value)` re-writes a value every frame (pointer chains re-evaluated).
In game: Settings > Developer mode > Developer > Memory (live values, watch list with
freeze, hex view). Code pages of FC64.dll refuse VirtualProtect, so code patching isn't possible.

Script helpers in `00_core.lua`: `FC4.fn(name)`, `FC4.has(name)`, `FC4.call(name, ...)`,
`FC4.me()`, `FC4.loadKV/saveKV`, `FC4.releaseGodMode()`.
Menu state keys published by the scripts: `cheat.*`, `avail.<Native>`, `pos.<n>`,
`armory.<id>`, `armory.queue`.

**Far Cry 4's Lua has a much lower per-function register limit than stock Lua 5.1.**
Keep functions short with few locals; a chunk that's too big fails to load.

## Lua 5.4
A stock Lua 5.4 state runs on the game's script thread next to the game's Lua 5.1.
`game.call(name, ...)` serializes the arguments to game-Lua source, runs
`FC4Bridge.call(...)` (defined in `00_core.lua`) and decodes the serialized results; values
that can't be copied stay in `FC4Bridge.refs` and come back as handles. Integers above
2^24 are passed as decimal strings, which is how the game's natives read entity ids.
API reference: `dist/fc4hook/lua/README.txt`. Scripts: `fc4hook\lua\autorun\*.lua`.

## Plugins
Export `extern "C" __declspec(dllexport) int FC4ModInit(const FC4HookAPI* api)` and check
`api->version >= FC4HOOK_API_VERSION`. Only touch Lua inside `OnGameTick` callbacks or
functions registered with `RegisterLuaFunction`; `OnDrawMenu` runs on the render thread
inside an ImGui frame (call `ImGui::SetCurrentContext(api->GetImGuiContext())` first).

API v3 (1.1) adds `HookVtable`/`UnhookVtable` (swap a virtual table entry), `OnPresent`
(render-thread callback before every Present) with `GetD3D11Device`/`GetD3D11Context`/
`GetSwapChain`, `FileId(path)`, `QueueLua54` and `GetLooseStatus`. Function hooks:
`FindPattern` + `CreateHook` (MinHook), as before.

## Mods
* Loose files (`src/vfs.cpp`): `fc4hook\mods\<Mod>\files\<game path>` from every enabled mod
  are packed (stored, 16-byte aligned) into `data_win32\fc4hook_loose.fat/.dat` at startup when
  the manifest (`fc4hook_loose.txt`) changed, and mounted at priority -3 (above everything).
  Ids are CRC-64 (poly 0xD800000000000000, reflected, init 0) of the lower-case path, the same
  as FC64's FUN_180001280; `__UNKNOWN\...\<16 hex>.ext` keeps its hash. See
  `dist/fc4hook/mods/README.txt`.
* File log: hooks FC64's path-to-id function (FUN_180060ed0 on 1.10) and records each new
  path once to `dumps\file_log.txt`.
* Archetypes are loaded per id from `entityarchetypeslibrary\<id>.ark.fcb` (FUN_1807249d0), so
  a new id = a new loose file; no table to patch.
* `data_win32\fc4hook_mods.fat/.dat`: a Dunia FAT2 v9 archive mounted shortly after startup in a
  free archive slot at priority -2 (above patch.dat), so its files override the game's.
* `fc4hook\languages\english\strings.bin` inside that archive is added to the game's
  string tables (custom names/descriptions).
* `fc4hook\mods\<Mod>\armory.txt` lists the mod's weapons in the Armory.

## Building
**Windows (Visual Studio 2019/2022 with C++ workload + CMake):**
```
cmake -B build -A x64
cmake --build build --config Release
```
Output: `build\Release\fc4hook.asi`.

**Linux cross-compile:** `pip install ziglang`, then `./build.sh` (output `build/fc4hook.asi`).
The release build is made this way.

Signatures target the Steam FC64.dll of patch 1.10 (timestamp 0x5c6150fe).
