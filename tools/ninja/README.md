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

Every stage at once (PowerShell), 26 sections, ~110 MB of .glb:

```
foreach ($s in 'M11','M13','M14','M21','M23','M24','M31','M33','M34','M41','M43','M44',
               'M51','M53','M54','M61','M64','M91','M93','M111','M121','G1','G2','G3','G4','G5') {
  py -3 tools/ninja/ninja2gltf.py <game_files>/default.xbe $s out/$s.glb --assets <assets>/extracted --collision
}
```

All 26 pass the Khronos glTF validator with no errors or warnings.

Options:

- `--assets <dir>`: textures by name from the extracted archives. Without
  it, materials keep only their colour.
- `--collision`: the collision meshes as well, as `areaN_collision` nodes.
  Hide them, or use them as colliders.
- `--all-trees`: every object tree in the section. Props and gimmicks (doors,
  crates, enemies) sit at their own origin, because the game places them at
  run time.
- `--colour N`: colour variant N (1-19) of the palettised textures. The
  custom-character parts use colour-mapped `.tga` textures (names starting
  with `@`). Each colour the editor offers is an `@name_pNN.icb`, an 8x8 TGA
  that carries only a palette. Without `--colour` the `.tga`'s own palette
  is used.
- `--objects 0x...,0x...`: only the trees at these section offsets.
- `--grid <spacing>`: lay the top-level groups out in a grid. Useful for
  part and prop libraries such as `MDLPL`, where everything sits at the
  origin.

Trees that are not in a stage table (characters, enemies, gimmicks) get
their textures from the texture list the game's code names next to them.
A character or enemy tree exports in bind pose: its nodes are its bones, so
it can be posed and animated as a node hierarchy.

Normals are exported (the packed 11:11:10 normals of vertex chunk 48).
Not yet exported: vertex weights, and prop placement. The game's enemy spawn
lists are in `.data` (per stage, a count and 0x5C-byte records: type,
position, rotation, patrol target), but which model each type uses is
decided in code. For a stage as the game shows it, props
included, see `xboxrecomp/tools/capture`.

Needs numpy and Pillow. The output is made from your own copy of the game:
do not redistribute it.
