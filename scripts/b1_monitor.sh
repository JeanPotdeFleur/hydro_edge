#!/bin/bash
# b1_monitor.sh - station telemetry during and after a sealed-enclosure burst.
#
#   ./b1_monitor.sh <out.csv> [duration_s]
#
# Separate process, nothing of the acquisition path is touched. Default
# duration is 7200 s: 5400 of burst and 1800 of cooling. The cooling half is
# not optional. Ninety minutes will probably not reach thermal equilibrium in a
# sealed box, so the rise alone gives a lower bound rather than an endurance;
# the decay after shutdown yields the same time constant by an independent
# route and is what makes the extrapolation defensible.
#
# Sysfs is read at 1 Hz, SMART at 30 s. Not for cost: a SMART read briefly
# interrupts the drive command queue, and that is the suspected mechanism
# behind the 0.409 ms cadence excursion of GATE A1. Polling it every second
# would plant the very artefact this burst is meant to look for.
#
# Ambient temperature has no sensor: the DS18B20 was never delivered. Write a
# number into ~/b1_ambient at any time and it is picked up on the next sample,
# so a reading taken by hand lands in the series with its own timestamp.

set -u

OUT=${1:?usage: b1_monitor.sh <out.csv> [duration_s]}
DUR=${2:-7200}
AMB=${AMBIENT_FILE:-/tmp/b1_ambient}   # absolute: the script runs as root

# Fail here rather than after ninety minutes. smartctl needs root, and the
# SSD temperature is the principal criterion of this gate now that the
# contact probe was never delivered: empty columns would void the run.
if [ "$(id -u)" -ne 0 ]; then
    echo "[FATAL] run me as root, or the SSD temperatures stay empty:" >&2
    echo "        sudo $0 $*" >&2
    exit 1
fi
for n in /sys/class/thermal/thermal_zone0/temp /proc/diskstats; do
    [ -r "$n" ] || { echo "[FATAL] cannot read $n" >&2; exit 1; }
done

sect_written() { awk -v d="$1" '$3==d {print $10; exit}' /proc/diskstats; }

echo "utc,elapsed_s,soc_c,cpu0_mhz,cpu1_mhz,cpu2_mhz,cpu3_mhz,throttled,\
sda_c,sdb_c,ambient_c,sda_wr_mbs,sdb_wr_mbs,vault_free_gb,vault2_free_gb,\
rss_mb,pps_count,acq_running" > "$OUT"

t0=$(date +%s)
pa=$(sect_written sda); pb=$(sect_written sdb); pt=$t0
sda_c=""; sdb_c=""; vf=""; v2f=""
i=0

while [ $(( $(date +%s) - t0 )) -lt "$DUR" ]; do
    now=$(date +%s)
    el=$(( now - t0 ))

    soc=$(awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone0/temp)
    f=()
    for c in 0 1 2 3; do
        v=$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
        f+=( $(( v / 1000 )) )
    done
    thr=$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)

    ca=$(sect_written sda); cb=$(sect_written sdb)
    dt=$(( now - pt )); [ "$dt" -lt 1 ] && dt=1
    wa=$(awk -v a="$ca" -v b="$pa" -v d="$dt" 'BEGIN{printf "%.1f",(a-b)*512/1e6/d}')
    wb=$(awk -v a="$cb" -v b="$pb" -v d="$dt" 'BEGIN{printf "%.1f",(a-b)*512/1e6/d}')
    pa=$ca; pb=$cb; pt=$now

    # SMART every thirty samples, and free space every ten: both fork, and
    # neither changes fast enough to deserve a reading per second.
    if [ $(( i % 30 )) -eq 0 ]; then
        sda_c=$(smartctl -d sat -A /dev/sda 2>/dev/null | awk '/Airflow_Temperature/{print $10}')
        sdb_c=$(smartctl -d sat -A /dev/sdb 2>/dev/null | awk '/Airflow_Temperature/{print $10}')
    fi
    if [ $(( i % 10 )) -eq 0 ]; then
        vf=$(df --output=avail -BG /mnt/vault  2>/dev/null | tail -1 | tr -dc '0-9')
        v2f=$(df --output=avail -BG /mnt/vault2 2>/dev/null | tail -1 | tr -dc '0-9')
    fi

    amb=""; [ -r "$AMB" ] && amb=$(tr -dc '0-9.-' < "$AMB")

    pid=$(pgrep -f 'build/hydro_edge' | head -1)
    if [ -n "$pid" ] && [ -r "/proc/$pid/statm" ]; then
        rss=$(awk '{printf "%.0f", $2*4/1024}' "/proc/$pid/statm"); run=1
    else
        rss=""; run=0
    fi

    pps=$(cut -d'#' -f2 /sys/class/pps/pps0/assert 2>/dev/null)

    printf '%s,%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d\n' \
        "$(date -u +%FT%TZ)" "$el" "$soc" \
        "${f[0]}" "${f[1]}" "${f[2]}" "${f[3]}" "$thr" \
        "$sda_c" "$sdb_c" "$amb" "$wa" "$wb" "$vf" "$v2f" \
        "$rss" "$pps" "$run" >> "$OUT"

    i=$(( i + 1 ))
    sleep 1
done

echo "[B1] done, $(wc -l < "$OUT") rows in $OUT"