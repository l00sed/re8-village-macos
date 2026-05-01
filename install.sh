#!/bin/bash
#
# install.sh - Install the RE8 video fix for CrossOver on macOS
#
# This sets up:
#   1. The MF shim DLL in the game directory
#   2. Required DLL overrides (mfplat, mfreadwrite)
#   3. Required environment variables (MetalFX off)
#   4. A launchd agent that auto-starts the decode server when the game runs
#
# Prerequisites:
#   - CrossOver (tested with 26.x)
#   - RE8 installed via Steam inside a CrossOver bottle
#   - ffmpeg (brew install ffmpeg)
#   - x86_64-w64-mingw32-gcc (only if building from source)
#
# Usage: ./install.sh [--bottle NAME] [--crossover PATH] [--bottle-dir PATH] [--game-dir PATH]
#
# If your Steam library — or the entire CrossOver bottle — lives on an
# external drive (or anywhere outside the default
# ~/Library/Application Support/CrossOver/Bottles location), pass
# --bottle-dir / --game-dir, set RE8_BOTTLE_DIR / RE8_GAME_DIR, or let the
# installer prompt you with a native macOS folder picker when the default
# locations can't be found.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOTTLE="${RE8_BOTTLE:-Steam}"
CX_APP="${RE8_CROSSOVER:-$HOME/Applications/CrossOver.app}"
CX_ROOT="$CX_APP/Contents/SharedSupport/CrossOver"
BOTTLE_DIR_OVERRIDE="${RE8_BOTTLE_DIR:-}"
GAME_DIR_OVERRIDE="${RE8_GAME_DIR:-}"

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
# DRIVE_C / USER_REG / CXBOTTLE / FLAG_FILE / DEFAULT_GAME_DIR are derived
# AFTER bottle-dir resolution (see preflight section below) so an interactive
# picker can update BOTTLE_DIR before they're computed.

CONFIG_DIR="$HOME/.config/re8-village-macos"
CONFIG_FILE="$CONFIG_DIR/config"

PLIST_LABEL="com.re8fix.decode-server"
PLIST_PATH="$HOME/Library/LaunchAgents/${PLIST_LABEL}.plist"

fail() { echo "ERROR: $1" >&2; exit 1; }
ok()   { echo "  OK: $1"; }
skip() { echo "  SKIP: $1 (already set)"; }

echo "============================================"
echo " RE8 Village Video Fix - Installer"
echo "============================================"
echo ""
echo "Bottle:     $BOTTLE"
echo "CrossOver:  $CX_APP"
[[ -n "$BOTTLE_DIR_OVERRIDE" ]] && echo "Bottle dir: $BOTTLE_DIR (override)"
[[ -n "$GAME_DIR_OVERRIDE" ]]   && echo "Game dir:   $GAME_DIR_OVERRIDE (override)"
echo ""

# ---------- Preflight ----------

echo "Checking prerequisites..."

[[ -d "$CX_ROOT" ]] || fail "CrossOver not found at $CX_APP"

# ---------- Locate bottle directory (supports bottles stored on external drives) ----------

prompt_for_folder() {
    # Generic native macOS folder picker. $1 is the dialog prompt.
    local prompt="$1"
    local picked
    picked=$(/usr/bin/osascript <<OSA 2>/dev/null || true
try
    set chosen to choose folder with prompt "${prompt}"
    POSIX path of chosen
on error
    return ""
end try
OSA
    )
    picked="${picked%/}"
    printf '%s' "$picked"
}

bottle_dir_looks_valid() {
    # A real CrossOver bottle has cxbottle.conf and a drive_c subdirectory.
    [[ -f "$1/cxbottle.conf" && -d "$1/drive_c" ]]
}

if ! bottle_dir_looks_valid "$BOTTLE_DIR"; then
    if [[ -n "$BOTTLE_DIR_OVERRIDE" ]]; then
        fail "Bottle directory '$BOTTLE_DIR' is not a valid CrossOver bottle (missing cxbottle.conf or drive_c)."
    fi

    echo ""
    echo "CrossOver bottle '$BOTTLE' was not found at the default location:"
    echo "  $DEFAULT_BOTTLE_DIR"
    echo ""
    echo "This is normal if your bottle lives on an external drive (e.g. under"
    echo "/Volumes/...) or in a custom CrossOver bottle directory."
    echo ""
    read -r -p "Open a folder picker to select the bottle directory manually? [Y/n] " reply
    reply="${reply:-Y}"
    if [[ ! "$reply" =~ ^[Yy] ]]; then
        fail "Bottle '$BOTTLE' not found at $BOTTLE_DIR. Re-run with --bottle-dir PATH."
    fi

    picked_bottle="$(prompt_for_folder "Select your CrossOver bottle folder (the one that contains cxbottle.conf and drive_c):")"
    [[ -n "$picked_bottle" ]] || fail "No folder selected."
    bottle_dir_looks_valid "$picked_bottle" || fail "Selected folder is not a valid CrossOver bottle (missing cxbottle.conf or drive_c): $picked_bottle"
    BOTTLE_DIR="$picked_bottle"
