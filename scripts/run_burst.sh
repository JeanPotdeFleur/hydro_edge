#!/usr/bin/env bash
#
# run_burst.sh <slot> <duration_s> - decide, then acquire.
#
# The three calendar timers stay armed and point here. Enabling and disabling
# timers from a script would require privileges a script must not have, and the
# slots themselves are fixed: only their number varies from day to day. So the
# wrapper either runs the burst or exits zero, having said in the journal why
# it did not.
#
# It also picks the emptier of the two volumes, applies the day's exposure, and
# leaves a record of the plan it applied beside the manifest, so that an archive
# read a year later states the schedule it was acquired under.
#
set -o nounset

SLOT="${1:?slot label, one of 1000 1300 1700}"
DURATION="${2:?burst duration in seconds}"

ROOT="${HYDRO_ROOT:-/home/bakerlab/hydro_edge}"
ACTIVE="${HYDRO_PLAN_ACTIVE:-/dev/shm/hydro_plan_active.json}"
BIN="${ROOT}/build/hydro_edge"

log() { echo "[SLOT ${SLOT}] $*"; }

# ---- is this slot scheduled today ------------------------------------------
decision="$(python3 - "$ACTIVE" "$SLOT" <<'PYEOF'
import json, sys
path, slot = sys.argv[1], sys.argv[2]
try:
    p = json.load(open(path))
    slots = [str(s) for s in p.get("slots", [])]
    src, exp = p.get("source", "?"), p.get("exposure_us")
except Exception:
    # No snapshot means fetch_plan never ran or the station rebooted since.
    # The standard schedule is the safe reading, not silence.
    print("RUN default none" if slot in ("1000", "1700") else "SKIP default")
    sys.exit(0)
if slot in slots:
    print(f"RUN {src} {exp if exp else 'none'}")
else:
    print(f"SKIP {src}")
PYEOF
)"

verdict="${decision%% *}"
rest="${decision#* }"
source_name="${rest%% *}"

if [ "$verdict" = "SKIP" ]; then
    log "not scheduled today (source: ${source_name}). Exiting without acquiring."
    exit 0
fi

plan_exposure="${rest##* }"
exposure="${HYDRO_EXPOSURE_US:?HYDRO_EXPOSURE_US is not set}"
if [ "$plan_exposure" != "none" ]; then
    exposure="$plan_exposure"
    log "exposure overridden by today's plan: ${exposure} us."
fi

# ---- pick the emptier volume -----------------------------------------------
avail() { df --output=avail -B1 "$1" 2>/dev/null | tail -n 1 | tr -d ' '; }
output="${HYDRO_OUTPUT:?HYDRO_OUTPUT is not set}"
alt="${HYDRO_OUTPUT_ALT:-/mnt/vault2}"
if mountpoint -q "$alt"; then
    a="$(avail "$output")"; b="$(avail "$alt")"
    if [ -n "$a" ] && [ -n "$b" ] && [ "$b" -gt "$a" ]; then
        log "switching to ${alt}: $(( b / 1024**3 )) GiB free against $(( a / 1024**3 ))."
        output="$alt"
    fi
fi

# ---- acquire ---------------------------------------------------------------
# The clock guard is conditional because the station may have to run with no
# network, hence with no disciplined clock. Refusing every burst would then be
# worse than naming a directory from a clock set by hand: free-running, that
# clock drifts at 1.67 ppm, some six seconds over six weeks, while the cadence
# inside a burst is anchored on the PPS edge and is unaffected. The manifest
# records clock_synchronized either way, so the provenance stays honest. The
# default remains to require synchronisation.
clock_flag="--require-clock-sync"
if [ "${HYDRO_REQUIRE_CLOCK_SYNC:-1}" = "0" ]; then
    clock_flag=""
    log "clock sync requirement waived; timestamps come from a free-running clock."
fi
log "${source_name} schedule, ${DURATION} s, exposure ${exposure} us, output ${output}."
started="$(date +%s)"

"$BIN" --output "$output" --duration "$DURATION" \
    --cam0-serial "${HYDRO_CAM0}" --cam1-serial "${HYDRO_CAM1}" \
    --exposure-us "$exposure" --gain-db "${HYDRO_GAIN_DB}" \
    --trigger "${HYDRO_TRIGGER}" ${clock_flag} &
child=$!

# systemd kills the whole control group, so the binary receives SIGTERM
# directly and runs its own drain. The wrapper only has to stay alive long
# enough to write the record afterwards.
trap 'log "stop requested; waiting for the burst to drain."' TERM INT
wait "$child"; status=$?
trap - TERM INT

# ---- record the plan beside the manifest -----------------------------------
burst_dir="$(find "$output" -maxdepth 1 -type d -name '20*Z' -newermt "@${started}" \
             -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)"
if [ -n "$burst_dir" ] && [ -d "$burst_dir" ]; then
    python3 - "$ACTIVE" "${burst_dir}/plan_applied.json" "$SLOT" "$exposure" "$output" <<'PYEOF'
import json, sys, datetime
active, dst, slot, exposure, output = sys.argv[1:6]
try:
    plan = json.load(open(active))
except Exception:
    plan = {"source": "default", "reason": "no snapshot at launch"}
json.dump({"slot": slot, "exposure_us_applied": int(exposure),
           "output_volume": output, "plan": plan,
           "written_utc": datetime.datetime.now(datetime.timezone.utc)
                                  .strftime("%Y-%m-%dT%H:%M:%SZ")},
          open(dst, "w"), indent=2)
PYEOF
    log "plan recorded at ${burst_dir}/plan_applied.json"
else
    log "no burst directory appeared; nothing to annotate."
fi

log "binary exited ${status}."
exit "$status"
