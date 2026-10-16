#!/bin/bash
# Package the Linux build for someone who has their own copy of the game: the
# executable, the .sh launchers, the README, a settings file to point at the
# game files, the licences and a QUICKSTART listing the libraries it needs. No
# game data, no saves, no logs (see the README, "Extracting the game files").
#
#   tools/package_linux.sh [build dir] [output dir]
#
# The build dir defaults to build-linux (then build). Writes
# <output dir>/blinx2-recomp-linux-<commit>.tar.gz and leaves the unpacked
# folder next to it. The launchers find the executable next to themselves.
#
# The executable runs where the C library is at least as new as the one it was
# built against (QUICKSTART says which). For a package that runs on most
# systems, build on an older base: linux/build-in-docker.sh (Ubuntu 24.04).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-}"
if [ -z "$build" ]; then
    for c in "$here/build-linux" "$here/build"; do
        [ -x "$c/blinx_2_recomp" ] && { build="$c"; break; }
    done
fi
exe="$build/blinx_2_recomp"
[ -x "$exe" ] || { echo "blinx_2_recomp not found in '${build}'; build it first (README.md, Linux and Steam Deck)" >&2; exit 1; }
out="${2:-$here/dist}"
mkdir -p "$out"; out="$(cd "$out" && pwd)"

blinx=unknown; xbr=unknown
if [ -f "$build/build_sha/build_sha.h" ]; then
    blinx=$(sed -n 's/.*BLINX2_GIT_SHA "\([^"]*\)".*/\1/p' "$build/build_sha/build_sha.h")
    xbr=$(sed -n 's/.*XBOXRECOMP_GIT_SHA "\([^"]*\)".*/\1/p' "$build/build_sha/build_sha.h")
fi
case "$blinx$xbr" in *dirty*) echo "warning: built from uncommitted sources ($blinx / $xbr)" >&2 ;; esac
name="blinx2-recomp-linux-$(printf '%s' "$blinx" | tr -cd '0-9A-Za-z.+-')"
stage="$out/$name"
rm -rf "$stage"; mkdir -p "$stage/licenses"

cp "$exe" "$stage/"
for f in play.sh play_software.sh record_blinx2.sh replay_blinx2.sh; do
    [ -f "$here/$f" ] && cp "$here/$f" "$stage/" && chmod +x "$stage/$f"
done
cp "$here/README.md" "$stage/"
xr="$(dirname "$here")/xboxrecomp"
[ -f "$xr/LICENSE" ] && cp "$xr/LICENSE" "$stage/licenses/xboxrecomp-LICENSE.txt"
[ -d "$xr/LICENSES" ] && cp -r "$xr/LICENSES/." "$stage/licenses/"

cat > "$stage/blinx2_settings.ini" <<'EOF'
; Blinx 2 (recomp) settings. The in-game menu (Escape) writes the rest.
; game_dir: the folder holding default.xbe and the .ipk/.iph files, e.g.
;   game_dir=/home/deck/Games/Blinx2/game_files
; Leave it empty to use a game_files folder next to blinx_2_recomp.
[paths]
game_dir=
EOF

# What the executable needs from the system: the newest C library and C++
# library versions it uses, and the shared libraries it links.
glibc=$(objdump -T "$exe" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | sed 's/GLIBC_//')
glibcxx=$(objdump -T "$exe" 2>/dev/null | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1 | sed 's/GLIBCXX_//')
needed=$(objdump -p "$exe" 2>/dev/null | awk '/NEEDED/ {print $2}' | tr '\n' ' ')
cat > "$stage/QUICKSTART.txt" <<EOF
Blinx 2 (recomp) for Linux x86-64 -- blinx2-recomp $blinx, xboxrecomp $xbr

You need your own copy of the game: the files extracted from the disc
(default.xbe and the .ipk/.iph archives). README.md, "Extracting the game
files", says how. None are included here.

1. Put them in a folder named game_files next to blinx_2_recomp, or write
   their folder into blinx2_settings.ini: game_dir=<folder>. Keep them on a
   Linux file system (under WSL, not on /mnt/c).
2. ./play.sh (Vulkan renderer if the system has a Vulkan driver, else
   software) or ./play_software.sh (software renderer). Keys and pads:
   README.md, "Controls". Escape opens the menu.

Saves go in <game files folder>/UDATA; the Z: cache in
\$XDG_DATA_HOME/xboxrecomp (~/.local/share/xboxrecomp). Logs and recordings go
next to the executable, or to \$XDG_STATE_HOME/blinx2-recomp if this folder is
read-only.

Requirements:
- 64-bit x86 Linux with glibc ${glibc:-?} or newer and libstdc++ with
  GLIBCXX_${glibcxx:-?} (GCC's runtime); the executable links: ${needed:-?}
- SDL2 (window, keyboard, pads, sound), or sdl2-compat over SDL3:
    Debian/Ubuntu: libsdl2-2.0-0   Fedora: SDL2 (or sdl2-compat)
    Arch/SteamOS:  sdl2 (or sdl2-compat; preinstalled on SteamOS)
- For the Vulkan renderer (optional): the Vulkan loader (libvulkan.so.1;
  Debian/Ubuntu: libvulkan1, Fedora: vulkan-loader, Arch: vulkan-icd-loader)
  and a Vulkan 1.2 driver (Mesa RADV/ANV, or NVIDIA's). Without one the game
  draws in software, which needs several CPU cores for 30 fps.
- Sound through SDL2: PulseAudio, PipeWire or ALSA.
EOF

bad=$(find "$stage" \( -iname '*.xbe' -o -iname '*.ipk' -o -iname '*.iph' -o -iname '*.sfd' \
        -o -iname '*.adx' -o -iname '*.xbx' -o -iname '*.rec' -o -iname '*.sched' -o -iname '*.log' \
        -o -name UDATA -o -name TDATA -o -name game_files -o -name recordings \) -print)
[ -z "$bad" ] || { echo "game data or run output in the package: $bad" >&2; exit 1; }

tar -C "$out" -czf "$stage.tar.gz" "$name"
echo "Package: $stage.tar.gz ($(du -m "$stage.tar.gz" | cut -f1) MB); glibc >= ${glibc:-?}"
ls "$stage"
