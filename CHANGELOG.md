# Changelog

## 0.6.3

- Overlay: Load SRT... - pick an external subtitle (.srt and common types) for the current video via mpv sub-add; dialog opens in the media folder and pre-fills sidecar <media>.srt when present

## 0.6.2

- Overlay: Generate SRT - offline full-file transcription with the selected Whisper Model + Language; writes .srt next to the media (UTF-8); progress via OSD; model-missing dialog reused

## 0.6.1

- Whisper Model: if the chosen ggml file is missing or fails to load, show a MessageBox with the model directory and Hugging Face download URL (same source as fetch-whisper-deps.ps1); no silent fallback without telling the user
- Whisper Model menu lists common ggml sizes even when not downloaded yet (marked download)

## 0.6.0

- Auto captions use MIT whisper.cpp with a warm whisper-server (no per-chunk process spawn)
- Releases package the open whisper CLI and model; PotPlayer Whisper is no longer shipped
- Overlay menu: pick any ggml model under the whisper models folders, and a caption language (including Auto). Both choices are remembered; changing the model restarts whisper-server

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
