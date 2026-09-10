#!/usr/bin/env python3
"""B2 - Ring buffer margin before frame loss (GATE A3).

Parses the sweep log produced by running the acquisition binary repeatedly
with an increasing deliberate consumer stall, and plots peak occupancy against
stall duration with the loss threshold marked.

Usage:
    ./gen_b2_buffer_stress_margin.py /tmp/b2_sweep.log

The log is expected to contain, per run, a line "### STALL=<n>" followed by
the binary's own reports. Everything is read from those reports rather than
recomputed, so the figure and the archive agree by construction.
"""
import os, sys, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

HERE = os.path.dirname(os.path.abspath(__file__))
src  = sys.argv[1] if len(sys.argv) > 1 else "/tmp/b2_sweep.log"

RE_HDR  = re.compile(r"###\s*STALL=(\d+)")
RE_PEAK = re.compile(r"ring buffer peak (\d+)/(\d+)")
RE_OVF  = re.compile(r"buffer overflows (\d+), late frames skipped (\d+)")
RE_CNT  = re.compile(r"(\d+) triggers, (\d+) pushed, (\d+) written")
RE_DONE = re.compile(r"completed=(true|false)")

runs, cur, cap = [], None, 60
with open(src) as f:
    for line in f:
        m = RE_HDR.search(line)
        if m:
            if cur: runs.append(cur)
            cur = {"stall": int(m.group(1))}
            continue
        if cur is None:
            continue
        m = RE_PEAK.search(line)
        if m:
            cur["peak"] = int(m.group(1)); cap = int(m.group(2))
        m = RE_OVF.search(line)
        if m:
            cur["overflow"] = int(m.group(1)); cur["late"] = int(m.group(2))
        m = RE_CNT.search(line)
        if m:
            cur["trig"], cur["push"], cur["writ"] = (int(m.group(i)) for i in (1, 2, 3))
        m = RE_DONE.search(line)
        if m:
            cur["completed"] = (m.group(1) == "true")
if cur: runs.append(cur)
runs = [r for r in runs if "peak" in r]
if not runs:
    sys.exit(f"no run found in {src}")
runs.sort(key=lambda r: r["stall"])

stall = np.array([r["stall"] for r in runs], float)
peak  = np.array([r["peak"] for r in runs], float)
lost  = np.array([r.get("trig", 0) - r.get("writ", 0) for r in runs], float)
ovf   = np.array([r.get("overflow", 0) for r in runs], float)

# First stall at which anything is lost. The ideal fill is two slots per
# second at 2 Hz, so the calculated threshold is capacity / 2.
idx_loss = np.where((lost > 0) | (ovf > 0))[0]
thr_meas = stall[idx_loss[0]] if len(idx_loss) else None
thr_calc = cap / 2.0

S.setup()
fig, ax = plt.subplots(figsize=(S.FULL_W, S.FULL_W * 0.44))

ax.plot(stall, np.minimum(stall * 2, cap), color="#b0b0b0", ls=":", lw=1.0,
        label="Calculated fill, 2 slots/s")
ok  = (lost == 0) & (ovf == 0)
ax.plot(stall[ok], peak[ok], ls="-", marker="o", ms=4,
        color=S.SERIES[0]["color"], label="Peak occupancy, no loss")
if (~ok).any():
    ax.plot(stall[~ok], peak[~ok], ls="none", marker="X", ms=7,
            color=S.LIMIT["color"], label="Peak occupancy, frames lost")

ax.axhline(cap, **S.LIMIT)
ax.annotate(f"ring buffer capacity, {cap} slots", xy=(stall.min(), cap),
            xytext=(2, -11), textcoords="offset points",
            fontsize=7.5, color=S.LIMIT["color"])
if thr_meas is not None:
    ax.axvline(thr_meas, color="#8c1d18", ls="-.", lw=1.0)
    ax.annotate(f"first loss at {thr_meas:.0f} s\n(calculated {thr_calc:.0f} s)",
                xy=(thr_meas, cap * 0.35), xytext=(-6, 0),
                textcoords="offset points", ha="right",
                fontsize=7.5, color="#8c1d18")
else:
    ax.annotate(f"no loss up to {stall.max():.0f} s; calculated threshold "
                f"{thr_calc:.0f} s", xy=(0.03, 0.92), xycoords="axes fraction",
                fontsize=7.5, color="#4d4d4d")

ax.set_xlabel("Deliberate consumer stall (s)")
ax.set_ylabel("Peak ring buffer occupancy (slots)")
ax.set_xlim(0, stall.max() * 1.05)
ax.set_ylim(0, cap * 1.18)
ax.legend(loc="lower right")
S.save(fig, os.path.join(HERE, "out", "res_buffer_stress_margin.png"))

print(f"{'stall':>6} {'peak':>6} {'trig':>6} {'writ':>6} {'lost':>6} {'ovf':>5} {'done':>6}")
for r in runs:
    print(f"{r['stall']:6d} {r['peak']:6d} {r.get('trig',0):6d} {r.get('writ',0):6d} "
          f"{r.get('trig',0)-r.get('writ',0):6d} {r.get('overflow',0):5d} "
          f"{str(r.get('completed','?')):>6}")
print(f"\ncalculated threshold {thr_calc:.0f} s"
      + (f", measured first loss at {thr_meas:.0f} s" if thr_meas else ", no loss observed"))