# Blinx 2: Masters of Time and Space — static recompilation

A static recompilation of **Blinx 2: Masters of Time and Space** (original
Xbox, NTSC-U) into a native Windows executable. The game's x86 code is
translated function by function into C and compiled with MSVC. The Xbox
hardware and kernel it talks to are reimplemented by the runtime of
[xboxrecomp](https://github.com/bruce-smith/xboxrecomp) (branch
`blinx2-fixes`), our fork of
[sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp).

- This repository: the game project that produces `blinx_2_recomp.exe`: its
  entry point, game-specific overrides (`src/recomp_manual.c`), the seed
  files the pipeline needs (`seeds/`) and the build.
- [xboxrecomp fork](https://github.com/bruce-smith/xboxrecomp) (branch
  `blinx2-fixes`): the recompiler pipeline (Python) and the runtime libraries
  (kernel, NV2A, APU, USB, ...).
- [blinx-2-recompile-full](https://github.com/bruce-smith/blinx-2-recompile-full):
  a superproject that pins matching commits of both as submodules.

## Status

Playable through the early game: the intro movies, the title screen, the
Time Sweeper hub, stage 01-01 and the first Tom-Tom Gang stages (including
the Subspace Dive gadget).

- **Graphics:** the game's NV2A pushbuffer is executed and drawn on the GPU
  through Vulkan (the default): 30 fps in the hub, stage 01-01 and the rainy
  stage 01-03. Where Vulkan is unavailable, or the GPU is lost mid-game,
  the game renders with the software NV2A renderer instead: parallel, SIMD
  rasterisation with register combiners, stencil, bump-environment mapping
  and render-to-texture. That renderer is the reference
  (`play_software.bat`), and on a fast multi-core CPU it holds about 30 fps
  in the hub and stage 01-01.
- **Audio:** the APU's voice processor is emulated and played through
  XAudio2. Music, effects and movie sound work. The DSP effects (reverb)
  are not emulated; Blinx 2 does not appear to use them.
- **Input:** keyboard and XInput pads.
- **Saves** work.
- **Known issues:** later parts of the game are largely untested. With the
  software renderer, busy scenes such as stage 01-03's rain dip into the low
  20s fps, and a slow or low-core-count CPU gives a low frame rate.

## Legal

No game files are included or distributed here, and none will be. The
recompiled code is generated from your own copy of the game and is not part
of this repository either. You need a legally obtained copy of the original
Xbox disc of Blinx 2.

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 with the "Desktop development with C++" workload (MSVC).
- CMake 3.20 or newer (the one bundled with Visual Studio works).
- Python 3.10+ (3.12 is what we use) with `capstone`:
  `py -3 -m pip install capstone` (and `pytest` if you want to run the
  toolkit's tests).
- Git.
- [xdvdfs](https://github.com/antangelo/xdvdfs) to extract the disc image.
- Disk space: about 2 GB for the game files, 1.5 GB for the generated C and
  a few GB for the build.

## Getting the source

Either clone the superproject, which pins matching commits of both repos:

```
git clone --recurse-submodules https://github.com/bruce-smith/blinx-2-recompile-full.git
```

or clone the two repositories side by side (the build expects
`../xboxrecomp` next to this one):

```
git clone https://github.com/bruce-smith/Blinx2-Recompiled.git blinx2-recomp
git clone -b blinx2-fixes https://github.com/bruce-smith/xboxrecomp.git xboxrecomp
```

The rest of this README assumes this layout:

```
<root>\
  blinx2-recomp\     this repository
  xboxrecomp\        the xboxrecomp fork, branch blinx2-fixes
  game_files\        the extracted disc (next section)
```

## Extracting the game files

1. Make a disc image (ISO/XISO) of your own Blinx 2 disc with your usual
   tool.
2. Extract it with xdvdfs into `<root>\game_files`:

   ```
   xdvdfs unpack "Blinx 2.iso" <root>\game_files
   ```

`game_files` must then contain `default.xbe` (about 38 MB) and the game's
`.ipk`/`.iph` archives (`media.ipk`, `lng_us.ipk`, ...) together with its
subfolders (`movie`, `adx`, ...), exactly as they are on the disc. Do not
rename or repack anything.

The exe looks for the game files in this order:

1. the folder in the environment variable `RECOMP_GAME_DIR`;
2. a `game_files` folder next to `blinx_2_recomp.exe`, if it holds
   `default.xbe`;
3. the folder set at configure time with `-DBLINX2_GAME_DIR=<path>`.

It logs the folder it uses (`[BOOT] game files: ...`). Your saves
(`UDATA`/`TDATA`) are written into that folder.

## Generating the recompiled C

The generated C (`src/recomp/gen/`, about 3 million lines) is not in the
repository; you generate it from your own `default.xbe`. Run every step from
the `xboxrecomp` folder: the tools read and write their intermediate output
relative to the current directory.

```
cd <root>\xboxrecomp
py -3 -m tools.xbe_parser   ..\game_files\default.xbe --json ..\game_files\mygame_analysis.json
py -3 -m tools.disasm       ..\game_files\default.xbe --text-only --extra-sections DSOUND,D3D,D3DX,XGRPH,XPP,SRCADV,SRCED,SRCAC,PSFD_I,PSFD_B,PSFD_P,PSFD00 --seed-functions ..\blinx2-recomp\seeds\seed_functions.json
py -3 -m tools.func_id      ..\game_files\default.xbe -v
py -3 -m tools.abi_analysis ..\game_files\default.xbe -v
py -3 -m tools.recomp       ..\game_files\default.xbe --all --split 250 --gen-dir ..\blinx2-recomp\src\recomp\gen --exclude-manual ..\blinx2-recomp\src\recomp_manual.c --coalesce-functions ..\blinx2-recomp\seeds\coalesce.json --yield-points ..\blinx2-recomp\seeds\yield_points.json --loop-heads
```

Expect about 20 minutes in total: seconds for the parser, about a minute
each for `disasm`, `func_id` and `abi_analysis`, and about 15 minutes for
`recomp`.

Each option matters:

- `--extra-sections`: Blinx 2 has x86 code outside `.text` (the XDK
  libraries and the Sofdec movie decoder). Code that is not swept turns into
  stubs and corrupts the guest stack.
- `--seed-functions`: entry points that are only reached indirectly and
  that static analysis cannot find.
- `--exclude-manual`: functions that `src/recomp_manual.c` overrides are
  emitted as `<name>_gen`; without it the link fails.
- `--coalesce-functions`: merges false function splits that would
  otherwise recurse on the host stack.
- `--yield-points` / `--loop-heads`: needed by the exact replay
  (`replay_blinx2.bat`); they cost almost nothing in normal play.

Keep the analysis file named `mygame_analysis.json` next to `default.xbe`,
and keep only one `.xbe` in `game_files`. The tools find the analysis by that
pattern.

## Building

```
cd <root>\blinx2-recomp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Add `-DBLINX2_GAME_DIR=<root>/game_files` to the first command to bake in
your game folder (otherwise use `RECOMP_GAME_DIR` or a `game_files` folder
next to the exe). The first build compiles the generated C and takes 15-20
minutes; later builds are incremental. The result is
`build\Release\blinx_2_recomp.exe`.

Regenerating the C rewrites every generated file, so the build after a
regeneration is a full rebuild again.

## Running

Double-click `build\Release\blinx_2_recomp.exe`. It opens a window, with sound
and keyboard/pad input on, and draws with Vulkan on your graphics card.

`play_software.bat` plays the same way with the software renderer, the
reference: use it if something looks wrong on Vulkan, to tell a renderer bug
from a game bug. Without a usable Vulkan driver the exe falls back to the
software renderer by itself. The log's `[GPU] renderer:` line says which
renderer ran, and why.

### Controls

| Xbox pad | Keyboard |
|---|---|
| D-pad | Arrow keys |
| Left stick | Numpad 8 / 4 / 2 / 6 |
| Right stick | I / J / K / L |
| A / B / X / Y | Z / X / A / S |
| Black / White | E / Q |
| Left / right trigger | 1 / 3 |
| Left / right stick click | Shift / Ctrl |
| Start / Back | Enter / Backspace |

An XInput pad (e.g. an Xbox controller) works as well. LB/RB map to
White/Black, and the triggers are analog.

Window: it opens at 2x size and is resizable with the aspect ratio kept.
F11 or Alt+Enter toggles full screen; F9 switches between nearest and linear
filtering.

### Files

- Saves: `UDATA` and `TDATA` in the game files folder (or in
  `RECOMP_SAVE_DIR`, if set).
- Cache (the game copies archives to its Z: drive):
  `%LOCALAPPDATA%\xboxrecomp`.
- Logs, next to the exe: `blinx2_recomp.log` (the game's log) and
  `xbox_kernel.log`. The previous five runs' logs are kept as
  `*.prev-<time>.log`. Attach them to bug reports.

### Recording and exact replay

- `record_blinx2.bat` plays as usual and records the run into
  `build\Release\recordings\<date-time>\`: a copy of the starting saves, the
  pad input, the guest thread schedule and what the game read from the host
  clock.
- `replay_blinx2.bat <recording folder> [window]` replays it exactly,
  headless by default, from a private copy of the recorded saves, and with
  the renderer the recording was made with (from its `game.log`). It is
  useful to reproduce a bug.

### Useful switches

Environment variables, set before starting the exe:

| Variable | Effect |
|---|---|
| `RECOMP_GAME_DIR=<dir>` | Folder with `default.xbe` and the archives. |
| `RECOMP_SAVE_DIR=<dir>` | Keep saves and cache in `<dir>` instead. |
| `RECOMP_GPU=software` | Software renderer, the reference (`play_software.bat`). The default is Vulkan, with software as the fallback. |
| `RECOMP_AUDIO=0` | No sound. |
| `RECOMP_FB_SCALE=<n>`, `RECOMP_FB_FILTER=linear` | Initial window scale and filter. |
| `RECOMP_RASTER_THREADS=<n>` | Software renderer: raster worker threads (default: CPU count minus 4). |
| `RECOMP_ASYNC_SYNC=0` | Software renderer: fully drain at sync points (slower; for troubleshooting). |
| `RECOMP_HEADLESS=1` | No window, no dialogs (for scripted runs). |
| `RECOMP_WATCHDOG_STALL=<s>` | Dump diagnostics and exit if no frame is presented for `<s>` seconds. |
| `RECOMP_VK_PRESENT=1` | Show frames through a Vulkan swapchain instead of the plain window drawing (needed for frame generation and full-resolution anti-aliasing). Menu: Graphics > Present through Vulkan. |
| `RECOMP_VK_SCALE=3` or `5` | Vulkan renderer: supersampling (anti-aliasing). Full resolution with `RECOMP_VK_PRESENT=1`. Menu: Graphics > Supersampling. |
| `RECOMP_VK_AA=fxaa` | Vulkan presentation: FXAA edge smoothing. Menu: Graphics > Anti-aliasing. |
| `RECOMP_VK_FPS_CAP=30` or `60` | Vulkan presentation: show at most that many frames a second. Menu: Graphics > Frame-rate cap. |
| `RECOMP_FB_FULLSCREEN=1` | Start in borderless full screen. Menu: Graphics > Start in fullscreen. |

### Frame generation

The game runs at 30 frames a second, as on the Xbox. Your graphics driver
can double the frames you see to 60 by generating one between each pair
(NVIDIA Smooth Motion, AMD Fluid Motion Frames). No mod or game setting
changes what the game itself does; the driver works on the frames it is
shown. Expect about one extra frame (~33 ms) of input delay.

1. In the game's menu (Escape) > Graphics, set:
   - **Present through Vulkan**: on (takes effect at the next start);
   - **Frame-rate cap (Vulkan presentation)**: 30, so the driver always
     gets one steady rate (the title screens run at 60);
   - **Start in fullscreen**: on, or press F11 once the game is running.

   (The same as `RECOMP_VK_PRESENT=1`, `RECOMP_VK_FPS_CAP=30` and
   `RECOMP_FB_FULLSCREEN=1`.)
2. Turn the driver feature on for `blinx_2_recomp.exe`:
   - **NVIDIA (GeForce RTX 40 series or newer, current driver):** open the
     NVIDIA App > Graphics > Program settings > Add a program, pick
     `build\Release\blinx_2_recomp.exe`, then under Driver settings set
     **Smooth Motion** to On.
   - **AMD (Radeon RX 6000 series or newer):** AMD Software: Adrenalin
     Edition > Gaming > Graphics (or the game's own profile) > **AMD Fluid
     Motion Frames** on.
3. Start the game and play in full screen. To check that it works, open the
   driver's performance overlay (NVIDIA: Alt+R; AMD: Ctrl+Shift+O): the
   displayed frame rate should be about 60 while the game's own frame
   counter (the window title) stays at 30.

To turn it off, set Smooth Motion / Fluid Motion Frames back to off.
Without Vulkan presentation (the default window) the drivers have nothing to
work on, so nothing changes. The Escape menu is drawn over the picture in
every mode.

## Exporting stages and characters (glTF)

`export_assets.bat` exports the game's stages and characters as glTF 2.0
(`.glb`) files, which Blender, Unity, Unreal, Godot and most other tools and
engines import. It reads your own copy of the game; keep the files for your
own use.

```
export_assets.bat [game_files] [extracted assets] [output folder]
```

The defaults are your `game_files` (or `RECOMP_GAME_DIR`), the superproject's
`assets\extracted` (the `.ipk` archives unpacked with blipks, see
`assets\README.md`; without them nothing is textured) and `export\` here.
It needs Python 3 with numpy and Pillow, and takes about ten minutes. Output:

- `stages\`: all 26 stage sections (`M11` = stage 01-01 ... `M121`, and
  `G1`-`G5`). Each has its textured geometry, its collision meshes (nodes
  named `*_collision`) and the game's object placements as marker nodes with
  the object type.
- `characters\<save>\`: your own characters, from each save in
  `game_files\UDATA`, as `ts0_<name>.glb` ... (Time Sweepers) and
  `tg0_<name>.glb` ... (Tom-Tom gang). Each is dressed as you made it
  (parts, designs, colours), rigged with the game's 147-bone skeleton, and
  carries the game's 298 motions as animations.
- `presets\`: the character editor's 45 stock characters, rigged.

Not exported yet: the editor's body-shape sliders (characters come out with
the default proportions), and the meshes of the placed objects (only their
markers). The tool behind it and all its options, for example a single
stage or character: `tools\ninja\README.md`.

## Linux and Steam Deck

The game builds and runs on x86-64 Linux as well (SteamOS is a target, not yet tried). The
recompiled C and the toolkit are the same; the host side uses SDL2 for the
window, keyboard, pads and sound. Status: it runs headless (intro movies,
title, the stage 01-01 attract demo at 30 fps in the software renderer);
the window, pad, sound and the Vulkan renderer have not yet
been tried on a real Linux desktop or a Deck.

**Build natively** (Ubuntu/Debian package names; the generated C must be in
`src/recomp/gen`, see above -- the Python pipeline runs on Linux too, with
`python3` instead of `py -3`):

```
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl2-dev libvulkan-dev libssl-dev libepoxy-dev libgl-dev
cd <root>/blinx2-recomp
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

Or **in Docker**, reproducibly: `linux/build-in-docker.sh` (gcc; pass `clang`
for clang). The compile of the generated C takes about 5 minutes on 12
cores. A container-built executable needs glibc 2.39 or newer where it runs.

**Run**: `./play.sh` (Vulkan by default, falling back to software), `./play_software.sh`
(software renderer), `./record_blinx2.sh` and
`./replay_blinx2.sh <folder> [window]` -- the `.bat` files' counterparts. Put
the game files in `build-linux/game_files` or point `RECOMP_GAME_DIR` at them.

Differences from Windows:

- Saves (UDATA/TDATA) are in the game files folder as on Windows; the Z:
  cache is in `$XDG_DATA_HOME/xboxrecomp` (`~/.local/share/xboxrecomp`).
  Logs and `blinx2_settings.ini` are next to the executable, or in
  `$XDG_STATE_HOME/blinx2-recomp` if that folder is read-only.
- File names are matched ignoring case, as on the console.
- Pads: anything SDL2 knows (Steam Deck controls, Xbox and PlayStation pads),
  with the same mapping as XInput on Windows.
- Under gamescope (Steam Deck Game Mode, Big Picture) the window starts full
  screen; `RECOMP_FB_FULLSCREEN=1` or `=0` overrides.
- Not yet on Linux: the in-game menu (Escape) and Vulkan presenting its own
  swapchain (`RECOMP_VK_PRESENT`); rendering itself works on either renderer.

## Troubleshooting

- **"Failed to load ...\default.xbe"**: the game files are not where the
  exe looks (see [Extracting the game files](#extracting-the-game-files)).
  The message names the path it tried.
- **"Cannot open include file: recomp_types.h"** or missing `recomp_*.c`:
  the `recomp` step did not finish. Run it again.
- **Link errors about `*_gen` functions or duplicate `sub_XXXXXXXX`**: the
  generated C is older than `src/recomp_manual.c`. Rerun the pipeline with
  `--exclude-manual` as shown.
- **Unresolved externals like `recomp_0042.c`**: the set of generated files
  changed. Rerun the CMake configure step.
- **Low frame rate**: check the log's `[GPU] renderer:` line. If it says
  software (no usable Vulkan driver, or `RECOMP_GPU=software`), rendering
  runs on the CPU: update the graphics driver, close other heavy programs;
  more cores help.
- **Something looks wrong**: try `play_software.bat`. If the software
  renderer shows it correctly, it is a Vulkan renderer bug; please report
  it with both logs.
- **A crash**: the game shows a message box and writes the details (guest
  registers, call stack) to `blinx2_recomp.log`.

## License and credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet (MIT).
  Our changes are in the [fork](https://github.com/bruce-smith/xboxrecomp).
- Blinx 2: Masters of Time and Space is © Artoon / Microsoft Game Studios.
  This project is not affiliated with or endorsed by them.
