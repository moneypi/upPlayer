# LightFC (single-object tracker)

ONNX export of [LightFC](https://github.com/LiYunfengLYF/LightFC) (MobileNetV2, template 128 / search 256) for upPlayer.

```powershell
.\scripts\fetch-lightfc-deps.ps1   # needs Google Drive for official checkpoint, or place ONNX here
# then rebuild
```

Expected files (not committed — large / third-party):

- `lightfc_z.onnx` — template encoder (`z` → `z_feat`)
- `lightfc_x.onnx` — tracker (`z_feat`,`x` → `score_map`,`size_map`,`offset_map`)

Hotkey **t**: arm tracker → click the target on video → tracks with a cyan box (skip busy frames).
