#include "yolo_worker.hpp"

#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <utility>

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

std::wstring FindModel() {
    const std::wstring root = ExeDirectory();
    const std::wstring candidates[] = {
        root + L"\\yolo26\\yolo26n.onnx",
        root + L"\\models\\yolo26n.onnx",
        root + L"\\yolo26n.onnx",
        root + L"\\..\\..\\third_party\\yolo26\\yolo26n.onnx",
    };
    for (const auto& c : candidates) {
        wchar_t full[MAX_PATH];
        if (GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr) && FileExists(full))
            return full;
    }
    return {};
}

}  // namespace

YoloWorker::YoloWorker(HWND notify) : notify_(notify) {
    run_ = true;
    thread_ = std::thread([this] { thread_main(); });
}

YoloWorker::~YoloWorker() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        run_ = false;
        mode_ = YoloMode::Off;
        clear_jobs_locked();
        wake_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

const wchar_t* YoloWorker::mode_label(YoloMode mode) {
    switch (mode) {
    case YoloMode::On:
        return L"On";
    default:
        return L"Off";
    }
}

void YoloWorker::clear_jobs_locked() {
    if (!awaiting_path_.empty()) {
        DeleteFileW(awaiting_path_.c_str());
        awaiting_path_.clear();
    }
    awaiting_tick_ = 0;
    awaiting_posted_ = false;
    for (auto& job : queue_)
        DeleteFileW(job.path.c_str());
    queue_.clear();
    busy_ = false;
}

bool YoloWorker::file_ready(const std::wstring& path) const {
    // No Sleep here — may run under mu_ (UI tick). Require size + short age since request.
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return false;
    if (info.nFileSizeLow <= 128 && info.nFileSizeHigh == 0)
        return false;
    if (awaiting_tick_ == 0)
        return true;
    return GetTickCount64() - awaiting_tick_ >= 40;
}

bool YoloWorker::promote_awaiting_locked() {
    if (awaiting_path_.empty() || !awaiting_posted_)
        return false;
    if (!file_ready(awaiting_path_)) {
        // Give up on stale screenshot requests.
        if (awaiting_tick_ != 0 && GetTickCount64() - awaiting_tick_ > 3000) {
            DeleteFileW(awaiting_path_.c_str());
            awaiting_path_.clear();
            awaiting_tick_ = 0;
            awaiting_posted_ = false;
            ++skipped_;
        }
        return false;
    }

    Job job;
    job.path = awaiting_path_;
    job.capture_tick = awaiting_tick_;
    awaiting_path_.clear();
    awaiting_tick_ = 0;
    awaiting_posted_ = false;

    // Keep only the latest frame.
    for (auto& old : queue_) {
        DeleteFileW(old.path.c_str());
        ++skipped_;
    }
    queue_.clear();
    queue_.push_back(std::move(job));
    wake_ = true;
    return true;
}

void YoloWorker::set_mode(YoloMode mode) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode_ == mode)
            return;
        mode_ = mode;
        skipped_ = 0;
        if (mode == YoloMode::Off) {
            clear_jobs_locked();
            overlay_ass_.clear();
            if (!overlay_bgra_path_.empty()) {
                DeleteFileW(overlay_bgra_path_.c_str());
                overlay_bgra_path_.clear();
            }
            overlay_w_ = overlay_h_ = overlay_stride_ = 0;
            status_text_.clear();
            last_count_ = 0;
            last_delay_ms_ = 0;
            last_infer_ms_ = 0;
        } else {
            status_text_ = std::wstring(L"YOLO26: ") + mode_label(mode);
            wake_ = true;
        }
    }
    cv_.notify_all();
    if (notify_)
        PostMessageW(notify_, WM_YOLO_UPDATE, 0, 0);
}

YoloMode YoloWorker::mode() const {
    std::lock_guard<std::mutex> lock(mu_);
    return mode_;
}

bool YoloWorker::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return busy_ || !queue_.empty() || !awaiting_path_.empty();
}

void YoloWorker::pump() {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode_ == YoloMode::Off)
            return;
        wake = promote_awaiting_locked();
    }
    if (wake)
        cv_.notify_all();
}

