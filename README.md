# Resident Evil Village (RE8) - macOS Video Fix for CrossOver

> **This repo has been superseded by [re-engine-macos](https://github.com/l00sed/re-engine-macos)**, a unified monorepo that supports all RE Engine games (RE3, RE8, RE7, RE2, RE4) with a single shared codebase.
>
> **If you're installing for the first time**, use the new repo instead.
>
> **If you're upgrading from this repo**, uninstall first (`./uninstall.sh`), then follow the new repo's instructions. The file naming convention has changed (`movie_*.bin` -> `re8_movie_*.bin`).
>
> This repo is preserved for reference but will no longer be updated.

---

Resident Evil Village runs on macOS via CrossOver at 50-60 FPS (without ray tracing) on Apple Silicon— but, it crashes on startup when trying to play the first intro video. CodeWeavers officially rates this game **"Installs, Will Not Run."**

This fix makes it run— with full video playback for intros and cutscenes.

## The Problem

RE8 uses Windows Media Foundation (MF) to play ASF/WMV intro and cutscene videos. CrossOver's Wine translates MF calls to GStreamer, but the GStreamer pipeline stalls when processing ASF container streams— the media source is created and the decode chain is set up (even using VideoToolbox hardware decoding), but no frames are ever produced. The game hangs waiting for the first video frame, then
crashes.

## How the Fix Works

The fix has two parts that work together:

**1. MF Shim DLL** (`mfreadwrite.dll`)— A Windows DLL placed in the game directory that intercepts `MFCreateSourceReaderFromByteStream`. Instead of calling into Wine's broken MF pipeline, it:

- Dumps the video byte stream to disk (`movie_N.bin`)
- Identifies the video by file size (the game has 3 known videos)
- Returns correctly-timed NV12 video frames from a pre-decoded raw file
- Uses a ring buffer of 8 frame slots so the game's render pipeline never reads stale data

**2. Decode Server** (`decode_server.sh`) -- A macOS-side background script that watches for the dumped `.bin` files and decodes them to raw NV12 using `ffmpeg`. The output file grows progressively so the shim can start serving frames before decoding finishes.

```
Game (Wine)                    macOS
-----------                    -----
re8.exe                        decode_server.sh
  |                              |
  +--> MFCreateSourceReader      |
  |      |                       |
  |      +--> dump movie_N.bin --+--> ffmpeg decode
  |      |                       |       |
  |      +--> ReadSample         |       v
  |      |      |                |    re8_video_N.nv12
  |      |      +--> read frame <------+
  |      |      |    from .nv12  |
  |      v      v                |
  |    video displayed           |
```

## Requirements

- **macOS** on Apple Silicon (tested on M3 Pro)
- **CrossOver** 26.x (tested with 26.1.0)
- **RE8** installed via Steam in a CrossOver bottle
- **ffmpeg**: `brew install ffmpeg`
- **mingw-w64** (only for building from source): `brew install mingw-w64`

## Quick Start

```bash
# Clone this repo
git clone https://github.com/youruser/re8-village-macos.git
cd re8-village-macos

# Install (copies DLL, sets Wine overrides, installs auto-launch agent)
./install.sh

# That's it— just launch RE8 from Steam normally.
# The decode server starts and stops automatically.
```

After running `install.sh`, you launch the game from Steam like normal. The installer sets up a macOS launchd agent that automatically starts the decode server when the game runs and stops it when the game exits. No manual steps needed.

If you prefer to manage the decode server manually, use `play.sh` instead:

```bash
./play.sh   # starts decode server, launches game, cleans up on exit
```

## Installation Details

`install.sh` does the following:

1. **Copies `mfreadwrite.dll`** into the RE8 game directory
2. **Sets DLL overrides** in the Wine bottle:
   - `mfplat` = `native,builtin` (the builtin is missing an export the game needs)
   - `mfreadwrite` = `native,builtin` (loads our shim from the game directory)
3. **Sets environment variables**:
   - `D3DM_ENABLE_METALFX=0` — MetalFX must be off or video playback stalls
   - `DXMT_ENABLE_NVEXT=0` — Same issue with NVIDIA extensions emulation
4. **Disables CrashReport.exe** — Wine's auto-debugger interferes with the game
5. **Installs a launchd agent** — auto-starts the decode server when the game runs (triggered by a flag file the shim creates on load) and stops it when the game exits

### Custom Bottle Name

If your bottle isn't named "Steam":

```bash
./install.sh --bottle "My Bottle"
./play.sh --bottle "My Bottle"
```

### External Drives & Custom Install Locations

If your CrossOver bottle or your Steam library lives outside the default
locations (for example on an external SSD under `/Volumes/...`), the installer
supports two additional overrides:

| Flag | Env var | What it points at |
|------|---------|-------------------|
| `--bottle-dir PATH` | `RE8_BOTTLE_DIR` | The bottle folder containing `cxbottle.conf` and `drive_c` |
| `--game-dir PATH`   | `RE8_GAME_DIR`   | The game folder containing `re8.exe` |

**Easiest path — just run the installer.** If either default location is
missing, the installer prints the path it tried, then asks:

```
Open a folder picker to select the bottle directory manually? [Y/n]
```

Press <kbd>Enter</kbd> and a native macOS folder picker appears so you can
browse to the right folder without typing long paths. The installer validates
the selection (checks for `cxbottle.conf` / `drive_c` for the bottle, and
`re8.exe` for the game).

**Power-user path — pass flags directly:**

```bash
./install.sh \
  --bottle "Resident Evil 7 & 8" \
  --bottle-dir "/Volumes/SSDGaming/CrossOver/Bottles/Resident Evil 7 & 8" \
  --game-dir   "/Volumes/SSDGaming/CrossOver/Bottles/Resident Evil 7 & 8/drive_c/Program Files (x86)/Steam/steamapps/common/Resident Evil Village BIOHAZARD VILLAGE"
```

(You can usually omit `--game-dir`: if the bottle path is correct and the game
sits inside it at the standard Steam location, the installer finds it
automatically.)

**The choice persists.** The resolved paths are written to
`~/.config/re8-village-macos/config`, so subsequent runs of `play.sh` and
`uninstall.sh` work with no flags. Override per-run via the same flags or env
vars if you need to. Re-run `./install.sh` (or edit / delete the config file)
to change the saved paths.

> **Note:** Even when the bottle and game live on an external drive, the shim
> DLL writes its flag file (`re8_video_fix.active`) and intermediate video
> buffers (`movie_*.bin`, `re8_video_*.nv12`) inside the bottle's `drive_c`,
> because the path is hardcoded into the compiled DLL as `C:\...`. The decode
> server and launchd agent automatically follow `RE8_BOTTLE_DIR` to watch the
> right location.

### Custom CrossOver Location

```bash
./install.sh --crossover "/path/to/CrossOver.app"
```

## Building from Source

The repo includes a pre-built DLL, but if you want to build it yourself:

```bash
brew install mingw-w64
cd shim
x86_64-w64-mingw32-gcc -shared \
    -o mfreadwrite.dll \
    mfreadwrite_shim.c \
    mfreadwrite.def \
    -Wl,--enable-stdcall-fixup -lole32
```

## File Structure

```
.
├── play.sh                    # Manual launch script (alternative to auto-launch)
├── install.sh                 # One-time setup (DLL, overrides, launchd agent)
├── uninstall.sh               # Remove everything
├── shim/
│   ├── mfreadwrite_shim.c     # Shim DLL source (~1100 lines of C)
│   ├── mfreadwrite.def        # DLL export definitions
│   └── mfreadwrite.dll        # Pre-built PE32+ x86_64 DLL
└── scripts/
    └── decode_server.sh       # Background video decode server
```

## Known Videos

The shim identifies RE8's three startup/cutscene videos by their ASF byte stream size and reports the correct duration to the game engine:

| Video | Content | Duration | Resolution | Codec |
|-------|---------|----------|------------|-------|
| 1 | RE Engine logo | 5.0s | 1920x1080 | VC-1 |
| 2 | CAPCOM logo | 71.1s | 1280x720 | WMV3 |
| 3 | Opening cutscene | 142.8s | 1920x1080 | VC-1 |

Unknown videos (from later in the game) fall back to a generous 5-minute default duration with black frames if the decode server can't process them in time.

## Debugging

The shim writes a log to `C:\mf_shim_debug.log` inside the Wine bottle (at `~/Library/Application Support/CrossOver/Bottles/<bottle>/drive_c/mf_shim_debug.log`).

This shows every MF API call, frame delivery timing, and whether frames came from the decoded video file (REAL) or the black fallback (BLACK).

The decode server logs to `~/Library/Logs/re8-decode-server.log`.

## Uninstalling

```bash
./uninstall.sh
```

This removes the launchd agent, shim DLL, restores CrashReport.exe, and
deletes the persisted config file at `~/.config/re8-village-macos/config`.
DLL overrides and environment variables are left in the bottle config (remove
manually via CrossOver's Wine Configuration if needed).

## Technical Background

For the full chain of issues that were diagnosed and solved:

1. **Crash 1**: Wine's `wg_format.c` doesn't handle `video/x-ms-asf` container caps (it shouldn't— that's a container, not a codec). The GStreamer demuxer pipeline fails to produce elementary streams.

2. **Crash 2**: The game's native MF DLLs import `MFGetCallStackTracingWeakReference` from `mfplat.dll`, which Wine's builtin doesn't export. Fixed by using the native Windows mfplat.dll from system32.

3. **Hang**: Wine's GStreamer pipeline can parse ASF and set up VideoToolbox HW decoding, but stalls during frame delivery -- likely a threading issue in Wine's wg_parser under Rosetta 2.

4. **State machine**: RE8's movie player requires real-time paced frame delivery (one frame every ~33ms). Returning all frames at once or returning immediate EOS both cause the game to get stuck in its video state machine.

5. **MetalFX**: D3DMetal's MetalFX upscaling and NVIDIA extension emulation must be disabled or the game won't advance past the first intro video.

## Performance

On Apple M3 Pro (16GB):
- 50-60 FPS in-game (without ray tracing enabled)
- D3DMetal translates DirectX 12 to Metal
- Game auto-configures to DX12 via the D3DMetal backend

## License

MIT
