#include "lightfc_runtime.hpp"

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

constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

float Hann1d(int i, int sz) {
    // Matches LightFC hann1d(centered=True): 0.5 * (1 - cos(2π/(sz+1) * (i+1)))
    return 0.5f * (1.f - std::cos((2.f * 3.14159265358979323846f / (sz + 1)) * static_cast<float>(i + 1)));
}

void BuildHann2d(std::vector<float>& out, int sz) {
    out.assign(static_cast<size_t>(sz * sz), 0.f);
    for (int y = 0; y < sz; ++y) {
        const float wy = Hann1d(y, sz);
        for (int x = 0; x < sz; ++x)
            out[static_cast<size_t>(y * sz + x)] = wy * Hann1d(x, sz);
    }
}

// Extract square crop centered on xywh box; resize to out_sz. Returns resize_factor = out_sz/crop_sz.
bool SampleTarget(const unsigned char* bgr, int width, int height, int stride, const LightFcBox& bb,
                  float area_factor, int out_sz, std::vector<float>& nchw, float* resize_factor) {
    if (!bgr || width <= 0 || height <= 0 || stride < width * 3 || out_sz <= 0)
        return false;
    const float w = (std::max)(1.f, bb.w);
    const float h = (std::max)(1.f, bb.h);
    const float crop_sz_f = std::ceil(std::sqrt(w * h) * area_factor);
    if (crop_sz_f < 1.f)
        return false;
    const int crop_sz = static_cast<int>(crop_sz_f);
    int x1 = static_cast<int>(std::lround(bb.x + 0.5f * w - crop_sz * 0.5f));
    int y1 = static_cast<int>(std::lround(bb.y + 0.5f * h - crop_sz * 0.5f));
    int x2 = x1 + crop_sz;
    int y2 = y1 + crop_sz;

    const int x1_pad = (std::max)(0, -x1);
    const int y1_pad = (std::max)(0, -y1);
    const int x2_pad = (std::max)(0, x2 - width + 1);
    const int y2_pad = (std::max)(0, y2 - height + 1);

    // Cropped region inside image.
    const int cx1 = x1 + x1_pad;
    const int cy1 = y1 + y1_pad;
    const int cx2 = x2 - x2_pad;
    const int cy2 = y2 - y2_pad;
    const int cw = (std::max)(0, cx2 - cx1);
    const int ch = (std::max)(0, cy2 - cy1);

    std::vector<unsigned char> padded(static_cast<size_t>(crop_sz * crop_sz * 3), 0);
    for (int y = 0; y < ch; ++y) {
        const unsigned char* src = bgr + static_cast<size_t>(cy1 + y) * stride + static_cast<size_t>(cx1) * 3;
        unsigned char* dst =
            padded.data() + static_cast<size_t>(y1_pad + y) * crop_sz * 3 + static_cast<size_t>(x1_pad) * 3;
        std::memcpy(dst, src, static_cast<size_t>(cw) * 3);
    }

    nchw.assign(static_cast<size_t>(3 * out_sz * out_sz), 0.f);
    const float scale = static_cast<float>(out_sz) / static_cast<float>(crop_sz);
    *resize_factor = scale;

    // Bilinear resize BGR→RGB + ImageNet normalize into NCHW.
    for (int oy = 0; oy < out_sz; ++oy) {
        const float sy = ((oy + 0.5f) / scale) - 0.5f;
        const int y0 = (std::max)(0, (std::min)(crop_sz - 1, static_cast<int>(std::floor(sy))));
        const int y1i = (std::max)(0, (std::min)(crop_sz - 1, y0 + 1));
        const float fy = sy - y0;
        for (int ox = 0; ox < out_sz; ++ox) {
            const float sx = ((ox + 0.5f) / scale) - 0.5f;
            const int x0 = (std::max)(0, (std::min)(crop_sz - 1, static_cast<int>(std::floor(sx))));
            const int x1i = (std::max)(0, (std::min)(crop_sz - 1, x0 + 1));
            const float fx = sx - x0;
            for (int c = 0; c < 3; ++c) {
                // BGR source → RGB order for model (c_rgb = 2-c when reading BGR).
                const int src_c = 2 - c;
                const auto sample = [&](int yy, int xx) -> float {
                    return padded[static_cast<size_t>((yy * crop_sz + xx) * 3 + src_c)] / 255.f;
                };
                const float v00 = sample(y0, x0);
                const float v01 = sample(y0, x1i);
                const float v10 = sample(y1i, x0);
                const float v11 = sample(y1i, x1i);
                const float v0 = v00 * (1.f - fx) + v01 * fx;
                const float v1 = v10 * (1.f - fx) + v11 * fx;
                const float v = v0 * (1.f - fy) + v1 * fy;
                nchw[static_cast<size_t>(c * out_sz * out_sz + oy * out_sz + ox)] = (v - kMean[c]) / kStd[c];
            }
        }
    }
    return true;
}

