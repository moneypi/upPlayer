#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

struct WhisperSegment {
    double t0 = 0.0;
    double t1 = 0.0;
    std::string text;
};

// Runs PotPlayer's bundled whisper.cpp CLI (Vulkan/CPU main64.exe).
class WhisperRuntime {
public:
    using ProgressFn = std::function<void(const std::wstring& status)>;

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
    mutable std::wstring last_error_;
    std::string last_detected_language_;
    HANDLE process_ = nullptr;
};
