#!/usr/bin/env bash
#
# b1_fill_sweep.sh -- sustained write throughput against volume fill.  v2
#
# Answers the one question GATE A1 and GATE A3 both left open: both measured
# a nearly empty drive, and the dynamic SLC cache of a QLC device shrinks as
# the volume fills. This writes the volume to roughly ninety per cent in
# fixed-size chunks and times each one, so that throughput can be plotted
# against fill and compared with the 64.5 MB/s the pipeline demands.
#
# Two properties the measurement depends on:
#   - O_DIRECT on the output, so the page cache is bypassed and what is timed
#     is the device and not the RAM.
#   - Incompressible payload, generated as an AES-CTR keystream. Writing zeros
#     would let any controller-side compression flatter the result, which is
#     exactly the reservation raised by the all-black GATE A1 archive.
#
# v2 fixes a fatal defect in v1: the per-chunk figures were parsed with `read`
# from an awk BEGIN block that emitted no trailing newline. `read` returns 1
# on EOF without a delimiter even though it assigns, and `set -o errexit`
# turned that into a silent exit after the very first chunk. errexit is gone
# and every failure is now handled where it happens. v2 also resumes from an
# existing directory, so an interruption costs one chunk and not the night.
#
# The data written is disposable. The result is the CSV.
#
# Usage:  ./b1_fill_sweep.sh [target_mount] [chunk_GiB] [min_free_GiB]
# Default: /mnt/vault2 16 700
#
set -o nounset

TARGET="${1:-/mnt/vault2}"
CHUNK_GIB="${2:-16}"
MIN_FREE_GIB="${3:-700}"

readonly BS_MIB=16                       # one stereo frame pair, near enough
readonly BLOCKS=$(( CHUNK_GIB * 1024 / BS_MIB ))
readonly CHUNK_BYTES=$(( CHUNK_GIB * 1024 * 1024 * 1024 ))
readonly MIN_FREE_BYTES=$(( MIN_FREE_GIB * 1024 * 1024 * 1024 ))

readonly WORKDIR="${TARGET}/b1_fill_test"
readonly CSV="${WORKDIR}/b1_fill_sweep.csv"
readonly THERMAL="/sys/class/thermal/thermal_zone0/temp"

die() { echo "[B1] FATAL: $*" >&2; exit 1; }

[[ -d "$TARGET" ]]      || die "target '$TARGET' does not exist"
mountpoint -q "$TARGET" || die "target '$TARGET' is not a mount point; refusing to fill the root filesystem"
command -v openssl >/dev/null || die "openssl not found"

mkdir -p "$WORKDIR" || die "cannot create '$WORKDIR'"

avail_bytes() { df --output=avail -B1 "$TARGET" | tail -n 1 | tr -d ' '; }
used_pct()    { df --output=pcent      "$TARGET" | tail -n 1 | tr -d ' %'; }

soc_temp() {
    if [[ -r "$THERMAL" ]]; then
        awk '{ printf "%.1f", $1/1000 }' "$THERMAL"
    else
        printf "NA"
    fi
}

# Resume rather than refuse. An interruption then costs the chunk in flight
# and nothing else, which matters for a run that occupies a whole night and
# cannot simply be repeated the next day.
i=0
while [[ -f "$(printf "%s/chunk_%05d.bin" "$WORKDIR" "$i")" ]]; do
    i=$(( i + 1 ))
done
cum=$(( i * CHUNK_BYTES ))

if [[ ! -f "$CSV" ]]; then
    echo "chunk,bytes_written_cum,used_pct_after,elapsed_s,mbps,soc_temp_c,utc" > "$CSV"
fi

echo "[B1] target=${TARGET} chunk=${CHUNK_GIB}GiB stop_at_free=${MIN_FREE_GIB}GiB"
(( i > 0 )) && echo "[B1] resuming at chunk ${i}, ${cum} bytes already on disk"
echo "[B1] starting at $(used_pct)% used, $(( $(avail_bytes) / 1024**3 )) GiB free"

stop_requested=0
on_signal() { stop_requested=1; echo "[B1] Signal received, stopping after this chunk."; }
trap on_signal INT TERM

while (( stop_requested == 0 )); do
    free_now=$(avail_bytes)
    if (( free_now < MIN_FREE_BYTES + CHUNK_BYTES )); then
        echo "[B1] Free space floor reached ($(( free_now / 1024**3 )) GiB). Done."
        break
    fi

    chunk_file=$(printf "%s/chunk_%05d.bin" "$WORKDIR" "$i")
    t0=$(date +%s%N)

    # openssl is killed by SIGPIPE once dd has taken its count; that is
    # expected and is why the pipeline status is read from dd alone.
    openssl enc -aes-256-ctr -nosalt -pass pass:b1sweep -in /dev/zero 2>/dev/null \
        | dd of="$chunk_file" bs="${BS_MIB}M" count="$BLOCKS" \
             iflag=fullblock oflag=direct status=none
    dd_status=$?

    t1=$(date +%s%N)

    if (( dd_status != 0 )); then
        echo "[B1] dd failed with status ${dd_status} on chunk ${i}. Stopping." >&2
        rm -f "$chunk_file"
        break
    fi

    elapsed_ns=$(( t1 - t0 ))
    (( elapsed_ns > 0 )) || elapsed_ns=1
    cum=$(( cum + CHUNK_BYTES ))

    elapsed_s=$(awk -v ns="$elapsed_ns" 'BEGIN { printf "%.3f", ns/1e9 }')
    mbps=$(awk -v ns="$elapsed_ns" -v b="$CHUNK_BYTES" 'BEGIN { printf "%.1f", b/(ns/1e9)/1e6 }')
    pct=$(used_pct)
    temp=$(soc_temp)

    printf "%d,%d,%s,%s,%s,%s,%s\n" \
        "$i" "$cum" "$pct" "$elapsed_s" "$mbps" "$temp" \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$CSV"

    printf "[B1] chunk %05d  %5s%% used  %8s s  %7s MB/s  %s C\n" \
        "$i" "$pct" "$elapsed_s" "$mbps" "$temp"

    i=$(( i + 1 ))
done

sync
echo "[B1] Stopped after ${i} chunks. CSV: ${CSV}"
echo "[B1] Copy the CSV out, then reclaim the space:"
echo "[B1]   cp '${CSV}' ~/hydro_edge/docs/"
echo "[B1]   rm -rf '${WORKDIR}' && sudo fstrim -v '${TARGET}'"