# HydroEdge station — operator guide

Agassiz rooftop, Hopkins Marine Station. Written 17 September 2026, repository
at commit `b925333`.

This is for someone who needs to check on the station, run a burst by hand, or
retrieve the drives. It is not a description of how the system works inside;
Appendix J of the monitoring report does that.

---

## 0. Getting in

Two separate things, and they are easy to confuse. The **repository** is the
code, and it lives on GitHub and on your own laptop. The **station** is the
Raspberry Pi on the roof, which holds its own copy of that code plus all the
data. Reading the code needs no access to the Pi; operating the station needs
no access to GitHub.

### 0.1 The repository, on your own machine

Public, no account or permission needed:

```bash
git clone https://github.com/JeanPotdeFleur/hydro_edge.git
cd hydro_edge
```

Then `code .` opens it in VS Code, or use *File → Open Folder*. To refresh
later, `git pull` inside the directory.

What is where:

| | |
|---|---|
| `main.cpp`, `Session.h`, `RingBuffer.h` | the acquisition binary |
| `scripts/` | the wrapper, the scheduler helpers, the diagnostics |
| `deploy/systemd/` | the service and timer units |
| `deploy/default/hydro-edge` | the settings template |
| `tools/` | `decode`, `cam_focus`, `cam_probe` |
| `docs/` | measurement data from every validation campaign |
| `figures/` | the scripts that draw the report figures from `docs/` |

The monitoring report, which explains the design decisions and the validation
campaigns, is **not** in the repository. Ask Max for it.

### 0.2 The station, over SSH

Install the **Remote - SSH** extension in VS Code. Then `Ctrl+Shift+P`,
*Remote-SSH: Connect to Host*, and enter:

```
bakerlab@hopkins1-pi5
```

A new window opens. Use *File → Open Folder* on `/home/bakerlab/hydro_edge`,
and open a terminal with `Ctrl+` backtick.

**Everything in that window is on the Pi.** The file explorer, the editor and
the terminal all act on the station, not on your laptop. A file you open there
is the Pi's file; a command you type there runs on the Pi. This catches people
out: `focus.jpg` in the explorer is the image the station just wrote, and it
refreshes by itself while a camera tool is running.

Plain `ssh bakerlab@hopkins1-pi5` from a terminal works just as well if you
prefer it; VS Code only adds the file browser.

### 0.3 The network, which decides how you connect

**As of 17 September 2026 the station has no working network.** Its Wi-Fi chip
sees only the two strongest access points in the neighbourhood, and none of the
four `Stanford` ones, which sit at −74 dBm or below. The chip is fine and the
regulatory domain has been fixed; the antenna is simply printed on the Pi's
board, under a PCIe HAT, between two SSDs, inside a metal enclosure. A USB
Wi-Fi adapter with an external antenna outside the vault is the remedy.

Until that is fitted, the only way in is to go to the roof and connect an
Ethernet cable directly between the laptop and the Pi. The hostname
`hopkins1-pi5` resolves over that link.

Once the adapter is in place, the Pi will take an address on the Stanford
network and you will be able to reach it from anywhere on campus.

While there is no network, three things do not work: the clock is not
disciplined, the morning status page is not published, and the daily plan
cannot be changed remotely. The station still acquires on its default schedule.

---

## 1. What the station is

Two Teledyne FLIR Blackfly S cameras, 16.1 MP global shutter, on a rail on the
Agassiz roof, 15 m above the water, looking north-north-east. A Raspberry Pi 5
in a sealed enclosure drives them, writing raw Bayer frames at 2 Hz to two
8 TB SSDs.

One burst is 40 minutes and 155 GB. Two bursts a day, at 10:00 and 17:00 local.
That is 310 GB a day, and fifty days across the two drives — so the drives are
retrieved every four to six weeks.

Nothing is compressed and nothing is deleted. A frame on disk is exactly what
the sensor read.

---

## 2. The one command that tells you everything

```bash
~/hydro_edge/scripts/station_status.sh
```

It prints, in one screen: UTC time, uptime, whether the clock is disciplined,
the CPU governor, the PPS pulse count, the die temperature, free space on both
archive volumes, the throttle flags, the timers and when they next fire, the
four most recent bursts with the volume each landed on, the verdict of the
acceptance script, and the count of journal anomalies over 24 hours.

Read it in this order:

- **`clock: yes`** — if this says `no`, no automatic burst will run at all.
- **`throttle: throttled=0x0`** — anything else means thermal or power trouble.
- **`vault:`** two lines — if only one, a drive is unmounted and the slots will
  not start.
- **`last bursts:`** — should show bursts from both volumes, alternating.
- **`verdict:`** — every burst should read `[PASS]`.

---

## 3. Seeing what the cameras see

```bash
( . /etc/default/hydro-edge && ~/hydro_edge/scripts/o2_pair_aim.sh "$HYDRO_CAM0" "$HYDRO_CAM1" )
```

Then open `~/hydro_edge/focus.jpg`. Left panel is camera `24260192`, right is
`24260193`. Each carries its own sharpness, saturation, digital-number
percentiles, exposure and gain.

Typed commands, each followed by Enter: `a` automatic exposure, `m` lock,
`e 6000` set the exposure of **both** heads in microseconds, `g 0` set the gain,
`r` reset the peak bar, `q` quit.

**This holds both cameras exclusively.** No burst can run while it is open.
Always quit with `q`.

---

## 4. Running a burst by hand

```bash
( . /etc/default/hydro-edge && ~/hydro_edge/build/hydro_edge \
    --output /mnt/vault2/manual --duration 600 \
    --cam0-serial "$HYDRO_CAM0" --cam1-serial "$HYDRO_CAM1" \
    --exposure-us "$HYDRO_EXPOSURE_US" --gain-db 0 --trigger "$HYDRO_TRIGGER" )
```

