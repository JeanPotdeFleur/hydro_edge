#!/usr/bin/env python3
"""B1 - Sustained write throughput against volume fill.

GATE A1 and GATE A3 both measured a nearly empty drive, and the dynamic SLC
cache of a QLC device shrinks as the volume fills. Section 3.2 carries that as
the central vulnerability of the long-burst protocol. This figure settles it.

Left panel, the operational question: is the 64.5 MB/s budget held over the
whole fill range. Right panel, the technical one: where the SLC cache goes.
The two raw device probes are the decisive evidence, being the same tool run
at both ends of the range.

Usage:
    ./gen_b1_fill_throughput.py [../docs/b1_fill_sweep.csv]
"""
import os, sys, csv
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

HERE = os.path.dirname(os.path.abspath(__file__))
src  = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "docs", "b1_fill_sweep.csv")
out  = os.path.join(HERE, "out", "res_fill_throughput.png")

# Reference levels, each measured elsewhere and cited rather than recomputed.
DEMAND   = 64.5    # MB/s, two sensors at 2 Hz, Appendix H
CONSUMER = 121.7   # MB/s, consumer drain measured in GATE A3 on a near-empty volume
RAW      = [(2, 135.0), (91, 135.0)]   # raw dd, zeros, O_DIRECT, 16 MiB blocks
RESTART_CHUNK = 6  # the run was interrupted after chunk 5 to take the first raw probe

with open(src) as f:
    rows = list(csv.DictReader(f))
if not rows:
    sys.exit(f"no data in {src}")

chunk = np.array([int(r["chunk"])              for r in rows])
fill  = np.array([float(r["used_pct_after"])   for r in rows])
mbps  = np.array([float(r["mbps"])             for r in rows])
temp  = np.array([float(r["soc_temp_c"])       for r in rows])
cum   = np.array([float(r["bytes_written_cum"]) for r in rows])

# The record carries a step at the restart, which is not a fill effect and must
# not be read as one. Everything quantitative is taken after it.
post = chunk >= RESTART_CHUNK
med_body = float(np.median(mbps[post & (fill < 89)]))
med_top  = float(np.median(mbps[fill >= 89]))

S.setup()
fig, (axL, axR) = plt.subplots(1, 2, figsize=(S.FULL_W, 0.42 * S.FULL_W))

# ---------------------------------------------------------------- left panel
axL.plot(fill, mbps, color=S.SERIES[0]["color"], lw=1.0,
         label="Pipeline, incompressible payload")
axL.plot([p[0] for p in RAW], [p[1] for p in RAW], linestyle="none",
         marker=S.SERIES[1]["marker"], ms=5, mfc="white",
         color=S.SERIES[0]["color"], label="Raw device, zeros")

axL.axhline(CONSUMER, color=S.SERIES[2]["color"], ls=":", lw=1.0)
axL.text(50, CONSUMER + 2.5, "consumer drain, GATE A3, 121.7", fontsize=7,
         color=S.SERIES[2]["color"], ha="center")

axL.axhline(DEMAND, **S.LIMIT)
axL.text(50, DEMAND + 2.5, "pipeline demand, 64.5 MB/s", fontsize=7,
         color=S.LIMIT["color"], ha="center")

axL.set_xlim(0, 100)
axL.set_ylim(0, 148)
axL.set_xlabel("Volume fill (per cent)")
axL.set_ylabel("Write throughput (MB/s)")
axL.legend(loc="lower left")
axL.text(0.04, 0.60,
         f"worst point {mbps[post].min():.1f} MB/s,\nmargin {mbps[post].min() / DEMAND:.2f}\u00d7",
         transform=axL.transAxes, fontsize=7, ha="left", va="top",
         color=S.SERIES[1]["color"])

# --------------------------------------------------------------- right panel
axR.plot(fill[post], mbps[post], color=S.SERIES[0]["color"], lw=0.8)
axR.plot(fill[~post], mbps[~post], color=S.SERIES[2]["color"], lw=0.8)

axR.axhline(med_body, color=S.SERIES[1]["color"], ls="--", lw=0.9)
axR.axhline(med_top,  color=S.SERIES[1]["color"], ls="--", lw=0.9)
axR.text(0.42, 0.97,
         f"median {med_body:.1f} below 89 per cent\n"
         f"median {med_top:.1f} above, {100 * (med_top / med_body - 1):+.1f} per cent",
         transform=axR.transAxes, fontsize=7, ha="left", va="top",
         color=S.SERIES[1]["color"])

axR.annotate("script restart,\nnot a fill effect",
             xy=(fill[RESTART_CHUNK - 1], mbps[RESTART_CHUNK - 1]),
             xycoords="data",
             xytext=(0.05, 0.58), textcoords="axes fraction",
             fontsize=7, color=S.SERIES[2]["color"],
             arrowprops=dict(arrowstyle="->", lw=0.7,
                             color=S.SERIES[2]["color"]))

axR.set_xlim(0, 100)
axR.set_ylim(116.0, 121.0)
axR.set_xlabel("Volume fill (per cent)")
axR.set_ylabel("Write throughput (MB/s)")

fig.tight_layout()
S.save(fig, out)

print(f"points            : {len(rows)}")
print(f"fill              : {fill.min():.0f} to {fill.max():.0f} per cent")
print(f"written           : {cum[-1] / 1024**4:.2f} TiB")
print(f"median, body      : {med_body:.2f} MB/s")
print(f"median, above 89  : {med_top:.2f} MB/s  ({100 * (med_top / med_body - 1):+.1f} per cent)")
print(f"worst point       : {mbps[post].min():.1f} MB/s, margin {mbps[post].min() / DEMAND:.2f}x")
print(f"SoC temperature   : {temp.min():.1f} to {temp.max():.1f} C, median {np.median(temp):.1f}")