#include "yolo_runtime.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#ifdef UPPLAYER_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace {

constexpr int kInput = 640;
constexpr int kMaxDet = 300;

const char* kCocoNames[80] = {
    "person",         "bicycle",       "car",           "motorcycle",    "airplane",
    "bus",            "train",         "truck",         "boat",          "traffic light",
    "fire hydrant",   "stop sign",     "parking meter", "bench",         "bird",
    "cat",            "dog",           "horse",         "sheep",         "cow",
    "elephant",       "bear",          "zebra",         "giraffe",       "backpack",
    "umbrella",       "handbag",       "tie",           "suitcase",      "frisbee",
    "skis",           "snowboard",     "sports ball",   "kite",          "baseball bat",
    "baseball glove", "skateboard",    "surfboard",     "tennis racket", "bottle",
    "wine glass",     "cup",           "fork",          "knife",         "spoon",
    "bowl",           "banana",        "apple",         "sandwich",      "orange",
    "broccoli",       "carrot",        "hot dog",       "pizza",         "donut",
    "cake",           "chair",         "couch",         "potted plant",  "bed",
    "dining table",   "toilet",        "tv",            "laptop",        "mouse",
    "remote",         "keyboard",      "cell phone",    "microwave",     "oven",
    "toaster",        "sink",          "refrigerator",  "book",          "clock",
    "vase",           "scissors",      "teddy bear",    "hair drier",    "toothbrush",
};

}  // namespace

struct YoloRuntime::Impl {
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "upPlayer-yolo"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::vector<float> input;
    std::vector<std::string> input_names_storage;
    std::vector<std::string> output_names_storage;
    std::vector<const char*> input_names;
    std::vector<const char*> output_names;
#endif
};

YoloRuntime::YoloRuntime() : impl_(new Impl) {}

YoloRuntime::~YoloRuntime() {
    delete impl_;
    impl_ = nullptr;
}

bool YoloRuntime::available() const {
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    return impl_ && impl_->session != nullptr;
#else
    return false;
#endif
}

std::wstring YoloRuntime::last_error() const {
    return last_error_;
}

const char* YoloRuntime::class_name(int id) {
    if (id < 0 || id >= 80)
        return "obj";
    return kCocoNames[id];
}

bool YoloRuntime::load(const std::wstring& model_path, std::wstring* error) {
    last_error_.clear();
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    try {
        impl_->options.SetIntraOpNumThreads(2);
        impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        impl_->session =
            std::make_unique<Ort::Session>(impl_->env, model_path.c_str(), impl_->options);

        Ort::AllocatorWithDefaultOptions allocator;
        impl_->input_names_storage.clear();
        impl_->output_names_storage.clear();
        impl_->input_names.clear();
        impl_->output_names.clear();

        for (size_t i = 0; i < impl_->session->GetInputCount(); ++i) {
            auto name = impl_->session->GetInputNameAllocated(i, allocator);
            impl_->input_names_storage.emplace_back(name.get());
        }
        for (size_t i = 0; i < impl_->session->GetOutputCount(); ++i) {
            auto name = impl_->session->GetOutputNameAllocated(i, allocator);
            impl_->output_names_storage.emplace_back(name.get());
        }
        for (auto& s : impl_->input_names_storage)
            impl_->input_names.push_back(s.c_str());
        for (auto& s : impl_->output_names_storage)
            impl_->output_names.push_back(s.c_str());

        impl_->input.assign(static_cast<size_t>(3 * kInput * kInput), 0.f);
        return true;
    } catch (const Ort::Exception& ex) {
        last_error_ = L"ONNX load failed: ";
        const std::string msg = ex.what();
        last_error_.append(msg.begin(), msg.end());
        impl_->session.reset();
        if (error)
            *error = last_error_;
        return false;
    } catch (...) {
        last_error_ = L"ONNX load failed";
        impl_->session.reset();
        if (error)
            *error = last_error_;
        return false;
    }
#else
    (void)model_path;
    last_error_ = L"Built without ONNX Runtime (place third_party/onnxruntime and rebuild)";
    if (error)
        *error = last_error_;
    return false;
#endif
}