`--duration` is in seconds. `Ctrl-C` once stops it gracefully: the ring buffer
is drained and `summary.json` written. A second `Ctrl-C` would cut that short.

At the end, read the `[SUMMARY]` block. Triggers, pushed and written must be
equal, and every loss counter must be zero.

Output goes to `<root>/<UTC>/cam<role>_<serial>/NNNNNN.raw`, one file per frame,
with `manifest.json` at the start and `summary.json` at the end. The frame index
is the trigger ordinal, never a write counter: a loss leaves a hole so that the
same number means the same instant on both cameras.

---

## 5. The schedule

Three timers exist; only two should be armed.

```bash
systemctl list-timers 'hydro-*' --no-pager
sudo systemctl enable --now hydro-burst-1000.timer hydro-burst-1700.timer
sudo systemctl disable --now hydro-burst-1300.timer
```

Each timer starts `hydro-slot@<slot>.service`, which runs `run_burst.sh`. That
wrapper reads the day's plan, picks the archive volume with more free space,
and launches the binary. To exercise the whole chain while watching it:

```bash
sudo systemctl start hydro-slot@1000.service
journalctl -u hydro-slot@1000.service -f
```

Two more timers run the housekeeping: `hydro-morning` at 08:00 takes a snapshot
and publishes the status page, `hydro-plan` at 09:50 re-reads the day's plan.
Both need a network.

Settings live in `/etc/default/hydro-edge`: camera serials, exposure, gain,
trigger source, and the two archive roots. Edit with `sudo nano`; the units read
the file at each start, so no `daemon-reload` is needed.

---

## 6. Checking the data

```bash
~/hydro_edge/scripts/verify_burst.py /mnt/vault /mnt/vault2
```

It reads the filesystem rather than trusting what the binary reported, and
checks every burst against its own manifest. Add `--quiet` for one line each.

To look at a frame:

```bash
~/hydro_edge/build/decode --jpg --scale 4 --out /tmp <some>.raw     # a preview
~/hydro_edge/build/decode --stats-only <some>.raw                   # levels only
```

`--stats-only` gives the median, the percentiles and the clipping per Bayer
channel. Clipped whites in the foam are the one defect that cannot be repaired
afterwards: a saturated patch has no gradient, and gradient is what the
velocimetry correlates.

---

## 7. Retrieving the drives

Every four to six weeks.

The wrapper writes each burst to whichever volume is the emptier, so the two
**alternate burst by burst**. Each drive therefore holds every other burst of
the interval, and a retrieval takes **both** drives, not one.

```bash
sudo systemctl stop hydro-burst-1000.timer hydro-burst-1700.timer
~/hydro_edge/scripts/verify_burst.py /mnt/vault /mnt/vault2   # before copying
sudo umount /mnt/vault /mnt/vault2
```

Swap the drives, then **remount `/mnt/vault` on the new drive**. This is not
optional: `hydro-slot@.service` carries `RequiresMountsFor=/mnt/vault`, so with
that mount absent no slot starts at all, even if the second volume is mounted
and half empty.

```bash
mount | grep /mnt/vault
sudo systemctl start hydro-burst-1000.timer hydro-burst-1700.timer
```

Replace the desiccant in the optical housings at the same visit. It is a
consumable: once saturated the protection stops and nothing announces it.

---

## 8. What not to do

**Do not touch the pan of an individual camera head.** The angle between the
optical axes is set by detents on the wall mount and cannot be recovered on the
roof. Aim by rotating the whole rail on the mast and tilting it; both preserve
the relative geometry.

**Do not open the optical housings.** They are sealed with desiccant inside.
Opening one on the roof seals the marine layer in with it.

**Do not run `netplan apply` over SSH.** It reconfigures every interface,
including the one carrying your session.

**Do not run `apt upgrade`** without a reason. Several services are masked or
disabled deliberately.

**Do not delete a burst directory while the binary is writing to it.**

---

## 9. When something is wrong

**No burst ran.** Check `clock:` in the status output first. An undisciplined
clock makes the binary refuse every burst — deliberately, since an archive named
from a stale date is worse than a missed slot.

**A slot failed.** `journalctl -u hydro-slot@1700.service -n 50`

**A volume is missing.** `mount | grep vault`, then remount. No slot starts
without `/mnt/vault`.

**The die is hot.** `vcgencmd get_throttled` should be `0x0`. Measured on
17 September in the sealed enclosure in full sun: the die stayed near 44 °C
against a threshold of 85, and the drives between 27 and 33 °C against 70. A
reading far above those deserves attention.

**A camera is not found.** `~/hydro_edge/build/cam_probe` lists what is
attached. The binary refuses to start with fewer than two cameras enumerated.

---

## 10. Numbers worth knowing

| | |
|---|---|
| Frame | 5320 × 3032, Bayer RG8, 16.13 MB |
| Rate | 2 Hz, both cameras, 64.5 MB/s sustained |
| Burst | 40 min, 155 GB |
| Daily | two bursts, 310 GB |
| Capacity | 15.6 TB usable, about fifty days |
| Camera serials | cam0 `24260192`, cam1 `24260193` |
| Aperture | f/8 at the unengraved detent, measured f/7.68 |
| Focus | infinity stop, locked. Sharp beyond 14.8 m |
| Exposure | 6006 µs, gain 0 dB, locked |
| Angle between axes | 27.8°, overlap 14.4 %, combined field 60.3° |
| Tilt | about 8.5° down, horizon near the top edge |
| Measured throughput | 118 MB/s sustained to 91 % full, 1.8× the demand |
