#include "lightfc_worker.hpp"

#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

std::wstring ExeDirectory() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    std::wstring path(buffer, length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        path.resize(slash);
    return path;
}

bool FileExists(const std::wstring& path) {
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool FindModels(std::wstring& z_path, std::wstring& x_path) {
    const std::wstring root = ExeDirectory();
    const std::wstring pairs[][2] = {
        {root + L"\\lightfc\\lightfc_z.onnx", root + L"\\lightfc\\lightfc_x.onnx"},
        {root + L"\\models\\lightfc_z.onnx", root + L"\\models\\lightfc_x.onnx"},
        {root + L"\\lightfc_z.onnx", root + L"\\lightfc_x.onnx"},
        {root + L"\\..\\..\\third_party\\lightfc\\lightfc_z.onnx",
         root + L"\\..\\..\\third_party\\lightfc\\lightfc_x.onnx"},
    };
    for (const auto& p : pairs) {
        wchar_t zfull[MAX_PATH], xfull[MAX_PATH];
        if (GetFullPathNameW(p[0].c_str(), MAX_PATH, zfull, nullptr) && FileExists(zfull) &&
            GetFullPathNameW(p[1].c_str(), MAX_PATH, xfull, nullptr) && FileExists(xfull)) {
            z_path = zfull;
            x_path = xfull;
            return true;
        }
    }
    return false;
}

}  // namespace

LightFcWorker::LightFcWorker(HWND notify) : notify_(notify) {
    run_ = true;
    thread_ = std::thread([this] { thread_main(); });
}

LightFcWorker::~LightFcWorker() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        run_ = false;
        mode_ = LightFcMode::Off;
        clear_jobs_locked();
        wake_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

const wchar_t* LightFcWorker::mode_label(LightFcMode mode) {
    switch (mode) {
    case LightFcMode::Arm:
        return L"Click target";
    case LightFcMode::Tracking:
        return L"Tracking";
    default:
        return L"Off";
    }
}

void LightFcWorker::clear_jobs_locked() {
    if (!awaiting_path_.empty()) {
        DeleteFileW(awaiting_path_.c_str());
        awaiting_path_.clear();
    }
    awaiting_tick_ = 0;
    awaiting_posted_ = false;
    awaiting_is_init_ = false;
    for (auto& job : queue_)
        DeleteFileW(job.path.c_str());
    queue_.clear();
    busy_ = false;
    pending_init_ = false;
}

bool LightFcWorker::file_ready(const std::wstring& path) const {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return false;
    if (info.nFileSizeLow <= 128 && info.nFileSizeHigh == 0)
        return false;
    if (awaiting_tick_ == 0)
        return true;
    return GetTickCount64() - awaiting_tick_ >= 40;
}

bool LightFcWorker::promote_awaiting_locked() {
    if (awaiting_path_.empty() || !awaiting_posted_)
        return false;
    if (!file_ready(awaiting_path_)) {
        if (awaiting_tick_ != 0 && GetTickCount64() - awaiting_tick_ > 3000) {
            DeleteFileW(awaiting_path_.c_str());
            awaiting_path_.clear();
            awaiting_tick_ = 0;
            awaiting_posted_ = false;
            awaiting_is_init_ = false;
            ++skipped_;
        }
        return false;
    }

    Job job;
    job.path = awaiting_path_;
    job.capture_tick = awaiting_tick_;
    job.is_init = awaiting_is_init_;
    job.init_box = awaiting_init_box_;
    awaiting_path_.clear();
    awaiting_tick_ = 0;
    awaiting_posted_ = false;
    awaiting_is_init_ = false;

    for (auto& old : queue_) {
        DeleteFileW(old.path.c_str());
        ++skipped_;
    }
    queue_.clear();
    queue_.push_back(std::move(job));
    wake_ = true;
    return true;
}

