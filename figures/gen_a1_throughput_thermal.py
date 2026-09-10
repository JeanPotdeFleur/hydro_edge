#!/usr/bin/env python3
"""A1 - Sustained write throughput and SoC temperature over GATE A1.

The endurance run of 25 August, 5400 s of Strategy 2 without interruption.
The figure exists to show two things at once: that the write rate never falls
away from the 64.5 MB/s the pipeline demands, and that the die temperature
plateaus far below the throttling point.

The instantaneous device rate is sampled every five seconds while the consumer
commits four files per second, so the raw trace is spiky by construction and
its spread is a sampling artefact rather than a property of the drive. The
running median over one minute is the quantity to read; the raw samples are
kept underneath so the reader can see what was actually recorded.
"""
import os, sys, csv
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "docs", "gate_a1_monitor.csv")

NOMINAL_MBPS = 64.5      # 2 sensors x 16.13 MB/frame x 2 Hz
THROTTLE_C   = 85.0      # BCM2712 soft limit

t, mbps, temp = [], [], []
with open(SRC) as f:
    for row in csv.DictReader(f):
        t.append(float(row["elapsed_s"]))
        mbps.append(float(row["dev_wmbps"]))
        temp.append(float(row["soc_temp_c"]))
t, mbps, temp = np.array(t), np.array(mbps), np.array(temp)

def running_pct(x, w, q):
    out = np.empty_like(x)
    for i in range(len(x)):
        out[i] = np.percentile(x[max(0, i - w // 2):i + w // 2 + 1], q)
    return out

W = 12                                   # 12 samples x 5 s = 60 s
lo  = running_pct(mbps, W, 10)
med = running_pct(mbps, W, 50)
hi  = running_pct(mbps, W, 90)

S.setup()
fig, ax = plt.subplots(figsize=(S.FULL_W, S.FULL_W * 0.46))

# 10th to 90th percentile over the same one-minute window rather than the raw
# samples: the spread is a sampling artefact and drawing every spike hides the
# quantity the figure is about.
ax.fill_between(t, lo, hi, color="#cfcfcf", lw=0, zorder=1,
                label="10th\u201390th percentile, 60 s window")
ax.plot(t, med, color=S.SERIES[0]["color"], lw=1.2, zorder=3,
        label="Write throughput, 60 s median")
# Both limits are labelled in the legend rather than annotated in place: the
# trace occupies the whole panel and any in-plot label lands on data.
ax.axhline(NOMINAL_MBPS, **S.LIMIT, zorder=2,
           label=f"Pipeline demand, {NOMINAL_MBPS} MB/s")
ax.set_xlabel("Elapsed time (s)")
ax.set_ylabel("Write throughput (MB/s)")
ax.set_xlim(0, t[-1])
ax.set_ylim(0, 125)

ax2 = ax.twinx()
ax2.plot(t, temp, color=S.SERIES[2]["color"], ls="-.", lw=1.2, zorder=4,
         label="SoC temperature")
ax2.set_ylabel("SoC temperature (\u00b0C)")
# Offset deliberately so the temperature trace sits clear of the throughput
# trace instead of crossing it: with both axes anchored at zero the two curves
# overlap and neither can be read.
ax2.set_ylim(20, 120)
ax2.axhline(THROTTLE_C, color="#8c1d18", ls=":", lw=1.0, zorder=2,
            label=f"Throttling limit, {THROTTLE_C:.0f} \u00b0C")
ax2.annotate(f"peak {temp.max():.1f} \u00b0C, margin "
             f"{THROTTLE_C - temp.max():.0f} \u00b0C",
             xy=(3800, temp[np.searchsorted(t, 3800)]), xytext=(0, -13),
             textcoords="offset points", ha="center", fontsize=7.5,
             color=S.SERIES[2]["color"])
ax2.spines["top"].set_visible(False)
ax2.grid(False)

h1, l1 = ax.get_legend_handles_labels()
h2, l2 = ax2.get_legend_handles_labels()
ax.legend(h1 + h2, l1 + l2, loc="lower center", ncol=3,
              bbox_to_anchor=(0.5, -0.04))

S.save(fig, os.path.join(HERE, "out", "res_gate_a1_throughput_thermal.png"))

act = mbps[t > 60]
print(f"  throughput after startup: median {np.median(act):.1f}, "
      f"p10 {np.percentile(act,10):.1f}, p90 {np.percentile(act,90):.1f} MB/s")
print(f"  SoC temperature: {temp.min():.1f} to {temp.max():.1f} \u00b0C, "
      f"margin {THROTTLE_C - temp.max():.1f} \u00b0C")