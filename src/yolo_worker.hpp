#pragma once

#include "yolo_runtime.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class YoloMode {
    Off = 0,
    // Drop captures when busy — overlay stays near live; never stall playback.
    On = 1,
};

// Async YOLO26 overlay. Playback is never blocked; busy frames are skipped.
class YoloWorker {
public:
    static constexpr UINT WM_YOLO_UPDATE = WM_APP + 41;

    explicit YoloWorker(HWND notify);
    ~YoloWorker();

    YoloWorker(const YoloWorker&) = delete;
    YoloWorker& operator=(const YoloWorker&) = delete;

    void set_mode(YoloMode mode);
    YoloMode mode() const;
    bool enabled() const { return mode() != YoloMode::Off; }
    bool busy() const;

    void pump();
    std::wstring try_begin_capture();
    void notify_capture_requested(const std::wstring& path);

    std::string overlay_ass() const;
    std::wstring overlay_bgra_path() const;
    int overlay_width() const;
    int overlay_height() const;
    int overlay_stride() const;
    std::wstring status_text() const;
    int last_count() const;
    int skipped_frames() const;
    int queued_frames() const;
    int last_delay_ms() const;
    int last_infer_ms() const;
    static const wchar_t* mode_label(YoloMode mode);

private:
    struct Job {
        std::wstring path;
        ULONGLONG capture_tick = 0;
    };

    void thread_main();
    void clear_jobs_locked();
    bool file_ready(const std::wstring& path) const;
    bool promote_awaiting_locked();
    bool load_model_locked(std::wstring* error);
    bool decode_jpeg_bgr(const std::wstring& path, std::vector<unsigned char>& bgr, int& w, int& h,
                         int& stride, std::wstring* error);
    static std::wstring build_hud_text(int det_count);
    static bool write_boxes_bgra(const std::wstring& path, int src_w, int src_h,
                                 const std::vector<YoloDetection>& dets, const std::wstring& hud,
                                 int* out_w, int* out_h);

    HWND notify_ = nullptr;
    YoloRuntime runtime_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> run_{false};
    YoloMode mode_ = YoloMode::Off;
    bool busy_ = false;
    bool wake_ = false;
    bool model_ready_ = false;
    std::wstring awaiting_path_;
    ULONGLONG awaiting_tick_ = 0;
    bool awaiting_posted_ = false;
    std::deque<Job> queue_;
    std::string overlay_ass_;
    std::wstring overlay_bgra_path_;
    int overlay_w_ = 0;
    int overlay_h_ = 0;
    int overlay_stride_ = 0;
    std::wstring status_text_;
    int last_count_ = 0;
    int skipped_ = 0;
    int last_delay_ms_ = 0;
    int last_infer_ms_ = 0;
};
