# Far Cry 4 animation clips ("aNi", CAnimationResource) - research notes

Status: being reverse-engineered for FC4Hook custom animations. FC64.dll patch 1.10 addresses.

## File
- 0x00..0x17: resource header (u32 0x81 version?, u32 hash, flags, u32 0x0160b41f, f32 ?).
- 0x18: the clip ("aNi" + 1 byte: '/' or '!'). Offsets below are from the clip start (file+0x18).
- Bone / weapon names appear later in the file as <crc32><name>\0.

## Clip header (clip+N)
- +0x04 u16: low 12 bits = track count (bones), top 2 bits = layout kind (0, 0x4000, 0x8000)
- +0x06 u16: frame count (used as the sampled frame / 8 key blocks)
- +0x08 f32: length in seconds (= CAnimationResource+0x50)
- +0x18 u16: number of CONSTANT rotation tracks
- +0x1c u16: number of ANIMATED rotation tracks (0 = static clip); const + animated = track count
- verified on 20 game clips: every constant rotation decodes to a valid quaternion
- 58 tracks = the first-person arms skeleton, 83 = third-person characters
- constant-rotation data offset: kind 0x4000: u16 +0x1c; kind 0x8000: (u8 +0x2e << 16) | u16 +0x22;
  kind 0: (u8 +0x3e << 16) | u16 +0x28
- animated data offset: kind 0x8000: (u8 +0x32 << 16) | u16 +0x2a; kind 0: (u8 +0x42 << 16) | u16 +0x30
- per-track info (built at load, 8 bytes each): u32 bone index (0xffffffff = unused),
  u32 flags: (flags & 0x30) == 0x30 constant rotation, == 0x10 animated rotation, (flags & 0xf) + 1 = bits

## Compressed rotation (FUN_1804ea590 / batch FUN_1804ee1a0): 3 x u16, "smallest three"
- c = (v & 0x7fff) * (sqrt(2)/32767) - 1/sqrt(2)      (third value: whole u16)
- top bits of v0, v1 = which component was dropped (it is >= 0, rebuilt as sqrt(1 - a^2 - b^2 - c^2)):
  (1,1) -> (c0, c1, c2, w)   (0,1) -> (c0, c1, z, c2)   (1,0) -> (c0, y, c1, c2)   (0,0) -> (x, c0, c1, c2)
- identity = bf ff bf ff 3f ff  ("ffbf ffbf ff3f" bytes)

## Sampler: FUN_1804f4990(skeletonCtx, outPose, ?, clip, trackInfo)
- outPose: 32 bytes per bone (rotation xyzw at +0), "has rotation" bits at outPose+2
- animated tracks: bit-packed keys (FUN_1804e8620 = bitstream read, FUN_1804ef090 / FUN_1804eef30 =
  key decode), two keys slerped by the frame fraction (skeletonCtx+4)

## Track flags: one byte per track at clip + u32(clip+0x10)
- (f & 0x30) == 0x30: constant rotation (next entry of the constant-rotation list)
- (f & 0x30) == 0x10: animated rotation, key width = (f & 0xf) + 1 bits
- (f & 0x30) == 0x00: no rotation
- bits 0x40 / 0x80: translation (constant / animated?) - TODO
- tracks are in skeleton bone order (58 = first-person arms, 83 = third-person body)

## Animated rotations (verified: all 20 test clips decode to unit quaternions, smooth)
- at clip + animOffset: u32 block offsets (relative to animOffset), nBlocks + 1 of them
- keys = frames + 1, in blocks of 8 keys (last block shorter)
- in a block, bit-packed LSB-first, for each animated track in track order:
  - 6-bit header: bits 0-2 = component c is constant in this block; bit 3 = a sign mask follows;
    bits 4-5 = index of the rebuilt (dropped) component
  - if bit 3: nKeys bits, bit k = rebuilt component of key k is negative
  - for each of the 3 stored components:
    - constant: 16 bits, value = u16 * (1/32767.5) - 1
    - else: u8 min, u8 range, then nKeys values of `bits` bits:
      value = min/127 - 1 + (range/127.5) * q / (2^bits - 1)
  - rebuilt component = sqrt(max(0, 1 - a^2 - b^2 - c^2)), inserted at its index
- decoder: tools/ani_decode.py

## Translations (positions) - FUN_1804f5060 (verified: bone lengths, smooth animated tracks)
- flags & 0xc0: 0xc0 = constant position, 0x40 = animated position, 0 = none
- constant positions at (u8 clip+0x3f << 16) | u16 clip+0x2a, in track order:
  if (clip+3 & 0x20): 3 x int16 / 16384 metres (6 bytes), else 3 x float (12 bytes)
- animated position count: u16 clip+0x1e (0 = none)
- per-track scale table at (u8 clip+0x41 << 16) | u16 clip+0x2e, u16 clip+0x20 entries, u32 each:
  low 8 bits = key bit width, high 24 bits * 0.00029802325 = scale (metres)
  if (u8 clip+0x45 << 16) | u16 clip+0x36 != 0 (an extra / root track), entry 0 belongs to it: the
  animated tracks use entries 1..
- animated data at (u8 clip+0x43 << 16) | u16 clip+0x32: same block table + blocks of 8 keys as
  rotations; per track: 3-bit header (component constant bits), then 3 components encoded exactly
  like rotation components (16-bit constant, or u8 min + u8 range + keys); value = scale * component

## Track -> bone
- clip + u32(clip+0x14): one u32 per track = crc32(bone name) (case as in the skeleton, e.g. "RightHand")
- first-person arms skeleton: graphics\_common\characters\narrative\firstpersonarms\firstperson_*.xbg
  (133 nodes; arm clips drive 58 of them)
- tools/ani2glb.py <clip.ani> <skeleton.xbg> <out.glb>: clip -> glTF armature + animation (Blender)

## More sections (copied and moved when rebuilding a clip)
- +0x38 (high byte +0x46): nested "aNi" clips for attached objects (the weapon's own animation, per hand)
- +0x3a (high byte +0x47): event block ("nbCF" EntityEvent: sounds etc.)
- +0x2c: a u16 list (unknown); +0x34 / +0x36: two root tracks (block streams)

## Writing a clip (src/aniclip.cpp, tools/ani_encode.py)
- template = the clip being replaced; its other sections are kept and moved; offsets rewritten
- additive layers (identity constant rotations) are exported on top of the rest pose and taken back off when encoding
- verified: re-encoding game clips gives <= 0.006 deg error and the same animated tracks; plays in game

## TODO
- the "extra" track at clip+0x36 (root motion?), events / name table after the flags, resource header
  (first 0x18 bytes), clip+0x14 region
