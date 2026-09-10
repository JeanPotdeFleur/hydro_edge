#!/usr/bin/env python3
"""A4 - Inter-sensor skew, software against hardware triggering (GATE A2).

Four alternating 60 s bursts of 2 September, 120 frames each, run without an
intervening power cycle so that the drift of the two sensor oscillators is
common to all of them.

The difference between the two device timestamps carries two terms: the real
inter-camera exposure offset, and the difference between two free-running
sensor oscillators drifting against one another at about 1.4 ppm. In a single
burst the second dominates by three orders of magnitude and the first cannot
be separated from it. The alternating design is what makes the separation
possible: the drift is common to all four bursts and the mode is not, so a
fit of the form skew = a + r*t + d*HW recovers both.

Left panel, the spread. Each burst is centred on its own median, which removes
the drift and leaves the frame-to-frame jitter of the trigger. Right panel,
the offset. The four medians are shown against the drift fitted on the two
software bursts alone; the two hardware bursts sit above it by a constant, and
that constant is the exposure offset the software trigger introduces by
issuing two sequential USB transactions.
"""
import os, sys, csv, glob, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "docs", "gate_a2")

runs = []
for path in sorted(glob.glob(os.path.join(SRC, "*_timing.csv"))):
    name = os.path.basename(path)
    mode = "line2" if "_line2_" in name else "software"
    sk = []
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                sk.append(float(row["dev_skew_us"]))
            except (KeyError, ValueError):
                pass
    if len(sk) >= 100:                      # the 20 s trial run is excluded
        runs.append({"stamp": name[:20], "mode": mode, "skew": np.array(sk)})

if not runs:
    sys.exit(f"no timing data under {SRC}")

for r in runs:
    r["med"] = np.median(r["skew"])
    r["res"] = r["skew"] - r["med"]
    r["sd"]  = r["skew"].std(ddof=0)

S.setup()
fig, (axL, axR) = plt.subplots(1, 2, figsize=(S.FULL_W, S.FULL_W * 0.42),
                               gridspec_kw={"width_ratios": [1.15, 1]})

# Left: the residual distributions, which is the comparison.
groups, labels, colors = [], [], []
for mode, col in (("software", S.SERIES[0]["color"]),
                  ("line2", S.SERIES[2]["color"])):
    sel = [r for r in runs if r["mode"] == mode]
    for k, r in enumerate(sel, 1):
        groups.append(r["res"])
        labels.append(f"{'SW' if mode=='software' else 'HW'}{k}\n{r['sd']:.1f}")
        colors.append(col)

bp = axL.boxplot(groups, labels=labels, widths=0.55, showfliers=True,
                 flierprops={"marker": ".", "ms": 2, "mfc": "#8c8c8c",
                             "mec": "none"},
                 medianprops={"color": "#000000", "lw": 1.0})
for patch, col in zip(bp["boxes"], colors):
    patch.set_color(col)
for elem in ("whiskers", "caps"):
    for k, ln in enumerate(bp[elem]):
        ln.set_color(colors[k // 2])
axL.set_ylabel("Skew residual about median (\u00b5s)", labelpad=1)
axL.set_xlabel("Burst, in order of execution, with $\\sigma$ (\u00b5s)")
axL.axhline(0, color="#b0b0b0", lw=0.6, zorder=0)

sw = [r["sd"] for r in runs if r["mode"] == "software"]
hw = [r["sd"] for r in runs if r["mode"] == "line2"]
if sw and hw:
    axL.annotate(f"software {np.mean(sw):.1f} \u00b5s vs hardware "
                 f"{np.mean(hw):.1f} \u00b5s\nratio {np.mean(sw)/np.mean(hw):.2f}",
                 xy=(0.03, 0.03), xycoords="axes fraction",
                 fontsize=7.5, color="#4d4d4d")

# Right: the medians against elapsed time, with drift and mode separated by a
# joint least-squares fit. Elapsed time comes from the burst directory names,
# which are the UTC start instants.
import datetime as _dt
t0 = None
for r in runs:
    d = _dt.datetime.strptime(r["stamp"], "%Y-%m-%dT%H-%M-%SZ")
    t0 = d if t0 is None else t0
    r["t"] = (d - t0).total_seconds()

tt  = np.array([r["t"] for r in runs])
mm  = np.array([r["med"] / 1000.0 for r in runs])
hwf = np.array([1.0 if r["mode"] == "line2" else 0.0 for r in runs])
A   = np.column_stack([np.ones_like(tt), tt, hwf])
coef, *_ = np.linalg.lstsq(A, mm, rcond=None)
a, rate, delta = coef
resid = mm - A @ coef

tl = np.linspace(tt.min() - 10, tt.max() + 10, 100)
axR.plot(tl, a + rate * tl, color="#a6a6a6", ls=":", lw=1.0,
         label=f"Fitted drift, {rate*1e3:.2f} \u00b5s/s")
axR.plot(tl, a + rate * tl + delta, color="#a6a6a6", ls="--", lw=1.0)
for mode, col, mk, lab in (("software", S.SERIES[0]["color"], "o", "Software trigger"),
                           ("line2", S.SERIES[2]["color"], "s", "Hardware, Line2")):
    xs = [r["t"] for r in runs if r["mode"] == mode]
    ys = [r["med"] / 1000.0 for r in runs if r["mode"] == mode]
    axR.plot(xs, ys, ls="none", marker=mk, ms=5, color=col, label=lab)

axR.annotate("", xy=(tt[1], a + rate * tt[1] + delta),
             xytext=(tt[1], a + rate * tt[1]),
             arrowprops={"arrowstyle": "<->", "lw": 0.9, "color": "#8c1d18"})
axR.annotate(f"{delta*1e3:+.0f} \u00b5s", xy=(tt[1], a + rate * tt[1] + delta / 2),
             xytext=(6, 0), textcoords="offset points", va="center",
             fontsize=8, color="#8c1d18")
axR.annotate(f"fit residual $\\leq$ {np.abs(resid).max()*1e3:.1f} \u00b5s",
             xy=(0.03, 0.03), xycoords="axes fraction",
             fontsize=7, color="#4d4d4d")
axR.set_xlabel("Time since first burst (s)")
axR.set_ylabel("Median inter-sensor skew (ms)")
axR.legend(loc="upper left", fontsize=7)

fig.subplots_adjust(wspace=0.34)
S.save(fig, os.path.join(HERE, "out", "res_gate_a2_skew_comparison.png"))

for i, r in enumerate(runs, 1):
    print(f"  {i} {r['mode']:9s} {r['stamp']}  median {r['med']/1000:.3f} ms  sd {r['sd']:.1f} us")
if sw and hw:
    print(f"  software mean sd {np.mean(sw):.1f} us | hardware {np.mean(hw):.1f} us"
          f" | ratio {np.mean(sw)/np.mean(hw):.2f}")
print(f"  common drift {rate*1e3:.3f} us/s ({rate*1e3:.3f} ppm)")
print(f"  mode offset  {delta*1e3:+.1f} us   <- inter-camera exposure offset "
      f"removed by hardware triggering")
print(f"  fit residual {np.abs(resid).max()*1e3:.2f} us max on {len(runs)} bursts")