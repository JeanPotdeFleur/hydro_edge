#!/usr/bin/env python3
"""A5 - Stability of the inter-camera exposure offset over a night.

GATE A2 measured a +297.2 us offset under software triggering and its removal
under hardware, on four bursts spanning 219 s, and recorded as a residual limit
that nothing established the offset over hours or over temperature. This is the
same experiment on a 13.9 h baseline: 84 bursts alternating software and line2
every ten minutes.

Left, the offset itself, estimated by adjacent triplets rather than by a fit.
For each triplet of alternating modes the flanking mode is interpolated
linearly between its two members and the middle burst is read against it, so
the common drift cancels locally and no model of it is assumed.

Right, the within-burst dispersion of the skew, which is the frame-to-frame
jitter of the trigger, plotted against time so that the separation between the
two modes can be seen to hold all night rather than on average.

Usage:
    ./gen_a5_a2_stability.py [../docs/a2_stability.csv]
"""
import os, sys, csv, datetime as dt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

HERE = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "docs",
                                                         "a2_stability.csv")
out = os.path.join(HERE, "out", "res_a2_stability.png")

GATE_A2 = 297.2   # us, measured on four bursts at commit a5140cd

with open(src) as f:
    rows = list(csv.DictReader(f))
rows.sort(key=lambda r: r["started_utc"])

t0 = dt.datetime.strptime(rows[0]["started_utc"], "%Y-%m-%dT%H:%M:%SZ")
el = np.array([(dt.datetime.strptime(r["started_utc"], "%Y-%m-%dT%H:%M:%SZ")
                - t0).total_seconds() / 3600.0 for r in rows])
skew = np.array([float(r["skew_median_us"]) for r in rows])
jit = np.array([float(r["skew_mad_sigma_us"]) for r in rows])
hw = np.array([r["mode"] == "line2" for r in rows])

# ------------------------------------------------ adjacent-triplet estimator
dt_h, dv = [], []
for i in range(len(el) - 2):
    if hw[i] == hw[i + 2] and hw[i + 1] != hw[i]:
        w = (el[i + 1] - el[i]) / (el[i + 2] - el[i])
        base = skew[i] + w * (skew[i + 2] - skew[i])
        dv.append((skew[i + 1] - base) * (1.0 if hw[i + 1] else -1.0))
        dt_h.append(el[i + 1])
dt_h, dv = np.array(dt_h), np.array(dv)
med = float(np.median(dv))
rob = 1.4826 * float(np.median(np.abs(dv - med)))
sem = float(np.std(dv, ddof=1) / np.sqrt(len(dv)))

S.setup()
fig, (axL, axR) = plt.subplots(1, 2, figsize=(S.FULL_W, 0.42 * S.FULL_W))

# ---------------------------------------------------------------- left panel
axL.axhspan(med - rob, med + rob, color=S.SERIES[3]["color"], alpha=0.30, lw=0)
axL.plot(dt_h, dv, linestyle="none", marker="o", ms=3.0, mfc="white",
         mew=0.8, color=S.SERIES[0]["color"])
axL.axhline(med, color=S.SERIES[1]["color"], ls="--", lw=1.0)
axL.axhline(GATE_A2, **S.LIMIT)
axL.text(0.5, 0.97,
         f"{len(dv)} adjacent triplets, drift removed locally\n"
         f"median {med:.1f} \u00b1 {sem:.1f} \u00b5s, robust dispersion {rob:.1f} \u00b5s",
         transform=axL.transAxes, fontsize=7, ha="center", va="top",
         color=S.SERIES[1]["color"])
axL.text(0.5, 0.03, f"GATE A2, four bursts over 219 s: {GATE_A2:.1f} \u00b5s",
         transform=axL.transAxes, fontsize=7, ha="center", va="bottom",
         color=S.LIMIT["color"])
axL.set_xlabel("Elapsed time (h)")
axL.set_ylabel("Inter-camera exposure offset (\u00b5s)")
axL.set_xlim(0, el[-1])
axL.set_ylim(med - 6 * rob, med + 6 * rob)

# --------------------------------------------------------------- right panel
for mask, lab, st in ((~hw, "Software", S.SERIES[0]), (hw, "Line2", S.SERIES[2])):
    axR.plot(el[mask], jit[mask], linestyle="none", marker=st["marker"],
             ms=3.2, mfc="white", mew=0.8, color=st["color"],
             label=f"{lab}, median {np.median(jit[mask]):.1f} \u00b5s")
axR.axhline(float(np.median(jit[~hw])), color=S.SERIES[0]["color"], ls=":", lw=0.9)
axR.axhline(float(np.median(jit[hw])), color=S.SERIES[2]["color"], ls=":", lw=0.9)
axR.text(0.03, 0.16, "the two ranges do not overlap\nin 84 bursts over 14 h",
         transform=axR.transAxes, fontsize=7, va="center",
         color=S.SERIES[1]["color"])
axR.set_xlabel("Elapsed time (h)")
axR.set_ylabel("Within-burst skew dispersion (\u00b5s)")
axR.set_xlim(0, el[-1])
axR.set_ylim(0, max(jit) * 1.35)
axR.legend(loc="upper right", fontsize=7)

fig.tight_layout()
S.save(fig, out)

# ------------------------------------------------------------------ read-out
A = np.column_stack([np.ones_like(el), el * 3600.0, hw.astype(float)])
coef, *_ = np.linalg.lstsq(A, skew, rcond=None)
print(f"bursts            : {len(rows)}  "
      f"({int((~hw).sum())} software, {int(hw.sum())} line2)")
print(f"baseline          : {el[-1]:.2f} h")
print(f"skew drift        : {coef[1]:.4f} ppm, "
      f"{(skew[-1] - skew[0]) / 1000:.1f} ms end to end")
print(f"offset, triplets  : {med:+.2f} +/- {sem:.2f} us, robust {rob:.2f}")
print(f"offset, GATE A2   : {GATE_A2:+.1f} us  -> agreement "
      f"{abs(med - GATE_A2):.1f} us")
for lab, v in (("software", jit[~hw]), ("line2", jit[hw])):
    print(f"jitter {lab:9s}: median {np.median(v):.2f}  "
          f"range {v.min():.2f} to {v.max():.2f} us")
print(f"jitter ratio      : {np.median(jit[~hw]) / np.median(jit[hw]):.3f}")
thirds = [np.median(dv[(dt_h >= a) & (dt_h < b)])
          for a, b in ((0, el[-1] / 3), (el[-1] / 3, 2 * el[-1] / 3),
                       (2 * el[-1] / 3, el[-1] + 1))]
print("offset by third   : " + ", ".join(f"{x:+.2f}" for x in thirds) + " us")