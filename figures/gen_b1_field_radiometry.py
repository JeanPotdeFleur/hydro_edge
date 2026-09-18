#!/usr/bin/env python3
"""B1 field - Scene radiometry over ninety minutes at a locked exposure.

The station measures its exposure once at arming and then holds it for the
whole burst. That is a decision of record and it has had no evidence behind it:
over ninety minutes a cloud can move the scene by more than two stops, and a
locked exposure that was right at the first frame can be wrong at the last.

This is the evidence. One frame per camera per minute is read back from disk
and passed through the same decode as the science data, giving the median
level, the upper percentiles and the clipped fraction per channel.

Two readings matter and they are not the same. The median says whether the
scene drifted. The 99.9th percentile and the clipped fraction say whether the
foam saturated, and that is the one defect no processing can repair: a
saturated patch is a plateau, it has no gradient, and gradient is precisely
what the velocimetry correlates. A dark frame can be scaled; a clipped one has
lost the signal.

The two cameras are drawn separately because they do not see the same scene.
One looks along the shore and carries dry sunlit rock, the other looks out to
sea and carries mostly foam, so the camera with more foam is the one against
which the exposure has to be judged.
"""
import os, sys, csv
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

S.setup()

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "docs", "b1_field_radiometry_20260917.csv")
OUT  = os.path.join(HERE, "out", "res_b1_radiometry.png")

FULL_SCALE = 255.0

series = {}
with open(SRC) as f:
    for r in csv.DictReader(f):
        role = r["role"]
        d = series.setdefault(role, {"t": [], "p50": [], "p99": [],
                                    "p999": [], "clip": []})
        d["t"].append(float(r["elapsed_s"]) / 60.0)
        d["p50"].append(float(r["p50"]))
        d["p99"].append(float(r["p99"]))
        d["p999"].append(float(r["p999"]))
        d["clip"].append(float(r["clipped_pct"]))

fig, (ax, ax2) = plt.subplots(
    2, 1, figsize=(S.FULL_W, 10.0 * S.CM), sharex=True,
    gridspec_kw={"height_ratios": [2, 1], "hspace": 0.12})

for i, role in enumerate(sorted(series)):
    d = series[role]
    st = {k: v for k, v in S.SERIES[i].items() if k != "marker"}
    ax.plot(d["t"], d["p50"], label=f"{role} median", **st)
    st2 = dict(st); st2["ls"] = ":" if st["ls"] != ":" else "--"
    ax.plot(d["t"], d["p99"], label=f"{role} 99th percentile", **st2)
    ax2.plot(d["t"], d["clip"], label=role, **st)

ax.axhline(FULL_SCALE, **S.LIMIT)
ax.text(0.5, FULL_SCALE * 1.06, "full scale, 255 DN", fontsize=8,
        color=S.LIMIT["color"], va="bottom")
ax.set_yscale("log")
ax.set_ylabel("Digital number (8 bit)")
ax.set_ylim(20, 400)
ax.set_yticks([20, 50, 100, 255])
ax.get_yaxis().set_major_formatter(plt.ScalarFormatter())
ax.legend(loc="lower left", ncol=2)
# The 99.9th percentile is pinned at full scale for every sample of the burst
# on both cameras and would be a flat line on the limit; it is stated in the
# caption instead of drawn.

ax2.set_ylabel("Clipped (%)")
ax2.set_xlabel("Time from start of burst (min)")
ax2.legend(loc="upper left", ncol=2)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
S.save(fig, OUT)

for role in sorted(series):
    d = series[role]
    print(f"{role}: median {d['p50'][0]:.0f} -> {d['p50'][-1]:.0f} DN "
          f"({100*(d['p50'][0]-d['p50'][-1])/d['p50'][0]:.0f} % fall), "
          f"p99 {d['p99'][0]:.0f} -> {d['p99'][-1]:.0f}, "
          f"clipped {np.min(d['clip']):.1f} to {np.max(d['clip']):.1f} %")