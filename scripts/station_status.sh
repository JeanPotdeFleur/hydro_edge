#!/bin/bash
# Morning check. Everything worth knowing about the station in one command,
# so that supervision does not depend on remembering which five to run.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -r /etc/default/hydro-edge ] && . /etc/default/hydro-edge
# Both archive volumes. run_burst.sh writes each burst to whichever of the two
# is the emptier, so they alternate: a single-volume view reports half the free
# space and shows every other burst, which reads as a station that has stopped.
VAULTS=""
for v in "${HYDRO_OUTPUT:-/mnt/vault}" "${HYDRO_OUTPUT_ALT:-/mnt/vault2}"; do
    [ -d "$v" ] && VAULTS="$VAULTS $v"
done
echo "=== $(date -u +%FT%TZ) | up $(uptime -p) ==="
echo "clock:    $(timedatectl | grep -oP 'synchronized: \K\w+')  |  gov: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
a=$(cut -d'#' -f2 /sys/class/pps/pps0/assert); sleep 3
b=$(cut -d'#' -f2 /sys/class/pps/pps0/assert)
echo "pps:      $((b-a)) pulses in 3 s (expect 3)"
echo "soc:      $(($(cat /sys/class/thermal/thermal_zone0/temp)/1000)) C"
for v in ${VAULTS:-none}; do
    [ "$v" = none ] && { echo "vault:    no archive volume mounted"; break; }
    printf 'vault:    %-12s %s\n' "$v" "$(df -h "$v" | tail -1 | awk '{print $4" free, "$5" used"}')"
done
echo "throttle: $(vcgencmd get_throttled)"
echo "timers:"; systemctl list-timers 'hydro-*' --no-pager 2>/dev/null | head -5
echo "last bursts:"
for v in $VAULTS; do ls -1 "$v" 2>/dev/null | grep -E '^20' | sed "s|\$|   $v|"; done | sort -r | head -4
echo "verdict:"; [ -n "$VAULTS" ] && "$HERE/verify_burst.py" --quiet $VAULTS 2>&1 | tail -12
echo "anomalies (24 h): $(journalctl -u 'hydro-burst@*' --since '24 hours ago' 2>/dev/null | grep -cE 'CRITICAL|FATAL|WARN')"