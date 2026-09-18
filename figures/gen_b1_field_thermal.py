    #!/usr/bin/env python3
"""B1 field - Die, drive and ambient temperature through a sealed-enclosure burst.

The 5400 s burst of 17 September on the Agassiz roof, in full sun, followed by
thirty minutes of cooling with the station idle. Every thermal figure until now
came from an open bench; this is the first in the enclosure that was actually
deployed.

The cooling half is what makes the figure an endurance rather than a lower
bound. Ninety minutes need not reach equilibrium in a sealed box, so the rise
alone would only say that the die had not yet exceeded a threshold. The decay
after shutdown gives the thermal time constant by a second and independent
route, and a rise that is flat over the last third while the decay confirms the
same constant is an equilibrium, not a coincidence.

The asymptote of the decay is fitted rather than assumed. Ambient was entered
by hand, the probe specified for the gate never having been delivered, so it
carries an unknown offset; letting the fit find its own asymptote keeps that
offset out of the time constant. The fit is a grid search on the asymptote with
a linear least squares on the logarithm at each step, which needs nothing
beyond numpy.

Drive temperatures come from SMART and are sampled every thirty seconds, not
every second: a SMART read briefly interrupts the drive command queue, and that
is the suspected mechanism behind the 0.409 ms cadence excursion of GATE A1.
They are drawn as steps, which is what they are.
"""
import os, sys, csv
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib.pyplot as plt
import fig_style as S

S.setup()

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "docs", "b1_field_telemetry_20260917.csv")
OUT  = os.path.join(HERE, "out", "res_b1_thermal_sealed.png")

DIE_LIMIT   = 85.0       # BCM2712 soft throttle
DRIVE_LIMIT = 70.0       # criterion of the gate, read on the SMART channel


def col(rows, name, cast=float):
    out = []
    for r in rows:
        v = r.get(name, "").strip()
        out.append(cast(v) if v else np.nan)
    return np.array(out, dtype=float)


with open(SRC) as f:
    rows = list(csv.DictReader(f))

t     = col(rows, "elapsed_s")
die   = col(rows, "soc_c")
sda   = col(rows, "sda_c")
sdb   = col(rows, "sdb_c")
amb   = col(rows, "ambient_c")
run   = col(rows, "acq_running")

# The burst window is where the acquisition process was alive. Nothing needs to
# be timed by hand: the column falls to zero at the exact sample the binary
# exited, which is also the start of the cooling curve.
active = np.where(run > 0.5)[0]
t_end  = t[active[-1]] if len(active) else t[-1]
t_beg  = t[active[0]]  if len(active) else t[0]


def decay_fit(tt, yy):
    """Return (T_inf, tau, fitted) for y = T_inf + A exp(-t/tau).

    Grid search on the asymptote because it is the parameter the hand-entered
    ambient cannot be trusted to supply; tau then follows from a straight line
    through log(y - T_inf).
    """
    best = None
    for tinf in np.arange(np.nanmin(yy) - 15.0, np.nanmin(yy), 0.25):
        d = yy - tinf
        if np.any(d <= 0):
            continue
        k, b = np.polyfit(tt, np.log(d), 1)
        if k >= 0:
            continue
        resid = np.sum((np.log(d) - (k * tt + b)) ** 2)
        if best is None or resid < best[0]:
            best = (resid, tinf, -1.0 / k, np.exp(b) * np.exp(k * tt) + tinf)
    return (None, None, None) if best is None else best[1:]


# The fit is only drawn when there is a transient to fit. On 17 September the
# die never departed from its idle value by more than a couple of degrees, so
# an exponential through the decay would be reading noise: a time constant
# extracted from a two-degree fall is not a thermal property of the enclosure.
m = (t >= t_end) & np.isfinite(die)
tinf = tau = fit = None
AMPLITUDE_FOR_FIT = 5.0
decay = (np.nanmedian(die[(t > t_end) & (t < t_end + 120)])
         - np.nanmedian(die[t > t[-1] - 120])) if m.sum() > 30 else 0.0
if m.sum() > 30 and decay >= AMPLITUDE_FOR_FIT:
    tinf, tau, fit = decay_fit(t[m] - t_end, die[m])
else:
    print(f"no fit: the die fell {decay:.1f} C after the burst, below the "
          f"{AMPLITUDE_FOR_FIT:.0f} C an exponential needs to mean anything")

fig, ax = plt.subplots(figsize=(S.FULL_W, 8.5 * S.CM))

ax.axvspan(t_beg / 60.0, t_end / 60.0, color="#000000", alpha=0.05, lw=0)
ax.text((t_beg + t_end) / 120.0, 15.5, "acquisition",
        ha="center", va="bottom", fontsize=8, color="#767676")

ax.plot(t / 60.0, die, label="SoC die", **{k: v for k, v in S.SERIES[0].items()
                                           if k != "marker"})
ax.step(t / 60.0, sda, where="post", label="SSD 0 (SMART)",
        **{k: v for k, v in S.SERIES[1].items() if k != "marker"})
ax.step(t / 60.0, sdb, where="post", label="SSD 1 (SMART)",
        **{k: v for k, v in S.SERIES[2].items() if k != "marker"})
if np.isfinite(amb).any():
    ax.plot(t / 60.0, amb, label="ambient (entered by hand)",
            **{k: v for k, v in S.SERIES[3].items() if k != "marker"})

if fit is not None:
    ax.plot((t[m]) / 60.0, fit, color=S.LIMIT["color"], ls="-", lw=1.0,
            label=f"decay fit, $\\tau$ = {tau/60.0:.0f} min")

ax.axhline(DIE_LIMIT, **S.LIMIT)
ax.text(1.0, DIE_LIMIT + 1.5, "die throttle, 85 °C",
        ha="left", va="bottom", fontsize=8, color=S.LIMIT["color"])
ax.axhline(DRIVE_LIMIT, **S.LIMIT)
ax.text(1.0, DRIVE_LIMIT + 1.5, "drive criterion, 70 °C",
        ha="left", va="bottom", fontsize=8, color=S.LIMIT["color"])

ax.set_xlabel("Time from start of monitoring (min)")
ax.set_ylabel("Temperature (°C)")
ax.set_ylim(14, DIE_LIMIT + 22)
ax.set_xlim(0, t[-1] / 60.0)
ax.legend(loc="upper center", ncol=3, bbox_to_anchor=(0.5, 1.0))

os.makedirs(os.path.dirname(OUT), exist_ok=True)
S.save(fig, OUT)

# Medians, not peaks: at 1 Hz the die reading carries a degree of sample
# noise, and a single high sample is not a temperature the enclosure reached.
first = (t >= t_beg) & (t < t_beg + 300)
last  = (t > t_end - 300) & (t <= t_end)
print(f"die median over the first five minutes {np.nanmedian(die[first]):.1f} C")
print(f"die median over the last five minutes  {np.nanmedian(die[last]):.1f} C")
print(f"die peak sample {np.nanmax(die[t <= t_end]):.1f} C, "
      f"headroom {DIE_LIMIT - np.nanmax(die[t <= t_end]):.1f} C")
print(f"drive peak {np.nanmax([np.nanmax(sda), np.nanmax(sdb)]):.0f} C, "
      f"criterion {DRIVE_LIMIT:.0f} C")
if tau:
    print(f"cooling time constant {tau:.0f} s, asymptote {tinf:.1f} C")