#!/usr/bin/env bash
#
# install_station_telemetry.sh
#
# Creates the daily-plan and telemetry layer: four scripts, six unit files and
# one static page. Run once from the repository root, then commit.
#
# Nothing in the validated acquisition path is touched. hydro_edge and
# hydro-burst@.service are left exactly as GATE S validated them and as the
# end-to-end check of 11 September exercised them; the three calendar timers
# are retargeted at a wrapper, and the wrapper is the only new path.
#
set -o nounset

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
echo "[INSTALL] repository root: $ROOT"

mkdir -p scripts deploy/systemd site

# ---------------------------------------------------------------- scripts
cat > scripts/morning_snapshot.sh <<'HYDRO_EOF'
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

timeout --signal=INT "$RUN_S" \
    "${ROOT}/build/cam_focus" --exposure-auto --out "$IMG" \
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
HYDRO_EOF

cat > scripts/fetch_plan.sh <<'HYDRO_EOF'
#!/usr/bin/env bash
#
# fetch_plan.sh - read the day's plan and freeze it locally.
#
# Run at 09:50, ten minutes before the first slot. The wrapper that launches a
# burst reads the local snapshot this writes and never the network, which is
# what implements the ten-minute rule and also means a burst no longer depends
# on the Wi-Fi being up at the instant it fires.
#
# Every failure resolves to the standard schedule. The plan is opt-in by date:
# it applies only if it carries today's local date, so an untouched plan ages
# out by itself rather than by any arithmetic on timestamps.
#
set -o nounset

SITE="${HYDRO_SITE:-/home/bakerlab/hopkins-station}"
ACTIVE="${HYDRO_PLAN_ACTIVE:-/dev/shm/hydro_plan_active.json}"
TODAY="$(TZ=America/Los_Angeles date +%F)"

log() { echo "[PLAN] $*"; }

if [ -d "${SITE}/.git" ]; then
    if git -C "$SITE" fetch --quiet origin main 2>/dev/null &&
       git -C "$SITE" reset --hard --quiet origin/main 2>/dev/null; then
        log "transport repository refreshed."
    else
        log "could not reach the transport repository; using whatever is on disk."
    fi
else
    log "no clone at ${SITE}; falling back to the standard schedule."
fi

python3 - "${SITE}/control/plan.json" "$ACTIVE" "$TODAY" <<'PYEOF'
import json, sys, os, datetime

src, dst, today = sys.argv[1], sys.argv[2], sys.argv[3]

SLOTS = {0: [], 1: ["1000"], 2: ["1000", "1700"], 3: ["1000", "1300", "1700"]}
DEFAULT = {"source": "default", "date": today, "bursts": 2,
           "slots": SLOTS[2], "exposure_us": None,
           "reason": "standard schedule"}

def emit(plan):
    plan["resolved_utc"] = datetime.datetime.now(datetime.timezone.utc) \
                                   .strftime("%Y-%m-%dT%H:%M:%SZ")
    tmp = dst + ".tmp"
    with open(tmp, "w") as f:
        json.dump(plan, f, indent=2)
    os.replace(tmp, dst)
    print(f"[PLAN] {plan['source']}: {plan['bursts']} burst(s) at "
          f"{', '.join(plan['slots']) or 'no slot'}, exposure "
          f"{plan['exposure_us'] if plan['exposure_us'] else 'from environment'}"
          f" ({plan['reason']})")
    sys.exit(0)

try:
    raw = json.load(open(src))
except FileNotFoundError:
    DEFAULT["reason"] = "no plan file"; emit(DEFAULT)
except Exception as exc:
    DEFAULT["reason"] = f"plan unreadable: {exc}"; emit(DEFAULT)

if str(raw.get("date")) != today:
    DEFAULT["reason"] = f"plan dated {raw.get('date')}, not {today}"
    emit(DEFAULT)

try:
    bursts = int(raw["bursts"])
except Exception:
    DEFAULT["reason"] = "bursts missing or not a number"; emit(DEFAULT)