LightFcBox ClipBox(LightFcBox b, int width, int height, float margin = 2.f) {
    b.w = (std::max)(1.f, b.w);
    b.h = (std::max)(1.f, b.h);
    b.x = (std::max)(margin, (std::min)(static_cast<float>(width) - margin - b.w, b.x));
    b.y = (std::max)(margin, (std::min)(static_cast<float>(height) - margin - b.h, b.y));
    if (b.x + b.w > width - margin)
        b.w = (std::max)(1.f, width - margin - b.x);
    if (b.y + b.h > height - margin)
        b.h = (std::max)(1.f, height - margin - b.y);
    return b;
}

}  // namespace

struct LightFcRuntime::Impl {
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "upPlayer-lightfc"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> z_session;
    std::unique_ptr<Ort::Session> x_session;
    std::vector<std::string> z_in_names_s, z_out_names_s, x_in_names_s, x_out_names_s;
    std::vector<const char*> z_in_names, z_out_names, x_in_names, x_out_names;
    std::vector<float> z_feat;  // 1x96x8x8
    std::vector<float> hann;
#endif
    bool has_target = false;
    LightFcBox state{};
};

LightFcRuntime::LightFcRuntime() : impl_(new Impl) {
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    BuildHann2d(impl_->hann, kFeatSz);
#endif
}

LightFcRuntime::~LightFcRuntime() {
    delete impl_;
    impl_ = nullptr;
}

bool LightFcRuntime::available() const {
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    return impl_ && impl_->z_session && impl_->x_session;
#else
    return false;
#endif
}

std::wstring LightFcRuntime::last_error() const {
    return last_error_;
}

void LightFcRuntime::reset() {
    if (!impl_)
        return;
    impl_->has_target = false;
    impl_->state = {};
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    impl_->z_feat.clear();
#endif
}

bool LightFcRuntime::has_target() const {
    return impl_ && impl_->has_target;
}

LightFcBox LightFcRuntime::state() const {
    return impl_ ? impl_->state : LightFcBox{};
}

bool LightFcRuntime::load(const std::wstring& z_model, const std::wstring& x_model, std::wstring* error) {
    last_error_.clear();
    reset();
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    try {
        impl_->options.SetIntraOpNumThreads(2);
        impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        impl_->z_session = std::make_unique<Ort::Session>(impl_->env, z_model.c_str(), impl_->options);
        impl_->x_session = std::make_unique<Ort::Session>(impl_->env, x_model.c_str(), impl_->options);

        Ort::AllocatorWithDefaultOptions allocator;
        auto fill_names = [&](Ort::Session& session, std::vector<std::string>& storage,
                              std::vector<const char*>& ptrs, bool inputs) {
            storage.clear();
            ptrs.clear();
            const size_t n = inputs ? session.GetInputCount() : session.GetOutputCount();
            for (size_t i = 0; i < n; ++i) {
                auto name = inputs ? session.GetInputNameAllocated(i, allocator)
                                   : session.GetOutputNameAllocated(i, allocator);
                storage.emplace_back(name.get());
            }
            for (auto& s : storage)
                ptrs.push_back(s.c_str());
        };
        fill_names(*impl_->z_session, impl_->z_in_names_s, impl_->z_in_names, true);
        fill_names(*impl_->z_session, impl_->z_out_names_s, impl_->z_out_names, false);
        fill_names(*impl_->x_session, impl_->x_in_names_s, impl_->x_in_names, true);
        fill_names(*impl_->x_session, impl_->x_out_names_s, impl_->x_out_names, false);

        impl_->z_feat.assign(static_cast<size_t>(kZFeatC * kZFeatH * kZFeatH), 0.f);
        return true;
    } catch (const Ort::Exception& ex) {
        last_error_ = L"LightFC ONNX load failed: ";
        const std::string msg = ex.what();
        last_error_.append(msg.begin(), msg.end());
        impl_->z_session.reset();
        impl_->x_session.reset();
        if (error)
            *error = last_error_;
        return false;
    } catch (...) {
        last_error_ = L"LightFC ONNX load failed";
        impl_->z_session.reset();
        impl_->x_session.reset();
        if (error)
            *error = last_error_;
        return false;
    }
#else
    (void)z_model;
    (void)x_model;
    last_error_ = L"Built without ONNX Runtime (place third_party/onnxruntime and rebuild)";
    if (error)
        *error = last_error_;
    return false;
#endif
}

