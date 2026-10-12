#include "whisper_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

std::wstring Resolve(const std::wstring& relative) {
    wchar_t full[MAX_PATH];
    if (GetFullPathNameW(relative.c_str(), MAX_PATH, full, nullptr) && FileExists(full))
        return full;
    return {};
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

void AppendWhisperCliCandidates(std::vector<std::wstring>& out, const std::wstring& root) {
    if (root.empty())
        return;
    out.push_back(root + L"\\Vulkan\\main64.exe");
    out.push_back(root + L"\\CPU\\main64.exe");
    out.push_back(root + L"\\main64.exe");
}

std::wstring PotPlayerInstallRoot() {
    const wchar_t* keys[] = {
        L"SOFTWARE\\DAUM\\PotPlayer64",
        L"SOFTWARE\\DAUM\\PotPlayerMini64",
        L"SOFTWARE\\WOW6432Node\\DAUM\\PotPlayer64",
        L"SOFTWARE\\WOW6432Node\\DAUM\\PotPlayerMini64",
    };
    for (const wchar_t* key : keys) {
        HKEY hkey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hkey) != ERROR_SUCCESS)
            continue;
        wchar_t path[MAX_PATH] = {};
        DWORD type = 0;
        DWORD size = sizeof(path);
        const LONG ok = RegQueryValueExW(hkey, L"ProgramPath", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(path), &size);
        RegCloseKey(hkey);
        if (ok == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && path[0]) {
            std::wstring dir(path);
            const auto slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                dir.resize(slash);
            if (FileExists(dir + L"\\PotPlayerMini64.exe") || FileExists(dir + L"\\PotPlayer64.exe") ||
                FileExists(dir + L"\\Module\\Whisper"))
                return dir;
        }
    }
    const std::wstring fallbacks[] = {
        std::wstring(L"C:\\Program Files\\DAUM\\PotPlayer"),
        std::wstring(L"C:\\Program Files (x86)\\DAUM\\PotPlayer"),
    };
    for (const std::wstring& dir : fallbacks) {
        if (FileExists(dir + L"\\Module\\Whisper"))
            return dir;
    }
    return {};
}

std::wstring FindCli() {
    const std::wstring root = ExeDirectory();
    std::vector<std::wstring> candidates;
    AppendWhisperCliCandidates(candidates, root + L"\\potplayer-whisper");
    AppendWhisperCliCandidates(candidates, root + L"\\..\\..\\third_party\\potplayer-whisper");

    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0)
        AppendWhisperCliCandidates(candidates, std::wstring(local) + L"\\upPlayer\\potplayer-whisper");

    const std::wstring pot = PotPlayerInstallRoot();
    if (!pot.empty())
        AppendWhisperCliCandidates(candidates, pot + L"\\Module\\Whisper");

    for (const std::wstring& c : candidates) {
        const std::wstring full = Resolve(c);
        if (!full.empty())
            return full;
    }
    return {};
}

std::wstring FirstExistingModel(const std::vector<std::wstring>& dirs) {
    const wchar_t* names[] = {
        L"ggml-base.bin",
        L"ggml-small.bin",
        L"ggml-medium.bin",
        L"ggml-large-v3.bin",
        L"ggml-tiny.bin",
    };
    for (const std::wstring& dir : dirs) {
        if (dir.empty())
            continue;
        for (const wchar_t* name : names) {
            const std::wstring path = dir + L"\\" + name;
            if (FileExists(path))
                return path;
        }
    }
    return {};
}

std::wstring FindModel() {
    std::vector<std::wstring> dirs;
    const std::wstring root = ExeDirectory();
    dirs.push_back(root + L"\\models");
    dirs.push_back(root + L"\\potplayer-whisper\\Model");

    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0)
        dirs.push_back(std::wstring(appdata) + L"\\PotPlayerMini64\\Model");

    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0)
        dirs.push_back(std::wstring(local) + L"\\upPlayer\\models");

    const std::wstring pot = PotPlayerInstallRoot();
    if (!pot.empty())
        dirs.push_back(pot + L"\\Model");

    return FirstExistingModel(dirs);
}

double ParseTimestamp(const std::string& value) {
    int h = 0, m = 0, s = 0, ms = 0;
    if (sscanf_s(value.c_str(), "%d:%d:%d,%d", &h, &m, &s, &ms) == 4 ||
        sscanf_s(value.c_str(), "%d:%d:%d.%d", &h, &m, &s, &ms) == 4) {
        return h * 3600.0 + m * 60.0 + s + ms / 1000.0;
    }
    return -1.0;
}

std::string JsonUnescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            const char n = in[++i];
            if (n == 'n')
                out.push_back('\n');
            else if (n == 't')
                out.push_back('\t');
            else if (n == 'r')
                out.push_back('\r');
            else if (n == '"' || n == '\\' || n == '/')
                out.push_back(n);
            else
                out.push_back(n);
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

