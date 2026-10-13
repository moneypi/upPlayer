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
    void stop_server();
    bool server_healthy() const;
    bool transcribe_via_server(const std::wstring& wav_path, const std::string& prompt,
                               std::vector<WhisperSegment>& out, std::wstring* error,
                               const std::string& language);
    bool transcribe_via_cli(const std::wstring& wav_path, const std::string& prompt,
                            std::vector<WhisperSegment>& out, std::wstring* error,
                            const std::string& language);

    mutable std::mutex mu_;
    mutable std::wstring last_error_;
    std::string last_detected_language_;
    HANDLE process_ = nullptr;       // one-shot CLI
    HANDLE server_process_ = nullptr;
    DWORD server_pid_ = 0;
    int server_port_ = 0;
    bool server_failed_ = false;  // avoid restart spam after hard fail
};
