# potplayer-whisper

Local copy of `C:\Program Files\DAUM\PotPlayer\Module\Whisper` for upPlayer auto captions.

**Not MIT-licensed.** PotPlayer / Kakao proprietary binaries — do not commit or redistribute.
Gitignores the binaries; only this README is tracked. See the repo root [NOTICE](../../NOTICE).

- Prefer `Vulkan\main64.exe` (GPU via Vulkan), fall back to `CPU\main64.exe`
- Models are **not** stored here; upPlayer looks for:
  1. `%APPDATA%\PotPlayerMini64\Model\ggml-*.bin` (shared with PotPlayer)
  2. `%LOCALAPPDATA%\upPlayer\models\ggml-base.bin`

Refresh from a newer PotPlayer install:

```powershell
Copy-Item "C:\Program Files\DAUM\PotPlayer\Module\Whisper\*" `
  "third_party\potplayer-whisper\" -Recurse -Force
```
