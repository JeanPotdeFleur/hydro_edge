#!/usr/bin/env bash
#
# push_status.sh - publish the morning image and the station's state.
#
# The station sits behind the institutional NAT with no inbound access, so no
# page can be served from it. A public repository is the transport in both
# directions: this pushes, fetch_plan.sh reads back.
#
# The image overwrites a single file. Git keeps every version in its history
# regardless, which is why the preview is generated at 900 px: about 150 kB a
# day, or 27 MB over a six-month pilot.
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
SITE="${HYDRO_SITE:-/home/bakerlab/hopkins-station}"
IMG="${HYDRO_MORNING_IMG:-/dev/shm/hydro_morning.jpg}"
META="${HYDRO_MORNING_META:-/dev/shm/hydro_morning_meta.json}"
ACTIVE="${HYDRO_PLAN_ACTIVE:-/dev/shm/hydro_plan_active.json}"
VAULTS="${HYDRO_OUTPUT:-/mnt/vault} ${HYDRO_OUTPUT_ALT:-/mnt/vault2}"

log() { echo "[PUSH] $*"; }

if [ ! -d "${SITE}/.git" ]; then
    log "FATAL no clone at ${SITE}. Clone the transport repository first."
    exit 1
fi

STATUS_TMP="/dev/shm/hydro_status.json"

# The page reported the station's own exposure without naming it. It is the
# figure to compare against what the morning scene called for, so it travels.
python3 - "$STATUS_TMP" "$META" "$ACTIVE" "$ROOT" "${HYDRO_EXPOSURE_US:-}" \
         "${HYDRO_GAIN_DB:-}" "${HYDRO_TRIGGER:-}" $VAULTS <<'PYEOF'
import json, os, re, subprocess, sys, datetime

dst, meta_path, active_path, root = sys.argv[1:5]
env_exposure, env_gain, env_trigger = sys.argv[5:8]
vaults = sys.argv[8:]

def run(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True,
                              text=True, timeout=30).stdout.strip()
    except Exception:
        return ""

def load(path):
    try:
        return json.load(open(path))
    except Exception:
        return {}

now = datetime.datetime.now(datetime.timezone.utc)
morning, plan = load(meta_path), load(active_path)

# ---- station -------------------------------------------------------------
temp = run("cat /sys/class/thermal/thermal_zone0/temp")
station = {
    "clock_synchronised": "yes" in run("timedatectl show -p NTPSynchronized --value").lower()
                          or run("timedatectl show -p NTPSynchronized --value") == "yes",
    "governor": run("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") or "unknown",
    "soc_temp_c": round(int(temp) / 1000, 1) if temp.isdigit() else None,
    "throttled": run("vcgencmd get_throttled").replace("throttled=", "") or "unknown",
    "uptime": run("uptime -p"),
}
c0 = run("cat /sys/class/pps/pps0/assert")
import time
time.sleep(1.2)
c1 = run("cat /sys/class/pps/pps0/assert")
station["pps_pulsing"] = bool(c0 and c1 and c0 != c1)

# ---- storage -------------------------------------------------------------
storage = []
for v in vaults:
    out = run(f"df --output=avail,pcent -B1 {v} | tail -1")
    parts = out.replace("%", "").split()
    if len(parts) == 2 and parts[0].isdigit():
        storage.append({"mount": v,
                        "free_gb": round(int(parts[0]) / 1e9, 1),
                        "used_pct": int(parts[1])})

# ---- recent bursts -------------------------------------------------------
bursts = []
for v in vaults:
    if not os.path.isdir(v):
        continue
    for name in sorted(os.listdir(v), reverse=True):
        d = os.path.join(v, name)
        if not (re.match(r"^20\d\d-", name) and os.path.isdir(d)):
            continue
        s = load(os.path.join(d, "summary.json"))
        if not s:
            continue
        c = s.get("counters", {})
        # run_burst.sh leaves the plan it applied beside the manifest, so the
        # note travels with the frames it explains rather than being kept
        # anywhere else. A burst run by hand has no such file.
        pa = load(os.path.join(d, "plan_applied.json"))
        bursts.append({
            "id": name, "volume": v,
            "note": str((pa.get("plan") or {}).get("note", ""))[:200],
            "slot": pa.get("slot"),
            "completed": bool(s.get("completed")),
            "frames": c.get("frames_written"),
            "target": c.get("target_triggers"),
            "lost": sum(int(c.get(k, 0) or 0) for k in
                        ("incomplete", "retrieval_errors", "write_errors",
                         "buffer_overflows", "transport_frame_id_gaps",
                         "late_frames_skipped")),
            "mbps": s.get("mean_write_MBps"),
            "ended_utc": s.get("ended_utc"),
        })
        if len(bursts) >= 24:
            break
bursts.sort(key=lambda b: b.get("ended_utc") or "", reverse=True)
bursts = bursts[:6]

