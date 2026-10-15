#include "caption_worker.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>

namespace {

// Slightly shorter chunks finish sooner on CPU whisper.cpp (Vulkan was faster).
constexpr double kChunkSec = 5.0;
constexpr double kOverlapSec = 0.75;
constexpr double kLeadSec = 10.0;
// Only jump-forward after this much lag (seek / extreme stall). Smaller gaps must
// still be transcribed 鈥?otherwise slow ASR silently skips sentences.
constexpr double kMaxLagBeforeCatchUp = 25.0;
constexpr size_t kSoftMaxChars = 32;
constexpr size_t kPromptChars = 80;

using SteadyClock = std::chrono::steady_clock;

std::wstring CaptionDiagPath() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"upplayer-captions-" + std::to_wstring(GetCurrentProcessId()) + L".log";
}

void CapDiag(const char* fmt, ...) {
    static std::mutex log_mu;
    static std::ofstream out;
    static bool opened = false;
    std::lock_guard<std::mutex> lock(log_mu);
    if (!opened) {
        const std::wstring path = CaptionDiagPath();
        out.open(path, std::ios::out | std::ios::trunc);
        opened = true;
        if (out) {
            out << "# caption delay diag pid=" << GetCurrentProcessId() << "\n";
            out << "# wall_ms playhead covered lag chunk_start chunk_len extract_ms asr_ms segs event\n";
            out.flush();
        }
    }
    if (!out)
        return;
    const auto now = SteadyClock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    char line[1024];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n <= 0)
        return;
    out << ms << " " << line << "\n";
    out.flush();
}

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

bool StartsWithCi(const std::wstring& text, const wchar_t* prefix) {
    const size_t n = wcslen(prefix);
    if (text.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        wchar_t a = text[i];
        wchar_t b = prefix[i];
        if (a >= L'A' && a <= L'Z')
            a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z')
            b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b)
            return false;
    }
    return true;
}

// Live streams cannot seek; caption by recording from the live edge on a second mpv.
// HTTP(S) file URLs keep the seek-based path used for local files.
bool IsLiveStreamUrl(const std::wstring& path) {
    return StartsWithCi(path, L"rtsp://") || StartsWithCi(path, L"rtsps://") ||
           StartsWithCi(path, L"rtmp://") || StartsWithCi(path, L"rtmps://") ||
           StartsWithCi(path, L"rtp://") || StartsWithCi(path, L"udp://") ||
           StartsWithCi(path, L"srt://");
}

bool IsBreakChar(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L',' || c == L'.' ||
           c == L'!' || c == L'?' || c == L';' || c == L':' ||
           c == L'\u3002' || c == L'\uff0c' || c == L'\uff01' || c == L'\uff1f' ||
           c == L'\u3001' || c == L'\uff1b' || c == L'\uff1a' || c == L'\u2026';
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

std::string NormalizeLanguage(std::string code) {
    for (char& c : code) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    if (code == "auto")
        return code;
    if (code.size() < 2 || code.size() > 8)
        return "auto";
    for (char c : code) {
        if (c < 'a' || c > 'z')
            return "auto";
    }
    return code;
}

std::string LoadLanguagePref() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return "auto";
    wchar_t buffer[64] = {};
    DWORD type = 0;
    DWORD size = sizeof(buffer);
    const LONG ok =
        RegQueryValueExW(key, L"WhisperLanguage", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);
    if (ok != ERROR_SUCCESS || type != REG_SZ)
        return "auto";
    std::string code;
    for (wchar_t c : std::wstring(buffer)) {
        if (c == 0)
            break;
        if (c > 0 && c < 128)
            code.push_back(static_cast<char>(c));
    }
    return NormalizeLanguage(code);
}