bool LightFcRuntime::initialize(const unsigned char* bgr, int width, int height, int stride,
                                const LightFcBox& init_bbox, std::wstring* error) {
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    if (!available()) {
        last_error_ = L"LightFC not loaded";
        if (error)
            *error = last_error_;
        return false;
    }
    std::vector<float> z_nchw;
    float rf = 1.f;
    if (!SampleTarget(bgr, width, height, stride, init_bbox, kTemplateFactor, kTemplate, z_nchw, &rf)) {
        last_error_ = L"LightFC template crop failed";
        if (error)
            *error = last_error_;
        return false;
    }
    try {
        std::array<int64_t, 4> shape{1, 3, kTemplate, kTemplate};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(mem, z_nchw.data(), z_nchw.size(), shape.data(),
                                                           shape.size());
        auto outputs = impl_->z_session->Run(Ort::RunOptions{nullptr}, impl_->z_in_names.data(), &input, 1,
                                             impl_->z_out_names.data(), impl_->z_out_names.size());
        if (outputs.empty() || !outputs[0].IsTensor()) {
            last_error_ = L"LightFC z output missing";
            if (error)
                *error = last_error_;
            return false;
        }
        float* data = outputs[0].GetTensorMutableData<float>();
        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const size_t n = info.GetElementCount();
        impl_->z_feat.assign(data, data + n);
        impl_->state = ClipBox(init_bbox, width, height);
        impl_->has_target = true;
        return true;
    } catch (const Ort::Exception& ex) {
        last_error_ = L"LightFC init infer failed: ";
        const std::string msg = ex.what();
        last_error_.append(msg.begin(), msg.end());
        if (error)
            *error = last_error_;
        return false;
    }
#else
    (void)bgr;
    (void)width;
    (void)height;
    (void)stride;
    (void)init_bbox;
    last_error_ = L"Built without ONNX Runtime";
    if (error)
        *error = last_error_;
    return false;
#endif
}

