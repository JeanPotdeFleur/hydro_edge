#!/bin/bash
#
# monitor_burst.sh - sample host state while an acquisition burst runs.
#
# The acquisition binary reports what it can see from inside its own process.
# This script reports what it cannot: resident memory as the kernel accounts
# it, SoC temperature and clock throttling, the volume of dirty page cache
# awaiting writeback, and the queue depth of the SATA device. Those four are
# where a frame loss originates when the application counters show nothing.
#
# Usage:  ./scripts/monitor_burst.sh [output.csv] [interval_seconds]
#
# Start it before the burst; it waits for the process to appear and stops on
# its own when the process exits. Ctrl+C also stops it cleanly.

set -u

OUT="${1:-/tmp/monitor_$(date -u +%Y%m%dT%H%M%SZ).csv}"
INTERVAL="${2:-2}"
PATTERN="hydro_edge"
DEV="sda"

echo "Monitoring to $OUT every ${INTERVAL}s. Waiting for '$PATTERN'..."

PID=""
for _ in $(seq 1 600); do
    PID=$(pgrep -f "build/${PATTERN}" | head -1)
    [ -n "$PID" ] && break
    sleep 0.5
done

if [ -z "$PID" ]; then
    echo "Process never appeared. Aborting."
    exit 1
fi

echo "Attached to PID $PID."
echo "elapsed_s,vm_rss_kb,vm_hwm_kb,threads,soc_temp_c,arm_clock_hz,throttled,dirty_kb,writeback_kb,mem_available_kb,vault_used_gb,dev_util_pct,dev_wmbps" > "$OUT"

START=$(date +%s)

# Baseline for the /proc/diskstats delta. Fields 6 and 7 of the device line
# are sectors written and milliseconds spent doing I/O.
read_disk() {
    awk -v d="$DEV" '$3==d {print $10, $13}' /proc/diskstats
}
PREV=$(read_disk)
PREV_SECT=$(echo "$PREV" | awk '{print $1}')
PREV_IOMS=$(echo "$PREV" | awk '{print $2}')

while kill -0 "$PID" 2>/dev/null; do
    sleep "$INTERVAL"
    kill -0 "$PID" 2>/dev/null || break

    NOW=$(date +%s)
    ELAPSED=$((NOW - START))

    RSS=$(awk '/VmRSS/{print $2}' "/proc/$PID/status" 2>/dev/null)
    HWM=$(awk '/VmHWM/{print $2}' "/proc/$PID/status" 2>/dev/null)
    THR=$(awk '/Threads/{print $2}' "/proc/$PID/status" 2>/dev/null)
    [ -z "${RSS:-}" ] && break

    TEMP=$(awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone0/temp)
    CLK=$(vcgencmd measure_clock arm 2>/dev/null | cut -d= -f2)
    THROT=$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)

    DIRTY=$(awk '/^Dirty:/{print $2}' /proc/meminfo)
    WB=$(awk '/^Writeback:/{print $2}' /proc/meminfo)
    AVAIL=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)

    USED=$(df -B1G --output=used /mnt/vault | tail -1 | tr -d ' ')

    CUR=$(read_disk)
    CUR_SECT=$(echo "$CUR" | awk '{print $1}')
    CUR_IOMS=$(echo "$CUR" | awk '{print $2}')
    DSECT=$((CUR_SECT - PREV_SECT))
    DIOMS=$((CUR_IOMS - PREV_IOMS))
    PREV_SECT=$CUR_SECT
    PREV_IOMS=$CUR_IOMS
    # 512-byte sectors; utilisation is I/O milliseconds over the wall interval.
    WMBPS=$(awk -v s="$DSECT" -v i="$INTERVAL" 'BEGIN{printf "%.1f", s*512/i/1048576}')
    UTIL=$(awk -v m="$DIOMS" -v i="$INTERVAL" 'BEGIN{printf "%.1f", m/(i*10)}')

    echo "$ELAPSED,$RSS,$HWM,$THR,$TEMP,${CLK:-0},${THROT:-NA},$DIRTY,$WB,$AVAIL,$USED,$UTIL,$WMBPS" >> "$OUT"
    printf "t=%-6s rss=%-9s hwm=%-9s temp=%-5s dirty=%-9s wMB/s=%-7s util=%s%%\n" \
        "$ELAPSED" "$RSS" "$HWM" "$TEMP" "$DIRTY" "$WMBPS" "$UTIL"
done

echo
echo "Process exited. Samples written to $OUT"
echo "Peak resident set: $(awk -F, 'NR>1 && $2>m{m=$2} END{print m}' "$OUT") kB"
echo "Peak SoC temperature: $(awk -F, 'NR>1 && $5>m{m=$5} END{print m}' "$OUT") C"
echo "Peak dirty page cache: $(awk -F, 'NR>1 && $8>m{m=$8} END{print m}' "$OUT") kB"