void LightFcWorker::set_mode(LightFcMode mode) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode_ == mode)
            return;
        mode_ = mode;
        skipped_ = 0;
        lost_streak_ = 0;
        if (mode == LightFcMode::Off) {
            clear_jobs_locked();
            runtime_.reset();
            if (!overlay_bgra_path_.empty()) {
                DeleteFileW(overlay_bgra_path_.c_str());
                overlay_bgra_path_.clear();
            }
            overlay_w_ = overlay_h_ = overlay_stride_ = 0;
            status_text_.clear();
            last_box_ = {};
        } else if (mode == LightFcMode::Arm) {
            runtime_.reset();
            pending_init_ = false;
            if (!overlay_bgra_path_.empty()) {
                DeleteFileW(overlay_bgra_path_.c_str());
                overlay_bgra_path_.clear();
            }
            overlay_w_ = overlay_h_ = overlay_stride_ = 0;
            last_box_ = {};
            status_text_ = L"LightFC: click target";
            wake_ = true;
        } else {
            status_text_ = L"LightFC: tracking";
            wake_ = true;
        }
    }
    cv_.notify_all();
    if (notify_)
        PostMessageW(notify_, WM_LIGHTFC_UPDATE, 0, 0);
}

LightFcMode LightFcWorker::mode() const {
    std::lock_guard<std::mutex> lock(mu_);
    return mode_;
}

bool LightFcWorker::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return busy_ || !queue_.empty() || !awaiting_path_.empty();
}

bool LightFcWorker::init_at(const LightFcBox& box) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode_ == LightFcMode::Off)
            return false;
        pending_init_ = true;
        pending_init_box_ = box;
        lost_streak_ = 0;
        mode_ = LightFcMode::Arm;  // stay armed until init capture completes
        status_text_ = L"LightFC: initializing…";
        wake_ = true;
    }
    cv_.notify_all();
    if (notify_)
        PostMessageW(notify_, WM_LIGHTFC_UPDATE, 0, 0);
    return true;
}

void LightFcWorker::pump() {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode_ == LightFcMode::Off)
            return;
        wake = promote_awaiting_locked();
    }
    if (wake)
        cv_.notify_all();
}

std::wstring LightFcWorker::try_begin_capture() {
    std::lock_guard<std::mutex> lock(mu_);
    if (mode_ == LightFcMode::Off)
        return {};
    // Only capture while tracking or when an init is pending.
    if (mode_ == LightFcMode::Arm && !pending_init_)
        return {};

    promote_awaiting_locked();
    if (!awaiting_path_.empty())
        return {};
    if (busy_ || !queue_.empty()) {
        ++skipped_;
        return {};
    }

    wchar_t temp_dir[MAX_PATH] = {};
    wchar_t temp_file[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_dir);
    if (!GetTempFileNameW(temp_dir, L"uplfc", 0, temp_file))
        return {};
    DeleteFileW(temp_file);
    std::wstring path = std::wstring(temp_file) + L".jpg";
    awaiting_path_ = path;
    awaiting_tick_ = GetTickCount64();
    awaiting_posted_ = false;
    awaiting_is_init_ = pending_init_;
    awaiting_init_box_ = pending_init_box_;
    if (pending_init_)
        pending_init_ = false;
    return path;
}

void LightFcWorker::notify_capture_requested(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode_ == LightFcMode::Off) {
            DeleteFileW(path.c_str());
            awaiting_path_.clear();
            awaiting_tick_ = 0;
            awaiting_posted_ = false;
            return;
        }
        if (awaiting_path_ == path || awaiting_path_.empty()) {
            awaiting_path_ = path;
            if (awaiting_tick_ == 0)
                awaiting_tick_ = GetTickCount64();
            awaiting_posted_ = true;
        }
        promote_awaiting_locked();
        wake_ = true;
    }
    cv_.notify_all();
}

