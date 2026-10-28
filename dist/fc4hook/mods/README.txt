FC4Hook mods
============

Every folder in fc4hook\mods\ is a mod. F8 > Mods lists them; untick one to switch it off
(takes effect the next time you start the game). A folder whose name starts with _ is off.

  fc4hook\mods\<ModName>\files\...     LOOSE FILES: game files the game loads instead of
                                        (or in addition to) its own
  fc4hook\mods\<ModName>\armory.txt    adds the mod's weapons to the Armory tab
  fc4hook\mods\load_order.txt          optional: one folder name per line; later wins

LOOSE FILES
-----------
Put a file in files\ at the same path it has inside the game's data, for example

  fc4hook\mods\GoldenAK\files\graphics\_textures\weapons\ak47_d.xbt

When the game starts, FC4Hook packs every enabled mod's files\ into
data_win32\fc4hook_loose.fat/.dat (only when something changed) and mounts it above all of
the game's archives. Nothing of the game's is modified; delete the mod folder to undo it.

  * New files work too. The game finds files by a hash of their path, so a path it never
    shipped is simply there when something asks for it. That's how you ADD things instead
    of replacing them:
      - a new archetype (weapon, NPC, prop, vehicle):
          files\entityarchetypeslibrary\<new id>.ark.fcb
        Pick an id that isn't used yet. The game builds the file name from the id, so any
        reference to that id (Armory, F8 > Developer > Spawner, your own archetypes, shop
        entries) loads your file. Its models/textures/materials go in files\ too.
      - new textures, models, materials, sounds, animation packages, at new paths that your
        own archetype files point to.
  * Files the Gibbed unpacker couldn't name (__UNKNOWN\<type>\<16 hex digits>.<ext>) can be
    shipped as they are: inside a folder called __UNKNOWN, a 16-digit hex name is used as
    the file's id directly.
  * Files must be in the game's own formats (.xbt textures, .xbg models, .fcb data,
    animation packages...). Convert PNG/DDS/FBX with the community Dunia tools first;
    FC4Hook loads files, it doesn't convert them.
  * Files the game reads while the splash screens are still up (before FC4Hook's archive is
    mounted) can't be replaced this way. Almost everything in the world can.
  * Status: F8 > Mods, and the "mods:" lines in fc4hook.log (incl. which mod overrides which).

FINDING THE FILES TO REPLACE
----------------------------
F8 > Settings > Developer mode, then F8 > Developer > File log > "Record file paths".
Play the bit of the game you're interested in (equip the gun, meet the NPC, play the
animation) and every path the game asks for is listed once, with its file id, newest first.
Also written to fc4hook\dumps\file_log.txt. Paths a loose mod supplies are marked "<- Mod".
Lines with [Type] come from the game's resource manager: everything it creates, with its
engine type, even files it only knows by id ("(no path)"). Extract and Swap work on those too.
Animations and move data load with the world: tick "Record from game start", restart the
game, load your save, then filter by Anim or Move.
The same tab turns any path into its file id, and:
  * Extract: click a line, then "Extract to dumps\extracted" copies the game's original
    file out of its archives (decompressed), ready to edit.
  * Swap test: click the file to replace, "Replace THIS...", click a file of the same kind,
    "...WITH this", "Create swap mod", restart the game. Writes mods\FC4Hook_SwapTest\
    (swaps.txt lists what it did; untick it in the Mods tab or delete it to undo).

EDITING TEXTURES
----------------
Textures are .xbt files. Big ones come in two halves: foo_d_mip0.xbt (full resolution) and
foo_d.xbt (the smaller mip levels). Endings: _d colour, _n normal map, _m mask.
  1. File log: click the texture, "Extract to dumps\extracted". FC4Hook extracts both
     halves and writes foo_d.dds next to them: the whole image, ready to edit.
  2. Open foo_d.dds in paint.net / GIMP / Photoshop (with a DDS plugin), paint, and save it
     as DDS, same name, same format (DXT1 / BC1 for most _d textures), WITH mipmaps.
  3. File log: "Install edited textures". Every edited .dds is packed back into .xbt files
     in mods\MyTextures\files\<the game's path>. Restart the game.
Outside the game: drag .xbt or .dds files onto fc4hook\tools\fc4tex.exe (keep the
original .xbt files next to the .dds; the result goes in a "packed" folder).

TESTING
-------
F8 > Developer > Spawner      spawn any archetype id in front of you (incl. your new ones)
F8 > Developer > Animation    attach animation packages and play animations on you or on
                              what you spawned, through the game's own animation system

THE OLD WAY: ONE ARCHIVE
------------------------
data_win32\fc4hook_mods.fat / fc4hook_mods.dat is still mounted too (below the loose
files), for mods that ship a ready-made Dunia archive.

ARMORY ENTRIES
--------------
fc4hook\mods\<ModName>\armory.txt, same format as fc4hook\armory\weapons.txt:

  [My Weapon Pack]
  Golden AK = 123456789012345

Co-op: both players need the same mods (same files) and the same game version.
