# YOLO26 (Ultralytics)

Local YOLO26n ONNX weights for upPlayer object detection overlay.

```powershell
.\scripts\fetch-yolo-deps.ps1
```

Produces `yolo26n.onnx` (NMS-free `1×300×6` output) and downloads ONNX Runtime into `third_party/onnxruntime`.

Weights / runtime binaries are **not** committed (large / redistributable separately). See Ultralytics and Microsoft ONNX Runtime licenses.
