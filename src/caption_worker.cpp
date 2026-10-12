#include "caption_worker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kChunkSec = 8.0;
constexpr double kOverlapSec = 0.8;
constexpr double kLeadSec = 6.0;
constexpr double kSeekSlack = 1.2;
constexpr size_t kSoftMaxChars = 32;
constexpr size_t kPromptChars = 80;

std::wstring Quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"')
            out += L"\\\"";
        else
            out.push_back(c);
    }
    out += L'"';
    return out;
}

std::wstring FindMpv() {
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        std::wstring dir(buffer);
        const auto slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir.resize(slash);
        const std::wstring candidate = dir + L"\\mpv.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    const DWORD length = SearchPathW(nullptr, L"mpv.exe", nullptr, MAX_PATH, buffer, nullptr);
    if (length > 0 && length < MAX_PATH)
        return buffer;
    return {};
}

bool IsBreakChar(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L',' || c == L'.' ||
           c == L'!' || c == L'?' || c == L';' || c == L':' || c == L'、' || c == L'，' ||
           c == L'。' || c == L'！' || c == L'？' || c == L'；' || c == L'：' || c == L'…';
}

void TrimWide(std::wstring& text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t' || text.front() == L'\n' ||
                             text.front() == L'\r'))
        text.erase(text.begin());
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\n' ||
                             text.back() == L'\r'))
        text.pop_back();
}

void AppendSplitCues(std::vector<CaptionCue>& cues, double t0, double t1, std::wstring text) {
    TrimWide(text);
    if (text.empty() || !(t1 > t0))
        return;

    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = (std::min)(text.size(), i + kSoftMaxChars);
        if (end < text.size()) {
            size_t break_at = end;
            for (size_t j = end; j > i + kSoftMaxChars / 2; --j) {
                if (IsBreakChar(text[j - 1])) {
                    break_at = j;
                    break;
                }
            }
            end = break_at;
        }
        std::wstring part = text.substr(i, end - i);
        TrimWide(part);
        if (!part.empty())
            parts.push_back(std::move(part));
        i = end;
        while (i < text.size() && (text[i] == L' ' || text[i] == L'\t'))
            ++i;
    }
    if (parts.empty())
        return;

    const double span = t1 - t0;
    size_t weight_total = 0;
    for (const auto& p : parts)
        weight_total += p.size();
    if (weight_total == 0)
        return;

    double cursor = t0;
    size_t used = 0;
    for (size_t n = 0; n < parts.size(); ++n) {
        used += parts[n].size();
        const double end =
            (n + 1 == parts.size()) ? t1 : (t0 + span * (static_cast<double>(used) / weight_total));
        CaptionCue cue;
        cue.start = cursor;
        cue.end = (std::max)(cursor + 0.15, end);
        cue.text = parts[n];
        cues.push_back(std::move(cue));
        cursor = cue.end;
    }
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}

}  // namespace

CaptionWorker::CaptionWorker(HWND notify) : notify_(notify) {
    run_ = true;
    thread_ = std::thread([this] { thread_main(); });
}

CaptionWorker::~CaptionWorker() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        run_ = false;
        enabled_ = false;
        wake_ = true;
    }
    cv_.notify_all();
    whisper_.cancel();
    if (thread_.joinable())
        thread_.join();
}

void CaptionWorker::set_enabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (enabled_ == enabled)
            return;
        enabled_ = enabled;
        if (!enabled) {
            status_ = CaptionStatus::Idle;
            status_text_.clear();
            cues_.clear();
            covered_until_ = 0.0;
            last_prompt_.clear();
            asr_language_ = "auto";
        } else {
            status_ = CaptionStatus::LoadingModel;
            status_text_ = L"Captions: starting…";
            covered_until_ = (std::max)(0.0, time_pos_ - 0.5);
            wake_ = true;
        }
    }
    if (!enabled)
        whisper_.cancel();
    cv_.notify_all();
    if (notify_)
        PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
}

bool CaptionWorker::enabled() const {
    std::lock_guard<std::mutex> lock(mu_);
    return enabled_;
}

void CaptionWorker::set_media(const std::wstring& path, double duration) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        media_path_ = path;
        duration_ = duration;
        cues_.clear();
        covered_until_ = 0.0;
        seek_epoch_ = 0.0;
        last_prompt_.clear();
        asr_language_ = "auto";
        if (enabled_) {
            status_ = CaptionStatus::Ready;
            status_text_.clear();
            wake_ = true;
        }
    }
    whisper_.cancel();
    cv_.notify_all();
    if (notify_)
        PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
}

