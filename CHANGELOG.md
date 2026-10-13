# Changelog

## 0.6.0

- Auto captions use MIT whisper.cpp with a warm whisper-server (no per-chunk process spawn)
- Releases package the open whisper CLI and model; PotPlayer Whisper is no longer shipped

## 0.5.1

- Match CLI RTSP latency: spawn `mpv --profile=low-latency --untimed <url>` with URL on argv
- Open URL: single **low-latency** checkbox (enables profile + untimed together)
- Title shows **[Track on]** when LightFC track is armed/tracking
- Auto captions: Douyin-style bold yellow text with thick black outline (bottom-center)

## 0.5.0

- LightFC single-object tracking (hotkey **t**): arm → click target → cyan box overlay; async ONNX, skip busy frames

## 0.4.0

- YOLO26 object detection overlay (hotkey **y**): async ONNX Runtime inference; skip busy frames so playback never stalls

## 0.3.4

- Fix auto captions after install: package local PotPlayer Whisper when present; find CLI/models in more locations
- Clearer caption status when Whisper CLI is absent (no bare “missing”)

## 0.3.3

- Fix post-install Start: launch unelevated (avoids mpv IPC failure from elevated setup)
- Prefer bundled `mpv.exe` next to upPlayer; longer IPC connect wait

## 0.3.2

- Setup UI: choose install folder, install progress page, then Finish / Start (no PowerShell)

## 0.3.1

- Register upPlayer for Windows **Open with** and Default Apps (PotPlayer/VLC-style), including uninstall cleanup

## 0.3.0

- Live auto captions via local PotPlayer Whisper module (Vulkan, CPU fallback); hotkey **a**
- Caption text styled like PotPlayer (bottom-center ASS overlay)
- About menu shows app version and playback backend
- MIT license + third-party NOTICE (mpv / PotPlayer Whisper not redistributed)
- Setup installer filename includes version: `upPlayer-Setup-x64-0.3.0.exe`

## 0.2.0

- NSIS setup packaging (Program Files install)
- Speed: Ctrl+Up / Ctrl+Down
- MinGW build link fixes

## 0.1.0

- Initial Win32 mpv front-end with seek bar, volume, clip export, and packaging zip
