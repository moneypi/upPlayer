#pragma once

#include <string>
#include <vector>

struct YoloDetection {
    float x1 = 0.f;
    float y1 = 0.f;
    float x2 = 0.f;
    float y2 = 0.f;
    float score = 0.f;
    int class_id = -1;
    const char* label = "";
};

// YOLO26n ONNX (NMS-free output 1x300x6). Never blocks the UI/playback thread by itself —
// callers must invoke from a worker and skip frames when busy.
class YoloRuntime {
public:
    YoloRuntime();
    ~YoloRuntime();

    YoloRuntime(const YoloRuntime&) = delete;
    YoloRuntime& operator=(const YoloRuntime&) = delete;

    bool load(const std::wstring& model_path, std::wstring* error = nullptr);
    bool available() const;
    std::wstring last_error() const;

    // BGR8 packed image (stride = width * 3). Fills detections in original image coords.
    bool infer_bgr(const unsigned char* bgr, int width, int height, int stride,
                   std::vector<YoloDetection>& out, float conf_threshold = 0.35f);

    static const char* class_name(int id);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::wstring last_error_;
};