void SaveLanguagePref(const std::string& code) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    std::wstring wide;
    wide.reserve(code.size());
    for (unsigned char c : code)
        wide.push_back(static_cast<wchar_t>(c));
    RegSetValueExW(key, L"WhisperLanguage", 0, REG_SZ, reinterpret_cast<const BYTE*>(wide.c_str()),
                   static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

std::wstring AsciiToWide(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (unsigned char c : text)
        out.push_back(static_cast<wchar_t>(c));
    return out;
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
    language_pref_ = LoadLanguagePref();
    asr_language_ = language_pref_;
    run_ = true;
    thread_ = std::thread([this] { thread_main(); });
}

CaptionWorker::~CaptionWorker() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        run_ = false;
        wake_ = true;
    }
    srt_cancel_ = true;
    cv_.notify_all();
    whisper_.cancel();
    if (thread_.joinable())
        thread_.join();
    if (srt_thread_.joinable())
        srt_thread_.join();
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
            asr_language_ = language_pref_;
        } else {
            status_ = CaptionStatus::LoadingModel;
            status_text_ = L"Captions: starting...";
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

std::vector<CaptionOption> CaptionWorker::languages() {
    static const CaptionOption kLanguages[] = {
        {L"auto", L"Auto"},
        {L"en", L"English"},
        {L"zh", L"Chinese"},
        {L"ja", L"Japanese"},
        {L"ko", L"Korean"},
        {L"fr", L"French"},
        {L"de", L"German"},
        {L"es", L"Spanish"},
        {L"pt", L"Portuguese"},
        {L"ru", L"Russian"},
        {L"it", L"Italian"},
        {L"ar", L"Arabic"},
        {L"hi", L"Hindi"},
        {L"th", L"Thai"},
        {L"vi", L"Vietnamese"},
        {L"id", L"Indonesian"},
        {L"tr", L"Turkish"},
        {L"nl", L"Dutch"},
        {L"pl", L"Polish"},
        {L"uk", L"Ukrainian"},
        {L"sv", L"Swedish"},
        {L"cs", L"Czech"},
        {L"da", L"Danish"},
        {L"fi", L"Finnish"},
        {L"el", L"Greek"},
        {L"he", L"Hebrew"},
        {L"hu", L"Hungarian"},
        {L"no", L"Norwegian"},
        {L"ro", L"Romanian"},
    };
    return std::vector<CaptionOption>(std::begin(kLanguages), std::end(kLanguages));
}

std::wstring CaptionWorker::language() const {
    std::lock_guard<std::mutex> lock(mu_);
    return AsciiToWide(language_pref_);
}

void CaptionWorker::set_language(const std::wstring& code) {
    std::string ascii;
    ascii.reserve(code.size());
    for (wchar_t c : code) {
        if (c > 0 && c < 128)
            ascii.push_back(static_cast<char>(c));
    }
    const std::string normalized = NormalizeLanguage(ascii);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (language_pref_ == normalized)
            return;
        language_pref_ = normalized;
        asr_language_ = normalized;
        if (enabled_)
            wake_ = true;
    }
    SaveLanguagePref(normalized);
    cv_.notify_all();
}

std::vector<CaptionOption> CaptionWorker::models() const {
    std::vector<CaptionOption> out;
    for (const WhisperModel& model : whisper_.models()) {
        CaptionOption option;
        option.id = model.filename;
        option.label = model.label;
        out.push_back(std::move(option));
    }
    return out;
}

std::wstring CaptionWorker::model_id() const {
    return whisper_.model_filename();
}

bool CaptionWorker::set_model(const std::wstring& filename) {
    const std::wstring previous = whisper_.model_filename();
    if (!whisper_.select_model(filename))
        return false;
    if (_wcsicmp(previous.c_str(), whisper_.model_filename().c_str()) == 0)
        return true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (enabled_) {
            status_ = CaptionStatus::LoadingModel;
            status_text_ = L"Captions: loading model...";
            wake_ = true;
        }
    }
    whisper_.cancel();
    cv_.notify_all();
    if (notify_)
        PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
    return true;
}

bool CaptionWorker::has_whisper_model() const {
    return !whisper_.model_path().empty();
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
        asr_language_ = language_pref_;
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
    std::wstring media;
    {
        std::lock_guard<std::mutex> lock(mu_);
        media = media_path_;
    }
    return extract_pcm_from(media, start, length, wav_path, error);
}

