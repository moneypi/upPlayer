# upPlayer

Lightweight Windows video player front-end for [mpv](https://mpv.io/). Opens local files and network URLs, embeds playback in a Win32 window, and includes a simple seek bar.

**Version:** 0.3.0 (right-click → **About upPlayer…**) — see [CHANGELOG](CHANGELOG.md)

**License:** upPlayer source is [MIT](LICENSE). Third-party components (mpv, PotPlayer Whisper binaries, models) have their own terms — see [NOTICE](NOTICE).

## Requirements

- Windows 10+
- Visual Studio 2022 (C++ desktop workload) or another CMake-compatible MSVC toolchain
- CMake 3.20+
- mpv submodule headers: `git submodule update --init --depth 1 third_party/mpv`
- Runtime: `mpv.exe` on `PATH`, or place `libmpv-2.dll` next to `upPlayer.exe` (optional `third_party/libmpv-win/`)

## Build

```powershell
git submodule update --init --depth 1 third_party/mpv
cmake -S . -B out -G "Visual Studio 17 2022" -A x64
cmake --build out --config Release
```

Binary: `out\Release\upPlayer.exe`

Package a Windows zip (pulls official mpv Windows build + runtime DLLs):

```powershell
.\scripts\package-release.ps1
```

Output: `build\dist\upPlayer-windows-x64.zip`

Build a Windows setup installer with **NSIS** (same Nullsoft installer family as PotPlayer; downloads NSIS if needed):

```powershell
.\scripts\package-setup.ps1
```

Output: `build\dist\upPlayer-Setup-x64-<version>.exe` (version from `src/version.hpp`; same files as the zip: `upPlayer.exe` + bundled mpv). Installs machine-wide to `C:\Program Files\upPlayer` (admin / UAC). Script: `scripts\upPlayer.nsi`.

Publish a GitHub release (tag + setup asset):

```powershell
.\scripts\package-setup.ps1
.\scripts\publish-release.ps1
```

## Run

```powershell
.\out\Release\upPlayer.exe
.\out\Release\upPlayer.exe D:\path\to\video.mp4
```

- Drag and drop files onto the window
- Right-click for the context menu
- **Ctrl+O** open file, **Ctrl+U** open network address

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| **Space** / **click video** | Play / pause |
| **←** / **→** | Seek −5s / +5s |
| **Ctrl+↓** / **Ctrl+↑** | Speed −0.1x / +0.1x (range 0.1x–20x) |
| **Backspace** | Reset speed to 1x |
| **1** | Window size 0.5× video |
| **2** | Window size 1× video |
| **3** | Window size 1.5× video |
| **4** | Window size 2× video |
| **5** | Fullscreen (keep aspect) |
| **6** | Fullscreen stretch |
| **Enter** / **double-click** | Toggle fullscreen |
| **i** | Show stats (mpv default) |
| **I** | Toggle stats on/off (mpv default) |
| **↓** / **↑** | Volume −2 / +2 (range 0–100) |
| **m** | Mute / unmute |
| **Mouse wheel** | Volume up / down (over video or seek bar) |
| **s** | Take snapshot (saved to Desktop) |
| **a** | Toggle live auto captions (PotPlayer Whisper Vulkan/CPU) |
| **z** | Mark clip In (A); pauses; selects In for `,` / `.` nudge |
| **x** | Mark clip Out (B); pauses; selects Out for `,` / `.` nudge |
| **,** / **.** | Nudge selected mark ±1 frame (or frame-step if none selected) |
| **Esc** | Cancel clip marks (or exit fullscreen) |
| **c** | Export A–B clip via mpv (`--stream-record`) |
| **Ctrl+O** | Open file |
| **Ctrl+U** | Open network address |

Numpad **1–6** work the same as the top-row digits. Keyboard volume keys also work.

Bottom bar: seek slider on the left, volume slider on the right (drag to set 0–100; muted shows empty). Clip In (green) / Out (orange) marks can be dragged on the seek bar; `,` / `.` nudge the selected mark by one frame (right-click → **Clip** to clear).

Clip export runs a separate `mpv.exe` with `--stream-record` (same binary used for playback; release zip bundles it). Output is a remux dump, so In may snap to a nearby keyframe.

### Auto captions

Press **a** to toggle live captions. For local use, copy PotPlayer’s Whisper module into `third_party/potplayer-whisper` (Vulkan `main64.exe`, CPU fallback). Those binaries are **not** MIT-licensed and are **not** shipped in this repo — see [NOTICE](NOTICE). Models are shared with PotPlayer (`%APPDATA%\PotPlayerMini64\Model\`) or `%LOCALAPPDATA%\upPlayer\models\ggml-base.bin`.

```powershell
Copy-Item "C:\Program Files\DAUM\PotPlayer\Module\Whisper\*" `
  "third_party\potplayer-whisper\" -Recurse -Force
```

Requires `mpv.exe` on `PATH` (or next to `upPlayer.exe`) to extract audio chunks.

When speed is not 1x, the window title shows the current rate (for example `[1.5x]`), and a short on-screen label appears after changing speed.