# ---- acceptance and journal ---------------------------------------------
# Both volumes, not the first alone. run_burst.sh writes each burst to whichever
# volume is the emptier, so the two alternate and half the archive sits on the
# second one; checking vaults[0] only would leave that half unverified until the
# drives are retrieved, up to six weeks later. verify_burst.py takes several
# roots, and an unmounted volume is dropped rather than passed as a bad path.
verify_roots = " ".join(v for v in vaults if os.path.isdir(v))
verify_raw = run(f"{root}/scripts/verify_burst.py --quiet {verify_roots} "
                 f"2>&1 | tail -12") if verify_roots else "no archive volume mounted"
anomalies = run("journalctl --since '24 hours ago' --no-pager "
                "| grep -cE 'CRITICAL|FATAL|WARNING' || true")
timers = []
for line in run("systemctl list-timers 'hydro-*' --no-pager | head -8").splitlines()[1:]:
    if "hydro" in line:
        timers.append(line.strip())

# ---- health --------------------------------------------------------------
faults, notes = [], []
if not station["clock_synchronised"]:
    faults.append("system clock is not disciplined, so bursts will be refused")
if station["soc_temp_c"] and station["soc_temp_c"] > 80:
    faults.append(f"die at {station['soc_temp_c']} C, near the 85 C throttle")
if station["throttled"] not in ("0x0", "unknown"):
    faults.append(f"throttling word {station['throttled']}")
if storage and all(s["free_gb"] < 400 for s in storage):
    faults.append("both volumes below 400 GB free")
if bursts and not bursts[0]["completed"]:
    faults.append(f"last burst {bursts[0]['id']} did not complete")
if bursts and bursts[0]["lost"]:
    faults.append(f"last burst shed {bursts[0]['lost']} frames")

expected = len(plan.get("slots", []) or [])
if expected and bursts:
    try:
        last = datetime.datetime.strptime(bursts[0]["ended_utc"], "%Y-%m-%dT%H:%M:%SZ") \
                       .replace(tzinfo=datetime.timezone.utc)
        if (now - last).total_seconds() > 36 * 3600:
            faults.append("no burst in the last 36 hours")
    except Exception:
        pass
elif expected and not bursts:
    faults.append("no burst on record")

if station["governor"] != "performance":
    notes.append(f"CPU governor is {station['governor']}, not performance")
if not station["pps_pulsing"]:
    notes.append("the GNSS timepulse is not advancing")
if anomalies.isdigit() and int(anomalies) > 0:
    notes.append(f"{anomalies} journal anomalies in 24 hours")
if storage and any(0 < s["free_gb"] < 1000 for s in storage):
    notes.append("a volume is below 1 TB free")
if not morning:
    notes.append("no morning capture this cycle")

verdict = "fault" if faults else ("attention" if notes else "nominal")

status = {
    "generated_utc": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
    "health": {"verdict": verdict, "faults": faults, "notes": notes},
    "morning": morning,
    "plan": plan,
    "settings": {
        "exposure_us": int(env_exposure) if env_exposure.isdigit() else None,
        "gain_db": env_gain or None,
        "trigger": env_trigger or None,
    },
    "station": station,
    "storage": storage,
    "bursts": bursts,
    "verify": verify_raw.splitlines()[-4:],
    "journal_anomalies_24h": int(anomalies) if anomalies.isdigit() else None,
    "timers": timers,
}
with open(dst, "w") as f:
    json.dump(status, f, indent=2)
print(f"[PUSH] status: {verdict}"
      + (f" - {faults[0]}" if faults else (f" - {notes[0]}" if notes else "")))
PYEOF

if [ ! -s "$STATUS_TMP" ]; then
    log "FATAL status.json was not produced."
    exit 1
fi

# The researchers edit control/plan.json in this same repository between 08:00
# and 09:45, so a push can lose the race against them and be rejected. Without
# a retry the day would carry no report at all, and the page would read that as
# a dead station: a false alarm is worse than a late report.
publish() {
    git -C "$SITE" fetch --quiet origin main || return 1
    git -C "$SITE" reset --hard --quiet origin/main || return 1
    cp -f "$STATUS_TMP" "${SITE}/status.json" || return 1
    if [ -s "$IMG" ]; then
        cp -f "$IMG" "${SITE}/latest.jpg" || return 1
    else
        log "no morning image to publish; status only."
    fi
    git -C "$SITE" add -A
    if git -C "$SITE" diff --cached --quiet; then
        log "nothing changed since the last push."
        return 0
    fi
    git -C "$SITE" -c user.name="hopkins1-pi5" \
        -c user.email="hopkins1-pi5@local" \
        commit -q -m "status $(date -u +%Y-%m-%dT%H:%MZ)" || return 1
    git -C "$SITE" push -q origin main || return 1
    return 0
}

for attempt in 1 2 3; do
    if publish; then
        log "published on attempt ${attempt}."
        exit 0
    fi
    log "attempt ${attempt} did not go through."
    [ "$attempt" -lt 3 ] && sleep 7
done
log "could not publish after three attempts; the state is on disk and the next"
log "cycle will carry it."
exit 1
