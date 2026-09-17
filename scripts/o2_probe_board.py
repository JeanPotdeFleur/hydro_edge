#!/usr/bin/env python3
"""Find the checkerboard in recorded frames, and find its size unaided.

    ./o2_probe_board.py <frame.raw> [more.raw ...] [--out annotated.jpg]

Answers three questions before forty minutes are spent on a sweep that might
detect nothing: is a board visible at all, what patternSize does the detector
want, and are the corners where the eye says they are.

The demosaic is the one decode.cpp uses, COLOR_BayerBG2BGR against a sensor
reporting BayerRG. The convention is kept deliberately: any residual half-pixel
phase is then common to the calibration and to the science data, and cancels.

Sizes are tried from the largest down and the search stops at the first hit. A
grid of 5x5 inner corners also contains 3x3 sub-grids, so an ascending search
would report a sub-grid and be wrong; a descending search that stops early also
costs a handful of attempts instead of thirty.

Detection runs at quarter resolution, where a ten-inch square at twenty metres
is still twenty-five pixels, far above what the detector needs, and one attempt
costs well under a second instead of two and a half. Sub-pixel refinement at
full resolution belongs to the calibration itself, not to this probe.
"""

import argparse
import os
import sys

import cv2
import numpy as np

WIDTH, HEIGHT = 5320, 3032
EXPECT = WIDTH * HEIGHT


def load(path, scale):
    size = os.path.getsize(path)
    if size != EXPECT:
        sys.exit(f"[FATAL] {path} is {size} B, expected {EXPECT} "
                 f"({WIDTH}x{HEIGHT}). Wrong file, or still being written.")
    bayer = np.fromfile(path, dtype=np.uint8).reshape(HEIGHT, WIDTH)
    bgr = cv2.cvtColor(bayer, cv2.COLOR_BayerBG2BGR)
    grey = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    small = cv2.resize(grey, (WIDTH // scale, HEIGHT // scale),
                       interpolation=cv2.INTER_AREA)
    return small, bgr


def find(grey, cols, rows):
    if hasattr(cv2, "findChessboardCornersSB"):
        ok, c = cv2.findChessboardCornersSB(grey, (cols, rows),
                                            flags=cv2.CALIB_CB_EXHAUSTIVE)
        return c if ok else None
    ok, c = cv2.findChessboardCorners(
        grey, (cols, rows),
        flags=cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE)
    return c if ok else None


def candidates(lo, hi):
    out = [(c, r) for c in range(lo, hi + 1) for r in range(lo, c + 1)]
    out.sort(key=lambda t: (t[0] * t[1], t[0]), reverse=True)
    return out


def probe(path, scale, lo, hi, out_path):
    grey, bgr = load(path, scale)
    clipped = 100.0 * (grey >= 255).sum() / grey.size
    print(f"\n[PROBE] {os.path.basename(path)}   {grey.shape[1]}x{grey.shape[0]}"
          f"   mean {grey.mean():.0f} DN   max {grey.max()}"
          f"   clipped {clipped:.2f}%")
    if clipped > 0.5:
        print("[PROBE] clipped whites put the corner where the signal stops "
              "rising, not where the edge is. Shorten the exposure.")

    for cols, rows in candidates(lo, hi):
        c = find(grey, cols, rows)
        if c is None:
            continue
        print(f"[PROBE] detected patternSize ({cols},{rows})"
              f"   -> a board of {cols+1} x {rows+1} squares"
              f"   -> {cols*rows} inner corners")
        if out_path:
            vis = cv2.resize(bgr, (grey.shape[1], grey.shape[0]),
                             interpolation=cv2.INTER_AREA)
            cv2.drawChessboardCorners(vis, (cols, rows), c, True)
            cv2.imwrite(out_path, vis, [cv2.IMWRITE_JPEG_QUALITY, 90])
            print(f"[PROBE] wrote {out_path}")
        return (cols, rows)

    print("[PROBE] nothing detected. In order of likelihood: the board is not "
          "wholly inside the frame, there is no clear white margin around the "
          "pattern, the whites are clipped, or the board is too small here.")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raws", nargs="+")
    ap.add_argument("--out", default=None, help="annotated JPEG of the first hit")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--min", type=int, default=3, dest="lo")
    ap.add_argument("--max", type=int, default=9, dest="hi")
    args = ap.parse_args()

    found = [probe(p, args.scale, args.lo, args.hi, args.out if i == 0 else None)
             for i, p in enumerate(args.raws)]

    hits = [f for f in found if f]
    print()
    if not hits:
        sys.exit(1)
    if len(set(hits)) > 1:
        print(f"[PROBE] the size is NOT stable across frames: {sorted(set(hits))}."
              " The outermost row is lost on some views, which usually means too "
              "little white margin. Fix the board before the sweep.")
        sys.exit(1)

    cols, rows = hits[0]
    net = 2 * cols * rows - 6
    print(f"[PROBE] use patternSize ({cols},{rows}) on {len(hits)}/{len(found)} "
          f"frames, {cols*rows} inner corners per view.")
    print(f"[PROBE] each view contributes {net} equations net of its own six "
          f"pose parameters, so plan on about {max(30, int(1500 / net))} "
          f"retained views per camera.")


if __name__ == "__main__":
    main()