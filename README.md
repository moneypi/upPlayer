# upPlayer

Lightweight Windows video player front-end for [mpv](https://mpv.io/). Opens local files and network URLs, embeds playback in a Win32 window, and includes a simple seek bar.

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

Package lite/full zips (full pulls official mpv Windows builds):

```powershell
.\scripts\package-release.ps1
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
| **Space** | Play / pause |
| **←** / **→** | Seek −5s / +5s |
| **[** / **]** | Speed −0.1x / +0.1x (range 0.1x–20x) |
| **Backspace** | Reset speed to 1x |
| **1** | Window size 0.5× video |
| **2** | Window size 1× video |
| **3** | Window size 1.5× video |
| **4** | Window size 2× video |
| **5** | Fullscreen (keep aspect) |
| **6** | Fullscreen stretch |
| **i** | Show stats (mpv default) |
| **I** | Toggle stats on/off (mpv default) |
| **Ctrl+O** | Open file |
| **Ctrl+U** | Open network address |

Numpad **1–6** work the same as the top-row digits. Size/fullscreen shortcuts follow QQ影音-style behavior.

When speed is not 1x, the window title shows the current rate (for example `[1.5x]`), and a short on-screen label appears after changing speed.
