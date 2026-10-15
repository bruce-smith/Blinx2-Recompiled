#!/usr/bin/env bash
# Build blinx2-recomp for Linux (x86-64) in a container, reproducibly:
#
#   linux/build-in-docker.sh [gcc|clang] [extra cmake args...]
#
# Needs Docker, the sibling layout <root>/blinx2-recomp + <root>/xboxrecomp,
# and the generated C in src/recomp/gen (see "Generating the recompiled C").
# The executable lands in <root>/blinx2-recomp/build-linux/blinx_2_recomp;
# run it with ./play.sh. Built on Ubuntu 24.04, it needs glibc 2.39 or newer
# and the SDL2 runtime on the machine that runs it; on an older distro, build
# natively instead (README, "Linux").
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
proj="$(dirname "$here")"
root="$(dirname "$proj")"
cc="${1:-gcc}"
[ $# -gt 0 ] && shift
image=blinx2-recomp-linux-build

[ -d "$root/xboxrecomp/src" ] || { echo "no xboxrecomp next to blinx2-recomp ($root)" >&2; exit 1; }
ls "$proj"/src/recomp/gen/recomp_*.c >/dev/null 2>&1 \
    || { echo "src/recomp/gen is empty: generate the C first" >&2; exit 1; }

docker build -q -t "$image" "$here" >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$root:/src" -w /src/blinx2-recomp "$image" \
    bash -c "cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
                   -DCMAKE_C_COMPILER=$cc $* \
             && cmake --build build-linux"
echo "Built: $proj/build-linux/blinx_2_recomp"