bool ParseJsonString(const std::string& json, size_t colon, std::string* value, size_t* end_pos) {
    if (colon == std::string::npos || !value || !end_pos)
        return false;
    size_t q1 = colon + 1;
    while (q1 < json.size() && (json[q1] == ' ' || json[q1] == '\t' || json[q1] == '\r' || json[q1] == '\n'))
        ++q1;
    if (q1 >= json.size() || json[q1] != '"')
        return false;
    size_t q2 = q1 + 1;
    while (q2 < json.size()) {
        if (json[q2] == '"' && json[q2 - 1] != '\\')
            break;
        ++q2;
    }
    if (q2 >= json.size())
        return false;
    *value = JsonUnescape(json.substr(q1 + 1, q2 - q1 - 1));
    *end_pos = q2;
    return true;
}

bool ParseJsonNumber(const std::string& json, size_t colon, double* value, size_t* end_pos) {
    if (colon == std::string::npos || !value || !end_pos)
        return false;
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n'))
        ++i;
    if (i >= json.size())
        return false;
    char* end = nullptr;
    const double n = std::strtod(json.c_str() + i, &end);
    if (end == json.c_str() + i)
        return false;
    *value = n;
    *end_pos = static_cast<size_t>(end - json.c_str());
    return true;
}

void TrimAscii(std::string& text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                             text.front() == '\r'))
        text.erase(text.begin());
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                             text.back() == '\r'))
        text.pop_back();
}

bool ParseWhisperJson(const std::string& json, std::vector<WhisperSegment>& out) {
    out.clear();
    size_t pos = 0;
    const size_t transcription = json.find("\"transcription\"");
    if (transcription != std::string::npos)
        pos = transcription;

    while (true) {
        const size_t offsets = json.find("\"offsets\"", pos);
        if (offsets == std::string::npos)
            break;
        const size_t from_key = json.find("\"from\"", offsets);
        const size_t from_colon = from_key == std::string::npos ? std::string::npos : json.find(':', from_key);
        double from_ms = -1.0;
        size_t after_from = 0;
        if (!ParseJsonNumber(json, from_colon, &from_ms, &after_from))
            break;
        const size_t to_key = json.find("\"to\"", after_from);
        const size_t to_colon = to_key == std::string::npos ? std::string::npos : json.find(':', to_key);
        double to_ms = -1.0;
        size_t after_to = 0;
        if (!ParseJsonNumber(json, to_colon, &to_ms, &after_to))
            break;
        const size_t text_key = json.find("\"text\"", after_to);
        const size_t text_colon = text_key == std::string::npos ? std::string::npos : json.find(':', text_key);
        std::string text;
        size_t after_text = 0;
        if (!ParseJsonString(json, text_colon, &text, &after_text))
            break;
        TrimAscii(text);
        WhisperSegment seg;
        seg.t0 = from_ms / 1000.0;
        seg.t1 = to_ms / 1000.0;
        seg.text = std::move(text);
        if (seg.t0 >= 0.0 && seg.t1 > seg.t0 && !seg.text.empty())
            out.push_back(std::move(seg));
        pos = after_text + 1;
    }
    if (!out.empty())
        return true;

    pos = transcription != std::string::npos ? transcription : 0;
    while (true) {
        const size_t ts = json.find("\"timestamps\"", pos);
        if (ts == std::string::npos)
            break;
        const size_t from_key = json.find("\"from\"", ts);
        const size_t from_colon = from_key == std::string::npos ? std::string::npos : json.find(':', from_key);
        std::string from;
        size_t after_from = 0;
        if (!ParseJsonString(json, from_colon, &from, &after_from))
            break;
        const size_t to_key = json.find("\"to\"", after_from);
        const size_t to_colon = to_key == std::string::npos ? std::string::npos : json.find(':', to_key);
        std::string to;
        size_t after_to = 0;
        if (!ParseJsonString(json, to_colon, &to, &after_to))
            break;
        const size_t text_key = json.find("\"text\"", after_to);
        const size_t text_colon = text_key == std::string::npos ? std::string::npos : json.find(':', text_key);
        std::string text;
        size_t after_text = 0;
        if (!ParseJsonString(json, text_colon, &text, &after_text))
            break;
        TrimAscii(text);
        WhisperSegment seg;
        seg.t0 = ParseTimestamp(from);
        seg.t1 = ParseTimestamp(to);
        seg.text = std::move(text);
        if (seg.t0 >= 0.0 && seg.t1 > seg.t0 && !seg.text.empty())
            out.push_back(std::move(seg));
        pos = after_text + 1;
    }
    return !out.empty();
}

std::string ReadFileUtf8(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 32 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok)
        return {};
    data.resize(read);
    return data;
}

}  // namespace

bool WhisperRuntime::available() const {
    return !cli_path().empty();
}