bool YoloRuntime::infer_bgr(const unsigned char* bgr, int width, int height, int stride,
                            std::vector<YoloDetection>& out, float conf_threshold) {
    out.clear();
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    if (!impl_ || !impl_->session || !bgr || width <= 0 || height <= 0 || stride < width * 3) {
        last_error_ = L"YOLO infer: bad input";
        return false;
    }

    const float scale =
        (std::min)(static_cast<float>(kInput) / width, static_cast<float>(kInput) / height);
    const int new_w = (std::max)(1, static_cast<int>(std::lround(width * scale)));
    const int new_h = (std::max)(1, static_cast<int>(std::lround(height * scale)));
    const int pad_x = (kInput - new_w) / 2;
    const int pad_y = (kInput - new_h) / 2;

    std::fill(impl_->input.begin(), impl_->input.end(), 114.f / 255.f);

    // Nearest-neighbor letterbox into RGB CHW float.
    for (int y = 0; y < new_h; ++y) {
        const int src_y = (std::min)(height - 1, static_cast<int>(y / scale));
        for (int x = 0; x < new_w; ++x) {
            const int src_x = (std::min)(width - 1, static_cast<int>(x / scale));
            const unsigned char* px = bgr + src_y * stride + src_x * 3;
            const int dx = pad_x + x;
            const int dy = pad_y + y;
            const size_t base = static_cast<size_t>(dy * kInput + dx);
            impl_->input[base] = px[2] / 255.f;                      // R
            impl_->input[kInput * kInput + base] = px[1] / 255.f;    // G
            impl_->input[2 * kInput * kInput + base] = px[0] / 255.f; // B
        }
    }

    try {
        const std::array<int64_t, 4> shape{1, 3, kInput, kInput};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            mem, impl_->input.data(), impl_->input.size(), shape.data(), shape.size());

        auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, impl_->input_names.data(),
                                           &input_tensor, 1, impl_->output_names.data(),
                                           impl_->output_names.size());
        if (outputs.empty() || !outputs[0].IsTensor()) {
            last_error_ = L"YOLO: empty output";
            return false;
        }

        const float* data = outputs[0].GetTensorData<float>();
        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const auto dims = info.GetShape();
        // Expect [1,300,6] or [300,6]
        int rows = kMaxDet;
        int cols = 6;
        if (dims.size() == 3) {
            rows = static_cast<int>(dims[1]);
            cols = static_cast<int>(dims[2]);
        } else if (dims.size() == 2) {
            rows = static_cast<int>(dims[0]);
            cols = static_cast<int>(dims[1]);
        }
        if (cols < 6 || rows <= 0) {
            last_error_ = L"YOLO: unexpected output shape";
            return false;
        }

        out.reserve(static_cast<size_t>(rows));
        for (int i = 0; i < rows; ++i) {
            const float* row = data + static_cast<size_t>(i) * cols;
            const float score = row[4];
            if (!(score >= conf_threshold))
                continue;
            const int cls = static_cast<int>(row[5]);
            float x1 = (row[0] - pad_x) / scale;
            float y1 = (row[1] - pad_y) / scale;
            float x2 = (row[2] - pad_x) / scale;
            float y2 = (row[3] - pad_y) / scale;
            x1 = (std::max)(0.f, (std::min)(static_cast<float>(width - 1), x1));
            y1 = (std::max)(0.f, (std::min)(static_cast<float>(height - 1), y1));
            x2 = (std::max)(0.f, (std::min)(static_cast<float>(width - 1), x2));
            y2 = (std::max)(0.f, (std::min)(static_cast<float>(height - 1), y2));
            if (x2 <= x1 || y2 <= y1)
                continue;
            YoloDetection det;
            det.x1 = x1;
            det.y1 = y1;
            det.x2 = x2;
            det.y2 = y2;
            det.score = score;
            det.class_id = cls;
            det.label = class_name(cls);
            out.push_back(det);
        }
        return true;
    } catch (const Ort::Exception& ex) {
        last_error_ = L"YOLO infer failed: ";
        const std::string msg = ex.what();
        last_error_.append(msg.begin(), msg.end());
        return false;
    } catch (...) {
        last_error_ = L"YOLO infer failed";
        return false;
    }
#else
    (void)bgr;
    (void)width;
    (void)height;
    (void)stride;
    (void)conf_threshold;
    last_error_ = L"Built without ONNX Runtime";
    return false;
#endif
}
