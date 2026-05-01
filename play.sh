#!/bin/bash
#
# play.sh - Launch Resident Evil Village on macOS via CrossOver
#
# This script handles everything automatically:
#   1. Starts the video decode server in the background
#   2. Launches the game through Steam/CrossOver
#   3. Cleans up when the game exits
#
# Usage: ./play.sh [--bottle NAME] [--crossover PATH] [--bottle-dir PATH] [--game-dir PATH]
#
set -euo pipefail

# ---------- Configuration (override via env or flags) ----------

CONFIG_FILE="$HOME/.config/re8-village-macos/config"
# Load persisted settings written by install.sh (sets RE8_BOTTLE,
# RE8_BOTTLE_DIR, RE8_GAME_DIR unless the user has already exported them).
if [[ -f "$CONFIG_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$CONFIG_FILE"
fi

BOTTLE="${RE8_BOTTLE:-Steam}"
CX_APP="${RE8_CROSSOVER:-$HOME/Applications/CrossOver.app}"
CX_ROOT="$CX_APP/Contents/SharedSupport/CrossOver"
FFMPEG="${RE8_FFMPEG:-$(command -v ffmpeg 2>/dev/null || echo /opt/homebrew/bin/ffmpeg)}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_ID="1196590"
BOTTLE_DIR_OVERRIDE="${RE8_BOTTLE_DIR:-}"
GAME_DIR_OVERRIDE="${RE8_GAME_DIR:-}"

# Parse flags
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bottle)     BOTTLE="$2"; shift 2 ;;
        --crossover)  CX_APP="$2"; CX_ROOT="$CX_APP/Contents/SharedSupport/CrossOver"; shift 2 ;;
        --bottle-dir) BOTTLE_DIR_OVERRIDE="$2"; shift 2 ;;
        --game-dir)   GAME_DIR_OVERRIDE="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

DEFAULT_BOTTLE_DIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
BOTTLE_DIR="${BOTTLE_DIR_OVERRIDE:-$DEFAULT_BOTTLE_DIR}"
DRIVE_C="$BOTTLE_DIR/drive_c"
DEFAULT_GAME_DIR="$DRIVE_C/Program Files (x86)/Steam/steamapps/common/Resident Evil Village BIOHAZARD VILLAGE"
GAME_DIR="${GAME_DIR_OVERRIDE:-$DEFAULT_GAME_DIR}"

# ---------- Preflight checks ----------

fail() { echo "ERROR: $1" >&2; exit 1; }

[[ -d "$CX_ROOT" ]]        || fail "CrossOver not found at $CX_APP"
[[ -x "$CX_ROOT/bin/cxstart" ]] || fail "cxstart not found in CrossOver"
[[ -d "$DRIVE_C" ]]         || fail "Bottle '$BOTTLE' not found"
[[ -f "$GAME_DIR/re8.exe" ]] || fail "RE8 not installed in bottle '$BOTTLE'"
[[ -f "$GAME_DIR/mfreadwrite.dll" ]] || fail "MF shim DLL not installed. Run install.sh first."
[[ -x "$FFMPEG" ]]          || fail "ffmpeg not found. Install via: brew install ffmpeg"

# ---------- Cleanup handler ----------

DECODE_PID=""
cleanup() {
    echo ""
    echo "Shutting down..."

    # Kill decode server and any ffmpeg children
    if [[ -n "$DECODE_PID" ]]; then
        kill "$DECODE_PID" 2>/dev/null || true
        # Kill child ffmpeg processes
        pkill -P "$DECODE_PID" 2>/dev/null || true
    fi

    # Clean up temporary video files
    rm -f "$DRIVE_C"/movie_*.bin
    rm -f "$DRIVE_C"/re8_video_*.nv12
    rm -f "$DRIVE_C"/re8_video_*.info

    echo "Done."
}
trap cleanup EXIT INT TERM

# ---------- Clean previous run artifacts ----------

rm -f "$DRIVE_C"/movie_*.bin
rm -f "$DRIVE_C"/re8_video_*.nv12
rm -f "$DRIVE_C"/re8_video_*.info

# ---------- Start decode server ----------

echo "Starting video decode server..."
bash "$SCRIPT_DIR/scripts/decode_server.sh" \
    --drive-c "$DRIVE_C" \
    --ffmpeg "$FFMPEG" &
DECODE_PID=$!
sleep 0.5

if ! kill -0 "$DECODE_PID" 2>/dev/null; then
    fail "Decode server failed to start"
fi
echo "Decode server running (PID $DECODE_PID)"

# ---------- Launch game ----------

echo "Launching Resident Evil Village..."
"$CX_ROOT/bin/cxstart" --bottle "$BOTTLE" --no-convert -- \
    "C:\\Program Files (x86)\\Steam\\steam.exe" -applaunch "$APP_ID"

# cxstart returns after the game process exits — cleanup runs via trap
echo "Game exited."
