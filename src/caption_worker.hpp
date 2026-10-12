#pragma once

#include "whisper_runtime.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct CaptionCue {
    double start = 0.0;
    double end = 0.0;
    std::wstring text;
};

enum class CaptionStatus {
    Idle,
    LoadingModel,
    Transcribing,
    Ready,
    Error,
};

class CaptionWorker {
public:
    static constexpr UINT WM_CAPTION_UPDATE = WM_APP + 40;

    explicit CaptionWorker(HWND notify);
    ~CaptionWorker();

    CaptionWorker(const CaptionWorker&) = delete;
    CaptionWorker& operator=(const CaptionWorker&) = delete;

    void set_enabled(bool enabled);
    bool enabled() const;

    void set_media(const std::wstring& path, double duration);
    void on_seek(double time_pos);
    void tick(double time_pos);

    std::vector<CaptionCue> cues() const;
    std::wstring active_text(double time_pos) const;
    CaptionStatus status() const;
    std::wstring status_text() const;

private:
    void thread_main();
    void clear_from(double time_pos);
    bool extract_pcm(double start, double length, const std::wstring& wav_path, std::wstring* error);
    static std::wstring utf8_to_wide(const std::string& text);

    HWND notify_ = nullptr;
    WhisperRuntime whisper_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> run_{false};
    bool enabled_ = false;
    bool wake_ = false;
    double time_pos_ = 0.0;
    double duration_ = -1.0;
    double covered_until_ = 0.0;
    double seek_epoch_ = 0.0;
    std::wstring media_path_;
    std::vector<CaptionCue> cues_;
    CaptionStatus status_ = CaptionStatus::Idle;
    std::wstring status_text_;
    std::wstring last_prompt_;
    std::string asr_language_{"auto"};
};