bool CaptionWorker::extract_pcm_from(const std::wstring& media, double start, double length,
                                     const std::wstring& wav_path, std::wstring* error) {
    const std::wstring mpv = FindMpv();
    if (mpv.empty()) {
        if (error)
            *error = L"mpv.exe not found (needed to extract audio for captions)";
        return false;
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
    // Longer wait for offline SRT chunks (up to ~30s of audio).
    const DWORD timeout_ms = length > 10.0 ? 300000 : 120000;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        if (error)
            *error = L"Failed to start mpv audio extract";
        return false;
    }
    WaitForSingleObject(pi.hProcess, timeout_ms);
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

bool CaptionWorker::extract_pcm_live(const std::wstring& media, double length,
                                     const std::wstring& wav_path, std::wstring* error) {
    const std::wstring mpv = FindMpv();
    if (mpv.empty()) {
        if (error)
            *error = L"mpv.exe not found (needed to extract audio for captions)";
        return false;
    }
    if (media.empty()) {
        if (error)
            *error = L"No media";
        return false;
    }

    DeleteFileW(wav_path.c_str());
    wchar_t len_arg[64];
    swprintf_s(len_arg, L"%.3f", (std::max)(0.5, length));

    // Second connection at the live edge only — never --start (live cannot seek).
    // Match the player's low-latency flags so this pull is not ~10s behind the picture.
    // Video stays on the primary player; caption delay comes from record + Whisper time.
    std::wstring command = Quote(mpv) + L" --no-config --vo=null --no-video --untimed";
    if (StartsWithCi(media, L"rtsp://") || StartsWithCi(media, L"rtsps://") ||
        StartsWithCi(media, L"rtmp://") || StartsWithCi(media, L"rtmps://"))
        command += L" --profile=low-latency";
    command += L" --length=";
    command += len_arg;
    command += L" --of=wav --oac=pcm_s16le --o=";
    command += Quote(wav_path);
    command += L" -- ";
    command += Quote(media);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = command;
    const DWORD timeout_ms = static_cast<DWORD>((std::max)(30.0, length + 25.0) * 1000.0);
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        if (error)
            *error = L"Failed to start live audio extract";
        return false;
    }
    WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    WIN32_FILE_ATTRIBUTE_DATA info{};
    const bool ok = GetFileAttributesExW(wav_path.c_str(), GetFileExInfoStandard, &info) &&
                    (info.nFileSizeLow > 44 || info.nFileSizeHigh > 0);
    if (!ok) {
        if (error)
            *error = exit_code != 0 ? L"Live audio extract failed" : L"Empty WAV from live extract";
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
            const double playhead = time_pos_;
            const double lag_before = playhead - start;
            // If ASR is only a little behind, keep going sequentially (no gaps).
            // Jump only when hopelessly behind; leave a lead window so catch-up still
            // captions recent speech instead of discarding everything up to now.
            // Live streams still use this to drop backlog; the next capture is always
            // from the live edge (not a seek to covered_until_).
            if (playhead > start + kMaxLagBeforeCatchUp) {
                const double jump_to = (std::max)(0.0, playhead - kLeadSec);
                CapDiag("event=SKIP_CATCHUP playhead=%.3f covered=%.3f lag=%.3f -> jump_to=%.3f", playhead,
                        start, lag_before, jump_to);
                start = jump_to;
                covered_until_ = start;
                clear_from(start);
            }
        }

        if (!enabled || media.empty())
            continue;

        const bool live = IsLiveStreamUrl(media);

        double playhead_at_job = 0.0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            playhead_at_job = time_pos_;
        }
        // Live: map this chunk onto the playhead when recording starts (second pull =
        // live edge). Local: start is the file position from covered_until_.
        if (live)
            start = playhead_at_job;
        CapDiag("event=JOB_BEGIN playhead=%.3f covered=%.3f lag=%.3f chunk_start=%.3f live=%d backend=%s",
                playhead_at_job, start, playhead_at_job - start, start, live ? 1 : 0,
                WideToUtf8(whisper_.backend_label()).c_str());

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
                L"Whisper CLI not found 鈥?run scripts\\fetch-whisper-deps.ps1";
            if (notify_)
                PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
            continue;
        }
        if (!whisper_.ensure_model()) {
            const std::wstring err = whisper_.last_error();
            {
                std::lock_guard<std::mutex> lock(mu_);
                status_ = CaptionStatus::Error;
                // Keep tagged missing:/server: prefix so the UI can open a help MessageBox.
                status_text_ = err.empty() ? L"missing:ggml-tiny.bin" : err;
            }
            if (notify_)
                PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
            continue;
        }

        if (!live && duration > 0.0 && start >= duration - 0.05) {
            std::lock_guard<std::mutex> lock(mu_);
            status_ = CaptionStatus::Ready;
            status_text_.clear();
            continue;
        }

        double length = kChunkSec;
        if (!live && duration > 0.0)
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
            status_text_.clear();  // do not mask active cues with "璇嗗埆涓?
        }
        if (notify_)
            PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);

        std::wstring extract_error;
        const auto t_extract0 = SteadyClock::now();
        const bool extracted =
            live ? extract_pcm_live(media, length, wav_path, &extract_error)
                 : extract_pcm(start, length, wav_path, &extract_error);
        const double extract_ms =
            std::chrono::duration<double, std::milli>(SteadyClock::now() - t_extract0).count();
        if (!extracted) {
            CapDiag("event=EXTRACT_FAIL playhead=%.3f chunk_start=%.3f chunk_len=%.3f live=%d extract_ms=%.0f",
                    playhead_at_job, start, length, live ? 1 : 0, extract_ms);
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
                CapDiag("event=STALE_AFTER_EXTRACT chunk_start=%.3f", start);
                DeleteFileW(wav_path.c_str());
                continue;
            }
            status_ = CaptionStatus::Transcribing;
        }

        std::vector<WhisperSegment> segments;
        std::wstring asr_error;
        const auto t_asr0 = SteadyClock::now();
        const bool ok = whisper_.transcribe(wav_path, prompt, segments, &asr_error, language);
        const double asr_ms =
            std::chrono::duration<double, std::milli>(SteadyClock::now() - t_asr0).count();
        DeleteFileW(wav_path.c_str());

        double playhead_after = 0.0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            playhead_after = time_pos_;
            if (!enabled_ || media_path_ != media || std::fabs(seek_epoch_ - seek_epoch) > 0.001) {
                CapDiag("event=STALE_AFTER_ASR chunk_start=%.3f extract_ms=%.0f asr_ms=%.0f", start, extract_ms,
                        asr_ms);
                continue;
            }
            if (!ok) {
                CapDiag("event=ASR_FAIL chunk_start=%.3f extract_ms=%.0f asr_ms=%.0f", start, extract_ms, asr_ms);
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

            int kept = 0;
            int dropped = 0;
            // whisper.cpp pads short WAV to ~30s; offsets often report to=30000 even for a
            // 5s chunk. Clamp to the real chunk, then shift late results onto the playhead
            // so the user still sees the sentence instead of a silent skip.
            for (const auto& seg : segments) {
                double rel0 = seg.t0;
                double rel1 = seg.t1;
                if (rel0 < 0.0)
                    rel0 = 0.0;
                if (rel1 > length) {
                    CapDiag("event=CLAMP_SEG raw_t0=%.3f raw_t1=%.3f chunk_len=%.3f text=%s",
                            start + seg.t0, start + seg.t1, length, seg.text.c_str());
                    rel1 = length;
                }
                if (rel0 >= length || rel1 <= rel0 + 0.05)
                    continue;
                double t0 = start + rel0;
                double t1 = start + rel1;
                if (t1 <= seek_epoch_ - 0.05) {
                    ++dropped;
                    CapDiag("event=DROP_SEG t0=%.3f t1=%.3f seek_epoch=%.3f text=%s", t0, t1, seek_epoch_,
                            seg.text.c_str());
                    continue;
                }
                const double span = t1 - t0;
                if (t1 <= playhead_after + 0.05) {
                    // Entirely behind the playhead when ASR finished 鈥?show briefly now.
                    const double show = (std::min)(4.0, (std::max)(1.6, span));
                    CapDiag("event=LATE_SHIFT raw_t0=%.3f raw_t1=%.3f playhead=%.3f -> show=%.2fs text=%s",
                            t0, t1, playhead_after, show, seg.text.c_str());
                    t0 = playhead_after;
                    t1 = playhead_after + show;
                } else if (t0 < playhead_after) {
                    CapDiag("event=LATE_TRIM raw_t0=%.3f t1=%.3f playhead=%.3f text=%s", t0, t1,
                            playhead_after, seg.text.c_str());
                    t0 = playhead_after;
                    if (t1 <= t0 + 0.4)
                        t1 = t0 + (std::min)(2.5, (std::max)(1.2, span));
                }
                AppendSplitCues(cues_, t0, t1, utf8_to_wide(seg.text));
                ++kept;
                const std::wstring wide = utf8_to_wide(seg.text);
                if (!wide.empty()) {
                    if (!last_prompt_.empty())
                        last_prompt_.push_back(L' ');
                    last_prompt_ += wide;
                    if (last_prompt_.size() > kPromptChars * 2)
                        last_prompt_ = last_prompt_.substr(last_prompt_.size() - kPromptChars);
                }
                CapDiag("event=SEG t0=%.3f t1=%.3f text=%s", t0, t1, seg.text.c_str());
            }

            const double advance = (std::max)(0.5, length - kOverlapSec);
            covered_until_ = start + advance;
            if (duration > 0.0 && covered_until_ > duration)
                covered_until_ = duration;
            const double lag_after = playhead_after - covered_until_;
            // Realtime factor: asr_ms for `length` seconds of audio. >1.0 means slower than realtime.
            const double rtf = (length > 0.01) ? ((asr_ms / 1000.0) / length) : 0.0;
            CapDiag("event=JOB_DONE playhead=%.3f covered=%.3f lag=%.3f chunk_start=%.3f chunk_len=%.3f "
                    "extract_ms=%.0f asr_ms=%.0f rtf=%.2f segs=%d kept=%d dropped=%d",
                    playhead_after, covered_until_, lag_after, start, length, extract_ms, asr_ms, rtf,
                    static_cast<int>(segments.size()), kept, dropped);
            status_ = CaptionStatus::Ready;
            status_text_.clear();
        }
        if (notify_)
            PostMessageW(notify_, WM_CAPTION_UPDATE, 0, 0);
    }
}


