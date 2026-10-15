#!/usr/bin/env bash
# Replay a run recorded with record_blinx2.sh (or record_blinx2.bat), exactly:
# the same game state at every frame, on any machine speed.
#
#   ./replay_blinx2.sh <recording folder> [window]
#
# Headless by default (no window, sound muted, log to <folder>/replay.log);
# "window" shows it. The run starts from a private copy of the recording's
# saves (${TMPDIR:-/tmp}/blinx2_replay_save), so your own saves are never
# touched. Extra switches for troubleshooting can be exported first, e.g.
#   RECOMP_INPUT_HASH=10,hash.bin ./replay_blinx2.sh <folder>
# Exit code 4: the run left the recording's path ("[REPLAY] diverged" in the
# log says where).
set -uo pipefail
if [ $# -lt 1 ]; then
    echo "usage: replay_blinx2.sh <recording folder> [window]" >&2
    exit 1
fi
rec="$(cd "$1" && pwd)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exe=""
for c in "$here/build/blinx_2_recomp" "$here/build/Release/blinx_2_recomp" \
         "$here/blinx_2_recomp"; do
    [ -x "$c" ] && { exe="$c"; break; }
done
[ -n "$exe" ] || { echo "blinx_2_recomp not found; build it first" >&2; exit 1; }
if [ ! -f "$rec/schedule.sched" ]; then
    echo "$rec has no schedule.sched -- not a record_blinx2 recording" >&2
    exit 1
fi

save="${TMPDIR:-/tmp}/blinx2_replay_save"
rm -rf "$save"
mkdir -p "$save"
# The partition images are large and never change; the game recreates them.
(cd "$rec/save" && find . -type f ! -name 'Partition*.img' -print0 \
    | xargs -0 -I{} cp --parents {} "$save/")

export RECOMP_SAVE_DIR="$save"
export RECOMP_SCHED_REPLAY="$rec/schedule.sched"
export RECOMP_INPUT_REPLAY="$rec/input.rec"
export RECOMP_INPUT_RECORD=0
# The renderer the recording was made with: a run replays exactly only with
# its own renderer; recordings from before Vulkan became the default
# (2026-10-14) were made in software.
export RECOMP_GPU=software
if [ -f "$rec/game.log" ] && grep -q "\[GPU\] renderer: vulkan" "$rec/game.log"; then
    export RECOMP_GPU=vulkan
fi
echo "Renderer: $RECOMP_GPU"

cd "$(dirname "$exe")"
if [ "${2:-}" = "window" ]; then
    echo "Replaying $rec in a window; log: $rec/replay.log"
    "$exe" 2>"$rec/replay.log"
    rc=$?
else
    # Headless, with the devices the recording ran with: the pushbuffer
    # executor, USB and the APU (muted).
    export RECOMP_HEADLESS=1 RECOMP_PB_EXEC=1 RECOMP_USB=1
    export RECOMP_AUDIO=1 RECOMP_AUDIO_MUTE=1
    echo "Replaying $rec headless; log: $rec/replay.log"
    "$exe" 2>"$rec/replay.log"
    rc=$?
fi
if [ $rc -eq 4 ]; then
    echo "Replay STOPPED: it diverged from the recording. See $rec/replay.log"
    exit 4
fi
echo "Replay finished (exit code $rc)."
exit $rc