fi

# Now that BOTTLE_DIR is known, derive the rest. DRIVE_C / FLAG_FILE stay
# inside the bottle even when the game itself lives on an external drive,
# because the compiled shim writes the flag and intermediate buffers via the
# bottle's C: drive.
DRIVE_C="$BOTTLE_DIR/drive_c"
USER_REG="$BOTTLE_DIR/user.reg"
CXBOTTLE="$BOTTLE_DIR/cxbottle.conf"
FLAG_FILE="$DRIVE_C/re8_video_fix.active"
DEFAULT_GAME_DIR="$DRIVE_C/Program Files (x86)/Steam/steamapps/common/Resident Evil Village BIOHAZARD VILLAGE"
GAME_DIR="${GAME_DIR_OVERRIDE:-$DEFAULT_GAME_DIR}"

ok "Bottle found at $BOTTLE_DIR"

# ---------- Locate game directory (supports external drives / non-default Steam libraries) ----------

prompt_for_game_dir() {
    prompt_for_folder "Select your 'Resident Evil Village BIOHAZARD VILLAGE' folder (the directory containing re8.exe):"
}

if [[ ! -f "$GAME_DIR/re8.exe" ]]; then
    if [[ -n "$GAME_DIR_OVERRIDE" ]]; then
        fail "RE8 not found at the path you provided: $GAME_DIR"
    fi

    echo ""
    echo "RE8 was not found at the default location:"
    echo "  $DEFAULT_GAME_DIR"
    echo ""
    echo "This is normal if your Steam library lives on an external drive or"
    echo "in a custom location."
    echo ""
    read -r -p "Open a folder picker to select the game directory manually? [Y/n] " reply
    reply="${reply:-Y}"
    if [[ ! "$reply" =~ ^[Yy] ]]; then
        fail "RE8 not installed. Install it via Steam in the '$BOTTLE' bottle, or re-run with --game-dir PATH."
    fi

    picked_dir="$(prompt_for_game_dir)"
    [[ -n "$picked_dir" ]] || fail "No folder selected."
    [[ -f "$picked_dir/re8.exe" ]] || fail "Selected folder does not contain re8.exe: $picked_dir"
    GAME_DIR="$picked_dir"
fi

# Persist the resolved paths so play.sh and uninstall.sh find them.
mkdir -p "$CONFIG_DIR"
cat > "$CONFIG_FILE" <<CFG
# Auto-generated by install.sh — safe to delete (uninstall removes it).
RE8_BOTTLE="$BOTTLE"
RE8_BOTTLE_DIR="$BOTTLE_DIR"
RE8_GAME_DIR="$GAME_DIR"
CFG
ok "Saved configuration to $CONFIG_FILE"

FFMPEG="$(command -v ffmpeg 2>/dev/null || echo "")"
if [[ -z "$FFMPEG" ]]; then
    for p in /opt/homebrew/bin/ffmpeg /usr/local/bin/ffmpeg; do
        [[ -x "$p" ]] && FFMPEG="$p" && break
    done
fi
[[ -n "$FFMPEG" ]] || fail "ffmpeg not found. Install via: brew install ffmpeg"
ok "ffmpeg found at $FFMPEG"

ok "RE8 found at $GAME_DIR"

# ---------- Step 1: Install shim DLL ----------

echo ""
echo "Step 1: Installing MF shim DLL..."

SHIM_DLL="$SCRIPT_DIR/shim/mfreadwrite.dll"
[[ -f "$SHIM_DLL" ]] || fail "Pre-built DLL not found at $SHIM_DLL. Build it first (see README)."

if [[ -f "$GAME_DIR/mfreadwrite.dll" ]]; then
    existing_size=$(/usr/bin/stat -f%z "$GAME_DIR/mfreadwrite.dll")
    shim_size=$(/usr/bin/stat -f%z "$SHIM_DLL")
    if [[ "$existing_size" != "$shim_size" ]]; then
        cp "$GAME_DIR/mfreadwrite.dll" "$GAME_DIR/mfreadwrite.dll.original"
        ok "Backed up existing mfreadwrite.dll"
    fi
fi

cp "$SHIM_DLL" "$GAME_DIR/mfreadwrite.dll"
ok "Installed mfreadwrite.dll to game directory"

# ---------- Step 2: DLL overrides in user.reg ----------

echo ""
echo "Step 2: Setting DLL overrides..."

[[ -f "$USER_REG" ]] || fail "user.reg not found in bottle"

