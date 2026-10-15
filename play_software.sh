#!/usr/bin/env bash
# Play Blinx 2 with the software renderer: the CPU draws the game. It is the
# reference: if something looks wrong with ./play.sh (Vulkan), see whether it
# looks right here -- if it does, it is a Vulkan renderer bug. Everything else
# is exactly ./play.sh. Needs a fast multi-core CPU for 30 fps.
export RECOMP_GPU=software
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/play.sh" "$@"