std::wstring LightFcWorker::overlay_bgra_path() const {
    std::lock_guard<std::mutex> lock(mu_);
    return overlay_bgra_path_;
}
int LightFcWorker::overlay_width() const {
    std::lock_guard<std::mutex> lock(mu_);
    return overlay_w_;
}
int LightFcWorker::overlay_height() const {
    std::lock_guard<std::mutex> lock(mu_);
    return overlay_h_;
}
int LightFcWorker::overlay_stride() const {
    std::lock_guard<std::mutex> lock(mu_);
    return overlay_stride_;
}
std::wstring LightFcWorker::status_text() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_text_;
}
LightFcBox LightFcWorker::last_box() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_box_;
}
int LightFcWorker::skipped_frames() const {
    std::lock_guard<std::mutex> lock(mu_);
    return skipped_;
}

bool LightFcWorker::load_model_locked(std::wstring* error) {
    if (model_ready_ && runtime_.available())
        return true;
    std::wstring z, x;
    if (!FindModels(z, x)) {
        if (error)
            *error = L"lightfc_z/x.onnx missing (run scripts/fetch-lightfc-deps.ps1)";
        return false;
    }
    if (!runtime_.load(z, x, error))
        return false;
    model_ready_ = true;
    return true;
}

bool LightFcWorker::decode_jpeg_bgr(const std::wstring& path, std::vector<unsigned char>& bgr, int& w,
                                    int& h, int& stride, std::wstring* error) {
    bgr.clear();
    w = h = stride = 0;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_init = SUCCEEDED(hr);
    const bool com_already = hr == S_FALSE;

    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (error)
            *error = L"WIC init failed";
        if (com_init && !com_already)
            CoUninitialize();
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                            &decoder);
    if (FAILED(hr) || !decoder) {
        factory->Release();
        if (error)
            *error = L"JPEG open failed";
        if (com_init && !com_already)
            CoUninitialize();
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        decoder->Release();
        factory->Release();
        if (error)
            *error = L"JPEG frame failed";
        if (com_init && !com_already)
            CoUninitialize();
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter ||
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        if (converter)
            converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        if (error)
            *error = L"JPEG convert failed";
        if (com_init && !com_already)
            CoUninitialize();
        return false;
    }

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    if (width == 0 || height == 0 || width > 8192 || height > 8192) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        if (error)
            *error = L"JPEG size invalid";
        if (com_init && !com_already)
            CoUninitialize();
        return false;
    }

    stride = static_cast<int>(width * 3);
    bgr.resize(static_cast<size_t>(stride) * height);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(bgr.size()), bgr.data());
    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    if (com_init && !com_already)
        CoUninitialize();
    if (FAILED(hr)) {
        bgr.clear();
        if (error)
            *error = L"JPEG copy failed";
        return false;
    }
    w = static_cast<int>(width);
    h = static_cast<int>(height);
    return true;
}

std::wstring LightFcWorker::build_hud_text(LightFcMode mode, float score) {
    wchar_t hud[80];
    if (mode == LightFcMode::Tracking)
        swprintf_s(hud, L"LightFC  score %.2f", static_cast<double>(score));
    else if (mode == LightFcMode::Arm)
        swprintf_s(hud, L"LightFC  click target");
    else
        swprintf_s(hud, L"LightFC  off");
    return hud;
}