add_dll_override() {
    local dll="$1"
    local mode="$2"

    if grep -q "\"${dll}\"=\"${mode}\"" "$USER_REG" 2>/dev/null; then
        skip "$dll = $mode"
        return
    fi

    if grep -q "\"${dll}\"=" "$USER_REG" 2>/dev/null; then
        sed -i '' "s/\"${dll}\"=.*/\"${dll}\"=\"${mode}\"/" "$USER_REG"
        ok "$dll override updated to $mode"
        return
    fi

    if grep -q '^\[Software\\\\Wine\\\\DllOverrides\]' "$USER_REG" 2>/dev/null; then
        sed -i '' "/^\[Software\\\\\\\\Wine\\\\\\\\DllOverrides\]/a\\
\"${dll}\"=\"${mode}\"" "$USER_REG"
        ok "$dll override added ($mode)"
    else
        echo '' >> "$USER_REG"
        echo '[Software\\Wine\\DllOverrides]' >> "$USER_REG"
        echo "\"${dll}\"=\"${mode}\"" >> "$USER_REG"
        ok "$dll override section created with $dll=$mode"
    fi
}

add_dll_override "mfplat" "native,builtin"
add_dll_override "mfreadwrite" "native,builtin"

# ---------- Step 3: Environment variables ----------

echo ""
echo "Step 3: Setting environment variables..."

set_bottle_env() {
    local key="$1"
    local value="$2"

    if grep -q "^\"${key}\" = " "$CXBOTTLE" 2>/dev/null; then
        local current
        current=$(grep "^\"${key}\" = " "$CXBOTTLE" | head -1 | sed "s/^\"${key}\" = \"//;s/\"$//")
        if [[ "$current" = "$value" ]]; then
            skip "$key=$value"
            return
        fi
        sed -i '' "s/^\"${key}\" = .*/\"${key}\" = \"${value}\"/" "$CXBOTTLE"
        ok "$key updated to $value"
    else
        if grep -q '^\[EnvironmentVariables\]' "$CXBOTTLE" 2>/dev/null; then
            sed -i '' "/^\[EnvironmentVariables\]/a\\
\"${key}\" = \"${value}\"" "$CXBOTTLE"
            ok "$key=$value added"
        else
            echo "" >> "$CXBOTTLE"
            echo "[EnvironmentVariables]" >> "$CXBOTTLE"
            echo "\"${key}\" = \"${value}\"" >> "$CXBOTTLE"
            ok "Created [EnvironmentVariables] with $key=$value"
        fi
    fi
}

set_bottle_env "D3DM_ENABLE_METALFX" "0"
set_bottle_env "DXMT_ENABLE_NVEXT" "0"

# ---------- Step 4: Disable crash reporter ----------

echo ""
echo "Step 4: Disabling crash reporter..."

if [[ -f "$GAME_DIR/CrashReport.exe" ]]; then
    mv "$GAME_DIR/CrashReport.exe" "$GAME_DIR/CrashReport.exe.bak"
    ok "Renamed CrashReport.exe -> CrashReport.exe.bak"
elif [[ -f "$GAME_DIR/CrashReport.exe.bak" ]]; then
    skip "CrashReport.exe already renamed"
else
    skip "CrashReport.exe not found"
fi

# ---------- Step 5: Install launchd agent ----------

echo ""
echo "Step 5: Installing launchd agent (auto-starts decode server)..."

# Unload existing agent if present
if launchctl list "$PLIST_LABEL" &>/dev/null; then
    launchctl unload "$PLIST_PATH" 2>/dev/null || true
    ok "Unloaded previous agent"
fi

# Remove stale flag file
rm -f "$FLAG_FILE"

DECODE_SERVER="$SCRIPT_DIR/scripts/decode_server.sh"
LOG_DIR="$HOME/Library/Logs"

mkdir -p "$HOME/Library/LaunchAgents"

cat > "$PLIST_PATH" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>${PLIST_LABEL}</string>

    <key>ProgramArguments</key>
    <array>
        <string>/bin/bash</string>
        <string>${DECODE_SERVER}</string>
        <string>--drive-c</string>
        <string>${DRIVE_C}</string>
        <string>--ffmpeg</string>
        <string>${FFMPEG}</string>
    </array>

    <key>KeepAlive</key>
    <dict>
        <key>PathState</key>
        <dict>
            <key>${FLAG_FILE}</key>
            <true/>
        </dict>
    </dict>

    <key>StandardOutPath</key>
    <string>${LOG_DIR}/re8-decode-server.log</string>
    <key>StandardErrorPath</key>
    <string>${LOG_DIR}/re8-decode-server.log</string>

    <key>WorkingDirectory</key>
    <string>${SCRIPT_DIR}</string>
</dict>
</plist>
PLIST

ok "Created $PLIST_PATH"

launchctl load "$PLIST_PATH"
ok "Loaded launchd agent"

echo ""
echo "============================================"
echo " Installation complete!"
echo "============================================"
echo ""
echo "How it works:"
echo "  - Just launch RE8 from Steam normally"
echo "  - The decode server starts automatically when the game runs"
echo "  - It stops automatically when the game exits"
echo ""
echo "Logs: $LOG_DIR/re8-decode-server.log"
echo ""
echo "To uninstall: ./uninstall.sh"
echo "Manual launch (if needed): ./play.sh"
echo ""
