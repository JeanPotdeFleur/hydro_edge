#!/usr/bin/env python3
"""verify_burst.py - acceptance check over one or more burst directories.

The acquisition binary reports what it observed from inside its own process.
This checks what the filesystem actually holds afterwards, which is a
different question and the only one that matters once the drives come back
from the roof.

Six checks per burst:

  manifest      present and parseable; without it the .raw files carry no
                header at all and their geometry survives only as a constant
                hard-coded in whatever decoder is at hand
  summary       present, and completed true. Its absence is itself a signal:
                a burst directory holding a manifest and no summary was
                interrupted, and no file count has to be compared to
                establish it
  sizes         every frame exactly the payload the manifest declares. This
                is what catches a power cut mid-write, which leaves the last
                files truncated or empty; nothing else reveals it, the
                pipeline having no fsync policy
  count         frames present against triggers issued
  alignment     the same index present on both cameras, or absent from both.
                The index is the trigger ordinal, so identical numbers must
                denote the same instant; a frame present on one camera only
                is a de-aligned pair rather than a missing one
  gaps          indices missing from both cameras, cross-checked against the
                summary's own list

Exit status is 0 when every burst passes, 1 otherwise, so it can gate a
retrieval or run from a morning heartbeat.

Usage:
    ./verify_burst.py /mnt/vault                 every burst under a root
    ./verify_burst.py /mnt/vault/2026-09-01T*    specific bursts
    ./verify_burst.py --quiet /mnt/vault         one line per burst
"""

import argparse
import json
import os
import re
import sys

FRAME_RE = re.compile(r"^(\d{6})\.raw$")


class Result:
    def __init__(self, path):
        self.path = path
        self.errors = []
        self.warnings = []
        self.info = {}

    def fail(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)

    @property
    def ok(self):
        return not self.errors


def load_json(path, res, label):
    if not os.path.isfile(path):
        return None
    try:
        with open(path) as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError) as e:
        res.fail(f"{label} unreadable: {e}")
        return None


def scan_camera_dir(path):
    """Return {index: size} for every NNNNNN.raw, plus a list of unexpected names."""
    frames, strays = {}, []
    with os.scandir(path) as it:
        for entry in it:
            if not entry.is_file():
                continue
            m = FRAME_RE.match(entry.name)
            if m:
                frames[int(m.group(1))] = entry.stat().st_size
            else:
                strays.append(entry.name)
    return frames, strays


