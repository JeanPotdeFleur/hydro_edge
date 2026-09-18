#!/bin/bash
# b1_radiometry.sh - radiometric drift over a burst, from the frames on disk.
#
#   ./b1_radiometry.sh <burst_root> <out.csv> [period_s] [duration_s]
#
# The exposure is measured once at arming and then locked for ninety minutes.
# That is a documented decision of the report and it has no evidence behind it:
# over an hour and a half a cloud can move the scene by more than two stops.
# This samples one frame per camera per minute and records what the sensor
# actually saw, so the decision is either confirmed or contradicted.
#
# The acquisition binary holds both cameras exclusively, so nothing else can
# open a sensor. The frames already written are read instead, through the same
# decode used for the science data, at one file per minute: negligible beside
# the 64.5 MB/s the consumer is writing. A frame counts only when its size is
# exactly one payload, a short file being one still under the writer's hand.

set -u

ROOT=${1:?usage: b1_radiometry.sh <burst_root> <out.csv> [period_s] [duration_s]}
OUT=${2:?output csv}
PER=${3:-60}
DUR=${4:-5400}
DEC=${DECODE_BIN:-$HOME/hydro_edge/build/decode}
SIZE=16130240

[ -x "$DEC" ] || { echo "[FATAL] $DEC is not executable." >&2; exit 1; }
echo "utc,elapsed_s,role,frame,p50,p99,p999,max,clipped_pct,r_pct,g_pct,b_pct,black_pct" > "$OUT"

newest() {
    d=$(ls -1dt "$ROOT"/*/"$1"_* 2>/dev/null | head -1) || return 1
    [ -n "$d" ] || return 1
    ls -1t "$d"/*.raw 2>/dev/null | head -4 | while read -r p; do
        [ "$(stat -c%s "$p" 2>/dev/null)" = "$SIZE" ] && { echo "$p"; break; }
    done
}

t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt "$DUR" ]; do
    el=$(( $(date +%s) - t0 ))
    for role in cam0 cam1; do
        f=$(newest "$role") || continue
        [ -n "$f" ] || continue
        "$DEC" --stats-only "$f" 2>/dev/null | awk -v u="$(date -u +%FT%TZ)" \
            -v e="$el" -v r="$role" -v n="$(basename "$f")" '
            /^\[OK\]/ {
                for (i = 1; i <= NF; i++) {
                    if ($i == "p50")     p50  = $(i+1)
                    if ($i == "p99")     p99  = $(i+1)
                    if ($i == "p99.9")   p999 = $(i+1)
                    if ($i == "max")     mx   = $(i+1)
                    if ($i == "clipped") { c = $(i+1); sub(/%/, "", c) }
                    if ($i == "black")   { k = $(i+1); sub(/%/, "", k) }
                }
                # the per-channel figures arrive as (R 1.23 G 4.56 B 7.89)
                gsub(/[()]/, " ")
                for (i = 1; i <= NF; i++) {
                    if ($i == "R") rr = $(i+1)
                    if ($i == "G") gg = $(i+1)
                    if ($i == "B") bb = $(i+1)
                }
                printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                       u, e, r, n, p50, p99, p999, mx, c, rr, gg, bb, k
            }' >> "$OUT"
    done
    sleep "$PER"
done

echo "[RAD] done, $(( $(wc -l < "$OUT") - 1 )) samples in $OUT"