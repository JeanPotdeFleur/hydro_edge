#!/bin/bash
# o2_pair_aim.sh - live side-by-side preview of both heads, for aiming them at
# one target before the paired f/8 comparison.
#
#   ./o2_pair_aim.sh <serial_cam0> <serial_cam1> [preview_width]
#
# Launches cam_focus with both serials. Nothing is recorded, nothing is written
# except the preview image. stdin stays attached so the typed commands work.
#
# Why both heads must be boresighted rather than merely pointed the same way:
# the ROI is a single rectangle in FocusConfig, applied to the same image
# coordinates in both cameras. Two heads pointed loosely at one facade measure
# two different patches of it, and the variance of a Laplacian is far more
# sensitive to what is in the patch than to the focus of the lens reading it.

set -u

SN0=${1:?usage: o2_pair_aim.sh <serial_cam0> <serial_cam1> [preview_width]}
SN1=${2:?second serial required}
PW=${3:-960}

FOCUS=${FOCUS_BIN:-$HOME/hydro_edge/build/cam_focus}
# /dev/shm is tmpfs. Two writes a second for an hour is some 1.4 GB, which does
# not belong on the card carrying the root filesystem.
PREV=${PREV_PATH:-$HOME/hydro_edge/focus.jpg}

[ -x "$FOCUS" ] || { echo "[FATAL] $FOCUS is not executable." >&2; exit 1; }

cat << TXT

  Open $PREV in the editor. Left panel is sn$SN0, right is sn$SN1;
  each carries its own serial, SHARP, SAT, DN and EXP in its overlay.

  Aim so that ONE feature of the facade sits in the centre box of BOTH
  panels. The centre box is the measured region and it is the same
  rectangle in both images, so a feature centred in one and off to the
  side in the other invalidates the comparison before it starts.

  Type, then press Enter:
      a          automatic exposure, for aiming
      m          lock exposure and gain where they are
      e <us>     set the exposure of BOTH heads, e.g.  e 4800
      g <db>     set the gain of both, keep it at  g 0
      r          reset the peak bar
      q          quit

  Once aimed: type  m,  then  e <us>  so both heads share one exposure,
  then read the two p50 values. If they disagree by more than a few DN
  the two rings are not at the same aperture and the sharpness figures
  cannot be compared.

TXT

exec "$FOCUS" --serial "$SN0" --serial "$SN1" \
              --exposure-auto --gain-db 0 --rate 2 \
              --preview-width "$PW" --out "$PREV"