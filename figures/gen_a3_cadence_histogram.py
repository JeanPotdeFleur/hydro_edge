#!/usr/bin/env python3
"""A3 - Distribution of inter-frame intervals over GATE A1.

10 799 intervals from the 5400 s endurance run, on a logarithmic count axis
because the distribution spans four orders of magnitude and a linear axis shows
only the core.

The aggregate standard deviation of 0.179 ms, quoted throughout the report, is
a contaminated statistic and this figure is the reason to stop quoting it
alone. The interquartile range is 1 to 3 microseconds and 98.8 per cent of
intervals fall within 20 microseconds of their mode; the robust estimator
1.4826 x MAD gives 1.5 microseconds. The 0.179 ms is produced entirely by 114
intervals, one per cent of the record.

Those 114 are not scattered. They occur as 58 adjacent pairs, and in 97 per
cent of pairs the two deviations have opposite signs: a single trigger departs
late, which lengthens the interval before it and shortens the one after by the
same amount. Median displacement 1.5 ms, worst case 3.0 ms, one event every
93 s. The perturbation is absorbed within one frame and never accumulates,
which is the property the absolute-instant anchoring of Stage 3 was introduced
to obtain and this is its evidence.

The distribution is also bimodal by construction. Frame A is fired on the PPS
edge and frame B at the instant anchored 500 ms later, so the two half-periods
separate by 0.092 ms. That separation is a fixed offset, not jitter.
"""
import os, sys, csv
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "docs", "gate_a1_timing_5400s.csv")

idx, iv = [], []
with open(SRC) as f:
    for row in csv.DictReader(f):
        v = float(row["interval_ms"])
        if v > 0:
            idx.append(int(row["index"]))
            iv.append(v)
idx, iv = np.array(idx), np.array(iv)
ab, ba = iv[idx % 2 == 0], iv[idx % 2 == 1]
med = {0: np.median(ab), 1: np.median(ba)}
dev = np.array([v - med[i % 2] for i, v in zip(idx, iv)])
robust = 1.4826 * np.median(np.abs(dev - np.median(dev)))
within = 100.0 * np.mean(np.abs(dev) <= 0.02)

S.setup()
fig, (axL, axR) = plt.subplots(1, 2, figsize=(S.FULL_W, S.FULL_W * 0.40),
                               gridspec_kw={"width_ratios": [1, 1.25]})

# Left: the core, linear, fine bins. The two half-periods separate by 0.092 ms
# and each is narrower than a single microsecond bin would resolve.
binsL = np.arange(499.90, 500.10 + 1e-9, 0.002)
axL.hist(iv, bins=binsL, color="#b8b8b8", edgecolor="none")
axL.axvline(med[1], color=S.SERIES[2]["color"], ls="--", lw=1.0)
axL.axvline(med[0], color=S.SERIES[0]["color"], ls="-", lw=1.0)
axL.axvline(500.0, **S.LIMIT)
axL.annotate(f"B\u2192A\n{med[1]:.3f}", xy=(med[1], 4600), xytext=(-4, 0),
             textcoords="offset points", ha="right", va="center",
             fontsize=7.5, color=S.SERIES[2]["color"])
axL.annotate(f"A\u2192B\n{med[0]:.3f}", xy=(med[0], 4600), xytext=(4, 0),
             textcoords="offset points", ha="left", va="center",
             fontsize=7.5, color=S.SERIES[0]["color"])
axL.set_xlabel("Frame interval (ms)")
axL.set_ylabel("Count")
axL.set_xlim(499.90, 500.10)
axL.set_ylim(0, 6200)
axL.set_xticks([499.90, 499.95, 500.00, 500.05, 500.10])
axL.tick_params(axis="x", labelrotation=30)

# Right: the whole range on a log count axis, where the 114 outliers that set
# the aggregate standard deviation are the only thing to see.
binsR = np.arange(496.9, 503.1 + 1e-9, 0.02)
axR.hist(iv, bins=binsR, color="#b8b8b8", edgecolor="none")
axR.axvline(500.0, **S.LIMIT)
axR.set_yscale("log")
axR.set_xlabel("Frame interval (ms)")
axR.set_ylabel("Count (log scale)")
axR.set_xlim(496.9, 503.1)
axR.set_ylim(0.7, 3e4)

axR.annotate(f"robust $\\sigma$ = {robust*1000:.1f} \u00b5s, {within:.1f} % within "
             f"\u00b120 \u00b5s\naggregate $\\sigma$ = {iv.std(ddof=0):.3f} ms, set by "
             f"58 isolated\nevents, each corrected on the next frame",
             xy=(0.02, 0.96), xycoords="axes fraction",
             ha="left", va="top", fontsize=7, color="#4d4d4d")

fig.subplots_adjust(wspace=0.32)
S.save(fig, os.path.join(HERE, "out", "res_gate_a1_cadence_histogram.png"))
print(f"  n={len(iv)}  aggregate sd={iv.std(ddof=0):.4f} ms  robust sigma={robust*1000:.2f} us")
print(f"  within +/-20 us of mode: {within:.2f} %")