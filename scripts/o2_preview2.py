#!/usr/bin/env python3
"""Live side-by-side preview of both cameras during an acquisition burst.

    ./o2_preview2.py [root] [--out focus.jpg] [--period 1.0]

The acquisition binary holds both sensors exclusively, so nothing else can open
a camera. This reads instead the frames it has already written and composes the
most recent complete one from each role into a single image, so that neither
head is aimed blind.

Three things keep the cost off the acquisition path. The file is memory-mapped
and sampled one pixel in four on both axes, so roughly a quarter of the pages
are faulted in rather than the whole sixteen megabytes. No demosaic is done: a
single Bayer position is sampled, which is monochrome but perfectly adequate to
see where a black and white board sits in the frame. And the composite is
written to a temporary and renamed, which is atomic within one filesystem, so
the viewer never opens a half-written file.

A frame is used only when its size is exactly one full payload. A short file is
still being written, and decoding it would show a torn image that reads as a
missed pose.
"""

import argparse
import glob
import os
import time

import cv2
import numpy as np

WIDTH, HEIGHT = 5320, 3032
EXPECT = WIDTH * HEIGHT
STEP = 4


def newest_complete(root, role):
    dirs = sorted(glob.glob(os.path.join(root, "*", f"{role}_*")),
                  key=lambda p: os.path.getmtime(p), reverse=True)
    if not dirs:
        return None
    raws = sorted(glob.glob(os.path.join(dirs[0], "*.raw")), reverse=True)
    for p in raws[:4]:
        try:
            if os.path.getsize(p) == EXPECT:
                return p
        except OSError:
            continue
    return None


def panel(path, label):
    h, w = HEIGHT // STEP, WIDTH // STEP
    if path is None:
        img = np.zeros((h, w), np.uint8)
        cv2.putText(img, f"{label}: no frame yet", (20, h // 2),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.0, 255, 2)
        return img
    m = np.memmap(path, dtype=np.uint8, mode="r", shape=(HEIGHT, WIDTH))
    img = np.ascontiguousarray(m[::STEP, ::STEP])
    del m
    text = f"{label}  {os.path.basename(path)}  mean {img.mean():.0f} DN"
    cv2.rectangle(img, (0, 0), (w, 34), 0, cv2.FILLED)
    cv2.putText(img, text, (10, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.6, 255, 1)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?",
                    default="/mnt/vault2/calib_o2/intrinsics")
    ap.add_argument("--out", default=os.path.expanduser("~/hydro_edge/focus.jpg"))
    ap.add_argument("--period", type=float, default=1.0)
    args = ap.parse_args()

    tmp = args.out + ".tmp"
    sep = np.full((HEIGHT // STEP, 4), 255, np.uint8)
    print(f"watching {args.root}   ->   {args.out}   (Ctrl-C to stop)")

    while True:
        try:
            left = panel(newest_complete(args.root, "cam0"), "cam0")
            right = panel(newest_complete(args.root, "cam1"), "cam1")
            # imencode rather than imwrite: imwrite picks its encoder from the
            # file extension, and the temporary does not end in .jpg.
            ok, buf = cv2.imencode(".jpg", np.hstack([left, sep, right]),
                                   [cv2.IMWRITE_JPEG_QUALITY, 80])
            if ok:
                with open(tmp, "wb") as f:
                    f.write(buf.tobytes())
                os.replace(tmp, args.out)
        except Exception as e:                     # a burst rotating underneath
            print(f"[PREVIEW] {type(e).__name__}: {e}")
        time.sleep(args.period)


if __name__ == "__main__":
    main()