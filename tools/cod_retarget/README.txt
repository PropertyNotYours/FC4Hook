CoD viewmodel animation -> FC4 first-person clip (retarget)

  python retarget.py <anim.seanim> <arms.semodel> <gun.semodel> <fc4 clip.glb> <out.glb> [<idle.seanim>]

<fc4 clip.glb>  a clip exported by FC4Hook (Workshop > Animations > Export .glb) - it says which game clip the
                result replaces and gives Ajay's skeleton and the gun's parts.
<idle.seanim>   CoD's idle for the same weapon: how the gun is normally held (used to line the right arm up).

Right arm: CoD's motion with per-bone offsets learned from both rigs' hold poses. Left arm: 2-bone IK to
where CoD puts the left wrist relative to the right hand. Gun: CoD's gun-vs-wrist change on FC4's hold.
Parts: tag_battery_1 -> CLIP, tag_battery_2 -> CLIP_BULLET, j_clip_release -> SLIDE_LOCK.
Output keys sit on Blender frames (24 fps grid, 1 frame = 1 game frame); the game's reload stat sets the speed.