std::string CaptionWorker::wide_to_utf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}

std::wstring CaptionWorker::suggest_srt_path(const std::wstring& media) {
    const auto slash = media.find_last_of(L"\\/");
    const auto dot = media.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        return media.substr(0, dot) + L".srt";
    return media + L".srt";
}

bool CaptionWorker::write_srt_file(const std::wstring& path, const std::vector<CaptionCue>& cues,
                                   std::wstring* error) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) {
        if (error)
            *error = L"Cannot write SRT file";
        return false;
    }
    // UTF-8 BOM helps Notepad / some players with CJK.
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    fwrite(bom, 1, 3, fp);

    auto format_ts = [](double seconds, char* buf, size_t buf_size) {
        if (!(seconds >= 0.0))
            seconds = 0.0;
        const int total_ms = static_cast<int>(seconds * 1000.0 + 0.5);
        const int ms = total_ms % 1000;
        int total_s = total_ms / 1000;
        const int s = total_s % 60;
        total_s /= 60;
        const int m = total_s % 60;
        const int h = total_s / 60;
        snprintf(buf, buf_size, "%02d:%02d:%02d,%03d", h, m, s, ms);
    };

    int index = 1;
    for (const auto& cue : cues) {
        if (cue.text.empty() || !(cue.end > cue.start))
            continue;
        char t0[32];
        char t1[32];
        format_ts(cue.start, t0, sizeof(t0));
        format_ts(cue.end, t1, sizeof(t1));
        fprintf(fp, "%d\n%s --> %s\n", index, t0, t1);
        const std::string utf8 = wide_to_utf8(cue.text);
        fwrite(utf8.data(), 1, utf8.size(), fp);
        fprintf(fp, "\n\n");
        ++index;
    }
    fclose(fp);
    if (index == 1) {
        if (error)
            *error = L"No speech detected (empty SRT)";
        // Still wrote an empty-ish file; treat as soft success with warning via error text.
        return true;
    }
    return true;
}

