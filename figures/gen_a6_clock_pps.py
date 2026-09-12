#!/usr/bin/env python3
"""A6 - The system clock against the GNSS timepulse.

The kernel stamps every PPS assert on CLOCK_REALTIME, and the true edge falls
on an exact second, so the fractional part of that timestamp is the offset
between the system clock and the reference. The measurement needs no hardware
and no daemon: it is already there, in one sysfs file.

Left, the offset over 17.8 h. What it shows is not the sawtooth of step
corrections one expects from an SNTP client but a slow wander: systemd-timesyncd
slews the frequency instead of stepping the phase, so the rate is disciplined
while the phase is left several milliseconds out and drifting.

Right, the residual once the local trend is removed, which isolates the
repeatability of the PPS capture itself from the wander of the clock it is
measured against. The shaded region is the period during which the station was
acquiring, and the difference across its edge is the cost of interrupt load on
that repeatability.

Usage:
    ./gen_a6_clock_pps.py [../docs/a6_pps_offset.csv] [../docs/a2_stability.csv]
"""
import os, sys, csv, datetime as dt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

HERE = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "docs",
                                                         "a6_pps_offset.csv")
soak = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "docs",
                                                          "a2_stability.csv")
out = os.path.join(HERE, "out", "res_a6_clock_pps.png")
WIN = 30          # samples per detrending window, 30 x 10 s = five minutes

def parse(ts):
    return dt.datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ")

with open(src) as f:
    rows = [r for r in csv.DictReader(f) if r.get("offset_us")]
if len(rows) < 2 * WIN:
    sys.exit(f"too few samples in {src}")

t = [parse(r["utc"]) for r in rows]
el = np.array([(x - t[0]).total_seconds() / 3600.0 for x in t])
sec = np.array([(x - t[0]).total_seconds() for x in t])
off = np.array([float(r["offset_us"]) for r in rows])
cnt = np.array([int(r["pulse_count"]) for r in rows])

# The acquisition window, taken from the soak summary when it is at hand so the
# figure never carries a hand-entered timestamp.
load_end = None
if os.path.exists(soak):
    with open(soak) as f:
        ends = [parse(r["started_utc"]) for r in csv.DictReader(f)]
    if ends:
        load_end = (max(ends) - t[0]).total_seconds() / 3600.0 + 120 / 3600.0

# --------------------------------------------------- residual, local detrend
res = np.full(len(off), np.nan)
for i in range(0, len(sec) - WIN, WIN):
    a, b = i, i + WIN
    p = np.polyfit(sec[a:b], off[a:b], 1)
    res[a:b] = off[a:b] - np.polyval(p, sec[a:b])
ok = ~np.isnan(res)

def robust(x):
    m = np.median(x)
    return 1.4826 * np.median(np.abs(x - m))

if load_end is not None:
    during = ok & (el < load_end)
    idle = ok & (el >= load_end)
else:
    during, idle = ok, np.zeros_like(ok)

S.setup()
fig, (axL, axR) = plt.subplots(1, 2, figsize=(S.FULL_W, 0.42 * S.FULL_W))

# ---------------------------------------------------------------- left panel
axL.plot(el, off / 1000.0, color=S.SERIES[0]["color"], lw=0.9)
axL.axhline(0.0, **S.LIMIT)
axL.text(0.5, 0.10, "exact UTC second", transform=axL.transAxes, fontsize=7,
         ha="center", color=S.LIMIT["color"])
axL.text(0.97, 0.95,
         f"no step correction in {el[-1]:.1f} h\n"
         f"peak to peak {(off.max() - off.min()) / 1000:.2f} ms\n"
         f"median absolute {np.median(np.abs(off)) / 1000:.2f} ms",
         transform=axL.transAxes, fontsize=7, ha="right", va="top",
         color=S.SERIES[1]["color"])
axL.set_xlabel("Elapsed time (h)")
axL.set_ylabel("Clock ahead of PPS edge (ms)")
axL.set_xlim(0, el[-1])
axL.set_ylim(-0.4, max(6.0, off.max() / 1000 * 1.35))

# --------------------------------------------------------------- right panel
if load_end is not None:
    axR.axvspan(0, load_end, color=S.SERIES[3]["color"], alpha=0.30, lw=0)
axR.plot(el[ok], res[ok], linestyle="none", marker=".", ms=1.6,
         color=S.SERIES[0]["color"])
axR.set_xlabel("Elapsed time (h)")
axR.set_ylabel("Residual about local trend (\u00b5s)")
axR.set_xlim(0, el[-1])
# Headroom above the cloud so the two labels never sit on the data.
lim = max(12.0, 6 * robust(res[ok]))
axR.set_ylim(-lim, 1.55 * lim)
if load_end is not None:
    axR.text(load_end / 2, 1.44 * lim,
             f"acquiring, {robust(res[during]):.2f} \u00b5s", fontsize=7,
             ha="center", va="top", color=S.SERIES[1]["color"])
    axR.text((load_end + el[-1]) / 2, 1.44 * lim,
             f"idle, {robust(res[idle]):.2f} \u00b5s", fontsize=7,
             ha="center", va="top", color=S.SERIES[1]["color"])

fig.tight_layout()
S.save(fig, out)

# ------------------------------------------------------------------ read-out
slopes = np.array([np.polyfit(sec[i:i + 60], off[i:i + 60], 1)[0]
                   for i in range(0, len(sec) - 60, 60)])
print(f"samples           : {len(rows)} over {el[-1]:.2f} h, "
      f"step {np.median(np.diff(sec)):.0f} s, gaps > 30 s: "
      f"{int((np.diff(sec) > 30).sum())}")
print(f"timepulse         : {cnt[-1] - cnt[0]} pulses in {sec[-1]:.0f} s "
      f"-> {(cnt[-1] - cnt[0]) / sec[-1]:.4f} Hz")
print(f"offset            : {off.min() / 1000:.2f} to {off.max() / 1000:.2f} ms, "
      f"median absolute {np.median(np.abs(off)) / 1000:.2f} ms")
print(f"mean rate error   : {(off[-1] - off[0]) / sec[-1]:+.4f} ppm")
print(f"local rate, 10 min: median {np.median(slopes):+.3f} ppm, "
      f"range {slopes.min():+.3f} to {slopes.max():+.3f}, sd {slopes.std():.3f}")
print(f"steps > 200 us    : "
      f"{int((np.abs(np.diff(off)) > 200).sum())}")
print(f"PPS capture       : robust {robust(res[ok]):.2f} us overall")
if load_end is not None:
    print(f"                    {robust(res[during]):.2f} us acquiring, "
          f"{robust(res[idle]):.2f} us idle, "
          f"factor {robust(res[during]) / robust(res[idle]):.1f}")
    for lab, m in (("acquiring", during), ("idle", idle)):
        print(f"  {lab:9s} |res| > 20 us: "
              f"{100 * (np.abs(res[m]) > 20).mean():.2f} per cent of "
              f"{int(m.sum())} samples, max {np.abs(res[m]).max():.1f} us")