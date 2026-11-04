CoD viewmodel animation -> FC4 first-person clip (retarget)

  python retarget2.py <anim.seanim> <arms.semodel> <gun.semodel> <fc4 clip.glb> <out.glb> [gunmove=0.6] [gunturn=1] [near=1]

<fc4 clip.glb>  a clip exported by FC4Hook (Workshop > Animations > Export .glb). It says which game clip the
                result replaces and gives Ajay's skeleton, his normal hold and the gun's parts.
gunmove gunturn near   shares of CoD's gun movement, gun turning, and how far the left hand and batteries get
                from the gun (1 = all of CoD's). The Ray Gun reload uses 0.4 0.35 0.4.

How it works (retarget2.py): CoD animates the gun on its own and the hands follow it, so this does too.
  - gun: CoD's turn and (damped) move since its first frame, applied to where FC4 holds the gun
  - right hand: locked to the grip, as in FC4's hold
  - left hand: where CoD's left wrist is relative to CoD's gun; eases from/to FC4's two-handed grip over
    the first 10 / last 18 frames; fingers follow CoD's (CoD numbers finger joints from 0, FC4 from 1)
  - arms: 2-bone IK from the shoulders; when a hand is out of reach the collarbone swings the shoulder
    forward (up to 10 cm)
  - parts: tag_battery_1 -> CLIP, tag_battery_2 -> CLIP_BULLET, j_clip_release -> SLIDE_LOCK
Output keys sit on Blender frames (24 fps grid, 1 frame = 1 game frame); the game's reload stat sets the speed.

Notes
  - Some CoD tracks store positions as offsets from the bind pose (the Ray Gun's batteries). The reader
    (cod_worlds in retarget.py) spots them (first key ~0, bone well away from its parent) and adds the bind.
  - Only replace clips that are full poses. "(last round)" reload variants are add-on layers the game plays
    on top of the reload: their exported .glb sits in a neutral pose. Replacing them doubles the arms.
  - Tag the result for another clip by replacing FC4_<16 hex> (it appears twice in the file) with that
    clip's id, e.g. to cover a weapon's "aiming" and "empty" reload variants.

retarget.py is the first version (copies joint rotations down the arm). It's kept as the library the new
one uses (CoD readers, glb helpers); its own run() is outdated.
