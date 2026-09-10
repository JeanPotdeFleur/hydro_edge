#!/usr/bin/env python3
"""D2 - Days of autonomy without solar input, against daily burst count.

Model plot, no measurement. Built on the dual-state power profile of Section
1.4: 25 W while acquiring, 5 W at idle, against 600 Wh of usable battery.

One curve per burst duration, because the answer depends on it and Section 1.4
does not say so. That section models one hour of acquisition per burst, which
matches none of the three strategies: Strategy 3 runs 40 minutes and Strategy 1
runs 17. Its three named profiles are plotted as markers so the discrepancy is
visible rather than buried, and so the figures quoted in the text can still be
located on the plot.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

P_ACTIVE, P_IDLE = 25.0, 5.0        # W, Section 1.4
BATTERY_WH = 600.0                  # 12 V 50 Ah LiFePO4, nominal usable
BURST_H = {"Strategy 1 (17 min)": 1024/3600,
           "Strategy 3 (40 min)": 2400/3600,
           "Strategy 2 (1.5 h)":  5400/3600}
NAMED = [(0, "standby"), (2, "standard"), (8, "storm")]   # Section 1.4, 1 h/burst

def autonomy(n, hours):
    t = np.minimum(n * hours, 24.0)
    return BATTERY_WH / (P_ACTIVE * t + P_IDLE * (24.0 - t))

S.setup()
fig, ax = plt.subplots(figsize=(S.FULL_W, S.FULL_W * 0.52))
n = np.linspace(0, 8, 400)

for (label, h), st in zip(BURST_H.items(), S.SERIES):
    ax.plot(n, autonomy(n, h), label=label, color=st["color"], ls=st["ls"])

# The three profiles named in Section 1.4, on its own one-hour convention.
for k, name in NAMED:
    a = autonomy(k, 1.0)
    ax.plot([k], [a], marker="x", ms=6, mew=1.4, color=S.LIMIT["color"],
            clip_on=False)
    ax.annotate(f"{name}\n{a:.2f} d", xy=(k, a), xytext=(4, 6),
                textcoords="offset points", fontsize=7, color=S.LIMIT["color"])

ax.plot([], [], marker="x", ls="none", color=S.LIMIT["color"],
        label="Section 1.4 profiles (1 h/burst)")

ax.set_xlabel("Bursts per day")
ax.set_ylabel("Autonomy without solar input (days)")
ax.set_xlim(0, 8)
ax.set_ylim(0, 5.6)
ax.legend(loc="upper right")
S.save(fig, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "out", "res_autonomy_vs_schedule.png"))

print(f"  retained plan, Strategy 3 at 2 bursts/day: "
      f"{autonomy(2, BURST_H['Strategy 3 (40 min)']):.2f} days")
for k, name in NAMED:
    print(f"  Section 1.4 {name:9s} ({k} bursts, 1 h each): {autonomy(k,1.0):.2f} days")