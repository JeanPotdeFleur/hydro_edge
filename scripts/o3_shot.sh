#!/bin/bash
# o3_shot.sh - one synchronised frame from both heads, with a preview.
#
#   ./o3_shot.sh <label>
#
# For GATE O3, where a target is carried to a series of known positions and
# each position needs one image from each camera at the same instant. The
# hardware trigger on Line 2 fires both sensors from the same pulse, so the
# pair is simultaneous to the skew measured in GATE A2 and the ordinal of the
# frame is the same on both: no timestamp arithmetic is needed to pair them.
#
# Two seconds are acquired rather than one frame. The binary has no single
# shot mode, and four triggers cost eight frames of disk against the certainty
# of having at least one clean one if somebody moves at the wrong moment.
#
# The label is not decoration. Twenty positions produce twenty burst
# directories named by the clock, and by tomorrow nobody will remember which
# was the rock at 80 m and which the one at 160. It is written into a register
# beside the data at the moment of capture, which is the only moment it is
# known for certain.

set -u

LABEL=${1:?usage: o3_shot.sh <label>, for instance  rock-80m-east}
ROOT=${O3_ROOT:-/mnt/vault2/gate_o3}
DUR=${O3_DURATION:-2}
PREV=${PREV_PATH:-$HOME/hydro_edge/focus.jpg}
BIN=${HYDRO_BIN:-$HOME/hydro_edge/build/hydro_edge}
REG="$ROOT/positions.csv"

[ -x "$BIN" ] || { echo "[FATAL] $BIN is not executable." >&2; exit 1; }
. /etc/default/hydro-edge
mkdir -p "$ROOT"
[ -f "$REG" ] || echo "utc,label,burst,exposure_us,trigger" > "$REG"

# A slot in flight holds both sensors, and so does cam_focus. Say so plainly
# rather than letting Spinnaker fail with an access error.
if pgrep -f 'build/hydro_edge --output' >/dev/null || pgrep -f cam_focus >/dev/null; then
    echo "[FATAL] a burst or cam_focus is holding the cameras. Stop it first." >&2
    exit 1
fi

echo "[O3] $LABEL: acquiring ${DUR} s on both heads"
"$BIN" --output "$ROOT" --duration "$DUR" \
       --cam0-serial "$HYDRO_CAM0" --cam1-serial "$HYDRO_CAM1" \
       --exposure-us "${O3_EXPOSURE_US:-$HYDRO_EXPOSURE_US}" --gain-db 0 \
       --trigger "$HYDRO_TRIGGER" > "$ROOT/.last_shot.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    echo "[FATAL] the binary exited $rc. Last lines:" >&2
    tail -12 "$ROOT/.last_shot.log" >&2
    exit $rc
fi

BURST=$(grep -m1 '^\[BURST\]' "$ROOT/.last_shot.log" | awk '{print $2}')
echo "$(date -u +%FT%TZ),$LABEL,$(basename "$BURST"),${O3_EXPOSURE_US:-$HYDRO_EXPOSURE_US},$HYDRO_TRIGGER" >> "$REG"

PREV="$PREV" BURST="$BURST" LABEL="$LABEL" python3 - << 'PY'
import glob, os, cv2, numpy as np

W, H, STEP = 5320, 3032, 2
burst, prev, label = os.environ["BURST"], os.environ["PREV"], os.environ["LABEL"]


def panel(role):
    """Last complete frame of a role, subsampled on one Bayer position.

    No demosaic: a checkerboard is black and white, and a single Bayer plane
    shows it as well as three interpolated ones for a tenth of the cost. Half
    resolution keeps the squares large enough to judge at a hundred metres.
    """
    d = glob.glob(os.path.join(burst, f"{role}_*"))
    if not d:
        return None
    raws = sorted(glob.glob(os.path.join(d[0], "*.raw")))
    for p in reversed(raws):
        if os.path.getsize(p) == W * H:
            m = np.memmap(p, np.uint8, "r", shape=(H, W))
            img = np.ascontiguousarray(m[::STEP, ::STEP])
            del m
            cv2.rectangle(img, (0, 0), (W // STEP, 40), 0, cv2.FILLED)
            cv2.putText(img, f"{label}   {role}   {os.path.basename(p)}",
                        (12, 29), cv2.FONT_HERSHEY_SIMPLEX, 0.7, 255, 2)
            return img
    return None


a, b = panel("cam0"), panel("cam1")
if a is None or b is None:
    raise SystemExit("[O3] no complete frame found in " + burst)
sep = np.full((H // STEP, 6), 255, np.uint8)
ok, buf = cv2.imencode(".jpg", np.hstack([a, sep, b]),
                       [cv2.IMWRITE_JPEG_QUALITY, 88])
if ok:
    with open(prev + ".tmp", "wb") as f:
        f.write(buf.tobytes())
    os.replace(prev + ".tmp", prev)
    print(f"[O3] preview -> {prev}")
PY

grep -E '^\[SUMMARY\] [0-9]+ triggers' "$ROOT/.last_shot.log"
echo "[O3] $LABEL recorded in $REG"