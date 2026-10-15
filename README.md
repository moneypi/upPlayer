# upPlayer — Dedicated to building an AI player with the lowest latency

Lightweight Windows video player front-end for [mpv](https://mpv.io/). Opens local files and network URLs, embeds playback in a Win32 window, and includes a simple seek bar.

**Version:** 0.6.7 (right-click → **About upPlayer…**) — see [CHANGELOG](CHANGELOG.md)

**License:** upPlayer source is [MIT](LICENSE). Third-party components (mpv, whisper.cpp, models) have their own terms — see [NOTICE](NOTICE).

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

Output: `build\dist\upPlayer-windows-x64-AI.zip` (or `-noAI` with `-IncludeAi:$false`)

Build both Windows setup installers (AI and no-AI):

```powershell
.\scripts\package-both.ps1
```

Outputs:
- `build\dist\upPlayer-Setup-x64-<version>-AI.exe` — Whisper (ggml-tiny), YOLO26, LightFC, ONNX Runtime
- `build\dist\upPlayer-Setup-x64-<version>.exe` — player + mpv only

Single variant: `.\scripts\package-setup.ps1 -IncludeAi $true` or `-IncludeAi $false`. The AI menu appears only when AI files are present next to the exe. Setup UI lets you choose the install folder (admin / UAC).

Publish a GitHub release (tag + both setup assets):

```powershell
.\scripts\package-both.ps1
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
| **s** | Take snapshot (saved to the snapshot folder; Pictures by default) |
| **a** | Toggle live auto captions (whisper.cpp) |
| **y** | Toggle YOLO26 detection (skips busy frames; never stalls play) |
| **t** | LightFC track: arm → click target on video (skip busy frames) |
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

### YOLO26 detection

Press **y** to toggle Ultralytics YOLO26n boxes/labels. Inference is async; busy frames are skipped so playback never stalls.

```powershell
.\scripts\fetch-yolo-deps.ps1   # ONNX Runtime + yolo26n.onnx
# then rebuild
```

### LightFC tracking

Press **t** to arm, then click the object on the video. LightFC tracks with a cyan box (async; busy frames skipped). Press **t** again to stop.

```powershell
.\scripts\fetch-lightfc-deps.ps1   # official checkpoint → lightfc_z/x.onnx
# then rebuild
```

### Auto captions

Press **a** to toggle live captions. Right-click → **AI** → **Whisper Model** / **Language** to pick a ggml model (anything `ggml-*.bin` under the whisper models folders) and a language, including **Auto**. Both choices are remembered; switching the model restarts whisper-server. Captions use MIT [whisper.cpp](https://github.com/ggml-org/whisper.cpp) (not PotPlayer libraries):

```powershell
.\scripts\fetch-whisper-deps.ps1   # Vulkan whisper-cli + ggml-tiny.bin → third_party\whisper-win\
```

Requires `mpv.exe` on `PATH` (or next to `upPlayer.exe`) to extract audio chunks. See [NOTICE](NOTICE).

When speed is not 1x, the window title shows the current rate (for example `[1.5x]`), and a short on-screen label appears after changing speed.