void CaptionWorker::on_seek(double time_pos) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        time_pos_ = time_pos;
        seek_epoch_ = time_pos;
        clear_from(time_pos);
        covered_until_ = (std::max)(0.0, time_pos - 0.25);
        if (enabled_)
            wake_ = true;
    }
    whisper_.cancel();
    cv_.notify_all();
}

void CaptionWorker::tick(double time_pos) {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        time_pos_ = time_pos;
        if (enabled_ && time_pos + kLeadSec > covered_until_ + 0.05) {
            wake_ = true;
            wake = true;
        }
    }
    if (wake)
        cv_.notify_all();
}

std::vector<CaptionCue> CaptionWorker::cues() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cues_;
}

std::wstring CaptionWorker::active_text(double time_pos) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::wstring best;
    double best_start = -1.0;
    for (const auto& cue : cues_) {
        if (time_pos + 0.05 >= cue.start && time_pos <= cue.end + 0.15) {
            if (cue.start >= best_start) {
                best_start = cue.start;
                best = cue.text;
            }
        }
    }
    return best;
}

CaptionStatus CaptionWorker::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

std::wstring CaptionWorker::status_text() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_text_;
}

void CaptionWorker::clear_from(double time_pos) {
    cues_.erase(std::remove_if(cues_.begin(), cues_.end(),
                               [time_pos](const CaptionCue& c) { return c.end > time_pos + 0.01; }),
                cues_.end());
}

