#!/usr/bin/env python3
"""B1 field against GATE A1 - the same load on an open bench and on the roof.

Both runs are 5400 s of Strategy 2 at 64.5 MB/s, the same binary, the same two
sensors. What differs is everything around them: A1 ran on 25 August on an open
bench indoors with an empty drive, B1 on 17 September inside a sealed enclosure
in full sun on the mast, with the drives a few hundred gigabytes in.

The pairing is what gives the figure its value. A single field measurement says
the die reached some temperature; a paired one says what the enclosure and the
sun cost over a bench, which is the number that transfers to a hotter day or to
a site without shade.

The two campaigns were sampled at different rates, five seconds for A1 and one
second for B1, so both are reduced to a running median over one minute before
being drawn. The time axis is the elapsed time of each burst, aligned at its
own start rather than at a wall clock, since the question is how each behaves
under load and not what hour of the day it was.
"""
import os, sys, csv
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

S.setup()

HERE = os.path.dirname(os.path.abspath(__file__))
A1   = os.path.join(HERE, "..", "docs", "gate_a1_monitor.csv")
B1   = os.path.join(HERE, "..", "docs", "b1_field_telemetry_20260917.csv")
OUT  = os.path.join(HERE, "out", "res_b1_vs_a1.png")

NOMINAL_MBPS = 64.5
DIE_LIMIT    = 85.0


def running_med(x, w):
    out = np.empty_like(x)
    for i in range(len(x)):
        seg = x[max(0, i - w // 2):i + w // 2 + 1]
        seg = seg[np.isfinite(seg)]
        out[i] = np.median(seg) if len(seg) else np.nan
    return out


def load_a1():
    t, temp, mbps = [], [], []
    with open(A1) as f:
        for r in csv.DictReader(f):
            t.append(float(r["elapsed_s"]))
            temp.append(float(r["soc_temp_c"]))
            mbps.append(float(r["dev_wmbps"]))
    return (np.array(t), np.array(temp), np.array(mbps), 12)   # 12 x 5 s


def load_b1():
    t, temp, mbps, run = [], [], [], []
    with open(B1) as f:
        for r in csv.DictReader(f):
            def g(k):
                v = r.get(k, "").strip()
                return float(v) if v else np.nan
            t.append(g("elapsed_s"))
            temp.append(g("soc_c"))
            mbps.append(np.nansum([g("sda_wr_mbs"), g("sdb_wr_mbs")]))
            run.append(g("acq_running"))
    t, temp, mbps, run = map(np.array, (t, temp, mbps, run))
    keep = run > 0.5                      # the burst only, to match A1
    t = t[keep] - t[keep][0]
    return (t, temp[keep], mbps[keep], 60)                     # 60 x 1 s


fig, (ax, ax2) = plt.subplots(
    2, 1, figsize=(S.FULL_W, 11.0 * S.CM), sharex=True,
    gridspec_kw={"height_ratios": [1, 1], "hspace": 0.12})

for i, (name, loader) in enumerate([("GATE A1, open bench, 25 August", load_a1),
                                    ("B1 field, sealed, in sun, 17 September", load_b1)]):
    t, temp, mbps, w = loader()
    st = {k: v for k, v in S.SERIES[i * 2].items() if k != "marker"}
    ax.plot(t / 60.0, running_med(temp, w), label=name, **st)
    ax2.plot(t / 60.0, running_med(mbps, w), label=name, **st)
    print(f"{name}: die median {np.nanmedian(temp):.1f} C, "
          f"peak {np.nanmax(temp):.1f} C, "
          f"throughput median {np.nanmedian(mbps):.1f} MB/s")

ax.axhline(DIE_LIMIT, **S.LIMIT)
ax.text(1, DIE_LIMIT + 2.5, "die throttle, 85 °C", fontsize=8,
        color=S.LIMIT["color"], va="bottom")
ax.set_ylabel("SoC die temperature (°C)")
ax.set_ylim(0, DIE_LIMIT + 10)
ax.legend(loc="lower right")

ax2.axhline(NOMINAL_MBPS, **S.LIMIT)
# Above the traces rather than across them: both campaigns run between 40 and
# 90 MB/s and the band over 100 is empty.
ax2.text(1, 122, "demand, 64.5 MB/s", fontsize=8, color=S.LIMIT["color"],
         va="bottom")
ax2.set_ylabel("Write throughput (MB/s)")
ax2.set_xlabel("Elapsed time within the burst (min)")
ax2.set_ylim(0, 140)
ax2.legend(loc="lower right")

os.makedirs(os.path.dirname(OUT), exist_ok=True)
S.save(fig, OUT)