#pragma once

#include <windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct WhisperSegment {
    double t0 = 0.0;
    double t1 = 0.0;
    std::string text;
};

// One ggml model discovered under a whisper models directory.
struct WhisperModel {
    std::wstring path;
    std::wstring filename;
    std::wstring label;
};

// MIT whisper.cpp: prefer a long-lived whisper-server (model stays warm), fall back to CLI.
class WhisperRuntime {
public:
    using ProgressFn = std::function<void(const std::wstring& status)>;

    WhisperRuntime() = default;
    ~WhisperRuntime();

    WhisperRuntime(const WhisperRuntime&) = delete;
    WhisperRuntime& operator=(const WhisperRuntime&) = delete;

    bool available() const;
    std::wstring cli_path() const;
    std::wstring model_path() const;
    std::wstring model_filename() const;
    std::wstring primary_models_dir() const;
    static std::wstring download_url_for_model(const std::wstring& filename);
    std::vector<WhisperModel> models() const;
    // Persist filename and restart whisper-server when the loaded model differs.
    bool select_model(const std::wstring& filename);
    std::wstring last_error() const;
    std::wstring backend_label() const;

    bool ensure_model(const ProgressFn& progress = {});

    bool transcribe(const std::wstring& wav_path, const std::string& prompt,
                    std::vector<WhisperSegment>& out, std::wstring* error = nullptr,
                    const std::string& language = "auto");

    std::string last_detected_language() const;
    void cancel();

private:
    bool ensure_server();
    bool start_server(const std::wstring& server, const std::wstring& model);
    void stop_server();
    bool server_healthy() const;
    bool transcribe_via_server(const std::wstring& wav_path, const std::string& prompt,
                               std::vector<WhisperSegment>& out, std::wstring* error,
                               const std::string& language);
    bool transcribe_via_cli(const std::wstring& wav_path, const std::string& prompt,
                            std::vector<WhisperSegment>& out, std::wstring* error,
                            const std::string& language);
    void load_settings() const;

    mutable std::mutex mu_;
    mutable std::wstring last_error_;
    mutable std::wstring selected_model_file_;
    mutable bool settings_loaded_ = false;
    std::string last_detected_language_;
    HANDLE process_ = nullptr;       // one-shot CLI
    HANDLE server_process_ = nullptr;
    DWORD server_pid_ = 0;
    int server_port_ = 0;
    std::wstring server_model_;
    bool server_failed_ = false;  // avoid restart spam after hard fail
};
