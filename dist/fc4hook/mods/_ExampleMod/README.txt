Example mod layout (this folder starts with _ so it's switched off; it changes nothing).

  _ExampleMod\
    README.txt
    armory.txt                     optional: Armory entries for your weapons
    files\                         loose files, at their path inside the game's data
      entityarchetypeslibrary\
        <id>.ark.fcb               e.g. a new weapon archetype
      graphics\...                 its models / textures / materials

To make your own: copy this folder, rename it (no leading _), put your files in files\,
restart the game, and check F8 > Mods.
