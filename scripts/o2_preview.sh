#!/bin/bash
# o2_preview.sh - live preview during an acquisition burst.
#
#   ./o2_preview.sh <cam0|cam1> [root]
#
# The acquisition binary holds both cameras exclusively, so no second process
# can open a sensor. This reads instead what the binary has already written to
# disk and turns the most recent complete frame into a JPEG. Read-only on the
# archive; the only thing written is the preview image and a scratch directory.
#
# Frames are picked by size, not by age alone: a .raw that is short is still
# being written, and decoding it would show a torn image and be taken for a
# missed pose. The scratch directory sits under $HOME so that the final move is
# a rename within one filesystem, hence atomic, and the viewer never opens a
# half-written file.

set -u

ROLE=${1:?usage: o2_preview.sh <cam0|cam1> [root]}
ROOT=${2:-/mnt/vault2/calib_o2/intrinsics}
DEC=${DECODE_BIN:-$HOME/hydro_edge/build/decode}
TMP=${TMP_DIR:-$HOME/.prevtmp}
OUT=${PREV_PATH:-$HOME/hydro_edge/focus.jpg}
SIZE=16130240            # 5320 x 3032 x 1 byte, the geometry decode assumes

[ -x "$DEC" ] || { echo "[FATAL] $DEC is not executable." >&2; exit 1; }
mkdir -p "$TMP"

echo "watching $ROOT for $ROLE   ->   $OUT   (Ctrl-C to stop)"
last=""
while true; do
    d=$(ls -1dt "$ROOT"/*/"$ROLE"_* 2>/dev/null | head -1)
    if [ -n "$d" ]; then
        f=$(ls -1t "$d"/*.raw 2>/dev/null | head -3 | while read -r p; do
                [ "$(stat -c%s "$p" 2>/dev/null)" = "$SIZE" ] && { echo "$p"; break; }
            done)
        if [ -n "$f" ] && [ "$f" != "$last" ]; then
            rm -f "$TMP"/*.jpg
            if "$DEC" --jpg --scale 4 --out "$TMP" "$f" >/dev/null 2>&1; then
                j=$(ls -1 "$TMP"/*.jpg 2>/dev/null | head -1)
                [ -n "$j" ] && mv -f "$j" "$OUT" && last=$f
            fi
        fi
    fi
    sleep 0.5
done