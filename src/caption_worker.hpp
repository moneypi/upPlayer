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

// Menu row for a whisper model filename or a language code ("auto", "en", "zh", ...).
struct CaptionOption {
    std::wstring id;
    std::wstring label;
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
    // WPARAM: 0 = progress (read srt_status), 1 = success (read last_srt_path), 2 = error (read srt_status).
    static constexpr UINT WM_SRT_DONE = WM_APP + 43;

    explicit CaptionWorker(HWND notify);
    ~CaptionWorker();

    CaptionWorker(const CaptionWorker&) = delete;
    CaptionWorker& operator=(const CaptionWorker&) = delete;

    void set_enabled(bool enabled);
    bool enabled() const;

    static std::vector<CaptionOption> languages();
    std::wstring language() const;
    void set_language(const std::wstring& code);

    std::vector<CaptionOption> models() const;
    std::wstring model_id() const;
    bool set_model(const std::wstring& filename);

    void set_media(const std::wstring& path, double duration);
    void on_seek(double time_pos);
    void tick(double time_pos);

    std::vector<CaptionCue> cues() const;
    std::wstring active_text(double time_pos) const;
    CaptionStatus status() const;
    std::wstring status_text() const;

    // Offline full-file transcription -> .srt beside the media (same basename).
    // Uses the currently selected Whisper Model + Language. Non-blocking.
    // Returns false if busy / no media / non-local path (error filled when provided).
    bool request_generate_srt(std::wstring* error = nullptr);
    bool srt_busy() const;
    std::wstring srt_status() const;
    std::wstring last_srt_path() const;

private:
    void thread_main();
    void srt_thread_main(std::wstring media, double duration, std::string language);
    void clear_from(double time_pos);
    bool extract_pcm(double start, double length, const std::wstring& wav_path, std::wstring* error);
    bool extract_pcm_from(const std::wstring& media, double start, double length,
                          const std::wstring& wav_path, std::wstring* error);
    static std::wstring utf8_to_wide(const std::string& text);
    static std::string wide_to_utf8(const std::wstring& text);
    static std::wstring suggest_srt_path(const std::wstring& media);
    static bool write_srt_file(const std::wstring& path, const std::vector<CaptionCue>& cues,
                               std::wstring* error);
    void post_srt(WPARAM code);

    HWND notify_ = nullptr;
    WhisperRuntime whisper_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    std::thread srt_thread_;
    std::atomic<bool> run_{false};
    std::atomic<bool> srt_busy_{false};
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
    std::string language_pref_{"auto"};
    std::string asr_language_{"auto"};
    std::wstring srt_status_;
    std::wstring last_srt_path_;
};
