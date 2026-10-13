#pragma once

#include <string>
#include <vector>

struct LightFcBox {
    float x = 0.f;  // xywh in image pixels
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
    float score = 0.f;
};

// LightFC (MobileNetV2) ONNX: template encoder + search tracker.
// Template 128 / search 256 / stride 16 — matches official LightFC config.
class LightFcRuntime {
public:
    static constexpr int kTemplate = 128;
    static constexpr int kSearch = 256;
    static constexpr int kStride = 16;
    static constexpr int kFeatSz = kSearch / kStride;  // 16
    static constexpr int kZFeatH = kTemplate / kStride;  // 8
    static constexpr int kZFeatC = 96;
    static constexpr float kTemplateFactor = 2.f;
    static constexpr float kSearchFactor = 4.f;

    LightFcRuntime();
    ~LightFcRuntime();

    LightFcRuntime(const LightFcRuntime&) = delete;
    LightFcRuntime& operator=(const LightFcRuntime&) = delete;

    bool load(const std::wstring& z_model, const std::wstring& x_model, std::wstring* error = nullptr);
    bool available() const;
    std::wstring last_error() const;

    // init_bbox = xywh in image coords. Caches template features.
    bool initialize(const unsigned char* bgr, int width, int height, int stride, const LightFcBox& init_bbox,
                    std::wstring* error = nullptr);

    // Updates state box (xywh). Returns false on infer failure.
    bool track(const unsigned char* bgr, int width, int height, int stride, LightFcBox& out,
               std::wstring* error = nullptr);

    void reset();
    bool has_target() const;
    LightFcBox state() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::wstring last_error_;
};