bool LightFcWorker::write_box_bgra(const std::wstring& path, int src_w, int src_h, const LightFcBox& box,
                                   const std::wstring& hud, int* out_w, int* out_h) {
    if (path.empty() || src_w <= 0 || src_h <= 0)
        return false;

    int width = src_w;
    int height = src_h;
    constexpr int kTargetShort = 1080;
    if ((std::min)(src_w, src_h) < kTargetShort) {
        if (src_h <= src_w) {
            height = kTargetShort;
            width = static_cast<int>(std::llround(static_cast<double>(src_w) * kTargetShort / src_h));
        } else {
            width = kTargetShort;
            height = static_cast<int>(std::llround(static_cast<double>(src_h) * kTargetShort / src_w));
        }
    }
    width = (std::min)(width, 1920);
    height = (std::min)(height, 1920);
    const double sx = static_cast<double>(width) / src_w;
    const double sy = static_cast<double>(height) / src_h;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !dib || !bits) {
        if (dib)
            DeleteObject(dib);
        if (dc)
            DeleteDC(dc);
        if (screen)
            ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old_bmp = SelectObject(dc, dib);
    const int stride = width * 4;
    ZeroMemory(bits, static_cast<size_t>(stride) * height);

    auto put = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return;
        unsigned char* p =
            static_cast<unsigned char*>(bits) + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = 255;
    };
    // Cyan box
    auto hline = [&](int x1, int x2, int y, int t) {
        if (x1 > x2)
            std::swap(x1, x2);
        for (int dy = 0; dy < t; ++dy)
            for (int x = x1; x <= x2; ++x)
                put(x, y + dy, 255, 220, 0);
    };
    auto vline = [&](int y1, int y2, int x, int t) {
        if (y1 > y2)
            std::swap(y1, y2);
        for (int dx = 0; dx < t; ++dx)
            for (int y = y1; y <= y2; ++y)
                put(x + dx, y, 255, 220, 0);
    };

    const int thickness = (std::max)(3, height / 220);
    if (box.w > 1.f && box.h > 1.f) {
        int x1 = static_cast<int>(std::lround(box.x * sx));
        int y1 = static_cast<int>(std::lround(box.y * sy));
        int x2 = static_cast<int>(std::lround((box.x + box.w) * sx));
        int y2 = static_cast<int>(std::lround((box.y + box.h) * sy));
        x1 = (std::max)(0, (std::min)(width - 1, x1));
        y1 = (std::max)(0, (std::min)(height - 1, y1));
        x2 = (std::max)(0, (std::min)(width - 1, x2));
        y2 = (std::max)(0, (std::min)(height - 1, y2));
        hline(x1, x2, y1, thickness);
        hline(x1, x2, y2 - thickness + 1, thickness);
        vline(y1, y2, x1, thickness);
        vline(y1, y2, x2 - thickness + 1, thickness);
    }

    if (!hud.empty()) {
        const int font_px = (std::max)(26, height / 36);
        HFONT font = CreateFontW(-font_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SIZE sz{};
        GetTextExtentPoint32W(dc, hud.c_str(), static_cast<int>(hud.size()), &sz);
        RECT chip{16, 16, 16 + sz.cx + 24, 16 + sz.cy + 16};
        HBRUSH brush = CreateSolidBrush(RGB(8, 40, 56));
        FillRect(dc, &chip, brush);
        DeleteObject(brush);
        RECT text_rc = chip;
        text_rc.left += 12;
        text_rc.top += 8;
        text_rc.right -= 12;
        text_rc.bottom -= 8;
        SetTextColor(dc, RGB(0, 230, 255));
        DrawTextW(dc, hud.c_str(), -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (font) {
            SelectObject(dc, old_font);
            DeleteObject(font);
        }
    }

    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(file, bits, static_cast<DWORD>(stride * height), &written, nullptr) &&
             written == static_cast<DWORD>(stride * height);
        CloseHandle(file);
    }
    SelectObject(dc, old_bmp);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    if (ok) {
        if (out_w)
            *out_w = width;
        if (out_h)
            *out_h = height;
    }
    return ok;
}

