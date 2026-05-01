#!/bin/bash
#
# uninstall.sh - Remove the RE8 video fix
#
# Removes the launchd agent, shim DLL, and restores original settings.
#
# Usage: ./uninstall.sh [--bottle NAME] [--bottle-dir PATH] [--game-dir PATH]
#
set -euo pipefail

CONFIG_DIR="$HOME/.config/re8-village-macos"
CONFIG_FILE="$CONFIG_DIR/config"
# Pick up persisted RE8_BOTTLE / RE8_BOTTLE_DIR / RE8_GAME_DIR if install.sh
# wrote them.
if [[ -f "$CONFIG_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$CONFIG_FILE"
fi

BOTTLE="${RE8_BOTTLE:-Steam}"
BOTTLE_DIR_OVERRIDE="${RE8_BOTTLE_DIR:-}"
GAME_DIR_OVERRIDE="${RE8_GAME_DIR:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bottle)     BOTTLE="$2"; shift 2 ;;
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

PLIST_LABEL="com.re8fix.decode-server"
PLIST_PATH="$HOME/Library/LaunchAgents/${PLIST_LABEL}.plist"

echo "Uninstalling RE8 video fix..."

# ---------- Remove launchd agent ----------

if [[ -f "$PLIST_PATH" ]]; then
    launchctl unload "$PLIST_PATH" 2>/dev/null || true
    rm -f "$PLIST_PATH"
    echo "  Removed launchd agent"
else
    echo "  No launchd agent found"
fi

# ---------- Remove flag file ----------

rm -f "$DRIVE_C/re8_video_fix.active"

# ---------- Remove shim DLL ----------

if [[ -f "$GAME_DIR/mfreadwrite.dll.original" ]]; then
    mv "$GAME_DIR/mfreadwrite.dll.original" "$GAME_DIR/mfreadwrite.dll"
    echo "  Restored original mfreadwrite.dll"
elif [[ -f "$GAME_DIR/mfreadwrite.dll" ]]; then
    rm -f "$GAME_DIR/mfreadwrite.dll"
    echo "  Removed shim mfreadwrite.dll"
fi

# ---------- Restore crash reporter ----------

if [[ -f "$GAME_DIR/CrashReport.exe.bak" ]]; then
    mv "$GAME_DIR/CrashReport.exe.bak" "$GAME_DIR/CrashReport.exe"
    echo "  Restored CrashReport.exe"
fi

# ---------- Clean up temp files ----------

rm -f "$DRIVE_C"/movie_*.bin
rm -f "$DRIVE_C"/re8_video_*.nv12
rm -f "$DRIVE_C"/re8_video_*.info
rm -f "$DRIVE_C"/mf_shim_debug.log
rm -f "$DRIVE_C"/.decoded_movie_*

# ---------- Remove persisted config ----------

if [[ -f "$CONFIG_FILE" ]]; then
    rm -f "$CONFIG_FILE"
    rmdir "$CONFIG_DIR" 2>/dev/null || true
    echo "  Removed config file $CONFIG_FILE"
fi

echo ""
echo "Uninstall complete."
echo ""
echo "NOTE: DLL overrides (mfplat, mfreadwrite) and environment variables"
echo "(D3DM_ENABLE_METALFX, DXMT_ENABLE_NVEXT) were left in the bottle"
echo "configuration. Remove them manually via CrossOver's Wine Configuration"
echo "if needed."