if bursts not in SLOTS:
    DEFAULT["reason"] = f"bursts={bursts} outside 0 to 3"; emit(DEFAULT)

exposure = raw.get("exposure_us", None)
if exposure is not None:
    try:
        exposure = int(exposure)
    except Exception:
        DEFAULT["reason"] = "exposure_us not a number"; emit(DEFAULT)
    if not 100 <= exposure <= 20000:
        DEFAULT["reason"] = f"exposure_us={exposure} outside 100 to 20000"
        emit(DEFAULT)

emit({"source": "plan", "date": today, "bursts": bursts,
      "slots": SLOTS[bursts], "exposure_us": exposure,
      "reason": "accepted", "note": str(raw.get("note", ""))[:200]})
PYEOF
HYDRO_EOF

cat > scripts/run_burst.sh <<'HYDRO_EOF'
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
log "${source_name} schedule, ${DURATION} s, exposure ${exposure} us, output ${output}."
started="$(date +%s)"

"$BIN" --output "$output" --duration "$DURATION" \
    --cam0-serial "${HYDRO_CAM0}" --cam1-serial "${HYDRO_CAM1}" \
    --exposure-us "$exposure" --gain-db "${HYDRO_GAIN_DB}" \
    --trigger "${HYDRO_TRIGGER}" --require-clock-sync &
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
HYDRO_EOF

cat > scripts/push_status.sh <<'HYDRO_EOF'
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

python3 - "$STATUS_TMP" "$META" "$ACTIVE" "$ROOT" $VAULTS <<'PYEOF'
import json, os, re, subprocess, sys, datetime

