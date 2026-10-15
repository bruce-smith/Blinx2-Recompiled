#!/usr/bin/env bash
# Play Blinx 2 as usual and record the run so it replays exactly
# (replay_blinx2.sh): the input, the order the game's threads and devices ran
# in, and everything the game read from the host clock. The Linux counterpart
# of record_blinx2.bat.
#
# Each run gets a folder <exe dir>/recordings/<date-time>/ holding
#   save/            the saves, title data and Z: cache the run started from
#   schedule.sched   the guest schedule (about 10-15 KB a second of play)
#   input.rec        the pad input
#   game.log         the game's log
# Close the game window to end the run; the recording is complete then.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exe=""
for c in "$here/build/blinx_2_recomp" "$here/build/Release/blinx_2_recomp" \
         "$here/blinx_2_recomp"; do
    [ -x "$c" ] && { exe="$c"; break; }
done
[ -n "$exe" ] || { echo "NOT RECORDING: blinx_2_recomp not found" >&2; exit 1; }
exe_dir="$(dirname "$exe")"

# A build that records a replayable schedule carries this string (sched.c).
if ! grep -aq "guest schedule (format %u)" "$exe"; then
    echo "NOT RECORDING: this blinx_2_recomp is too old to make exact recordings." >&2
    exit 1
fi
# Two instances share the saves and the Z: cache.
if pgrep -f -x "$exe" >/dev/null 2>&1; then
    echo "NOT RECORDING: the game is already running from $exe_dir. Close it first." >&2
    exit 1
fi

# Where the saves are when RECOMP_SAVE_DIR is not set: UDATA/TDATA in the game
# folder (found as the game finds it), the Z: cache under the XDG data dir.
game_dir="${RECOMP_GAME_DIR:-}"
[ -z "$game_dir" ] && [ -f "$exe_dir/game_files/default.xbe" ] && game_dir="$exe_dir/game_files"
if [ -z "$game_dir" ]; then
    for cache in "$exe_dir/CMakeCache.txt" "$exe_dir/../CMakeCache.txt"; do
        [ -f "$cache" ] && { game_dir="$(grep -m1 '^BLINX2_GAME_DIR:' "$cache" | cut -d= -f2-)"; break; }
    done
fi
cache_dir="${XDG_DATA_HOME:-$HOME/.local/share}/xboxrecomp"

stamp="$(date +%Y%m%d-%H%M%S)"
rec="$exe_dir/recordings/$stamp"
mkdir -p "$rec/save"
echo "Saving the starting state to $rec/save ..."
copy_tree() {   # src dst: everything but the partition images
    [ -d "$1" ] || return 0
    mkdir -p "$2"
    (cd "$1" && find . -type f ! -name 'Partition*.img' -print0 \
        | xargs -0 -I{} cp --parents {} "$2/")
}
if [ -n "${RECOMP_SAVE_DIR:-}" ]; then
    copy_tree "$RECOMP_SAVE_DIR" "$rec/save"
else
    copy_tree "$cache_dir" "$rec/save"
    [ -n "$game_dir" ] && copy_tree "$game_dir/UDATA" "$rec/save/UDATA"
    [ -n "$game_dir" ] && copy_tree "$game_dir/TDATA" "$rec/save/TDATA"
fi

export RECOMP_SCHED_RECORD="$rec/schedule.sched"
export RECOMP_INPUT_RECORD="$rec/input.rec"
echo "Recording this run to $rec"
echo "Game log: $rec/game.log"
cd "$exe_dir"
"$exe" 2>"$rec/game.log"
rc=$?
echo
echo "Game exited (code $rc)."
echo "Recording saved in $rec"
echo "Replay it with:  ./replay_blinx2.sh \"$rec\""