std::wstring YoloWorker::try_begin_capture() {
    std::lock_guard<std::mutex> lock(mu_);
    if (mode_ == YoloMode::Off)
        return {};

    promote_awaiting_locked();

    // One mpv screenshot in flight until its JPEG is ready (prevents flood → silent drops).
    if (!awaiting_path_.empty())
        return {};

    if (busy_ || !queue_.empty()) {
        ++skipped_;
        return {};
    }

    wchar_t temp_dir[MAX_PATH] = {};
    wchar_t temp_file[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_dir);
    if (!GetTempFileNameW(temp_dir, L"upyolo", 0, temp_file))
        return {};
    DeleteFileW(temp_file);
    std::wstring path = std::wstring(temp_file) + L".jpg";
    awaiting_path_ = path;
    awaiting_tick_ = GetTickCount64();
    awaiting_posted_ = false;
    return path;
}

void YoloWorker::notify_capture_requested(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode_ == YoloMode::Off) {
            DeleteFileW(path.c_str());
            awaiting_path_.clear();
            awaiting_tick_ = 0;
            awaiting_posted_ = false;
            return;
        }
        // Keep awaiting until JPEG exists — do NOT enqueue yet.
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

std::string YoloWorker::overlay_ass() const {
    std::lock_guard<std::mutex> lock(mu_);
    return overlay_ass_;
}

std::wstring YoloWorker::overlay_bgra_path() const {
    std::lock_guard<std::mutex> lock(mu_);
    return overlay_bgra_path_;
}

int YoloWorker::overlay_width() const {
    std::lock_guard<std::mutex> lock(mu_);
    return overlay_w_;
}

int YoloWorker::overlay_height() const {
    std::lock_guard<std::mutex> lock(mu_);
    return overlay_h_;
}

int YoloWorker::overlay_stride() const {
    std::lock_guard<std::mutex> lock(mu_);
    return overlay_stride_;
}

std::wstring YoloWorker::status_text() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_text_;
}

int YoloWorker::last_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_count_;
}

int YoloWorker::skipped_frames() const {
    std::lock_guard<std::mutex> lock(mu_);
    return skipped_;
}

int YoloWorker::queued_frames() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(queue_.size());
}

int YoloWorker::last_delay_ms() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_delay_ms_;
}

int YoloWorker::last_infer_ms() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_infer_ms_;
}

bool YoloWorker::load_model_locked(std::wstring* error) {
    if (model_ready_ && runtime_.available())
        return true;
    const std::wstring model = FindModel();
    if (model.empty()) {
        if (error)
            *error = L"yolo26n.onnx missing (third_party/yolo26 or next to exe)";
        return false;
    }
    if (!runtime_.load(model, error))
        return false;
    model_ready_ = true;
    return true;
}

bool YoloWorker::decode_jpeg_bgr(const std::wstring& path, std::vector<unsigned char>& bgr, int& w,
                                 int& h, int& stride, std::wstring* error) {
    bgr.clear();
    w = h = stride = 0;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_init = SUCCEEDED(hr);
    const bool com_already = hr == S_FALSE;

    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (error)
            *error = L"WIC init failed";
        if (com_init && !com_already)
            CoUninitialize();
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &decoder);
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
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
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
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(bgr.size()),
                               bgr.data());
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

std::wstring YoloWorker::build_hud_text(int det_count) {
    wchar_t hud[64];
    swprintf_s(hud, L"YOLO26  det %d", det_count);
    return hud;
}

