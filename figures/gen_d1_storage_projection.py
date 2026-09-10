#!/usr/bin/env python3
"""D1 - Cumulative storage occupancy against days of operation.

Model plot, no measurement. It converts the burst volumes of Section 1.6 into
the quantity the maintenance interval is actually set by: how long the vault
lasts before it must be swapped.

The usable ceiling is the two 8 TB drives after formatting and after the
superuser reserve is cleared, which the filesystem reports as 7.3 TiB each.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

# Section 1.6, Table 9. Volumes are per burst, both sensors, at 64.52 MB/s.
BURST_GB = {"Strategy 1": 66.07, "Strategy 2": 348.41, "Strategy 3": 154.85}

# Free space actually reported by the filesystem across the two drives, which
# is the figure the service interval must be set against: 6.9 TiB on the vault
# holding the GATE A1 archive and 7.3 TiB on the second drive.
CAPACITY_TB = (6.9 + 7.3) * 1.0995116  # TiB -> TB

SCENARIOS = [
    ("Strategy 3, 2 bursts/day", BURST_GB["Strategy 3"] * 2),
    ("Strategy 3, 3 bursts/day", BURST_GB["Strategy 3"] * 3),
    ("Strategy 2, 1 burst/day",  BURST_GB["Strategy 2"] * 1),
    ("Strategy 1, 2.5 bursts/day", BURST_GB["Strategy 1"] * 2.5),
]

S.setup()
fig, ax = plt.subplots(figsize=(S.FULL_W, S.FULL_W * 0.52))
days = np.arange(0, 91)

for (label, gb_day), st in zip(SCENARIOS, S.SERIES):
    ax.plot(days, days * gb_day / 1000.0, label=label,
            color=st["color"], ls=st["ls"])
    full = CAPACITY_TB * 1000.0 / gb_day
    if full <= days[-1]:
        ax.plot([full], [CAPACITY_TB], marker=st["marker"], ms=4,
                color=st["color"], clip_on=False)
        ax.annotate(f"{full:.0f} d", xy=(full, CAPACITY_TB),
                    xytext=(0, -13 - 9 * (SCENARIOS.index((label, gb_day)) % 2)),
                    textcoords="offset points", ha="center",
                    fontsize=7, color=st["color"])

ax.axhline(CAPACITY_TB, **S.LIMIT)
ax.annotate(f"usable vault capacity, {CAPACITY_TB:.1f} TB",
            xy=(89, CAPACITY_TB), xytext=(-2, 4), textcoords="offset points",
            ha="right", fontsize=8, color=S.LIMIT["color"])

# The maintenance interval the architecture is designed around.
ax.axvspan(28, 42, color="#000000", alpha=0.05, lw=0)
ax.annotate("target service\ninterval, 4-6 weeks", xy=(35, 1.0),
            ha="center", fontsize=7, color="#4d4d4d")

ax.set_xlabel("Days of operation")
ax.set_ylabel("Cumulative archive volume (TB)")
ax.set_xlim(0, 90)
ax.set_ylim(0, CAPACITY_TB * 1.25)
ax.legend(loc="lower right")
S.save(fig, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "out", "res_storage_projection.png"))

for label, gb_day in SCENARIOS:
    print(f"  {label:28s} {gb_day:7.1f} GB/day -> "
          f"{CAPACITY_TB*1000/gb_day:5.1f} days to full")