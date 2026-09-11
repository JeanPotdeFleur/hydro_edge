# hydro_edge

Acquisition software for a two-camera coastal imaging station on the Agassiz
rooftop at Hopkins Marine Station, Monterey. Two 16.1 MP global-shutter sensors
are triggered together at 2 Hz from a GNSS timepulse and written to local SSDs
at 64.5 MB/s sustained, with no dropped frames. The imagery feeds surface
kinematics — foam as a passive tracer, PIV and optical current metering — for
comparison against nearshore circulation models around surface-piercing rocks.

Runs headless on a Raspberry Pi 5 under Ubuntu Server 24.04. Master's thesis
work, Baker Coastal Lab and the Environmental Fluid Mechanics Laboratory,
Stanford University.

## Folders

- **deploy/** — systemd units and the environment file that let the station
  schedule and run itself unattended.
- **docs/** — raw artefacts of every validation gate: manifests, summaries,
  per-frame timing, host monitoring. Every figure and every number in the
  report traces back here.
- **figures/** — one Python script per report figure, each reading from
  `docs/`, with the rendered PNGs in `figures/out/`.
- **scripts/** — operational tooling: daily plan and morning telemetry,
  archive verification, station status, burst monitoring.
- **site/** — source of truth for the public status page the station publishes
  to each morning: <https://jeanpotdefleur.github.io/hopkins-station/>
- **tools/** — standalone diagnostics built alongside the acquisition binary,
  each safe to run on a deployed station.

## Root files

- **main.cpp** — the acquisition binary. Producer pinned to one core, consumer
  to another, cadence anchored on an absolute instant derived from the PPS edge.
- **Session.h** — command line, deterministic camera configuration, and the
  three-layer loss accounting.
- **RingBuffer.h** — bounded stereo frame queue between the two threads.
  Overflow is a counted return value, never an exception.
- **GpioTrigger.h** — hardware trigger: both camera lines dropped in one bulk
  write, so the two sensors expose together.
- **CMakeLists.txt** — four targets: `hydro_edge`, `cam_probe`, `cam_focus`,
  `decode`.
