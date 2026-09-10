"""Shared figure style for the Stanford camera system report.

Conventions imposed by the report and wired into the LaTeX, so they are set
once here rather than repeated in every script:

  PNG at 300 dpi, 16 cm full width and 7.5 cm half width, no title inside the
  figure since the caption is written in LaTeX, axis labels in English with
  units, and a palette that survives conversion to greyscale.

The last constraint is the one that shapes the choices below. Colour alone is
never allowed to carry meaning: every series is distinguished by line style or
marker as well, so a reader looking at a monochrome print loses nothing.
"""

import matplotlib
matplotlib.use("Agg")          # headless node, no display server, never a window
import matplotlib.pyplot as plt

CM = 1.0 / 2.54
FULL_W = 16.0 * CM             # 6.30 in
HALF_W = 7.5 * CM              # 2.95 in

# Distinguishable in greyscale: luminance is monotonic across the sequence and
# each entry carries its own dash pattern.
SERIES = [
    {"color": "#1a1a1a", "ls": "-",   "marker": "o"},
    {"color": "#4d4d4d", "ls": "--",  "marker": "s"},
    {"color": "#767676", "ls": "-.",  "marker": "^"},
    {"color": "#a6a6a6", "ls": ":",   "marker": "D"},
]

# Thresholds and limits are drawn the same way everywhere so the eye learns
# them once: thin, dashed, mid grey, always annotated.
LIMIT = {"color": "#8c1d18", "ls": "--", "lw": 1.0}


def setup():
    plt.rcParams.update({
        "font.size":        9,
        "axes.labelsize":   9,
        "axes.titlesize":   9,
        "xtick.labelsize":  8,
        "ytick.labelsize":  8,
        "legend.fontsize":  8,
        "figure.dpi":       300,
        "savefig.dpi":      300,
        "savefig.bbox":     "tight",
        "savefig.pad_inches": 0.02,
        "axes.grid":        True,
        "grid.alpha":       0.25,
        "grid.linewidth":   0.5,
        "axes.spines.top":  False,
        "axes.spines.right": False,
        "lines.linewidth":  1.2,
        "legend.frameon":   False,
    })


def save(fig, path):
    fig.savefig(path)
    plt.close(fig)
    print(f"wrote {path}")