std::wstring WhisperRuntime::cli_path() const {
    return FindCli();
}

std::wstring WhisperRuntime::model_path() const {
    return FindModel();
}

std::wstring WhisperRuntime::last_error() const {
    return last_error_;
}

std::wstring WhisperRuntime::backend_label() const {
    const std::wstring cli = cli_path();
    if (cli.find(L"\\Vulkan\\") != std::wstring::npos)
        return L"PotPlayer Whisper (Vulkan)";
    if (cli.find(L"\\CPU\\") != std::wstring::npos)
        return L"PotPlayer Whisper (CPU)";
    return cli.empty() ? L"Whisper CLI not found" : L"PotPlayer Whisper";
}

bool WhisperRuntime::ensure_model(const ProgressFn& progress) {
    last_error_.clear();
    if (!model_path().empty())
        return true;
    last_error_ =
        L"No Whisper model. Install one in PotPlayer (Preferences → Speech) "
        L"or place ggml-base.bin in %LOCALAPPDATA%\\upPlayer\\models\\";
    if (progress)
        progress(last_error_);
    return false;
}

std::string WhisperRuntime::last_detected_language() const {
    return last_detected_language_;
}

bool WhisperRuntime::transcribe(const std::wstring& wav_path, const std::string& prompt,
                                std::vector<WhisperSegment>& out, std::wstring* error,
                                const std::string& language) {
    out.clear();
    last_error_.clear();
    const std::wstring cli = cli_path();
    const std::wstring model = model_path();
    if (cli.empty()) {
        last_error_ = L"PotPlayer Whisper CLI missing (third_party/potplayer-whisper)";
        if (error)
            *error = last_error_;
        return false;
    }
    if (model.empty()) {
        last_error_ = L"Whisper model missing";
        if (error)
            *error = last_error_;
        return false;
    }

    wchar_t temp_dir[MAX_PATH] = {};
    wchar_t out_base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_dir);
    if (!GetTempFileNameW(temp_dir, L"upwh", 0, out_base)) {
        last_error_ = L"Cannot create temp output";
        if (error)
            *error = last_error_;
        return false;
    }
    DeleteFileW(out_base);
    const std::wstring json_path = std::wstring(out_base) + L".json";

    SYSTEM_INFO sysinfo{};
    GetSystemInfo(&sysinfo);
    int threads = static_cast<int>(sysinfo.dwNumberOfProcessors);
    if (threads < 2)
        threads = 2;
    if (threads > 8)
        threads = 8;

    std::string lang = language.empty() ? "auto" : language;
    for (char& c : lang) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }

    std::wstring cmd = Quote(cli) + L" -m " + Quote(model) + L" -f " + Quote(wav_path) + L" -l ";
    cmd += std::wstring(lang.begin(), lang.end());
    cmd += L" -t " + std::to_wstring(threads) + L" -oj -of " + Quote(out_base) + L" -np";
    if (!prompt.empty()) {
        const int needed =
            MultiByteToWideChar(CP_UTF8, 0, prompt.data(), static_cast<int>(prompt.size()), nullptr, 0);
        std::wstring wide_prompt;
        if (needed > 0) {
            wide_prompt.resize(static_cast<size_t>(needed));
            MultiByteToWideChar(CP_UTF8, 0, prompt.data(), static_cast<int>(prompt.size()), wide_prompt.data(),
                                needed);
        }
        cmd += L" --prompt " + Quote(wide_prompt);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd;
    std::wstring cwd = ExeDirectory();
    const auto slash = cli.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        cwd = cli.substr(0, slash);

    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        last_error_ = L"Failed to start PotPlayer Whisper";
        if (error)
            *error = last_error_;
        return false;
    }
    process_ = pi.hProcess;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    process_ = nullptr;

    std::string json = ReadFileUtf8(json_path);
    DeleteFileW(json_path.c_str());
    if (json.empty()) {
        last_error_ = exit_code != 0 ? L"Whisper failed" : L"Whisper produced no JSON";
        if (error)
            *error = last_error_;
        return false;
    }

    {
        const size_t key = json.find("\"language\"");
        if (key != std::string::npos) {
            const size_t colon = json.find(':', key);
            std::string detected;
            size_t end = 0;
            if (ParseJsonString(json, colon, &detected, &end) && !detected.empty() && detected != "auto") {
                for (char& c : detected) {
                    if (c >= 'A' && c <= 'Z')
                        c = static_cast<char>(c - 'A' + 'a');
                }
                last_detected_language_ = detected;
            }
        }
    }

    if (!ParseWhisperJson(json, out)) {
        if (exit_code != 0) {
            last_error_ = L"Whisper failed";
            if (error)
                *error = last_error_;
            return false;
        }
        return true;
    }
    return true;
}

void WhisperRuntime::cancel() {
    HANDLE proc = process_;
    if (proc)
        TerminateProcess(proc, 1);
}
