FC4Hook v1.1.0 - script hook & mod menu for Far Cry 4 (PC)
===========================================================

FC4Hook loads into Far Cry 4 and gives you an in-game menu (F8) with:

  CHEATS     God mode, unlimited ammo, infinite magazine, no fall damage, speed boost,
             fly & noclip, launch, money / XP / skill points, refill ammo, slow motion (adjustable),
             time of day + time lock, storms/blizzard, 5 saved teleport positions,
             "unstick" teleport, unlock safe houses / fast travel / shop / crafting.
             Switches stay on through deaths and loads, and can be remembered between
             sessions. Optional hotkeys.
  ARMORY     Give yourself any weapon in the game: every handgun, SMG, shotgun,
             assault rifle, LMG, sniper, launcher, special weapon, melee weapon and
             all signature weapons (73 total). Search, favourites, give a whole
             category at once. Attachments menu: fit any attachment your gun supports.
  TRAVEL     Jump to any bell tower you've liberated, any outpost or safe house, or
             your current objective, from anywhere on the map.
  CO-OP      Fixes co-op guests never receiving bell tower rewards.
  CONSOLE    Live log with filters, copy and an in-game Lua console.
  MODS       Install mods as plain folders: fc4hook\mods\<ModName>\files\ holds game files
             (textures, models, archetypes, animations...) the game loads instead of
             its own, or in addition to them. Switch mods on/off in the Mods tab.
  MODDING    Runs Lua scripts and C++ plugins inside the game, a file log that shows every
             file the game loads, a spawner, animation tools, a memory binder, Lua 5.4.

REQUIREMENTS
------------
  * Far Cry 4 for PC, latest patch (1.10). Tested on the Steam version.
  * Ultimate ASI Loader (x64) by ThirteenAG:
      https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases
    Download the x64 build and put its dll in  Far Cry 4\bin\  renamed to  winmm.dll
    (skip this if another ASI mod already installed it).

INSTALL
-------
  1. Close the game.
  2. Copy the contents of this archive's "bin" folder into  Far Cry 4\bin\
     You should end up with:
        Far Cry 4\bin\winmm.dll          (the ASI loader, see above)
        Far Cry 4\bin\fc4hook.asi
        Far Cry 4\bin\fc4hook\...
  3. Start the game. A green "FC4Hook v1.1.0 loaded" banner shows at the top left.
     Load a save, then press F8.

  Steam default location:  C:\Program Files (x86)\Steam\steamapps\common\Far Cry 4\
  (Steam: right-click Far Cry 4 > Manage > Browse local files)

UPDATING FROM AN OLDER FC4HOOK
------------------------------
  Copy over the old files. Your settings, saved positions and co-op tower data are kept.
  From 1.0.0: nothing to delete.
  Old files you can delete:  fc4hook\mods\armory.txt  and these scripts if present:
  armory_test.lua, extract_*.lua, find_weather_presets.lua, autorun\00_hello.lua.
  FC4Hook 1.0 contains no custom weapons. If you use a weapon mod that was made for
  FC4Hook 0.x, keep its files: give it an armory.txt in its mods\ folder (see
  fc4hook\mods\README.txt) to list it in the Armory.

USING IT
--------
  F8                 open/close the menu (change the key in Settings)
  Cheats tab         tick a switch to turn it on; it stays on until you untick it
  Armory tab         "Give" puts the weapon straight in your weapon wheel.
                     Click + to add a favourite. Your own entries: fc4hook\armory\custom.txt
  Settings tab       menu key, hotkeys, menu size, log level, crash reports
  Console tab        what FC4Hook is doing; Lua commands for script authors

  Features are greyed out until you're in the world (the game's scripts aren't
  running in the main menu).

CO-OP
-----
  Cheats and the Armory work for you only. For the co-op tower reward fix both players
  need FC4Hook. If you use custom content (fc4hook_mods files), both players need the
  exact same files or the joining player crashes.
  Please only use cheats with people who are fine with it.

TROUBLESHOOTING
---------------
  No banner / F8 does nothing
      The ASI loader isn't loading FC4Hook: check winmm.dll and fc4hook.asi are both
      directly in Far Cry 4\bin\, and that you used the x64 loader.
  Banner shows, but everything is greyed out
      Load a save first. If the top of the menu says "signatures missing", your game
      version isn't supported (FC4Hook needs patch 1.10).
  A weapon says "failed"
      DLC weapons need the DLC; custom weapons need their mod files installed.
  Game crashes
      Look in Far Cry 4\bin\fc4hook\crashes\ and send that file plus
      Far Cry 4\bin\fc4hook\fc4hook.log with your bug report.

UNINSTALL
---------
  Delete  Far Cry 4\bin\fc4hook.asi  and the  Far Cry 4\bin\fc4hook\  folder, and
  Far Cry 4\data_win32\fc4hook_loose.* if you used loose-file mods.
  (Leave winmm.dll if other ASI mods use it.) FC4Hook never changes game files.
  "Unlock all ..." cheats change your save game; those stay.

FILES
-----
  bin\fc4hook.asi                      the mod
  bin\fc4hook\fc4hook.ini              settings (created on first start)
  bin\fc4hook\fc4hook.log              log of this session (fc4hook.prev.log = last one)
  bin\fc4hook\armory\weapons.txt       built-in weapon list (replaced on update)
  bin\fc4hook\armory\custom.txt        your own weapon list (created on first use)
  bin\fc4hook\scripts\autorun\         Lua that runs when you load in
  bin\fc4hook\plugins\                 C++ plugins (*.dll); example included (disabled)
  bin\fc4hook\mods\                    mods (one folder each); see README.txt there
  data_win32\fc4hook_loose.fat/.dat/.txt  built by FC4Hook from the mods' files\ folders
                                       (safe to delete; rebuilt at the next start)
  bin\fc4hook\crashes\                 crash reports

FOR MODDERS
-----------
  Turn on Settings > Developer mode for the native browser (every function the game
  exposes to Lua, callable from the menu), a memory viewer with watch list and freeze,
  a script runner, a Lua editor, a file log (every file path the game loads), a spawner,
  animation tools and diagnostics. How to make loose-file mods: fc4hook\mods\README.txt. Lua scripts can read/write game
  memory through Mem.* (see scripts\autorun\05_memory.lua). FC4Hook also embeds Lua 5.4
  for mods (64-bit integers, game.* bridge): see fc4hook\lua\README.txt. The source code and plugin API (include\fc4hook.h) are in the separate
  "FC4Hook source" download.

Credits: Dear ImGui (Omar Cornut, MIT), MinHook (Tsuda Kageyu, BSD 2-clause),
Ultimate ASI Loader (ThirteenAG). See LICENSES.txt.
Not affiliated with or endorsed by Ubisoft.