dst, meta_path, active_path, root = sys.argv[1:5]
vaults = sys.argv[5:]

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
        bursts.append({
            "id": name, "volume": v,
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
verify_raw = run(f"{root}/scripts/verify_burst.py --quiet {vaults[0]} 2>&1 | tail -8")
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
HYDRO_EOF

chmod +x scripts/morning_snapshot.sh scripts/fetch_plan.sh \
          scripts/run_burst.sh scripts/push_status.sh

# ---------------------------------------------------------------- units
cat > deploy/systemd/hydro-morning.service <<'HYDRO_EOF'
[Unit]
Description=Hydro station morning capture and publication
Documentation=https://github.com/JeanPotdeFleur/hydro_edge
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
User=bakerlab
Group=bakerlab
EnvironmentFile=/etc/default/hydro-edge

# Sequential and in this order: the push has nothing to publish until the
# capture has run. Two timers would have let the second fire first.
ExecStart=/home/bakerlab/hydro_edge/scripts/morning_snapshot.sh
ExecStart=/home/bakerlab/hydro_edge/scripts/push_status.sh

# A failed capture must not suppress the status push, which is precisely what
# an operator needs to see when the cameras are the thing that failed.
SuccessExitStatus=0 1

SyslogIdentifier=hydro-morning
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
HYDRO_EOF

cat > deploy/systemd/hydro-morning.timer <<'HYDRO_EOF'
[Unit]
Description=Hydro station morning capture at 08:00 Pacific

[Timer]
# The system clock is UTC, so the calendar is qualified explicitly.
OnCalendar=*-*-* 08:00:00 America/Los_Angeles
# A missed morning is missed: an image taken at an arbitrary hour after a boot
# would misinform the operator about the light.
Persistent=false
AccuracySec=30s
Unit=hydro-morning.service

[Install]
WantedBy=timers.target
HYDRO_EOF

cat > deploy/systemd/hydro-plan.service <<'HYDRO_EOF'
[Unit]
Description=Hydro station daily plan resolution
Documentation=https://github.com/JeanPotdeFleur/hydro_edge
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
User=bakerlab
Group=bakerlab
EnvironmentFile=/etc/default/hydro-edge
ExecStart=/home/bakerlab/hydro_edge/scripts/fetch_plan.sh

SyslogIdentifier=hydro-plan
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
HYDRO_EOF

cat > deploy/systemd/hydro-plan.timer <<'HYDRO_EOF'
[Unit]
Description=Hydro station plan resolution at 09:50 Pacific

[Timer]
# Ten minutes before the first slot, which is the rule: an edit arriving after
# this has no effect on today, and a burst never reads the network as it fires.
OnCalendar=*-*-* 09:50:00 America/Los_Angeles
Persistent=false
AccuracySec=10s
Unit=hydro-plan.service

[Install]
WantedBy=timers.target
HYDRO_EOF

cat > 'deploy/systemd/hydro-slot@.service' <<'HYDRO_EOF'
[Unit]
Description=Hydro edge burst slot %i, subject to the day's plan
Documentation=https://github.com/JeanPotdeFleur/hydro_edge

# Same prerequisite as hydro-burst@.service: with /mnt/vault unmounted,
# statvfs reports the free space of the boot microSD instead.
RequiresMountsFor=/mnt/vault

# Ordering only. The clock gate is inside the binary and frees the slot fast.
Wants=time-sync.target
After=time-sync.target network-online.target

[Service]
Type=exec
User=bakerlab
Group=bakerlab

EnvironmentFile=/etc/default/hydro-edge

# The wrapper decides whether this slot runs today, picks the emptier volume,
# applies the day's exposure, and records the plan beside the manifest. It
# exits zero without acquiring when the slot is not scheduled, so a suppressed
# burst is not a failure.
ExecStart=/home/bakerlab/hydro_edge/scripts/run_burst.sh %i 2400

# Deliberately no restart, for the reason given in hydro-burst@.service: the
# binary exits 1 on any incomplete burst, so a restart would relaunch a
# forty-minute acquisition that has already written 155 GB.
Restart=no

RuntimeMaxSec=3000

# systemd signals the whole control group, so the binary receives SIGTERM
# directly and drains the ring buffer itself. Sixty stereo slots hold 1.936 GB,
# about sixteen seconds at the rate measured in GATE A3.
KillSignal=SIGTERM
TimeoutStopSec=180

SyslogIdentifier=hydro-slot
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
HYDRO_EOF

# Retarget the three calendar timers at the wrapper. hydro-burst@.service is
# left untouched: it remains what GATE S validated and what the gates, the
# soak and every manual run still use.
for slot in 1000 1300 1700; do
    f="deploy/systemd/hydro-burst-${slot}.timer"
    [ -f "$f" ] || { echo "[INSTALL] WARNING ${f} absent, skipped."; continue; }
    sed -i "s|^Unit=hydro-burst@2400\.service$|Unit=hydro-slot@${slot}.service|" "$f"
    grep -q "^Unit=hydro-slot@${slot}.service$" "$f" \
        && echo "[INSTALL] ${f} retargeted at hydro-slot@${slot}.service" \
        || echo "[INSTALL] WARNING ${f} still points elsewhere; check it by hand."
done

# The drain figure in the existing unit was understated.
sed -i 's|about twelve seconds at the measured write rate|about sixteen seconds at the rate measured in GATE A3|' \
    'deploy/systemd/hydro-burst@.service' 2>/dev/null || true

# ---------------------------------------------------------------- page
cat > site/index.html <<'HYDRO_EOF'
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Hopkins rocky-shore camera station</title>
<style>
  :root{
    --deep:#0E242B;     /* Monterey water in shade */
    --slate:#17333C;
    --line:#2A4A54;
    --foam:#EBF0EE;
    --kelp:#8AA79A;
    --brass:#C79A3F;
    --brick:#B24A40;
    --tide:#6FA79E;
  }
  *{box-sizing:border-box}
  html{-webkit-text-size-adjust:100%}
  body{
    margin:0;background:var(--deep);color:var(--foam);
    font-family:"Avenir Next","Avenir","Segoe UI",system-ui,sans-serif;
    font-size:17px;line-height:1.5;
  }
  .num{font-family:ui-monospace,"SF Mono",Menlo,monospace;
       font-variant-numeric:tabular-nums;font-size:.94em}
  a{color:var(--tide)}
  a:focus-visible,button:focus-visible{outline:2px solid var(--brass);outline-offset:3px}

  figure{margin:0;position:relative;background:var(--slate)}
  figure img{display:block;width:100%;height:auto;opacity:0;transition:opacity .5s ease}
  figure img.in{opacity:1}
  @media (prefers-reduced-motion:reduce){figure img{transition:none}}
  .verdict{
    position:absolute;left:0;right:0;bottom:0;
    padding:.6rem 1.1rem;background:rgba(14,36,43,.88);
    backdrop-filter:blur(6px);border-top:1px solid var(--line);
    display:flex;gap:.9rem;align-items:baseline;flex-wrap:wrap;
  }
  .dot{width:.62rem;height:.62rem;border-radius:50%;flex:0 0 auto;
       background:var(--tide);transform:translateY(-.1rem)}
  .attention .dot{background:var(--brass)} .fault .dot{background:var(--brick)}
  .verdict b{font-weight:600} .verdict span{color:var(--kelp);font-size:.9rem}

  main{max-width:44rem;margin:0 auto;padding:1.6rem 1.1rem 4rem}
  h1{font-size:1.02rem;font-weight:600;letter-spacing:.01em;margin:0 0 .15rem}
  .sub{color:var(--kelp);font-size:.9rem;margin:0 0 2rem}

  .today{border-left:2px solid var(--brass);padding:.1rem 0 .1rem 1rem;margin:0 0 1.5rem}
  .today p{margin:.15rem 0}
  .today .big{font-size:1.3rem;font-weight:600}
  button{
    margin-top:.9rem;font:inherit;font-size:.95rem;color:var(--deep);
    background:var(--brass);border:0;border-radius:2px;
    padding:.55rem 1rem;cursor:pointer;
  }
  button:hover{background:#d8ab4c}

  dl{display:grid;grid-template-columns:1fr auto;gap:0;margin:0 0 1.6rem;
     border-top:1px solid var(--line)}
  dt,dd{margin:0;padding:.5rem 0;border-bottom:1px solid var(--line)}
  dd{text-align:right}
  dt{color:var(--kelp)}
  h2{font-size:.95rem;font-weight:600;color:var(--foam);margin:0 0 .3rem}

  table{width:100%;border-collapse:collapse;font-size:.93rem}
  th{text-align:left;font-weight:500;color:var(--kelp);padding:.45rem 0;
     border-bottom:1px solid var(--line)}
  td{padding:.45rem 0;border-bottom:1px solid var(--line)}
  td:last-child,th:last-child{text-align:right}
  .bad{color:var(--brick)}
  ul.why{margin:.4rem 0 0;padding-left:1.1rem;color:var(--kelp);font-size:.92rem}
  footer{color:var(--kelp);font-size:.85rem;margin-top:2.2rem}
</style>
</head>
<body>

<figure>
  <img id="shot" alt="Most recent morning view from the station">
  <figcaption class="verdict" id="verdict">
    <span class="dot"></span><b id="vtext">Loading</b><span id="vwhen"></span>
  </figcaption>
</figure>

<main>
  <h1>Hopkins rocky-shore camera station</h1>
  <p class="sub">Agassiz rooftop, Hopkins Marine Station. The view above is
  taken fresh at 08:00 each morning.</p>

  <section class="today">
    <p class="big" id="planline">&mdash;</p>
    <p id="planwhy"></p>
    <button id="edit" type="button">Change today's plan</button>
    <p class="sub" style="margin:.6rem 0 0">Edits made before 09:45 Pacific take
    effect today. Set <span class="num">date</span> to today's date,
    <span class="num">bursts</span> between 0 and 3, and
    <span class="num">exposure_us</span> between 100 and 20000.</p>
  </section>

  <h2>Metered this morning</h2>
  <dl id="metered"></dl>

  <h2>Station</h2>
  <dl id="station"></dl>

  <h2>Recent bursts</h2>
  <table><thead><tr><th>Burst</th><th>Frames</th><th>Result</th></tr></thead>
    <tbody id="bursts"></tbody></table>

  <footer id="foot"></footer>
</main>

<script>
const OWNER = "JeanPotdeFleur", REPO = "hopkins-station";
const EDIT = `https://github.com/${OWNER}/${REPO}/edit/main/control/plan.json`;
document.getElementById("edit").onclick = () => window.open(EDIT, "_blank");

const q = "?t=" + Date.now();
const img = document.getElementById("shot");
img.onload = () => img.classList.add("in");
img.src = "latest.jpg" + q;

const pad = n => String(n).padStart(2, "0");
const local = iso => {
  const d = new Date(iso);
  return d.toLocaleString("en-GB", { weekday: "short", day: "numeric",
    month: "short", hour: "2-digit", minute: "2-digit",
    timeZone: "America/Los_Angeles" });
};
const row = (dl, k, v) => {
  const dt = document.createElement("dt"); dt.textContent = k;
  const dd = document.createElement("dd"); dd.className = "num";
  dd.textContent = (v === null || v === undefined) ? "\u2014" : v;
  dl.append(dt, dd);
};

fetch("status.json" + q).then(r => r.json()).then(s => {
  const ageH = (Date.now() - new Date(s.generated_utc)) / 3.6e6;
  const box = document.getElementById("verdict");
  const vt = document.getElementById("vtext");
  const vw = document.getElementById("vwhen");

  if (ageH > 26) {
    box.className = "verdict fault";
    vt.textContent = "No report today";
    vw.textContent = "The station last reported " + local(s.generated_utc) +
                     ". Check that it is powered and on the network.";
  } else {
    const h = s.health || {};
    box.className = "verdict " + (h.verdict === "fault" ? "fault"
                  : h.verdict === "attention" ? "attention" : "");
    vt.textContent = h.verdict === "fault" ? "Needs attention now"
                   : h.verdict === "attention" ? "Running, with something to note"
                   : "Running normally";
    vw.textContent = "Reported " + local(s.generated_utc);
    const all = (h.faults || []).concat(h.notes || []);
    if (all.length) {
      const ul = document.createElement("ul"); ul.className = "why";
      all.forEach(t => { const li = document.createElement("li");
                         li.textContent = t; ul.append(li); });
      box.append(ul);
    }
  }

  const p = s.plan || {}, slots = p.slots || [];
  const hhmm = t => t.slice(0, 2) + ":" + t.slice(2);
  document.getElementById("planline").textContent =
    slots.length === 0 ? "No burst today"
    : `${slots.length} burst${slots.length > 1 ? "s" : ""} \u2014 ` +
      slots.map(hhmm).join(" and ");
  document.getElementById("planwhy").textContent =
    (p.source === "plan" ? "From today's plan" : "Standard schedule")
    + (p.exposure_us ? `, exposure ${p.exposure_us} \u00b5s`
                     : ", exposure from the station's own setting")
    + (p.reason && p.source !== "plan" ? ` (${p.reason})` : "");

  const m = s.morning || {}, r = m.radiometry || {};
  const dm = document.getElementById("metered");
  row(dm, "Exposure the scene called for", m.exposure_us ? m.exposure_us + " \u00b5s" : null);
  row(dm, "Gain", m.gain_db !== undefined ? m.gain_db + " dB" : null);
  row(dm, "Saturated pixels", r.sat_pct !== undefined ? r.sat_pct + " %" : null);
  row(dm, "Median level", r.p50 !== undefined ? r.p50 + " DN" : null);
  row(dm, "Brightest", r.pmax !== undefined ? r.pmax + " DN" : null);

  const st = s.station || {}, ds = document.getElementById("station");
  (s.storage || []).forEach(v =>
    row(ds, "Free on " + v.mount, v.free_gb + " GB, " + v.used_pct + " % used"));
  row(ds, "Die temperature", st.soc_temp_c !== null ? st.soc_temp_c + " \u00b0C" : null);
  row(ds, "Clock disciplined", st.clock_synchronised ? "yes" : "no");
  row(ds, "Timepulse advancing", st.pps_pulsing ? "yes" : "no");
  row(ds, "Journal anomalies, 24 h", s.journal_anomalies_24h);

  const tb = document.getElementById("bursts");
  (s.bursts || []).forEach(b => {
    const tr = document.createElement("tr");
    const ok = b.completed && !b.lost;
    tr.innerHTML =
      `<td class="num">${b.ended_utc ? local(b.ended_utc) : b.id}</td>` +
      `<td class="num">${b.frames ?? "\u2014"}${b.target ? " / " + b.target : ""}</td>` +
      `<td class="${ok ? "" : "bad"}">${ok ? "complete"
        : (b.completed ? b.lost + " frames lost" : "incomplete")}</td>`;
    tb.append(tr);
  });
  if (!(s.bursts || []).length) {
    tb.innerHTML = '<tr><td colspan="3">No burst on record yet.</td></tr>';
  }

  document.getElementById("foot").textContent =
    "Served from GitHub Pages. The station pushes here each morning; it cannot "
    + "be reached from outside the campus network.";
}).catch(() => {
  const box = document.getElementById("verdict");
  box.className = "verdict fault";
  document.getElementById("vtext").textContent = "Cannot read the station's report";
  document.getElementById("vwhen").textContent =
    "status.json is missing or malformed in this repository.";
});
</script>
</body>
</html>
HYDRO_EOF

cat > site/README.md <<'HYDRO_EOF'
# Hopkins rocky-shore camera station

**Status page: https://JeanPotdeFleur.github.io/hopkins-station/**

Open that page. It shows the view from the station as it was at 08:00 this
morning, whether the station is running, and what it is scheduled to acquire
today. Reading it needs no account.

## The only file to edit

`control/plan.json` sets today's acquisition. Nothing else here is meant to be
changed by hand, and the button on the status page opens this file directly.

```json
{
  "date": "2026-09-14",
  "bursts": 2,
  "exposure_us": 2200,
  "note": "low tide at 08:10, keeping the morning slot"
}
```

| Field | What it does |
| --- | --- |
| `date` | Today's date, Pacific time. The plan is ignored unless this matches, so a plan nobody updated expires on its own. |
| `bursts` | How many 40-minute bursts to acquire: 0, 1, 2 or 3. One is 10:00; two is 10:00 and 17:00; three is 10:00, 13:00 and 17:00. |
| `exposure_us` | Optional. Exposure in microseconds, between 100 and 20000. Leave it out to keep the station's own setting. |
| `note` | Optional free text. It is copied into the archive beside the frames. |

Commit before **09:45 Pacific** for the change to apply the same day. The
station reads the plan at 09:50 and does not look again, so a burst already
under way is never altered by a late edit.

If any field is missing, out of range or the file will not parse, the station
acquires two bursts at 10:00 and 17:00 rather than nothing at all. The reason
it fell back is shown on the status page.

## What the station writes here

`latest.jpg` and `status.json`, once each morning, both overwritten. Do not
edit them.

`index.html` is the status page. Its source of truth is `site/index.html` in
the [hydro_edge](https://github.com/JeanPotdeFleur/hydro_edge) repository: if
it is ever broken here, copy it back from there.
HYDRO_EOF

echo
echo "[INSTALL] created:"
printf '  %s\n' scripts/morning_snapshot.sh scripts/fetch_plan.sh \
    scripts/run_burst.sh scripts/push_status.sh \
    deploy/systemd/hydro-morning.service deploy/systemd/hydro-morning.timer \
    deploy/systemd/hydro-plan.service deploy/systemd/hydro-plan.timer \
    'deploy/systemd/hydro-slot@.service' site/index.html site/README.md
echo
echo "[INSTALL] Next, in order:"
echo "  1. bash -n on each script, then commit."
echo "  2. Clone the transport repository:"
echo "       git clone git@github.com:JeanPotdeFleur/hopkins-station.git ~/hopkins-station"
echo "  3. Copy site/index.html and site/README.md into it, commit and push."
echo "  4. Add HYDRO_OUTPUT_ALT and HYDRO_SITE to /etc/default/hydro-edge."
echo "  5. Install the units, reload, enable the two new timers."
echo "  6. Test each script by hand before arming anything."