void LightFcWorker::thread_main() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return !run_ || wake_ || !queue_.empty(); });
            if (!run_)
                break;
            wake_ = false;
            if (mode_ == LightFcMode::Off) {
                clear_jobs_locked();
                continue;
            }
            promote_awaiting_locked();
            if (queue_.empty())
                continue;
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }

        std::wstring err;
        if (!load_model_locked(&err)) {
            std::lock_guard<std::mutex> lock(mu_);
            status_text_ = err.empty() ? L"LightFC: model missing" : err;
            busy_ = false;
            DeleteFileW(job.path.c_str());
            if (notify_)
                PostMessageW(notify_, WM_LIGHTFC_UPDATE, 0, 0);
            continue;
        }

        std::vector<unsigned char> bgr;
        int w = 0, h = 0, stride = 0;
        if (!decode_jpeg_bgr(job.path, bgr, w, h, stride, &err)) {
            std::lock_guard<std::mutex> lock(mu_);
            status_text_ = err.empty() ? L"LightFC: decode failed" : err;
            busy_ = false;
            DeleteFileW(job.path.c_str());
            if (notify_)
                PostMessageW(notify_, WM_LIGHTFC_UPDATE, 0, 0);
            continue;
        }
        DeleteFileW(job.path.c_str());

        const auto t0 = std::chrono::steady_clock::now();
        LightFcBox box{};
        bool ok = false;
        if (job.is_init) {
            ok = runtime_.initialize(bgr.data(), w, h, stride, job.init_box, &err);
            if (ok)
                box = runtime_.state();
        } else {
            ok = runtime_.track(bgr.data(), w, h, stride, box, &err);
        }
        const int infer_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                .count());

        wchar_t temp_dir[MAX_PATH] = {};
        wchar_t temp_file[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp_dir);
        GetTempFileNameW(temp_dir, L"uplfc", 0, temp_file);
        DeleteFileW(temp_file);
        const std::wstring bgra_path = std::wstring(temp_file) + L".bgra";

        std::wstring hud;
        int ow = 0, oh = 0;
        bool lost = false;
        bool draw = false;
        LightFcBox draw_box{};
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (ok) {
                if (job.is_init) {
                    mode_ = LightFcMode::Tracking;
                    lost_streak_ = 0;
                    last_box_ = box;
                    last_infer_ms_ = infer_ms;
                    hud = build_hud_text(mode_, box.score);
                    status_text_ = hud;
                    draw = true;
                    draw_box = box;
                } else if (mode_ == LightFcMode::Tracking) {
                    last_infer_ms_ = infer_ms;
                    if (box.score < kLostScore) {
                        ++lost_streak_;
                        hud = build_hud_text(mode_, box.score);
                        status_text_ = hud + L"  (weak)";
                        draw = last_box_.w > 1.f && last_box_.h > 1.f;
                        draw_box = last_box_;
                        if (lost_streak_ >= kLostFrames) {
                            lost = true;
                            draw = false;
                            // Stay armed so the next click reselects (not pause).
                            mode_ = LightFcMode::Arm;
                            lost_streak_ = 0;
                            pending_init_ = false;
                            runtime_.reset();
                            clear_jobs_locked();
                            if (!overlay_bgra_path_.empty()) {
                                DeleteFileW(overlay_bgra_path_.c_str());
                                overlay_bgra_path_.clear();
                            }
                            overlay_w_ = overlay_h_ = overlay_stride_ = 0;
                            last_box_ = {};
                            status_text_ = L"LightFC: lost — click to reselect";
                            hud.clear();
                        }
                    } else {
                        lost_streak_ = 0;
                        last_box_ = box;
                        hud = build_hud_text(mode_, box.score);
                        status_text_ = hud;
                        draw = true;
                        draw_box = box;
                    }
                }
            } else {
                status_text_ = err.empty() ? L"LightFC: infer failed" : err;
            }
        }

        if (draw && write_box_bgra(bgra_path, w, h, draw_box, hud, &ow, &oh)) {
            std::lock_guard<std::mutex> lock(mu_);
            if (!overlay_bgra_path_.empty())
                DeleteFileW(overlay_bgra_path_.c_str());
            overlay_bgra_path_ = bgra_path;
            overlay_w_ = ow;
            overlay_h_ = oh;
            overlay_stride_ = ow * 4;
        } else {
            DeleteFileW(bgra_path.c_str());
        }
        (void)lost;

        {
            std::lock_guard<std::mutex> lock(mu_);
            busy_ = false;
        }
        if (notify_)
            PostMessageW(notify_, WM_LIGHTFC_UPDATE, 0, 0);
    }
}
