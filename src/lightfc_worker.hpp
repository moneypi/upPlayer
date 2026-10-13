#pragma once

#include "lightfc_runtime.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class LightFcMode {
    Off = 0,
    Arm = 1,      // Waiting for click to set target
    Tracking = 2  // Actively tracking
};

// Async LightFC single-object tracker. Skip busy frames; never stall playback.
class LightFcWorker {
public:
    static constexpr UINT WM_LIGHTFC_UPDATE = WM_APP + 42;

    explicit LightFcWorker(HWND notify);
    ~LightFcWorker();

    LightFcWorker(const LightFcWorker&) = delete;
    LightFcWorker& operator=(const LightFcWorker&) = delete;

    void set_mode(LightFcMode mode);
    LightFcMode mode() const;
    bool enabled() const { return mode() != LightFcMode::Off; }
    bool tracking() const { return mode() == LightFcMode::Tracking; }
    bool arming() const { return mode() == LightFcMode::Arm; }
    bool busy() const;

    // Begin tracking at xywh (image pixels). Switches to Tracking on success.
    bool init_at(const LightFcBox& box);

    void pump();
    std::wstring try_begin_capture();
    void notify_capture_requested(const std::wstring& path);

    std::wstring overlay_bgra_path() const;
    int overlay_width() const;
    int overlay_height() const;
    int overlay_stride() const;
    std::wstring status_text() const;
    LightFcBox last_box() const;
    int skipped_frames() const;
    static const wchar_t* mode_label(LightFcMode mode);

private:
    struct Job {
        std::wstring path;
        ULONGLONG capture_tick = 0;
        bool is_init = false;
        LightFcBox init_box{};
    };

    void thread_main();
    void clear_jobs_locked();
    bool file_ready(const std::wstring& path) const;
    bool promote_awaiting_locked();
    bool load_model_locked(std::wstring* error);
    bool decode_jpeg_bgr(const std::wstring& path, std::vector<unsigned char>& bgr, int& w, int& h,
                         int& stride, std::wstring* error);
    static std::wstring build_hud_text(LightFcMode mode, float score);
    static bool write_box_bgra(const std::wstring& path, int src_w, int src_h, const LightFcBox& box,
                               const std::wstring& hud, int* out_w, int* out_h);

    HWND notify_ = nullptr;
    LightFcRuntime runtime_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> run_{false};
    LightFcMode mode_ = LightFcMode::Off;
    bool busy_ = false;
    bool wake_ = false;
    bool model_ready_ = false;
    std::wstring awaiting_path_;
    ULONGLONG awaiting_tick_ = 0;
    bool awaiting_posted_ = false;
    bool awaiting_is_init_ = false;
    LightFcBox awaiting_init_box_{};
    std::deque<Job> queue_;
    std::wstring overlay_bgra_path_;
    int overlay_w_ = 0;
    int overlay_h_ = 0;
    int overlay_stride_ = 0;
    std::wstring status_text_;
    LightFcBox last_box_{};
    int skipped_ = 0;
    int last_infer_ms_ = 0;
    int lost_streak_ = 0;
    // Pending init requested from UI (consumed on next capture).
    bool pending_init_ = false;
    LightFcBox pending_init_box_{};

    // Sigmoid peak below this for kLostFrames consecutive track frames → auto Off.
    static constexpr float kLostScore = 0.6f;
    static constexpr int kLostFrames = 8;
};