bool LightFcRuntime::track(const unsigned char* bgr, int width, int height, int stride, LightFcBox& out,
                           std::wstring* error) {
#ifdef UPPLAYER_HAS_ONNXRUNTIME
    if (!available() || !impl_->has_target || impl_->z_feat.empty()) {
        last_error_ = L"LightFC not initialized";
        if (error)
            *error = last_error_;
        return false;
    }
    std::vector<float> x_nchw;
    float resize_factor = 1.f;
    if (!SampleTarget(bgr, width, height, stride, impl_->state, kSearchFactor, kSearch, x_nchw,
                      &resize_factor)) {
        last_error_ = L"LightFC search crop failed";
        if (error)
            *error = last_error_;
        return false;
    }
    try {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> zshape{1, kZFeatC, kZFeatH, kZFeatH};
        std::array<int64_t, 4> xshape{1, 3, kSearch, kSearch};
        const size_t z_elems = impl_->z_feat.size();
        if (z_elems != static_cast<size_t>(kZFeatC * kZFeatH * kZFeatH)) {
            const int hw = static_cast<int>(std::lround(std::sqrt(static_cast<double>(z_elems) / kZFeatC)));
            zshape = {1, kZFeatC, hw, hw};
        }

        // Export uses input_names=["z_feat","x"]. Build tensors in session order.
        std::vector<Ort::Value> inputs;
        inputs.reserve(2);
        auto make_z = [&]() {
            return Ort::Value::CreateTensor<float>(mem, impl_->z_feat.data(), z_elems, zshape.data(), zshape.size());
        };
        auto make_x = [&]() {
            return Ort::Value::CreateTensor<float>(mem, x_nchw.data(), x_nchw.size(), xshape.data(), xshape.size());
        };
        if (impl_->x_in_names_s.size() >= 2) {
            for (const auto& n : impl_->x_in_names_s) {
                if (n == "x" || n.find("search") != std::string::npos)
                    inputs.push_back(make_x());
                else
                    inputs.push_back(make_z());
            }
        }
        if (inputs.size() != 2) {
            inputs.clear();
            inputs.push_back(make_z());
            inputs.push_back(make_x());
        }

        auto outputs = impl_->x_session->Run(Ort::RunOptions{nullptr}, impl_->x_in_names.data(), inputs.data(),
                                             inputs.size(), impl_->x_out_names.data(), impl_->x_out_names.size());
        if (outputs.size() < 3) {
            last_error_ = L"LightFC x outputs missing";
            if (error)
                *error = last_error_;
            return false;
        }

        // Map outputs by name when possible.
        float* score = nullptr;
        float* size = nullptr;
        float* offset = nullptr;
        for (size_t i = 0; i < outputs.size() && i < impl_->x_out_names_s.size(); ++i) {
            const std::string& n = impl_->x_out_names_s[i];
            float* p = outputs[i].GetTensorMutableData<float>();
            if (n.find("score") != std::string::npos)
                score = p;
            else if (n.find("size") != std::string::npos)
                size = p;
            else if (n.find("offset") != std::string::npos)
                offset = p;
        }
        if (!score)
            score = outputs[0].GetTensorMutableData<float>();
        if (!size)
            size = outputs[1].GetTensorMutableData<float>();
        if (!offset)
            offset = outputs[2].GetTensorMutableData<float>();

        // Hann window for localization; raw peak for confidence (sigmoid ∈ (0,1)).
        int best = 0;
        float best_hann = -1e30f;
        float raw_peak = 0.f;
        for (int i = 0; i < kFeatSz * kFeatSz; ++i) {
            if (score[i] > raw_peak)
                raw_peak = score[i];
            const float v = score[i] * impl_->hann[static_cast<size_t>(i)];
            if (v > best_hann) {
                best_hann = v;
                best = i;
            }
        }
        const int idx_y = best / kFeatSz;
        const int idx_x = best % kFeatSz;
        const float ox = offset[0 * kFeatSz * kFeatSz + best];
        const float oy = offset[1 * kFeatSz * kFeatSz + best];
        const float bw = size[0 * kFeatSz * kFeatSz + best];
        const float bh = size[1 * kFeatSz * kFeatSz + best];

        // Normalized cx,cy,w,h in search crop, then to crop pixels.
        const float cx_n = (static_cast<float>(idx_x) + ox) / kFeatSz;
        const float cy_n = (static_cast<float>(idx_y) + oy) / kFeatSz;
        float cx = cx_n * kSearch / resize_factor;
        float cy = cy_n * kSearch / resize_factor;
        float w = bw * kSearch / resize_factor;
        float h = bh * kSearch / resize_factor;

        const float cx_prev = impl_->state.x + 0.5f * impl_->state.w;
        const float cy_prev = impl_->state.y + 0.5f * impl_->state.h;
        const float half_side = 0.5f * kSearch / resize_factor;
        const float cx_real = cx + (cx_prev - half_side);
        const float cy_real = cy + (cy_prev - half_side);

        LightFcBox box;
        box.x = cx_real - 0.5f * w;
        box.y = cy_real - 0.5f * h;
        box.w = w;
        box.h = h;
        box.score = raw_peak;
        box = ClipBox(box, width, height);

        // Low confidence: keep last good state so the search window does not drift.
        constexpr float kMinAcceptScore = 0.6f;
        if (raw_peak >= kMinAcceptScore) {
            impl_->state = box;
            out = box;
        } else {
            out = impl_->state;
            out.score = raw_peak;
        }
        return true;
    } catch (const Ort::Exception& ex) {
        last_error_ = L"LightFC track infer failed: ";
        const std::string msg = ex.what();
        last_error_.append(msg.begin(), msg.end());
        if (error)
            *error = last_error_;
        return false;
    }
#else
    (void)bgr;
    (void)width;
    (void)height;
    (void)stride;
    (void)out;
    last_error_ = L"Built without ONNX Runtime";
    if (error)
        *error = last_error_;
    return false;
#endif
}