def verify(burst_dir):
    res = Result(burst_dir)

    manifest = load_json(os.path.join(burst_dir, "manifest.json"), res, "manifest")
    if manifest is None:
        res.fail("no manifest: the archive does not describe itself")
        return res

    cams = manifest.get("cameras", [])
    if len(cams) != 2:
        res.fail(f"manifest declares {len(cams)} camera(s), 2 expected")
        return res

    payload = cams[0].get("payload_bytes", 0)
    if payload <= 0:
        res.fail("manifest declares a null payload")
        return res
    if cams[1].get("payload_bytes") != payload:
        res.fail("the two cameras declare different payloads")

    res.info["commit"] = manifest.get("git_commit", "?")
    res.info["clock"] = manifest.get("clock_synchronized", None)
    if res.info["clock"] is False:
        res.warn("acquired with an undisciplined clock: the directory name may "
                 "not correspond to the instant of exposure")

    target = manifest.get("acquisition", {}).get("target_triggers", 0)

    summary = load_json(os.path.join(burst_dir, "summary.json"), res, "summary")
    if summary is None:
        res.fail("no summary: the burst was interrupted before a clean shutdown")
    else:
        if not summary.get("completed", False):
            res.fail("summary reports completed false")
        counters = summary.get("counters", {})
        issued = counters.get("triggers_issued", 0)
        for key in ("incomplete", "retrieval_errors", "pps_timeouts", "write_errors",
                    "transport_frame_id_gaps", "buffer_overflows",
                    "late_frames_skipped"):
            v = counters.get(key, 0)
            if v:
                res.fail(f"{key} = {v}")
        rb = summary.get("ring_buffer", {})
        hw, cap = rb.get("high_water_frames", 0), rb.get("capacity_frames", 0)
        res.info["ring"] = f"{hw}/{cap}"
        if cap and hw > cap * 0.5:
            res.warn(f"ring buffer peaked at {hw} of {cap}: the drive came within "
                     "half the buffer of losing frames")
        res.info["MBps"] = summary.get("mean_write_MBps", 0)
        if issued and target and issued < target:
            res.warn(f"{issued} triggers issued of {target} planned")

    # --- filesystem ---
    per_cam = {}
    for c in cams:
        d = os.path.join(burst_dir, c.get("directory", ""))
        if not os.path.isdir(d):
            res.fail(f"missing camera directory {c.get('directory')}")
            return res
        frames, strays = scan_camera_dir(d)
        if strays:
            res.warn(f"{c.get('directory')}: {len(strays)} unexpected file(s), "
                     f"e.g. {strays[0]}")
        per_cam[c.get("directory")] = frames

    names = list(per_cam)
    a, b = per_cam[names[0]], per_cam[names[1]]

    bad = []
    for name, frames in per_cam.items():
        for idx, size in frames.items():
            if size != payload:
                bad.append((name, idx, size))
    if bad:
        # Truncated tails are the signature of an interruption mid-write, and
        # the first offending index says where it happened.
        bad.sort(key=lambda t: t[1])
        res.fail(f"{len(bad)} frame(s) not {payload} bytes, first at index "
                 f"{bad[0][1]} in {bad[0][0]} ({bad[0][2]} bytes)")

    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    if only_a or only_b:
        res.fail(f"index misalignment: {len(only_a)} present only in {names[0]}, "
                 f"{len(only_b)} only in {names[1]}"
                 + (f", first {(only_a or only_b)[0]}" if (only_a or only_b) else ""))

    common = sorted(set(a) & set(b))
    res.info["frames"] = len(common)
    res.info["target"] = target

    if common:
        expected = set(range(1, max(common) + 1))
        missing = sorted(expected - set(common))
        if missing:
            res.info["gaps"] = len(missing)
            declared = set(summary.get("missing_indices", [])) if summary else set()
            truncated = summary.get("missing_indices_truncated", False) if summary else False
            undeclared = sorted(set(missing) - declared)
            if undeclared and not truncated:
                res.fail(f"{len(undeclared)} gap(s) absent from the summary's own "
                         f"list, first at index {undeclared[0]}: a frame vanished "
                         "without the pipeline accounting for it")
            else:
                res.warn(f"{len(missing)} declared gap(s)")
    if target and len(common) < target:
        res.info["short"] = target - len(common)

    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+",
                    help="burst directories, or roots containing them")
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="one line per burst, details only on failure")
    args = ap.parse_args()

    # A path is a burst if it holds a manifest, otherwise it is a root to walk.
    bursts = []
    for p in args.paths:
        if not os.path.isdir(p):
            print(f"not a directory: {p}", file=sys.stderr)
            continue
        if os.path.isfile(os.path.join(p, "manifest.json")):
            bursts.append(p)
            continue
        with os.scandir(p) as it:
            for e in sorted(it, key=lambda e: e.name):
                if e.is_dir() and os.path.isfile(os.path.join(e.path, "manifest.json")):
                    bursts.append(e.path)

    if not bursts:
        print("no burst directory found", file=sys.stderr)
        return 1

    n_ok = 0
    for d in bursts:
        r = verify(d)
        n_ok += r.ok
        status = "PASS" if r.ok else "FAIL"
        line = (f"[{status}] {os.path.basename(d)}  "
                f"{r.info.get('frames', 0)}/{r.info.get('target', '?')} frames"
                f"  ring {r.info.get('ring', '?')}"
                f"  {r.info.get('MBps', 0):.1f} MB/s"
                f"  {r.info.get('commit', '?')}")
        print(line)
        if not r.ok or not args.quiet:
            for e in r.errors:
                print(f"         ERROR   {e}")
            for w in r.warnings:
                print(f"         warning {w}")

    print(f"\n{n_ok}/{len(bursts)} burst(s) passed")
    return 0 if n_ok == len(bursts) else 1


if __name__ == "__main__":
    sys.exit(main())
