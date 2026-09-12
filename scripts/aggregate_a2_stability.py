#!/usr/bin/env python3
"""Reduce an alternating trigger-mode soak to one row per burst.

GATE A2 fitted skew(t) = a + r*t + d*1[hardware] on four bursts spanning 219 s
and recorded as a residual limit that nothing established the offset d over
hours, over temperature, or across a power cycle. This reduces a night of
alternating bursts to the quantities that fit needs, so that the same estimator
runs on a baseline two hundred times longer.

Per burst: the trigger mode and start instant from the manifest, the median
inter-sensor skew and its robust dispersion from timing.csv, and the cadence
and loss counters from summary.json. The median and 1.4826*MAD are used rather
than mean and standard deviation for the reason established in GATE A1: a
handful of isolated scheduling excursions set an aggregate standard deviation
and misdescribe the behaviour it summarises.

Usage:  ./aggregate_a2_stability.py [/mnt/vault/a2soak] [out.csv]
"""
import csv, json, os, sys, statistics as st

src = sys.argv[1] if len(sys.argv) > 1 else "/mnt/vault/a2soak"
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser(
    "~/hydro_edge/docs/a2_stability.csv")

if not os.path.isdir(src):
    sys.exit(f"no such directory: {src}")

FIELDS = ["burst_id", "started_utc", "mode", "n_frames",
          "skew_median_us", "skew_mad_sigma_us", "skew_p05_us", "skew_p95_us",
          "cadence_sd_ms", "soc_note", "completed", "frames_written",
          "losses", "mean_write_MBps"]

def robust_sigma(xs):
    m = st.median(xs)
    return 1.4826 * st.median([abs(x - m) for x in xs])

rows, skipped = [], []
for name in sorted(os.listdir(src)):
    d = os.path.join(src, name)
    if not os.path.isdir(d):
        continue
    try:
        man = json.load(open(os.path.join(d, "manifest.json")))
        summ = json.load(open(os.path.join(d, "summary.json")))
    except Exception as exc:
        skipped.append(f"{name}: {exc}")
        continue

    skews = []
    try:
        with open(os.path.join(d, "timing.csv")) as f:
            for r in csv.DictReader(f):
                if r.get("status") == "0" and r.get("dev_skew_us"):
                    skews.append(float(r["dev_skew_us"]))
    except Exception as exc:
        skipped.append(f"{name}: timing.csv {exc}")
        continue
    if len(skews) < 10:
        skipped.append(f"{name}: only {len(skews)} usable frames")
        continue

    skews.sort()
    c = summ.get("counters", {})
    losses = sum(int(c.get(k, 0) or 0) for k in
                 ("incomplete", "retrieval_errors", "write_errors",
                  "buffer_overflows", "transport_frame_id_gaps",
                  "late_frames_skipped", "pps_timeouts", "trigger_errors"))
    rows.append({
        "burst_id": name,
        "started_utc": man.get("started_utc", ""),
        "mode": man.get("acquisition", {}).get("trigger_source", "?"),
        "n_frames": len(skews),
        "skew_median_us": round(st.median(skews), 3),
        "skew_mad_sigma_us": round(robust_sigma(skews), 3),
        "skew_p05_us": round(skews[int(0.05 * len(skews))], 3),
        "skew_p95_us": round(skews[min(int(0.95 * len(skews)), len(skews) - 1)], 3),
        "cadence_sd_ms": summ.get("cadence", {}).get("sd_ms"),
        "soc_note": "",
        "completed": bool(summ.get("completed")),
        "frames_written": c.get("frames_written"),
        "losses": losses,
        "mean_write_MBps": summ.get("mean_write_MBps"),
    })

if not rows:
    sys.exit("no usable burst found")

rows.sort(key=lambda r: r["started_utc"])
os.makedirs(os.path.dirname(dst), exist_ok=True)
with open(dst, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=FIELDS)
    w.writeheader()
    w.writerows(rows)

by_mode = {}
for r in rows:
    by_mode.setdefault(r["mode"], []).append(r)

print(f"[AGG] {len(rows)} bursts -> {dst}")
for mode, rs in sorted(by_mode.items()):
    jit = [r["skew_mad_sigma_us"] for r in rs]
    print(f"[AGG]   {mode:9s} n={len(rs):3d}  "
          f"within-burst dispersion median {st.median(jit):6.2f} us  "
          f"span {min(jit):.2f} to {max(jit):.2f}")
bad = [r for r in rows if not r["completed"] or r["losses"]]
print(f"[AGG] incomplete or lossy bursts: {len(bad)}"
      + ("" if not bad else " -> " + ", ".join(r["burst_id"] for r in bad[:5])))
first, last = rows[0]["started_utc"], rows[-1]["started_utc"]
print(f"[AGG] baseline {first} to {last}")
if skipped:
    print(f"[AGG] skipped {len(skipped)}:")
    for s in skipped[:5]:
        print("[AGG]   " + s)