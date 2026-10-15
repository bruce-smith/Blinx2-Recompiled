# Blinx 2 models and stages to glTF

`ninja2gltf.py` exports Blinx 2's models and stages straight from your own
`default.xbe` to glTF 2.0 (`.glb`). Blender, Unity, Unreal and Godot import
it. It also gets the parts of a stage you never see, and the collision.

The game keeps its models in XBE sections:

- `M<region><stage>`: a stage, e.g. `M11` is stage 01-01, `M13` is 01-03;
- `G1`-`G5`;
- `MDL*`: characters, enemies and gimmicks.

They are Sega Ninja chunk models (`NJS_OBJECT` trees and `NJS_CNK_MODEL`
vertex/strip chunks). The game's stage table pairs each area's model with
its collision tree and texture list. Textures are found by name in the
`.ipk` archives once extracted with blipks (see `assets/README.md` in the
superproject).

```
py -3 tools/ninja/ninja2gltf.py <game_files>/default.xbe M11 stage0101.glb --assets <assets>/extracted --collision
```

Every stage at once: section `stages`, with a folder as the output. It
writes `M11.glb` ... `M121.glb` and `G1.glb` ... `G5.glb` (26 files,
~110 MB). The options go after the three positional arguments:

```
py -3 tools/ninja/ninja2gltf.py <game_files>/default.xbe stages out --assets <assets>/extracted --collision --placements
```

All 26 pass the Khronos glTF validator with no errors or warnings. The
tool writes standard glTF 2.0: UVs plus embedded PNG textures in the
materials (alpha as MASK/BLEND), vertex normals, and named nodes.

Options:

- `--assets <dir>`: textures by name from the extracted archives. Without
  it, materials keep only their colour.
- `--collision`: the collision meshes as well, as `areaN_collision` nodes.
  Hide them, or use them as colliders.
- `--placements`: the stage's objects (gimmicks, items, enemies' set
  pieces) where the game places them. The data is the game's placement
  lists: per object a type id, a position, a rotation and a parameter. Each
  object becomes a marker node `objTT_param` (plus `extras.type` and
  `extras.param`) under `placements` and `placements_2`. Markers carry no
  mesh yet: which model each type draws is decided by the game's code
  (see below).
- `--all-trees`: every object tree in the section. Props and gimmicks (doors,
  crates, enemies) sit at their own origin, because the game places them at
  run time.
- `--colour N`: colour variant N (1-19) of the palettised textures. The
  custom-character parts use colour-mapped `.tga` textures (names starting
  with `@`). Each colour the editor offers is an `@name_pNN.icb`, an 8x8 TGA
  that carries only a palette. Without `--colour` the `.tga`'s own palette
  is used.
- `--objects 0x...,0x...`: only the trees at these section offsets.
- `--skin` (with `--objects`): export each tree as one skinned mesh, rigged.
  Characters are weighted chunk models. Every node is a bone. Nodes write
  their vertices into a buffer shared by the whole tree, plain or weighted
  (vertex type 44: position, packed normal, u16 weight /255, u16 index).
  The strips index that buffer. The result is a glTF skin with all nodes
  as joints and up to 4 weights per vertex, in bind pose. The player
  characters are the 147-node trees in `MDLPL` (e.g. `--objects 0x21e44`),
  the skeleton the `MOTPL` motions drive.
- `--edt <preset>` (with `--skin`): dress the character the way the
  game's character editor does. A preset is
  `edit_char/mdl_param_list_tsNNN.edt` (Time Sweeper, the cat, tree
  `0x21e44`) or `..._tgNNN.edt` (Tom-Tom, the pig, tree `0x5b7dc`). Each
  part (face, eyes, jacket, gloves, legs, boots, goggles, metal, emblem,
  armband; Tom-Tom also cap, pants, band, tattoos) has a design and a
  colour. The design picks the texture from the game's own name tables;
  the colour picks the `_Pnn` palette.

  ```
  py -3 tools/ninja/ninja2gltf.py <game_files>/default.xbe MDLPL ts004.glb --objects 0x21e44 --skin --edt <assets>/extracted/<lng>/edit_char/mdl_param_list_ts004.edt --assets <assets>/extracted
  ```

  All 45 presets export with no validator errors or warnings. Not applied
  yet: the body-shape sliders (preset bytes 24-41), and Tom-Tom colours
  above 19, which keep the texture's own palette.
- `--grid <spacing>`: lay the top-level groups out in a grid. Useful for
  part and prop libraries such as `MDLPL`, where everything sits at the
  origin.

Trees that are not in a stage table (characters, enemies, gimmicks) get
their textures from the texture list the game's code names next to them.
A character or enemy tree exports in bind pose: its nodes are its bones, so
it can be posed and animated as a node hierarchy. For a rigged mesh that
deforms with them, use `--skin`.

Object angles are float radians in Blinx 2 (not Ninja's BAMS integers).
Normals are exported (the packed 11:11:10 normals of vertex chunk 48).
Not yet exported: motions, and the model of each placed object. The object
lists are in `.data`. Per stage index `(region - 1) * 5 + stage - 1` they
are at 0x491C18 and 0x491DF8, 40-byte records. The enemy spawn lists are at
0x4923C8: 0x5C-byte records with type, position, rotation and patrol
target. Each type picks its model in its own code. For a stage exactly as
the game shows it, props drawn, see `xboxrecomp/tools/capture`.

Needs numpy and Pillow. The output is made from your own copy of the game:
do not redistribute it.
