#!/bin/bash
# Morning check. Everything worth knowing about the station in one command,
# so that supervision does not depend on remembering which five to run.
echo "=== $(date -u +%FT%TZ) | up $(uptime -p) ==="
echo "clock:    $(timedatectl | grep -oP 'synchronized: \K\w+')  |  gov: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
a=$(cut -d'#' -f2 /sys/class/pps/pps0/assert); sleep 3
b=$(cut -d'#' -f2 /sys/class/pps/pps0/assert)
echo "pps:      $((b-a)) pulses in 3 s (expect 3)"
echo "soc:      $(($(cat /sys/class/thermal/thermal_zone0/temp)/1000)) C"
echo "vault:    $(df -h /mnt/vault | tail -1 | awk '{print $4" free, "$5" used"}')"
echo "throttle: $(vcgencmd get_throttled)"
echo "timers:"; systemctl list-timers 'hydro-*' --no-pager 2>/dev/null | head -5
echo "last bursts:"; ls -1t /mnt/vault 2>/dev/null | grep -E '^20' | head -4
echo "verdict:"; ./scripts/verify_burst.py --quiet /mnt/vault 2>&1 | tail -6
echo "anomalies (24 h): $(journalctl -u 'hydro-burst@*' --since '24 hours ago' 2>/dev/null | grep -cE 'CRITICAL|FATAL|WARN')"
