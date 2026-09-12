#!/usr/bin/env bash
#
# install_night_tests.sh -- two overnight measurements, neither of which
# touches the validated acquisition path.
#
#   GATE A2 stability. GATE A2 established a +297.2 us inter-camera exposure
#   offset under software triggering and its removal under hardware, and
#   recorded as a residual limit that the four bursts span 219 s: nothing said
#   the offset holds over hours, over temperature, or across a power cycle.
#   This alternates software and line2 bursts every ten minutes over a night,
#   which is the same experiment on a baseline two hundred times longer.
#
#   A6, clock against PPS. The kernel stamps each PPS assert on CLOCK_REALTIME.
#   The true edge falls on an exact second, so the fractional part of that
#   timestamp is the offset between the system clock and the reference, with no
#   extra instrumentation at all.
#
# The soak stops at 06:50 local, before the 08:00 telemetry cycle, so the
# cameras are free when the morning capture runs.
#
set -o nounset

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
echo "[NIGHT] repository root: $ROOT"

mkdir -p scripts deploy/systemd

cat > 'deploy/systemd/hydro-a2soak@.service' <<'HYDRO_EOF'
[Unit]
Description=GATE A2 stability soak, 120 s burst under %i triggering
Documentation=https://github.com/JeanPotdeFleur/hydro_edge

RequiresMountsFor=/mnt/vault
Wants=time-sync.target
After=time-sync.target network-online.target

[Service]
Type=exec
User=bakerlab
Group=bakerlab

EnvironmentFile=/etc/default/hydro-edge

# The instance name is the trigger mode, not the duration: the point of the
# run is to alternate modes at a fixed duration, which the duration-templated
# environment file of hydro-burst@.service cannot express.
ExecStartPre=/bin/mkdir -p /mnt/vault/a2soak
ExecStart=/home/bakerlab/hydro_edge/build/hydro_edge \
    --output /mnt/vault/a2soak \
    --duration 120 \
    --cam0-serial ${HYDRO_CAM0} \
    --cam1-serial ${HYDRO_CAM1} \
    --exposure-us ${HYDRO_EXPOSURE_US} \
    --gain-db ${HYDRO_GAIN_DB} \
    --trigger %i \
    --require-clock-sync

Restart=no
RuntimeMaxSec=240
KillSignal=SIGTERM
TimeoutStopSec=60

SyslogIdentifier=hydro-a2soak
StandardOutput=journal
StandardError=journal
HYDRO_EOF

cat > deploy/systemd/hydro-a2soak-sw.timer <<'HYDRO_EOF'
[Unit]
Description=GATE A2 stability soak, software leg

[Timer]
# Minutes 0, 20 and 40 of every hour from 17:00 to 06:50 local. The window
# closes before 08:00 so the morning capture finds the sensors free.
OnCalendar=*-*-* 17..23,00..06:00/20 America/Los_Angeles
Persistent=false
AccuracySec=1s
Unit=hydro-a2soak@software.service

[Install]
WantedBy=timers.target
HYDRO_EOF

cat > deploy/systemd/hydro-a2soak-hw.timer <<'HYDRO_EOF'
[Unit]
Description=GATE A2 stability soak, hardware leg

[Timer]
# Minutes 10, 30 and 50: interleaved with the software leg, so the two modes
# alternate every ten minutes and the common drift is shared between them.
OnCalendar=*-*-* 17..23,00..06:10/20 America/Los_Angeles
Persistent=false
AccuracySec=1s
Unit=hydro-a2soak@line2.service

[Install]
WantedBy=timers.target
HYDRO_EOF

cat > scripts/a6_pps_offset.sh <<'HYDRO_EOF'
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
HYDRO_EOF

chmod +x scripts/a6_pps_offset.sh

echo
echo "[NIGHT] created:"
printf '  %s\n' 'deploy/systemd/hydro-a2soak@.service' \
    deploy/systemd/hydro-a2soak-sw.timer \
    deploy/systemd/hydro-a2soak-hw.timer \
    scripts/a6_pps_offset.sh
echo
echo "[NIGHT] Then, in order:"
echo "  1. sudo cp deploy/systemd/hydro-a2soak* /etc/systemd/system/ && sudo systemctl daemon-reload"
echo "  2. sudo systemctl start hydro-a2soak@line2.service   # one burst by hand first"
echo "  3. sudo systemctl enable --now hydro-a2soak-sw.timer hydro-a2soak-hw.timer"
echo "  4. tmux new -s a6   then   taskset -c 0 ./scripts/a6_pps_offset.sh"
echo "  5. In the morning: disable both timers, stop the sampler, send me the CSVs."