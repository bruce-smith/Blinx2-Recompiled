#!/usr/bin/env bash
# Play Blinx 2 on Linux / SteamOS: the counterpart of double-clicking
# build\Release\blinx_2_recomp.exe on Windows. Window, sound, pad and keyboard
# are on by default (src/main.c), and the game files are found as on Windows:
# RECOMP_GAME_DIR, else game_files/ next to the executable, else the
# BLINX2_GAME_DIR the build was configured with.
#
#   ./play.sh              Vulkan renderer (the default), software if Vulkan
#                          cannot start
#   RECOMP_GPU=software ./play.sh   the CPU renderer (see play_software.sh)
#
# The log goes to blinx2_recomp.log next to the executable, as on Windows (the
# game writes it, rotating the last runs', when stderr is /dev/null).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exe=""
for c in "$here/build-linux/blinx_2_recomp" "$here/build/blinx_2_recomp" "$here/build/Release/blinx_2_recomp" \
         "$here/blinx_2_recomp"; do
    [ -x "$c" ] && { exe="$c"; break; }
done
[ -n "$exe" ] || { echo "blinx_2_recomp not found; build it first (see README.md)" >&2; exit 1; }
exec "$exe" "$@" 2>/dev/null
