# FC4Hook code map

FC4Hook is a Far Cry 4 mod framework. It is a DLL (`fc4hook.asi`) that the game loads at startup. It hooks
the game's renderer to draw an in-game menu, runs Lua scripts on the game's script thread, and adds
loose-file modding. This page says what every file is for, so you can read one file without the rest.

Build: `bash build.sh` (Linux, cross-compiles with zig) or `CMakeLists.txt` (Visual Studio).
Everything is in `namespace fc4`. `src/core.h` is the one internal header every file includes.

## How the pieces fit together

1. `main.cpp` runs when the game loads the DLL: reads the config, installs hooks, starts threads.
2. `overlay.cpp` hooks D3D11 `Present`. Each frame it polls input and draws the menu: a sidebar plus the page
   the player picked. The pages are `page_*.cpp`.
3. Menu buttons rarely do the work themselves. They send a short Lua snippet to the game
   (`QueueLua`, in `game.cpp`). The snippet calls a function from a script in `dist/fc4hook/scripts/autorun/`,
   which runs on the game's own script thread, the only place it is safe to call game functions.
4. Scripts publish results back with `SetState(key, value)`. Pages read them with `GetState(key)`.
   This is how a checkbox knows whether god mode is on.

## Core (src/)

| File | What it does |
|---|---|
| `main.cpp` | DLL entry point. Loads config and logging, installs hooks, starts the bridge. |
| `core.h` | Internal declarations: config struct, logging, state store, every cross-file function. Holds the version. |
| `util.cpp` | Config (`fc4hook.ini`), logger, crash reports, byte-pattern scanner, MinHook wrappers. |
| `game.cpp` | The Lua bridge. Finds the game's embedded Lua 5.1 by byte signature, runs queued Lua on the script thread, stores shared state, knows the bell tower data, mounts mod archives, and logs file opens. |
| `input.cpp` | Hooks DirectInput so the game gets no keyboard or mouse while the menu is open or Fly is on. |
| `plugins.cpp` | Loads `fc4hook\plugins\*.dll` (API in `include/fc4hook.h`). |
| `lua54.cpp` | A second, stock Lua 5.4 runtime with 64-bit integers, for scripts in `fc4hook\lua\`. Exposes `game.*`, `mem.*`, `fc4.*`. |
| `luaui.cpp` | `fc4.ui`: lets Lua 5.4 scripts declare ImGui windows that the overlay draws. |
| `memory.cpp` | Reading, writing, scanning and freezing game memory from Lua, with address expressions. |
| `audio.cpp` | Makes game audio follow slow motion by changing XAudio2 voice speeds. |

## Menu (src/)

| File | Sidebar page |
|---|---|
| `overlay.cpp` | The frame around the pages: Present hook, ImGui start-up, font, input polling (mouse is ignored when the game is not focused), sidebar, page dispatch, hotkeys. |
| `menu_common.h` / `.cpp` | Shared widgets (`Help`, `ActionButton`, `Debounced`...) and the declarations of every page function. |
| `page_cheats.cpp` | Play > Cheats |
| `page_weapons.cpp` | Play > Weapons (the armory: weapon lists, favourites, attachments) |
| `page_teleport.cpp` | Play > Teleport |
| `page_spawn.cpp` | Play > Spawn |
| `page_coop.cpp` | Play > Co-op |
| `page_workshop.cpp` | Mods > Weapon Workshop |
| `page_models.cpp` | Mods > Models & Textures |
| `page_installed_mods.cpp` | Mods > Installed Mods |
| `page_developer.cpp` | Developer > Lua, Natives, Co-op Debug, Diagnostics (and the script-running helpers) |
| `page_memory.cpp` | Developer > Memory |
| `page_animation.cpp` | Developer > Animation |
| `page_file_activity.cpp` | Developer > File Activity |
| `page_console.cpp` | FC4Hook > Console |
| `page_settings.cpp` | FC4Hook > Settings and About |

To add a page: write `DrawXPage()` in a new `page_x.cpp`, declare it in `menu_common.h`, and add one row to
the `pages[]` table and one `else if` to the dispatch in `DrawMenu()` (`overlay.cpp`).

## Modding back end (src/)

| File | What it does |
|---|---|
| `vfs.cpp` | Loose-file mods. Packs `mods\<Mod>\files\...` into an archive the game mounts above its own, and records which files the game asks for. |
| `extract.cpp` | Reads the game's own `.fat/.dat` archives (LZO1X) and extracts files; builds "swap A for B" test mods. |
| `fcb.cpp` | Reads and edits Dunia `.fcb` object files in place (never changes a value's size). |
| `workshop.cpp` | Weapon Workshop logic: clone a weapon's three archetypes under new ids, edit stats, write the mod. |
| `texconv.cpp` | `.xbt` <-> `.dds` texture conversion (portable C++; also built as the stand-alone `fc4tex.exe`). |
| `texmake.cpp` | PNG / JPG / TGA -> `.xbt`, plus material copies that point at them. |
| `glbwrite.cpp` | `.glb` -> `.xbg` for completely new weapon models, including skinned ones. |
| `aniclip.cpp` | Far Cry 4 animation clips ("aNi"): read, export to `.glb`, import from `.glb`. Format notes: `docs/ANI_FORMAT.md`. |
| `minijson.h` | Tiny JSON reader used for glTF files. |

## Runtime files (dist/fc4hook/, copied to Far Cry 4\bin\fc4hook\)

| Path | Purpose |
|---|---|
| `scripts/autorun/*.lua` | Game-side scripts, loaded in number order. `00_core` helpers, `05_memory`, `10_coop_towers`, `20_tower_travel`, `30_armory`, `40_cheats`, `45_cheats_plus`, `46_cheats_world`, `50_teleports`, `60_spawner`, `65_workshop`. |
| `scripts/*.lua`, `scripts/coop/` | Scripts run on demand from Developer > Lua or the Co-op Debug page. |
| `lua/` | Lua 5.4 scripts and their README. |
| `armory/` | `weapons.txt` (built-in list), `attachment_names.txt`. |
| `animation/states.txt` | Names of animation states, shown on the Animation page. |
| `mods/` | Example mod and the mod-folder README. |

## Tools (tools/)

| File | Purpose |
|---|---|
| `fc4tex.cpp` | Command-line wrapper around `texconv.cpp`: drag `.xbt` / `.dds` files onto `fc4tex.exe`. |
| `blood_dragon/` | Porting Far Cry 3 Blood Dragon weapons and animations. See its README. |

## Gotchas worth knowing

* Menu code runs on the render thread; Lua runs on the game's script thread. Never call game Lua from a page:
  queue it with `Lua(code, chunkName)`.
* Pages that need the world loaded call `NeedWorldNote()` and disable widgets with `LuaReady()`.
* Lua 5.1 in the game uses 32-bit float numbers, so ids and addresses are passed as strings and handled in C++.
* `GetAsyncKeyState` reports input for the whole desktop. Anything polled with it must check
  `GameHasFocus()` first (the mouse already does; hotkeys do).
