Far Cry 3 Blood Dragon weapons -> Far Cry 4 (same engine, same file formats)

1. Unpack Blood Dragon:   dunia_extract.exe <Blood Dragon>\data_win32 <out folder>
   (dunia_find.exe finds which files a weapon uses.)
2. Model -> .glb:         python fc3xbg2glb.py <bd weapon.xbg> <out.glb> --extracted <out folder>
     --map BONE=NODE,...      put the triangles of a Blood Dragon bone on a node of the Far Cry 4 base
                              weapon, so FC4's animations move them (skinned base weapons, e.g. the PKM)
     --point NAME=x,y,z[,qx,qy,qz,qw]   move a base-weapon node (game space: x right, y forward, z up)
   Points the base weapon has too (IK_LeftHand, FX_FIRE, FX_CASING) are exported automatically.
3. FC4Hook > Workshop > your weapon > Model: use the .glb.

The Overkill (bd_minigun) on the PKM:
   python fc3xbg2glb.py e6d127ce06c54b47.xbg overkill.glb --extracted <out folder>
       --map BARREL_01=PKM_Tripod2,FLAP_01=PKM_AmmoLid,CHAIN=PKM_AmmoBox
       --point PKM_AmmoLid=0.0938,0.2163,0.0458,0,0.2470,0,0.9690
   Barrels on PKM_Tripod2 (never animated by the PKM), the feed flap on the PKM's ammo-box lid (its bind pose
   moved to the lid's rest pose in the clips), the ammo chain on the ammo box.
   Its clips: for each PKM clip, export it from FC4Hook (Workshop > Animations > Export .glb), then
       clipdump.exe <the clip file> > clip.json
       python overkill_clips.py clip.glb clip.json new.glb [--spin]
   and use new.glb on that clip's row. --spin on the five fire clips (0.70 s) adds the barrel spin:
   af433ff668a9595f c207a4111e4fa7ab c207a4111e504655 f310ebb7ad954afc f4e18001d5cda661.
   The others that move the flap or chain: af18bd92f3779bc3 ce21c5c853b87bc9 (reloads), 39bc1abe31ea5b49
   6ed3c92a6c8d462e 6ed3de9dcc8d462e bb30ffe294814e10 (belt poses).
   Replaced clips play for every weapon that uses them, so the plain PKM's lid and box move oddly while
   the Overkill's mod is on.
