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

Options:

- `--assets <dir>`: textures by name from the extracted archives. Without
  it, materials keep only their colour.
- `--collision`: the collision meshes as well, as `areaN_collision` nodes.
  Hide them, or use them as colliders.
- `--all-trees`: every object tree in the section. Props and gimmicks (doors,
  crates, enemies) sit at their own origin, because the game places them at
  run time.
- `--objects 0x...,0x...`: only the trees at these section offsets.
- `--grid <spacing>`: lay the top-level groups out in a grid. Useful for
  part and prop libraries such as `MDLPL`, where everything sits at the
  origin.

Trees that are not in a stage table (characters, enemies, gimmicks) get
their textures from the texture list the game's code names next to them.
A character or enemy tree exports in bind pose: its nodes are its bones, so
it can be posed and animated as a node hierarchy.

Not yet exported: normals (the packed normals are skipped), vertex
weights, and prop placement. For a stage as the game shows it, props
included, see `xboxrecomp/tools/capture`.

Needs numpy and Pillow. The output is made from your own copy of the game:
do not redistribute it.
