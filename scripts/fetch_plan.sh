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
    log "no clone at ${SITE}; reading whatever plan is on disk without refreshing it."
fi

# When the plan carries no explicit date, the day it applies to is the day it
# was committed. Asking a researcher to retype today's date every morning is a
# needless source of error, and the commit timestamp is a fact GitHub records
# for them. Only a plan prepared for a future date needs the field filled in.
COMMITTED="$(TZ=America/Los_Angeles git -C "$SITE" log -1 \
    --format=%cd --date=format-local:%Y-%m-%d -- control/plan.json 2>/dev/null)"
[ -n "$COMMITTED" ] || COMMITTED="unknown"

python3 - "${SITE}/control/plan.json" "$ACTIVE" "$TODAY" "$COMMITTED" <<'PYEOF'
import json, sys, os, datetime

src, dst, today, committed = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

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

# An explicit date wins, which is how a plan is prepared a day ahead; with
# none, the plan speaks for the day it was committed and expires on its own.
stated = raw.get("date")
if stated:
    effective, origin = str(stated), "stated"
else:
    effective, origin = committed, "committed"

if effective != today:
    DEFAULT["reason"] = (f"plan {origin} for {effective}, not {today}"
                         if effective != "unknown"
                         else "commit date of the plan could not be read")
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

emit({"source": "plan", "date": today, "date_from": origin,
      "bursts": bursts, "slots": SLOTS[bursts], "exposure_us": exposure,
      "reason": f"accepted, dated by {origin} date",
      "note": str(raw.get("note", ""))[:200]})
PYEOF
