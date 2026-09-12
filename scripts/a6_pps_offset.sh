#!/usr/bin/env bash
#
# a6_pps_offset.sh -- sample the system clock against the GNSS timepulse.
#
# The kernel stamps every PPS assert on CLOCK_REALTIME. The true edge falls on
# an exact second by construction, so the fractional part of that timestamp is
# the offset between the system clock and the reference. No extra hardware and
# no extra daemon: the measurement is already there, in one sysfs file.
#
# What a night of it shows is the discipline regime: the sawtooth of SNTP step
# corrections, and the free-running drift of the oscillator between them. That
# is the quantity the absolute-timestamping requirement turns on, and the
# argument for or against moving to chrony with a PPS refclock.
#
# Pinned to core 0 by the caller, away from the producer on 2 and the consumer
# on 3.
#
# Usage:  ./a6_pps_offset.sh [output.csv] [interval_s]
#
set -o nounset

OUT="${1:-${HOME}/hydro_edge/docs/a6_pps_offset.csv}"
INTERVAL="${2:-10}"
DEV="/sys/class/pps/pps0/assert"
THERMAL="/sys/class/thermal/thermal_zone0/temp"

if [ ! -r "$DEV" ]; then
    echo "[A6] FATAL cannot read ${DEV}." >&2
    exit 1
fi

# A source that does not advance its counter is not pulsing, and sampling it
# for a night would produce a flat line mistaken for a perfect clock.
c0="$(cut -d'#' -f2 < "$DEV")"
sleep 2
c1="$(cut -d'#' -f2 < "$DEV")"
if [ "$c0" = "$c1" ]; then
    echo "[A6] FATAL ${DEV} is silent: counter held at ${c0}." >&2
    exit 1
fi
echo "[A6] ${DEV} pulsing (${c0} -> ${c1}), sampling every ${INTERVAL} s into ${OUT}"

[ -f "$OUT" ] || echo "utc,assert_s,assert_ns,offset_us,pulse_count,soc_temp_c" > "$OUT"

trap 'echo "[A6] stopped after $(( $(wc -l < "$OUT") - 1 )) samples."; exit 0' INT TERM

while :; do
    raw="$(cat "$DEV" 2>/dev/null)" || raw=""
    case "$raw" in
        *.*#*)
            s="${raw%%.*}"
            rest="${raw#*.}"
            ns="${rest%%#*}"
            cnt="${rest#*#}"
            # Fold onto [-500 ms, +500 ms]: a clock running just behind the
            # edge reads 0.999... and would otherwise jump a full second.
            read -r off temp < <(awk -v ns="$ns" -v th="$THERMAL" 'BEGIN {
                n = ns + 0;
                if (n > 500000000) n -= 1000000000;
                printf "%.1f ", n / 1000;
                if ((getline t < th) > 0) printf "%.1f", t / 1000; else printf "NA";
            }')
            printf '%s,%s,%s,%s,%s,%s\n' \
                "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$s" "$ns" "$off" "$cnt" "$temp" >> "$OUT"
            ;;
        *)
            printf '%s,,,,,\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$OUT"
            ;;
    esac
    sleep "$INTERVAL"
done
