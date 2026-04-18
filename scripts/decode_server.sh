#!/bin/bash
#
# decode_server.sh - Video decode server for RE8 MF shim
#
# Watches for movie_*.bin files dumped by the shim DLL, decodes them
# to raw NV12 frames using ffmpeg so the shim can serve real video.
#
# Designed to be started automatically by launchd when the game runs.
# Handles SIGTERM gracefully (sent by launchd when the game exits).
#
# Usage:
#   ./decode_server.sh [--drive-c PATH] [--ffmpeg PATH]
#
set -uo pipefail

# ---------- Configuration ----------

DRIVE_C="${RE8_DRIVE_C:-$HOME/Library/Application Support/CrossOver/Bottles/Steam/drive_c}"
FFMPEG="${RE8_FFMPEG:-$(command -v ffmpeg 2>/dev/null || echo /opt/homebrew/bin/ffmpeg)}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --drive-c) DRIVE_C="$2"; shift 2 ;;
        --ffmpeg)  FFMPEG="$2"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

TARGET_FPS=30
TARGET_WIDTH=1920
TARGET_HEIGHT=1080
FRAME_SIZE=$(( TARGET_WIDTH * TARGET_HEIGHT * 3 / 2 ))

echo "[decode_server] Started (pid $$)"
echo "[decode_server] Watching: $DRIVE_C/movie_*.bin"

# ---------- Signal handling ----------

SHUTTING_DOWN=0
FFMPEG_PIDS=()

shutdown() {
    SHUTTING_DOWN=1
    echo "[decode_server] Shutting down..."

    # Kill any running ffmpeg processes
    for pid in "${FFMPEG_PIDS[@]}"; do
        kill "$pid" 2>/dev/null
    done

    # Clean up temp files and markers
    rm -f "$DRIVE_C"/movie_*.bin
    rm -f "$DRIVE_C"/re8_video_*.nv12
    rm -f "$DRIVE_C"/re8_video_*.info
    rm -f "$DRIVE_C"/.decoded_movie_*

    echo "[decode_server] Stopped."
    exit 0
}

trap shutdown SIGTERM SIGINT SIGHUP

# ---------- Track decoded files (using marker files for bash 3.2 compat) ----------

is_decoded() {
    [[ -f "$DRIVE_C/.decoded_movie_$1" ]]
}

mark_decoded() {
    touch "$DRIVE_C/.decoded_movie_$1"
}

cleanup_markers() {
    rm -f "$DRIVE_C"/.decoded_movie_*
}

decode_video() {
    local bin_file="$1"
    local num="$2"
    local nv12_file="$DRIVE_C/re8_video_${num}.nv12"
    local info_file="$DRIVE_C/re8_video_${num}.info"

    echo "[decode_server] Decoding movie_${num}.bin -> re8_video_${num}.nv12"

    # Decode: scale to 1920x1080, 30fps, NV12 raw output
    # File grows progressively so the shim can read frames as they're produced
    "$FFMPEG" -y -hide_banner -loglevel warning \
        -i "$bin_file" \
        -vf "fps=${TARGET_FPS},scale=${TARGET_WIDTH}:${TARGET_HEIGHT}" \
        -pix_fmt nv12 \
        -f rawvideo \
        -an \
        "$nv12_file" 2>&1 &
    local ffmpeg_pid=$!
    FFMPEG_PIDS+=("$ffmpeg_pid")

    sleep 0.3

    # Write info file
    echo "width=${TARGET_WIDTH}" > "$info_file"
    echo "height=${TARGET_HEIGHT}" >> "$info_file"
    echo "fps=${TARGET_FPS}" >> "$info_file"
    echo "frame_size=${FRAME_SIZE}" >> "$info_file"
    echo "pid=${ffmpeg_pid}" >> "$info_file"

    # Wait for completion
    wait "$ffmpeg_pid" 2>/dev/null
    local exit_code=$?

    if [[ $exit_code -eq 0 ]]; then
        local file_size
        file_size=$(/usr/bin/stat -f%z "$nv12_file" 2>/dev/null || echo 0)
        local frame_count=$(( file_size / FRAME_SIZE ))
        echo "[decode_server] Done: movie_${num} -> ${frame_count} frames ($(( file_size / 1024 / 1024 ))MB)"
        echo "frame_count=${frame_count}" >> "$info_file"
        echo "complete=1" >> "$info_file"
    elif [[ $SHUTTING_DOWN -eq 0 ]]; then
        echo "[decode_server] ERROR: ffmpeg failed for movie_${num} (exit $exit_code)" >&2
    fi
}

# ---------- Main watch loop ----------

FLAG_FILE="$DRIVE_C/re8_video_fix.active"
IDLE_COUNT=0

while [[ $SHUTTING_DOWN -eq 0 ]]; do
    # Exit if the game has exited (flag file removed by DLL_PROCESS_DETACH)
    if [[ ! -f "$FLAG_FILE" ]]; then
        IDLE_COUNT=$((IDLE_COUNT + 1))
        # Give it a few seconds grace period (flag might not exist yet on startup)
        if [[ $IDLE_COUNT -gt 20 ]]; then
            echo "[decode_server] Flag file removed, game exited. Shutting down."
            shutdown
        fi
    else
        IDLE_COUNT=0
    fi

    for bin_file in "$DRIVE_C"/movie_*.bin; do
        [[ -f "$bin_file" ]] || continue

        num=$(basename "$bin_file" | sed 's/movie_\([0-9]*\)\.bin/\1/')
        is_decoded "$num" && continue

        mark_decoded "$num"
        decode_video "$bin_file" "$num" &
    done
    sleep 0.5
done
