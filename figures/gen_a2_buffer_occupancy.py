#!/usr/bin/env python3
"""A2 - Ring buffer occupancy over GATE A1.

Read from the heartbeat lines of the burst log, which report occupancy and
running peak once every 122 triggers, that is once a minute. Eighty-nine
samples over 5400 s: coarse, and it is what the run recorded.

The figure exists to show that the buffer never left its floor. Occupancy is
zero at every sample and the running peak reaches one slot of sixty, so the
margin is a factor of sixty on a quantity that was sized by calculation.

What it cannot show, and this is the reason GATE A3 was later specified, is
whether the buffer absorbs. An idle drive and an open bench give it no
occasion to fill, so the absence of overflow here is evidence about the drive
rather than about the buffer.
"""
import os, sys, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "docs", "gate_a1_burst.log")
PAT  = re.compile(r"index (\d+)/\d+, written \d+, buffer (\d+)/(\d+) \(peak (\d+)\)")

idx, occ, peak = [], [], []
cap = 60
with open(SRC) as f:
    for line in f:
        m = PAT.search(line)
        if m:
            idx.append(int(m.group(1)))
            occ.append(int(m.group(2)))
            cap = int(m.group(3))
            peak.append(int(m.group(4)))
t = np.array(idx) * 0.5          # trigger ordinal at 2 Hz
occ, peak = np.array(occ), np.array(peak)

S.setup()
fig, ax = plt.subplots(figsize=(S.HALF_W * 1.6, S.HALF_W * 0.95))
ax.step(t, peak, where="post", color=S.SERIES[0]["color"], ls="-",
        label="Running peak occupancy")
ax.step(t, occ, where="post", color=S.SERIES[2]["color"], ls="--",
        label="Instantaneous occupancy")
ax.axhline(cap, **S.LIMIT, label=f"Ring buffer capacity, {cap} slots")
ax.set_xlabel("Elapsed time (s)")
ax.set_ylabel("Occupancy (slots)")
ax.set_xlim(0, 5400)
ax.set_ylim(0, cap * 1.08)
ax.legend(loc="center right", fontsize=7)
ax.annotate(f"peak {peak.max()} of {cap} slots\nmargin \u00d7{cap/max(peak.max(),1):.0f}",
            xy=(0.03, 0.10), xycoords="axes fraction",
            fontsize=7.5, color="#4d4d4d")
S.save(fig, os.path.join(HERE, "out", "res_gate_a1_buffer_occupancy.png"))
print(f"  {len(t)} heartbeats, occupancy max {occ.max()}, running peak {peak.max()}/{cap}")