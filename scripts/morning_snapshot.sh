#!/usr/bin/env bash
#
# morning_snapshot.sh - the dedicated morning capture.
#
# It cannot be extracted from the last burst, which would date from 17:40 the
# previous day. The image is written to tmpfs and never to the vault: hours of
# JPEG writes at 2 Hz would be NAND wear bought for nothing.
#
# The capture runs under automatic exposure, so the value the loop converges on
# is itself the exposure the scene calls for. That figure is parsed out and
# carried into status.json, where it becomes the recommendation an operator sees
# next to the image.
#
set -o nounset

# The acquisition settings live in the environment file the service units read
# as an EnvironmentFile. Sourced here as well, so that a run by hand reports
# and applies exactly what a scheduled run does. Under systemd the variables
# are already set and this does nothing.
if [ -z "${HYDRO_EXPOSURE_US:-}" ] && [ -r /etc/default/hydro-edge ]; then
    set -a
    . /etc/default/hydro-edge
    set +a
fi

ROOT="${HYDRO_ROOT:-/home/bakerlab/hydro_edge}"
IMG="${HYDRO_MORNING_IMG:-/dev/shm/hydro_morning.jpg}"
LOG="/dev/shm/hydro_morning.log"
META="${HYDRO_MORNING_META:-/dev/shm/hydro_morning_meta.json}"
RUN_S="${HYDRO_SNAPSHOT_S:-30}"
WIDTH="${HYDRO_SNAPSHOT_WIDTH:-900}"

log() { echo "[MORNING] $*"; }

# Never contend for the sensors. A burst holds both of them exclusively, and
# losing acquisition to a status image would be an absurd trade.
if pgrep -f "${ROOT}/build/hydro_edge" >/dev/null 2>&1; then
    log "a burst is running; skipping the snapshot."
    exit 0
fi

if [ ! -x "${ROOT}/build/cam_focus" ]; then
    log "FATAL cam_focus is not built at ${ROOT}/build/cam_focus"
    exit 1
fi

rm -f "$IMG" "$META"
log "capturing for ${RUN_S} s under automatic exposure, preview width ${WIDTH}."

# cam_focus requires --serial, and takes it once per camera. Binding by serial
# rather than by enumeration order is the same discipline the acquisition
# binary follows: the order the SDK reports is not contractual.
timeout --signal=INT "$RUN_S" \
    "${ROOT}/build/cam_focus" \
    --serial "${HYDRO_CAM0}" --serial "${HYDRO_CAM1}" \
    --exposure-auto --out "$IMG" \
    --preview-width "$WIDTH" --rate 2 > "$LOG" 2>&1
rc=$?

# 124 is the timeout expiring, which is the nominal path; 130 is the SIGINT it
# sends being handled. Anything else is worth a line in the journal.
if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && [ "$rc" -ne 130 ]; then
    log "cam_focus exited ${rc}. Last lines follow."
    tail -n 5 "$LOG" | sed 's/^/[MORNING]   /'
fi

if [ ! -s "$IMG" ]; then
    log "no image produced; leaving yesterday's in place."
    exit 1
fi

python3 - "$LOG" "$META" <<'PYEOF'
import json, re, sys, os, datetime

log_path, meta_path = sys.argv[1], sys.argv[2]
text = open(log_path, errors="replace").read()

# "[FOCUS] sn 24260192: 60 frames, 0 missed. Final exposure 1840.0 us, gain 0 dB."
cams = []
for m in re.finditer(
        r"sn\s+(\d+):\s+(\d+)\s+frames,\s+(\d+)\s+missed\."
        r"\s+Final exposure\s+([\d.]+)\s+us,\s+gain\s+(-?[\d.]+)\s+dB", text):
    cams.append({"serial": m.group(1), "frames": int(m.group(2)),
                 "missed": int(m.group(3)),
                 "exposure_us": round(float(m.group(4)), 1),
                 "gain_db": round(float(m.group(5)), 1)})

# Last per-frame line of camera 0 carries the radiometry.
rad = {}
per = re.findall(
    r"\[0 sn\d+\].*?sat\s+([\d.]+)%.*?p50\s+(\d+)\s+p99\.9\s+(\d+)\s+max\s+(\d+)",
    text)
if per:
    sat, p50, p999, pmax = per[-1]
    rad = {"sat_pct": float(sat), "p50": int(p50),
           "p999": int(p999), "pmax": int(pmax)}

out = {"captured_utc": datetime.datetime.now(datetime.timezone.utc)
                               .strftime("%Y-%m-%dT%H:%M:%SZ"),
       "cameras": cams, "radiometry": rad}
if cams:
    out["exposure_us"] = cams[0]["exposure_us"]
    out["gain_db"] = cams[0]["gain_db"]

with open(meta_path, "w") as f:
    json.dump(out, f, indent=2)
print("[MORNING] metered " +
      (f"{out.get('exposure_us', '?')} us, gain {out.get('gain_db', '?')} dB"
       if cams else "nothing: the log carried no final exposure line") +
      (f", saturation {rad['sat_pct']} per cent, median {rad['p50']} DN"
       if rad else ""))
PYEOF

log "image at ${IMG}, metadata at ${META}."
