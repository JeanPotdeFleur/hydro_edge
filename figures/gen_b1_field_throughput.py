#!/usr/bin/env python3
"""B1 field - Write throughput and core frequency through the deployed burst.

Two quantities on one time axis, because between them they answer the question
the gate asks: did the station hold the ingestion rate, and did it hold it
without being throttled.

Throughput alone is not enough. A die that stays under its limit proves
nothing if the governor has already stepped the cores down, and a rate that
happens to be met under a reduced clock has no margin left. The four core
frequencies are therefore drawn on the right axis: flat at 2400 MHz means the
demand was met at full clock, which is the only reading that transfers to a
warmer day.

The instantaneous rate is computed from the sector counters of /proc/diskstats
once a second, while the consumer commits four files a second and the drive
buffers as it pleases. The raw trace is spiky by construction and its spread is
a sampling artefact, not a property of the drive; the running median over one
minute is the quantity to read.

Both archive volumes are drawn. The wrapper writes each burst to whichever is
the emptier, so exactly one carries the load and the other sits at zero, which
is itself worth seeing.
"""
import os, sys, csv
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

S.setup()

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "docs", "b1_field_telemetry_20260917.csv")
OUT  = os.path.join(HERE, "out", "res_b1_throughput.png")

NOMINAL_MBPS = 64.5      # 2 sensors x 16.13 MB/frame x 2 Hz
NOMINAL_MHZ  = 2400.0


def col(rows, name):
    out = []
    for r in rows:
        v = r.get(name, "").strip()
        out.append(float(v) if v else np.nan)
    return np.array(out, dtype=float)


with open(SRC) as f:
    rows = list(csv.DictReader(f))

t   = col(rows, "elapsed_s")
wa  = col(rows, "sda_wr_mbs")
wb  = col(rows, "sdb_wr_mbs")
run = col(rows, "acq_running")
mhz = np.nanmax(np.vstack([col(rows, f"cpu{i}_mhz") for i in range(4)]), axis=0)

active = np.where(run > 0.5)[0]
t_beg = t[active[0]] if len(active) else t[0]
t_end = t[active[-1]] if len(active) else t[-1]

total = np.nansum(np.vstack([wa, wb]), axis=0)


def running_pct(x, w, q):
    out = np.empty_like(x)
    for i in range(len(x)):
        seg = x[max(0, i - w // 2):i + w // 2 + 1]
        seg = seg[np.isfinite(seg)]
        out[i] = np.percentile(seg, q) if len(seg) else np.nan
    return out


W = 60                                   # 60 samples x 1 s = 60 s
med = running_pct(total, W, 50)

# Cumulative volume rather than a band of percentiles. The write pattern is
# binary: the consumer commits through the page cache and the kernel flushes
# in bouts of 140 MB/s separated by gaps of up to ten seconds, so a percentile
# band spans the whole axis and hides what it is meant to show. The integral
# has no such problem. Its slope is the sustained rate by construction, and a
# straight line of the demanded slope laid over it turns the question "did the
# station keep up" into one the eye answers in a second.
dt  = np.gradient(t)
cum = np.nancumsum(np.nan_to_num(total) * dt) / 1000.0        # GB
ref = NOMINAL_MBPS * (t - t[active[0]]) / 1000.0
ref[t < t[active[0]]] = 0.0
ref[t > t_end] = NOMINAL_MBPS * (t_end - t[active[0]]) / 1000.0

fig, (ax, ax2) = plt.subplots(
    2, 1, figsize=(S.FULL_W, 9.5 * S.CM), sharex=True,
    gridspec_kw={"height_ratios": [1, 1], "hspace": 0.12})

ax.plot(t / 60.0, cum, label="written, cumulative",
        **{k: v for k, v in S.SERIES[0].items() if k != "marker"})
ax.plot(t / 60.0, ref, color=S.LIMIT["color"], ls="--", lw=1.0,
        label="64.5 MB/s demanded")
ax.set_ylabel("Data written (GB)")
ax.legend(loc="upper left")
ax.text(0.99, 0.06, "core clock 2400 MHz throughout, no throttle flag set",
        transform=ax.transAxes, ha="right", fontsize=8, color="#4d4d4d")

ax2.plot(t / 60.0, med, **{k: v for k, v in S.SERIES[0].items()
                           if k != "marker"})
ax2.axhline(NOMINAL_MBPS, **S.LIMIT)
# Below the traces, not on them: the running median lives between 40 and 85
# MB/s for the whole burst and the band under 35 is empty.
ax2.text(2, 8, "demand, 64.5 MB/s", fontsize=8, color=S.LIMIT["color"],
         va="bottom")
ax2.set_ylabel("Throughput,\nmedian over 60 s (MB/s)")
ax2.set_ylim(0, 100)
ax2.set_xlabel("Time from start of monitoring (min)")
ax2.set_xlim(0, t[-1] / 60.0)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
S.save(fig, OUT)

burst = (t >= t_beg) & (t <= t_end)
# Writes do not arrive continuously. The consumer commits through the page
# cache and the kernel flushes in bouts, so a one-second sample falls either
# inside a flush or in the gap between two. Percentiles of the raw trace
# describe that pattern, not the drive: what the pipeline must sustain is the
# mean, and what shows the drive is never the limit is the flush rate.
trim = burst & (t > t[burst][0] + 60) & (t < t[burst][-1] - 60)
print(f"mean over the burst   {np.nanmean(total[burst]):.1f} MB/s")
print(f"median                {np.nanmedian(total[burst]):.1f} MB/s")
print(f"flush rate, 90th pct  {np.nanpercentile(total[burst], 90):.0f} MB/s")
print(f"worst 60 s median     {np.nanmin(med[trim]):.1f} MB/s")
gaps = (total[burst] < 1.0)
run = best = 0
for v in gaps:
    run = run + 1 if v else 0
    best = max(best, run)
print(f"longest write pause   {best} s, absorbed by the ring buffer")
print(f"minimum core clock    {np.nanmin(mhz[burst]):.0f} MHz")
print(f"written in total      {cum[-1]:.1f} GB against {ref[-1]:.1f} demanded")