std::wstring CaptionWorker::utf8_to_wide(const std::string& text) {
    if (text.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

bool CaptionWorker::extract_pcm(double start, double length, const std::wstring& wav_path,
                                std::wstring* error) {
    const std::wstring mpv = FindMpv();
    if (mpv.empty()) {
        if (error)
            *error = L"mpv.exe not found (needed to extract audio for captions)";
        return false;
    }
    std::wstring media;
    {
        std::lock_guard<std::mutex> lock(mu_);
        media = media_path_;
    }
    if (media.empty()) {
        if (error)
            *error = L"No media";
        return false;
    }

    DeleteFileW(wav_path.c_str());
    wchar_t start_arg[64];
    wchar_t len_arg[64];
    swprintf_s(start_arg, L"%.3f", (std::max)(0.0, start));
    swprintf_s(len_arg, L"%.3f", (std::max)(0.2, length));

    // Do not use --ao=null: some mpv builds fail to init that driver and produce no WAV.
    const std::wstring command =
        Quote(mpv) + L" --no-config --vo=null --no-video --untimed --hr-seek=yes --start=" +
        start_arg + L" --length=" + len_arg + L" --of=wav --oac=pcm_s16le --o=" + Quote(wav_path) +
        L" -- " + Quote(media);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = command;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        if (error)
            *error = L"Failed to start mpv audio extract";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    WIN32_FILE_ATTRIBUTE_DATA info{};
    const bool ok = GetFileAttributesExW(wav_path.c_str(), GetFileExInfoStandard, &info) &&
                    (info.nFileSizeLow > 44 || info.nFileSizeHigh > 0);
    if (!ok) {
        if (error)
            *error = exit_code != 0 ? L"Audio extract failed" : L"Empty WAV from extract";
        return false;
    }
    return true;
}

void CaptionWorker::thread_main() {
    while (true) {
        std::wstring media;
        double start = 0.0;
        double duration = -1.0;
        double seek_epoch = 0.0;
        std::string language = "auto";
        std::string prompt;
        bool enabled = false;

        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] {
                if (!run_)
                    return true;
                if (!enabled_)
                    return false;
                if (media_path_.empty())
                    return false;
                return wake_ || (time_pos_ + kLeadSec > covered_until_ + 0.05);
            });
            if (!run_)
                return;
            wake_ = false;
            enabled = enabled_;
            media = media_path_;
            duration = duration_;
            seek_epoch = seek_epoch_;
            language = asr_language_;
            if (!last_prompt_.empty()) {
                const std::wstring& p = last_prompt_;
                const size_t from = p.size() > kPromptChars ? p.size() - kPromptChars : 0;
                prompt = WideToUtf8(p.substr(from));
            }
            start = covered_until_;
            if (start < 0.0)
                start = 0.0;
            if (time_pos_ > start + kChunkSec + kSeekSlack) {
                // Playback jumped ahead of coverage; catch up from near playhead.
                start = (std::max)(0.0, time_pos_ - 0.5);
                covered_until_ = start;
            }
        }

        if (!enabled || media.empty())
            continue;

        {
            std::lock_guard<std::mutex> lock(mu_);
            status_ = CaptionStatus::LoadingModel;
            status_text_ = L"Captions: " + whisper_.backend_label();
        }
        if (notify_)
            PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);

        if (!whisper_.available()) {
            std::lock_guard<std::mutex> lock(mu_);
            status_ = CaptionStatus::Error;
            status_text_ =
                L"Whisper CLI not found — copy PotPlayer Module\\Whisper to potplayer-whisper next to upPlayer";
            if (notify_)
                PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
            continue;
        }
        if (!whisper_.ensure_model()) {
            std::lock_guard<std::mutex> lock(mu_);
            status_ = CaptionStatus::Error;
            status_text_ = whisper_.last_error();
            if (notify_)
                PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
            continue;
        }

        if (duration > 0.0 && start >= duration - 0.05) {
            std::lock_guard<std::mutex> lock(mu_);
            status_ = CaptionStatus::Ready;
            status_text_.clear();
            continue;
        }

        double length = kChunkSec;
        if (duration > 0.0)
            length = (std::min)(length, duration - start);
        if (length < 0.35)
            continue;

        wchar_t temp_dir[MAX_PATH] = {};
        wchar_t temp_file[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp_dir);
        if (!GetTempFileNameW(temp_dir, L"upcm", 0, temp_file))
            continue;
        const std::wstring wav_path = std::wstring(temp_file) + L".wav";
        DeleteFileW(temp_file);

        {
            std::lock_guard<std::mutex> lock(mu_);
            status_ = CaptionStatus::Transcribing;
            status_text_.clear();  // do not mask active cues with "识别中"
        }
        if (notify_)
            PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);

        std::wstring extract_error;
        if (!extract_pcm(start, length, wav_path, &extract_error)) {
            DeleteFileW(wav_path.c_str());
            std::lock_guard<std::mutex> lock(mu_);
            if (!enabled_)
                continue;
            status_ = CaptionStatus::Error;
            status_text_ = extract_error;
            if (notify_)
                PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
            Sleep(400);
            continue;
        }

        {
            bool stale = false;
            std::lock_guard<std::mutex> lock(mu_);
            stale = !enabled_ || media_path_ != media || std::fabs(seek_epoch_ - seek_epoch) > 0.001;
            if (stale) {
                DeleteFileW(wav_path.c_str());
                continue;
            }
            status_ = CaptionStatus::Transcribing;
        }

        std::vector<WhisperSegment> segments;
        std::wstring asr_error;
        const bool ok = whisper_.transcribe(wav_path, prompt, segments, &asr_error, language);
        DeleteFileW(wav_path.c_str());

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!enabled_ || media_path_ != media || std::fabs(seek_epoch_ - seek_epoch) > 0.001)
                continue;
            if (!ok) {
                status_ = CaptionStatus::Error;
                status_text_ = asr_error.empty() ? L"Whisper failed" : asr_error;
                if (notify_)
                    PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
                continue;
            }

            if (asr_language_ == "auto") {
                const std::string detected = whisper_.last_detected_language();
                if (!detected.empty())
                    asr_language_ = detected;
            }

            for (const auto& seg : segments) {
                const double t0 = start + seg.t0;
                const double t1 = start + seg.t1;
                if (t1 <= seek_epoch_ - 0.05)
                    continue;
                AppendSplitCues(cues_, t0, t1, utf8_to_wide(seg.text));
                const std::wstring wide = utf8_to_wide(seg.text);
                if (!wide.empty()) {
                    if (!last_prompt_.empty())
                        last_prompt_.push_back(L' ');
                    last_prompt_ += wide;
                    if (last_prompt_.size() > kPromptChars * 2)
                        last_prompt_ = last_prompt_.substr(last_prompt_.size() - kPromptChars);
                }
            }

            const double advance = (std::max)(0.5, length - kOverlapSec);
            covered_until_ = start + advance;
            if (duration > 0.0 && covered_until_ > duration)
                covered_until_ = duration;
            status_ = CaptionStatus::Ready;
            status_text_.clear();
        }
        if (notify_)
            PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
    }
}