void CaptionWorker::post_srt(WPARAM code) {
    if (notify_)
        PostMessageW(notify_, WM_SRT_DONE, code, 0);
}

bool CaptionWorker::srt_busy() const {
    return srt_busy_.load();
}

std::wstring CaptionWorker::srt_status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return srt_status_;
}

std::wstring CaptionWorker::last_srt_path() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_srt_path_;
}

bool CaptionWorker::request_generate_srt(std::wstring* error) {
    if (srt_busy_.exchange(true)) {
        if (error)
            *error = L"SRT generation is already running";
        return false;
    }
    srt_cancel_ = false;

    std::wstring media;
    double duration = -1.0;
    std::string language;
    {
        std::lock_guard<std::mutex> lock(mu_);
        media = media_path_;
        duration = duration_;
        language = language_pref_;
        srt_status_ = L"Starting...";
        last_srt_path_.clear();
    }

    auto starts_with_ci = [](const std::wstring& s, const wchar_t* prefix) {
        const size_t n = wcslen(prefix);
        if (s.size() < n)
            return false;
        for (size_t i = 0; i < n; ++i) {
            wchar_t a = s[i];
            wchar_t b = prefix[i];
            if (a >= L'A' && a <= L'Z')
                a = static_cast<wchar_t>(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z')
                b = static_cast<wchar_t>(b - L'A' + L'a');
            if (a != b)
                return false;
        }
        return true;
    };

    if (media.empty()) {
        srt_busy_ = false;
        if (error)
            *error = L"Open a local media file first";
        return false;
    }
    if (starts_with_ci(media, L"http://") || starts_with_ci(media, L"https://") ||
        starts_with_ci(media, L"rtsp://") || starts_with_ci(media, L"rtsps://") ||
        starts_with_ci(media, L"rtp://") || starts_with_ci(media, L"udp://")) {
        srt_busy_ = false;
        if (error)
            *error = L"Generate Subtitle needs a local media file (not a network stream)";
        return false;
    }
    if (!(duration > 0.5)) {
        srt_busy_ = false;
        if (error)
            *error = L"Media duration is unknown; wait until the file is fully opened";
        return false;
    }

    if (srt_thread_.joinable())
        srt_thread_.join();
    srt_thread_ = std::thread([this, media, duration, language] {
        srt_thread_main(media, duration, language);
    });
    post_srt(0);
    return true;
}

void CaptionWorker::cancel_generate_srt() {
    if (!srt_busy_.load())
        return;
    srt_cancel_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        srt_status_ = L"Cancelling...";
    }
    whisper_.cancel();
    post_srt(0);
}
void CaptionWorker::srt_thread_main(std::wstring media, double duration, std::string language) {
    constexpr double kSrtChunkSec = 28.0;
    constexpr double kSrtOverlapSec = 1.0;

    auto set_status = [this](const std::wstring& text) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            srt_status_ = text;
        }
        post_srt(0);
    };

    if (!whisper_.ensure_model([&](const std::wstring& status) { set_status(status); })) {
        std::wstring err = whisper_.last_error();
        if (err.empty())
            err = L"Whisper model missing";
        {
            std::lock_guard<std::mutex> lock(mu_);
            srt_status_ = err;
        }
        srt_busy_ = false;
        post_srt(2);
        return;
    }

    set_status(L"Generating SRT... 0%");

    std::vector<CaptionCue> all_cues;
    std::string prompt;
    std::string asr_lang = language.empty() ? "auto" : language;
    double covered = 0.0;
    int chunk_index = 0;

    while (covered < duration - 0.05) {
        if (srt_cancel_.load() || !run_.load()) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                srt_status_ = L"Cancelled";
            }
            srt_busy_ = false;
            post_srt(3);
            return;
        }

        double start = covered;
        double length = kSrtChunkSec;
        if (start + length > duration)
            length = duration - start;
        if (length < 0.3)
            break;

        wchar_t progress[128];
        swprintf_s(progress, L"Generating SRT... %.0f%%", (std::min)(99.0, (start / duration) * 100.0));
        set_status(progress);

        wchar_t temp_dir[MAX_PATH] = {};
        wchar_t temp_file[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp_dir);
        if (!GetTempFileNameW(temp_dir, L"usrt", 0, temp_file)) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                srt_status_ = L"Cannot create temp WAV";
            }
            srt_busy_ = false;
            post_srt(2);
            return;
        }
        const std::wstring wav_path = std::wstring(temp_file) + L".wav";
        DeleteFileW(temp_file);

        std::wstring extract_error;
        if (!extract_pcm_from(media, start, length, wav_path, &extract_error)) {
            DeleteFileW(wav_path.c_str());
            {
                std::lock_guard<std::mutex> lock(mu_);
                srt_status_ = extract_error.empty() ? L"Audio extract failed" : extract_error;
            }
            srt_busy_ = false;
            post_srt(2);
            return;
        }

        std::vector<WhisperSegment> segments;
        std::wstring asr_error;
        const bool ok = whisper_.transcribe(wav_path, prompt, segments, &asr_error, asr_lang);
        DeleteFileW(wav_path.c_str());
        if (!ok) {
            if (srt_cancel_.load() || !run_.load()) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    srt_status_ = L"Cancelled";
                }
                srt_busy_ = false;
                post_srt(3);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                srt_status_ = asr_error.empty() ? whisper_.last_error() : asr_error;
                if (srt_status_.empty())
                    srt_status_ = L"Transcription failed";
            }
            srt_busy_ = false;
            post_srt(2);
            return;
        }

        if (asr_lang == "auto") {
            const std::string detected = whisper_.last_detected_language();
            if (!detected.empty())
                asr_lang = detected;
        }

        for (const auto& seg : segments) {
            double rel0 = seg.t0;
            double rel1 = seg.t1;
            if (rel0 < 0.0)
                rel0 = 0.0;
            if (rel1 > length)
                rel1 = length;
            if (rel0 >= length || rel1 <= rel0 + 0.05)
                continue;
            CaptionCue cue;
            cue.start = start + rel0;
            cue.end = start + rel1;
            cue.text = utf8_to_wide(seg.text);
            TrimWide(cue.text);
            if (cue.text.empty())
                continue;
            // Drop cues that fall mostly inside the previous overlap window.
            if (!all_cues.empty() && cue.end <= all_cues.back().end + 0.05 &&
                cue.start < covered + 0.05)
                continue;
            all_cues.push_back(std::move(cue));
            const std::string u8 = seg.text;
            if (!u8.empty()) {
                if (!prompt.empty())
                    prompt.push_back(' ');
                prompt += u8;
                if (prompt.size() > kPromptChars * 2)
                    prompt = prompt.substr(prompt.size() - kPromptChars);
            }
        }

        const double advance = (std::max)(0.5, length - kSrtOverlapSec);
        covered = start + advance;
        if (covered > duration)
            covered = duration;
        ++chunk_index;
        (void)chunk_index;
    }

    const std::wstring out_path = suggest_srt_path(media);
    std::wstring write_error;
    if (!write_srt_file(out_path, all_cues, &write_error)) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            srt_status_ = write_error.empty() ? L"Failed to write SRT" : write_error;
        }
        srt_busy_ = false;
        post_srt(2);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        last_srt_path_ = out_path;
        if (all_cues.empty())
            srt_status_ = write_error.empty()
                              ? (L"Saved empty SRT (no speech): " + out_path)
                              : write_error;
        else {
            wchar_t buf[64];
            swprintf_s(buf, L"Saved %d cues", static_cast<int>(all_cues.size()));
            srt_status_ = std::wstring(buf) + L": " + out_path;
        }
    }
    srt_busy_ = false;
    post_srt(1);
}
