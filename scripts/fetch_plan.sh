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
TEMPLATE="${HYDRO_PLAN_TEMPLATE:-${HYDRO_ROOT:-/home/bakerlab/hydro_edge}/deploy/plan_template.json}"
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
# on their behalf. Only a plan prepared for a future date needs the field.
COMMITTED="$(TZ=America/Los_Angeles git -C "$SITE" log -1 \
    --format=%cd --date=format-local:%Y-%m-%d -- control/plan.json 2>/dev/null)"
[ -n "$COMMITTED" ] || COMMITTED="unknown"

python3 - "${SITE}/control/plan.json" "$ACTIVE" "$TODAY" "$COMMITTED" \
         "$TEMPLATE" <<'PYEOF'
import json, sys, os, datetime

src, dst, today, committed, template = sys.argv[1:6]

SLOTS = {0: [], 1: ["1000"], 2: ["1000", "1700"], 3: ["1000", "1300", "1700"]}
DEFAULT = {"source": "default", "date": today, "bursts": 2,
           "slots": SLOTS[2], "exposure_us": None,
           "reason": "standard schedule"}

def is_blank(raw):
    """Is the file still the untouched template?"""
    try:
        blank = json.load(open(template))
    except Exception:
        return False
    return isinstance(raw, dict) and all(
        raw.get(k) == blank.get(k)
        for k in ("bursts", "exposure_us", "note", "date"))


def wants_reset(raw):
    """Should the plan file be put back to the template?

    Yes whenever it differs from it, whether it was accepted or refused: an
    accepted plan has been consumed and must not be re-consumed tomorrow by an
    unrelated edit, and a refused one is malformed and better replaced. No when
    it states a date still to come, which is the one reason to keep a plan
    sitting in the file.
    """
    try:
        blank = json.load(open(template))
    except Exception:
        return False
    stated = raw.get("date") if isinstance(raw, dict) else None
    if stated and str(stated) > today:
        return False
    if not isinstance(raw, dict):
        return True
    return any(raw.get(k) != blank.get(k)
               for k in ("bursts", "exposure_us", "note", "date"))


def emit(plan, reset=False):
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
    # A distinct status rather than a file or a flag: the caller is a shell
    # script and this is the one thing it needs to know.
    sys.exit(10 if reset else 0)

try:
    raw = json.load(open(src))
except FileNotFoundError:
    DEFAULT["reason"] = "no plan file"; emit(DEFAULT)
except Exception as exc:
    DEFAULT["reason"] = f"plan unreadable: {exc}"; emit(DEFAULT, True)

# A file identical to the template is not a plan of two bursts; it is the
# absence of a plan. The distinction is invisible in the schedule, both giving
# 10:00 and 17:00, and visible on the page, which would otherwise announce a
# plan nobody chose.
if is_blank(raw):
    DEFAULT["reason"] = "no plan set for today"; emit(DEFAULT)

# An explicit date wins, which is how a plan is prepared a day ahead; with
# none, the plan speaks for the day it was committed.
stated = raw.get("date")
if stated:
    effective, origin = str(stated), "stated"
else:
    effective, origin = committed, "committed"

if effective != today:
    DEFAULT["reason"] = (f"plan {origin} for {effective}, not {today}"
                         if effective != "unknown"
                         else "commit date of the plan could not be read")
    emit(DEFAULT, wants_reset(raw))

try:
    bursts = int(raw["bursts"])
except Exception:
    DEFAULT["reason"] = "bursts missing or not a number"
    emit(DEFAULT, wants_reset(raw))

if bursts not in SLOTS:
    DEFAULT["reason"] = f"bursts={bursts} outside 0 to 3"
    emit(DEFAULT, wants_reset(raw))

exposure = raw.get("exposure_us", None)
if exposure is not None:
    try:
        exposure = int(exposure)
    except Exception:
        DEFAULT["reason"] = "exposure_us not a number"
        emit(DEFAULT, wants_reset(raw))
    if not 100 <= exposure <= 20000:
        DEFAULT["reason"] = f"exposure_us={exposure} outside 100 to 20000"
        emit(DEFAULT, wants_reset(raw))

emit({"source": "plan", "date": today, "date_from": origin,
      "bursts": bursts, "slots": SLOTS[bursts], "exposure_us": exposure,
      "reason": f"accepted, dated by {origin} date",
      "note": str(raw.get("note", ""))[:200]},
     wants_reset(raw))
PYEOF

rc=$?

# The plan file is versioned, so nothing empties it on its own: yesterday's
# bursts and yesterday's note are still there this morning. Worse, changing
# only the note makes the commit date today, and yesterday's bursts silently
# become today's plan. Once read, the file is therefore put back to the
# template. A plan stating a future date is left alone, that being the one
# reason to keep one sitting in the file.
if [ "$rc" -eq 10 ] && [ -d "${SITE}/.git" ] && [ -r "$TEMPLATE" ]; then
    for attempt in 1 2 3; do
        if git -C "$SITE" fetch --quiet origin main 2>/dev/null &&
           git -C "$SITE" reset --hard --quiet origin/main 2>/dev/null &&
           cp -f "$TEMPLATE" "${SITE}/control/plan.json" &&
           git -C "$SITE" add control/plan.json &&
           { git -C "$SITE" diff --cached --quiet ||
             git -C "$SITE" -c user.name="hopkins1-pi5" \
                 -c user.email="hopkins1-pi5@local" \
                 commit -q -m "plan: read and reset for $TODAY"; } &&
           git -C "$SITE" push -q origin main 2>/dev/null; then
            log "plan file reset to the blank template."
            break
        fi
        log "could not reset the plan file, attempt ${attempt}."
        [ "$attempt" -lt 3 ] && sleep 5
    done
fi

exit 0
