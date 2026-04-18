#!/bin/bash
#
# decode_server.sh - Video decode server for RE8 MF shim
#
# Watches for movie_*.bin files dumped by the shim DLL, decodes them
# to raw NV12 frames using ffmpeg so the shim can serve real video.
#
# Usage:
#   ./decode_server.sh [--drive-c PATH] [--ffmpeg PATH]
#
# The shim writes ASF video data to C:\movie_N.bin. This script detects
# those files, decodes them to C:\re8_video_N.nv12 (raw NV12 frames at
# 1920x1080 30fps), and the shim reads frames from the growing file.
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

echo "[decode_server] Watching: $DRIVE_C/movie_*.bin"
echo "[decode_server] Output:   ${TARGET_WIDTH}x${TARGET_HEIGHT} @ ${TARGET_FPS}fps NV12"

# ---------- Track decoded files ----------

declare -A DECODED

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
        file_size=$(stat -f%z "$nv12_file" 2>/dev/null || echo 0)
        local frame_count=$(( file_size / FRAME_SIZE ))
        echo "[decode_server] Done: movie_${num} -> ${frame_count} frames ($(( file_size / 1024 / 1024 ))MB)"
        echo "frame_count=${frame_count}" >> "$info_file"
        echo "complete=1" >> "$info_file"
    else
        echo "[decode_server] ERROR: ffmpeg failed for movie_${num} (exit $exit_code)" >&2
    fi
}

# ---------- Main watch loop ----------

while true; do
    for bin_file in "$DRIVE_C"/movie_*.bin; do
        [[ -f "$bin_file" ]] || continue

        num=$(basename "$bin_file" | sed 's/movie_\([0-9]*\)\.bin/\1/')
        [[ "${DECODED[$num]:-}" = "1" ]] && continue

        DECODED[$num]=1
        decode_video "$bin_file" "$num" &
    done
    sleep 0.5
done