bool YoloWorker::write_boxes_bgra(const std::wstring& path, int src_w, int src_h,
                                  const std::vector<YoloDetection>& dets, const std::wstring& hud,
                                  int* out_w, int* out_h) {
    if (path.empty() || src_w <= 0 || src_h <= 0)
        return false;

    // Upscale canvas so text is rasterized near display DPI instead of tiny camera res (e.g. 580px).
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
    bmi.bmiHeader.biHeight = -height;  // top-down
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
    auto hline = [&](int x1, int x2, int y, int t) {
        if (x1 > x2)
            std::swap(x1, x2);
        for (int dy = 0; dy < t; ++dy)
            for (int x = x1; x <= x2; ++x)
                put(x, y + dy, 0, 255, 0);
    };
    auto vline = [&](int y1, int y2, int x, int t) {
        if (y1 > y2)
            std::swap(y1, y2);
        for (int dx = 0; dx < t; ++dx)
            for (int y = y1; y <= y2; ++y)
                put(x + dx, y, 0, 255, 0);
    };

    const int thickness = (std::max)(3, height / 220);
    const int font_px = (std::max)(28, height / 32);
    // ANTIALIASED on opaque chips — avoid ClearType (fringing/blur on transparent overlays).
    HFONT font = CreateFontW(-font_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_LEFT | TA_TOP);

    for (const auto& d : dets) {
        int x1 = static_cast<int>(std::lround(d.x1 * sx));
        int y1 = static_cast<int>(std::lround(d.y1 * sy));
        int x2 = static_cast<int>(std::lround(d.x2 * sx));
        int y2 = static_cast<int>(std::lround(d.y2 * sy));
        if (x2 < x1)
            std::swap(x1, x2);
        if (y2 < y1)
            std::swap(y1, y2);
        x1 = (std::max)(0, (std::min)(width - 1, x1));
        y1 = (std::max)(0, (std::min)(height - 1, y1));
        x2 = (std::max)(0, (std::min)(width - 1, x2));
        y2 = (std::max)(0, (std::min)(height - 1, y2));
        if (x2 - x1 < 2 || y2 - y1 < 2)
            continue;
        hline(x1, x2, y1, thickness);
        hline(x1, x2, y2 - thickness + 1, thickness);
        vline(y1, y2, x1, thickness);
        vline(y1, y2, x2 - thickness + 1, thickness);

        wchar_t label[96];
        swprintf_s(label, L"%hs  %.0f%%", d.label ? d.label : "obj",
                   static_cast<double>(d.score * 100.f));
        SIZE sz{};
        GetTextExtentPoint32W(dc, label, lstrlenW(label), &sz);
        const int pad_x = 10;
        const int pad_y = 6;
        const int bar_h = sz.cy + pad_y * 2;
        const int bar_w = sz.cx + pad_x * 2;
        int bar_y = y1 - bar_h - 2;
        if (bar_y < 0)
            bar_y = y1 + thickness + 2;
        int bar_x = x1;
        if (bar_x + bar_w > width)
            bar_x = (std::max)(0, width - bar_w);
        RECT chip{bar_x, bar_y, bar_x + bar_w, bar_y + bar_h};
        HBRUSH chip_brush = CreateSolidBrush(RGB(16, 140, 48));
        FillRect(dc, &chip, chip_brush);
        DeleteObject(chip_brush);
        RECT text_rc = chip;
        text_rc.left += pad_x;
        text_rc.top += pad_y;
        text_rc.right -= pad_x;
        text_rc.bottom -= pad_y;
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, label, -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    if (!hud.empty()) {
        const int hud_font_px = (std::max)(26, font_px - 2);
        HFONT hud_font =
            CreateFontW(-hud_font_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ prev = hud_font ? SelectObject(dc, hud_font) : nullptr;
        SIZE sz{};
        GetTextExtentPoint32W(dc, hud.c_str(), static_cast<int>(hud.size()), &sz);
        RECT chip{16, 16, 16 + sz.cx + 24, 16 + sz.cy + 16};
        HBRUSH brush = CreateSolidBrush(RGB(12, 28, 28));
        FillRect(dc, &chip, brush);
        DeleteObject(brush);
        RECT text_rc = chip;
        text_rc.left += 12;
        text_rc.top += 8;
        text_rc.right -= 12;
        text_rc.bottom -= 8;
        SetTextColor(dc, RGB(80, 255, 210));
        DrawTextW(dc, hud.c_str(), -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (hud_font) {
            SelectObject(dc, prev);
            DeleteObject(hud_font);
        }
    }

    auto* px = static_cast<unsigned char*>(bits);
    for (int i = 0; i < width * height; ++i) {
        unsigned char* p = px + static_cast<size_t>(i) * 4;
        if (p[0] | p[1] | p[2] | p[3]) {
            p[3] = 255;  // already opaque RGB; keep straight→premultiplied identical at a=255
        } else {
            p[3] = 0;
        }
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(file, bits, static_cast<DWORD>(stride) * height, &written, nullptr) == TRUE &&
             written == static_cast<DWORD>(stride) * height;
        CloseHandle(file);
    }

    if (font) {
        SelectObject(dc, old_font);
        DeleteObject(font);
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

void YoloWorker::thread_main() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                if (!run_)
                    return true;
                if (mode_ == YoloMode::Off)
                    return false;
                promote_awaiting_locked();
                return !queue_.empty() && !busy_;
            });
            if (!run_)
                return;
            if (mode_ == YoloMode::Off)
                continue;
            promote_awaiting_locked();
            if (queue_.empty() || busy_)
                continue;
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            status_text_.clear();
        }

        std::wstring err;
        if (!load_model_locked(&err)) {
            DeleteFileW(job.path.c_str());
            std::lock_guard<std::mutex> lock(mu_);
            busy_ = false;
            status_text_ = err.empty() ? runtime_.last_error() : err;
            if (notify_)
                PostMessageW(notify_, WM_YOLO_UPDATE, 0, 0);
            Sleep(400);
            continue;
        }

        std::vector<unsigned char> bgr;
        int w = 0, h = 0, stride = 0;
        if (!decode_jpeg_bgr(job.path, bgr, w, h, stride, &err)) {
            DeleteFileW(job.path.c_str());
            std::lock_guard<std::mutex> lock(mu_);
            busy_ = false;
            status_text_ = err;
            if (notify_)
                PostMessageW(notify_, WM_YOLO_UPDATE, 0, 0);
            continue;
        }
        DeleteFileW(job.path.c_str());

        int skipped_snapshot = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            skipped_snapshot = skipped_;
        }

        std::vector<YoloDetection> dets;
        const ULONGLONG infer_t0 = GetTickCount64();
        const bool ok = runtime_.infer_bgr(bgr.data(), w, h, stride, dets, 0.35f);
        const ULONGLONG infer_t1 = GetTickCount64();
        const int infer_ms = static_cast<int>(infer_t1 - infer_t0);
        const int delay_ms = job.capture_tick > 0 ? static_cast<int>(infer_t1 - job.capture_tick)
                                                  : infer_ms;

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (mode_ == YoloMode::Off) {
                busy_ = false;
                overlay_ass_.clear();
                continue;
            }
            last_infer_ms_ = infer_ms;
            last_delay_ms_ = delay_ms;
            if (!ok) {
                status_text_ = runtime_.last_error();
            } else {
                overlay_ass_.clear();  // labels/HUD are baked into the BGRA overlay
                last_count_ = static_cast<int>(dets.size());
                status_text_.clear();
                const std::wstring hud = build_hud_text(last_count_);

                wchar_t temp_dir[MAX_PATH] = {};
                wchar_t temp_file[MAX_PATH] = {};
                GetTempPathW(MAX_PATH, temp_dir);
                std::wstring bgra_path;
                if (GetTempFileNameW(temp_dir, L"upbox", 0, temp_file)) {
                    DeleteFileW(temp_file);
                    bgra_path = std::wstring(temp_file) + L".bgra";
                    int bw = 0, bh = 0;
                    if (write_boxes_bgra(bgra_path, w, h, dets, hud, &bw, &bh)) {
                        if (!overlay_bgra_path_.empty() && overlay_bgra_path_ != bgra_path)
                            DeleteFileW(overlay_bgra_path_.c_str());
                        overlay_bgra_path_ = bgra_path;
                        overlay_w_ = bw;
                        overlay_h_ = bh;
                        overlay_stride_ = bw * 4;
                    } else {
                        DeleteFileW(bgra_path.c_str());
                    }
                }
            }
            busy_ = false;
            promote_awaiting_locked();
        }
        cv_.notify_all();
        if (notify_)
            PostMessageW(notify_, WM_YOLO_UPDATE, 0, 0);
    }
}
