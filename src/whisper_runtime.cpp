#include "whisper_runtime.hpp"

#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

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

void AppendOpenWhisperCliCandidates(std::vector<std::wstring>& out, const std::wstring& root) {
    if (root.empty())
        return;
    // Official ggml-org/whisper.cpp Windows builds (MIT).
    out.push_back(root + L"\\whisper-cli.exe");
    out.push_back(root + L"\\main.exe");
    out.push_back(root + L"\\whisper.exe");
    out.push_back(root + L"\\Release\\whisper-cli.exe");
    out.push_back(root + L"\\Release\\main.exe");
    out.push_back(root + L"\\bin\\whisper-cli.exe");
}

void AppendPotPlayerWhisperCliCandidates(std::vector<std::wstring>& out, const std::wstring& root) {
    if (root.empty())
        return;
    // Proprietary PotPlayer Module\\Whisper — optional local fallback only.
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

void AppendWhisperRoots(std::vector<std::wstring>& roots) {
    const std::wstring root = ExeDirectory();
    roots.push_back(root + L"\\whisper");
    roots.push_back(root);
    roots.push_back(root + L"\\..\\..\\third_party\\whisper-win");
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0)
        roots.push_back(std::wstring(local) + L"\\upPlayer\\whisper");
}

std::wstring FindCli() {
    std::vector<std::wstring> candidates;
    std::vector<std::wstring> roots;
    AppendWhisperRoots(roots);
    for (const std::wstring& r : roots)
        AppendOpenWhisperCliCandidates(candidates, r);

    // Optional PotPlayer fallback (proprietary; not shipped by upPlayer).
    const std::wstring root = ExeDirectory();
    AppendPotPlayerWhisperCliCandidates(candidates, root + L"\\potplayer-whisper");
    AppendPotPlayerWhisperCliCandidates(candidates, root + L"\\..\\..\\third_party\\potplayer-whisper");
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0)
        AppendPotPlayerWhisperCliCandidates(candidates,
                                            std::wstring(local) + L"\\upPlayer\\potplayer-whisper");
    const std::wstring pot = PotPlayerInstallRoot();
    if (!pot.empty())
        AppendPotPlayerWhisperCliCandidates(candidates, pot + L"\\Module\\Whisper");

    for (const std::wstring& c : candidates) {
        const std::wstring full = Resolve(c);
        if (!full.empty())
            return full;
    }
    return {};
}

std::wstring FindServer() {
    std::vector<std::wstring> roots;
    AppendWhisperRoots(roots);
    for (const std::wstring& r : roots) {
        const std::wstring full = Resolve(r + L"\\whisper-server.exe");
        if (!full.empty())
            return full;
    }
    return {};
}

std::wstring ToLower(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return text;
}

bool SameText(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring FileNameOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return path;
    return path.substr(slash + 1);
}

std::wstring ModelLabel(const std::wstring& filename) {
    std::wstring stem = filename;
    if (stem.size() >= 5 && SameText(stem.substr(0, 5), L"ggml-"))
        stem = stem.substr(5);
    if (stem.size() >= 4 && SameText(stem.substr(stem.size() - 4), L".bin"))
        stem.resize(stem.size() - 4);
    for (wchar_t& c : stem) {
        if (c == L'-')
            c = L' ';
    }
    if (!stem.empty() && stem[0] >= L'a' && stem[0] <= L'z')
        stem[0] = static_cast<wchar_t>(stem[0] - L'a' + L'A');
    return stem.empty() ? filename : stem;
}

int ModelSortKey(const std::wstring& filename) {
    const std::wstring lower = ToLower(filename);
    struct Rank {
        const wchar_t* name;
        int key;
    };
    static const Rank kOrder[] = {
        {L"ggml-tiny.bin", 10},
        {L"ggml-tiny.en.bin", 11},
        {L"ggml-base.bin", 20},
        {L"ggml-base.en.bin", 21},
        {L"ggml-small.bin", 30},
        {L"ggml-small.en.bin", 31},
        {L"ggml-medium.bin", 40},
        {L"ggml-medium.en.bin", 41},
        {L"ggml-large-v1.bin", 50},
        {L"ggml-large-v2.bin", 51},
        {L"ggml-large-v3.bin", 52},
        {L"ggml-large-v3-turbo.bin", 53},
        {L"ggml-large.bin", 54},
    };
    for (const Rank& item : kOrder) {
        if (lower == item.name)
            return item.key;
    }
    if (lower.find(L"tiny") != std::wstring::npos)
        return 15;
    if (lower.find(L"base") != std::wstring::npos)
        return 25;
    if (lower.find(L"small") != std::wstring::npos)
        return 35;
    if (lower.find(L"medium") != std::wstring::npos)
        return 45;
    if (lower.find(L"large") != std::wstring::npos)
        return 55;
    return 1000;
}

void AppendModelDirs(std::vector<std::wstring>& dirs) {
    const std::wstring root = ExeDirectory();
    dirs.push_back(root + L"\\whisper\\models");
    dirs.push_back(root + L"\\models");
    dirs.push_back(root + L"\\..\\..\\third_party\\whisper-win\\models");
    dirs.push_back(root + L"\\potplayer-whisper\\Model");

    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
        dirs.push_back(std::wstring(local) + L"\\upPlayer\\whisper\\models");
        dirs.push_back(std::wstring(local) + L"\\upPlayer\\models");
    }

    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0)
        dirs.push_back(std::wstring(appdata) + L"\\PotPlayerMini64\\Model");

    const std::wstring pot = PotPlayerInstallRoot();
    if (!pot.empty())
        dirs.push_back(pot + L"\\Model");
}

std::vector<WhisperModel> DiscoverModels() {
    std::vector<std::wstring> dirs;
    AppendModelDirs(dirs);

    std::vector<WhisperModel> found;
    for (const std::wstring& dir : dirs) {
        if (dir.empty())
            continue;
        const std::wstring pattern = dir + L"\\ggml-*.bin";
        WIN32_FIND_DATAW data{};
        const HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
        if (handle == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (data.nFileSizeHigh == 0 && data.nFileSizeLow == 0)
                continue;
            const std::wstring filename = data.cFileName;
            bool duplicate = false;
            for (const WhisperModel& existing : found) {
                if (SameText(existing.filename, filename)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;
            WhisperModel model;
            model.path = dir + L"\\" + filename;
            model.filename = filename;
            model.label = ModelLabel(filename);
            found.push_back(std::move(model));
        } while (FindNextFileW(handle, &data));
        FindClose(handle);
    }

    std::sort(found.begin(), found.end(), [](const WhisperModel& a, const WhisperModel& b) {
        const int ka = ModelSortKey(a.filename);
        const int kb = ModelSortKey(b.filename);
        if (ka != kb)
            return ka < kb;
        return _wcsicmp(a.filename.c_str(), b.filename.c_str()) < 0;
    });
    return found;
}

std::wstring PreferredModelPath(const std::vector<WhisperModel>& models, const std::wstring& preferred) {
    if (!preferred.empty()) {
        for (const WhisperModel& model : models) {
            if (SameText(model.filename, preferred))
                return model.path;
        }
    }
    const wchar_t* fallback[] = {
        L"ggml-tiny.bin",
        L"ggml-base.bin",
        L"ggml-small.bin",
        L"ggml-medium.bin",
        L"ggml-large-v3.bin",
    };
    for (const wchar_t* name : fallback) {
        for (const WhisperModel& model : models) {
            if (SameText(model.filename, name))
                return model.path;
        }
    }
    if (!models.empty())
        return models.front().path;
    return {};
}

std::wstring LoadRegString(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buffer[512] = {};
    DWORD type = 0;
    DWORD size = sizeof(buffer);
    const LONG ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);
    if (ok != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return {};
    return buffer;
}

void SaveRegString(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
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

// whisper-server verbose_json: segments[{text,start,end}, ...] with times in seconds.
bool ParseVerboseJson(const std::string& json, std::vector<WhisperSegment>& out) {
    out.clear();
    const size_t segments = json.find("\"segments\"");
    if (segments == std::string::npos)
        return false;
    size_t pos = segments;
    while (true) {
        const size_t text_key = json.find("\"text\"", pos);
        if (text_key == std::string::npos)
            break;
        // Stop once we leave the segments array (detected_language follows).
        const size_t detected = json.find("\"detected_language\"", pos);
        if (detected != std::string::npos && detected < text_key)
            break;
        const size_t text_colon = json.find(':', text_key);
        std::string text;
        size_t after_text = 0;
        if (!ParseJsonString(json, text_colon, &text, &after_text))
            break;
        const size_t start_key = json.find("\"start\"", after_text);
        const size_t start_colon =
            start_key == std::string::npos ? std::string::npos : json.find(':', start_key);
        double start = -1.0;
        size_t after_start = 0;
        if (!ParseJsonNumber(json, start_colon, &start, &after_start))
            break;
        const size_t end_key = json.find("\"end\"", after_start);
        const size_t end_colon = end_key == std::string::npos ? std::string::npos : json.find(':', end_key);
        double end = -1.0;
        size_t after_end = 0;
        if (!ParseJsonNumber(json, end_colon, &end, &after_end))
            break;
        TrimAscii(text);
        // Drop newline-only / whitespace leftovers from server.
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        WhisperSegment seg;
        seg.t0 = start;
        seg.t1 = end;
        seg.text = std::move(text);
        if (seg.t0 >= 0.0 && seg.t1 > seg.t0 && !seg.text.empty())
            out.push_back(std::move(seg));
        pos = after_end + 1;
    }
    return !out.empty();
}

bool ParseAnyWhisperJson(const std::string& json, std::vector<WhisperSegment>& out) {
    if (ParseVerboseJson(json, out))
        return true;
    return ParseWhisperJson(json, out);
}

void CaptureDetectedLanguage(const std::string& json, std::string* out_lang) {
    if (!out_lang)
        return;
    size_t key = json.find("\"detected_language\"");
    if (key == std::string::npos)
        key = json.find("\"language\"");
    if (key == std::string::npos)
        return;
    const size_t colon = json.find(':', key);
    std::string detected;
    size_t end = 0;
    if (!ParseJsonString(json, colon, &detected, &end) || detected.empty() || detected == "auto")
        return;
    for (char& c : detected) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    // Server may return "chinese" — map common names to whisper codes.
    if (detected == "chinese")
        detected = "zh";
    else if (detected == "english")
        detected = "en";
    else if (detected == "japanese")
        detected = "ja";
    else if (detected == "korean")
        detected = "ko";
    *out_lang = detected;
}

std::string MultipartField(const std::string& boundary, const std::string& name, const std::string& value) {
    std::string part;
    part += "--" + boundary + "\r\n";
    part += "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n";
    part += value;
    part += "\r\n";
    return part;
}

bool HttpPostMultipart(int port, const std::string& path, const std::string& body, const std::string& boundary,
                       std::string* response, std::wstring* error) {
    if (!response)
        return false;
    response->clear();
    HINTERNET session = WinHttpOpen(L"upPlayer/whisper", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (error)
            *error = L"WinHTTP open failed";
        return false;
    }
    HINTERNET connect = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        if (error)
            *error = L"WinHTTP connect failed";
        return false;
    }
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wpath.data(), wlen);
    HINTERNET request =
        WinHttpOpenRequest(connect, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (error)
            *error = L"WinHTTP request failed";
        return false;
    }
    // Long ASR jobs (Intel UHD stalls) need generous timeouts.
    WinHttpSetTimeouts(request, 5000, 5000, 30000, 120000);

    const std::wstring ctype =
        L"Content-Type: multipart/form-data; boundary=" + std::wstring(boundary.begin(), boundary.end());
    BOOL ok = WinHttpSendRequest(request, ctype.c_str(), static_cast<DWORD>(-1),
                                 (LPVOID)body.data(), static_cast<DWORD>(body.size()),
                                 static_cast<DWORD>(body.size()), 0);
    if (ok)
        ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (error)
            *error = L"Whisper server request failed";
        return false;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    std::string data;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
            break;
        const size_t off = data.size();
        data.resize(off + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, data.data() + off, avail, &read)) {
            data.resize(off);
            break;
        }
        data.resize(off + read);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    *response = std::move(data);
    if (status < 200 || status >= 300) {
        if (error)
            *error = L"Whisper server HTTP " + std::to_wstring(status);
        return false;
    }
    return true;
}

bool HttpGetOk(int port, const wchar_t* path) {
    HINTERNET session = WinHttpOpen(L"upPlayer/whisper", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;
    HINTERNET connect = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    WinHttpSetTimeouts(request, 1000, 1000, 1000, 2000);
    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok)
        ok = WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    if (ok) {
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok && status >= 200 && status < 500;
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

WhisperRuntime::~WhisperRuntime() {
    cancel();
    stop_server();
}

bool WhisperRuntime::available() const {
    return !cli_path().empty() || !FindServer().empty();
}

std::wstring WhisperRuntime::cli_path() const {
    return FindCli();
}

void WhisperRuntime::load_settings() const {
    if (settings_loaded_)
        return;
    selected_model_file_ = LoadRegString(L"WhisperModel");
    settings_loaded_ = true;
}

std::wstring WhisperRuntime::model_path() const {
    std::wstring selected;
    {
        std::lock_guard<std::mutex> lock(mu_);
        load_settings();
        selected = selected_model_file_;
    }
    return PreferredModelPath(DiscoverModels(), selected);
}

std::wstring WhisperRuntime::model_filename() const {
    return FileNameOf(model_path());
}


std::wstring WhisperRuntime::primary_models_dir() const {
    return ExeDirectory() + L"\\whisper\\models";
}

std::wstring WhisperRuntime::download_url_for_model(const std::wstring& filename) {
    std::wstring name = filename.empty() ? L"ggml-tiny.bin" : FileNameOf(filename);
    if (name.empty())
        name = L"ggml-tiny.bin";
    // Same source as scripts/fetch-whisper-deps.ps1
    return L"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/" + name;
}

std::vector<WhisperModel> WhisperRuntime::models() const {
    std::vector<WhisperModel> found = DiscoverModels();
    const wchar_t* catalog[] = {
        L"ggml-tiny.bin",
        L"ggml-tiny.en.bin",
        L"ggml-base.bin",
        L"ggml-base.en.bin",
        L"ggml-small.bin",
        L"ggml-small.en.bin",
        L"ggml-medium.bin",
        L"ggml-medium.en.bin",
        L"ggml-large-v3.bin",
        L"ggml-large-v3-turbo.bin",
    };
    for (const wchar_t* name : catalog) {
        bool have = false;
        for (const WhisperModel& existing : found) {
            if (SameText(existing.filename, name)) {
                have = true;
                break;
            }
        }
        if (have)
            continue;
        WhisperModel model;
        model.path = primary_models_dir() + L"\\" + name;
        model.filename = name;
        model.label = ModelLabel(name) + L" (download)";
        found.push_back(std::move(model));
    }
    std::sort(found.begin(), found.end(), [](const WhisperModel& a, const WhisperModel& b) {
        const int ka = ModelSortKey(a.filename);
        const int kb = ModelSortKey(b.filename);
        if (ka != kb)
            return ka < kb;
        return _wcsicmp(a.filename.c_str(), b.filename.c_str()) < 0;
    });
    return found;
}

bool WhisperRuntime::select_model(const std::wstring& filename) {
    if (filename.empty())
        return false;
    const std::vector<WhisperModel> available = DiscoverModels();
    std::wstring resolved;
    std::wstring stored_name;
    for (const WhisperModel& model : available) {
        if (SameText(model.filename, filename)) {
            resolved = model.path;
            stored_name = model.filename;
            break;
        }
    }
    if (resolved.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = L"missing:" + filename;
        return false;
    }

    bool restart = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        settings_loaded_ = true;
        selected_model_file_ = stored_name;
        server_failed_ = false;
        restart = server_process_ && !SameText(server_model_, resolved);
    }
    SaveRegString(L"WhisperModel", stored_name);
    if (restart)
        stop_server();
    return true;
}

std::wstring WhisperRuntime::last_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_;
}

std::wstring WhisperRuntime::backend_label() const {
    const std::wstring server = FindServer();
    const std::wstring cli = cli_path();
    const std::wstring path = !server.empty() ? server : cli;
    if (path.empty())
        return L"Whisper CLI not found";

    std::wstring label;
    const bool pot = path.find(L"potplayer-whisper") != std::wstring::npos ||
                     path.find(L"\\Module\\Whisper") != std::wstring::npos ||
                     path.find(L"/Module/Whisper") != std::wstring::npos ||
                     path.find(L"main64.exe") != std::wstring::npos;
    if (pot) {
        if (path.find(L"\\Vulkan\\") != std::wstring::npos)
            label = L"PotPlayer Whisper (Vulkan)";
        else if (path.find(L"\\CPU\\") != std::wstring::npos)
            label = L"PotPlayer Whisper (CPU)";
        else
            label = L"PotPlayer Whisper";
    } else {
        const auto slash = path.find_last_of(L"\\/");
        bool vulkan = false;
        if (slash != std::wstring::npos) {
            const std::wstring vulkan_dll = path.substr(0, slash + 1) + L"ggml-vulkan.dll";
            vulkan = FileExists(vulkan_dll);
        }
        label = vulkan ? L"whisper.cpp (Vulkan)" : L"whisper.cpp (CPU)";
        if (!server.empty()) {
            const bool warm = server_healthy();
            label += warm ? L" server" : L" +server";
        }
        const std::wstring model = model_filename();
        if (!model.empty()) {
            label += L" · ";
            label += ModelLabel(model);
        }
    }
    return label;
}

bool WhisperRuntime::ensure_model(const ProgressFn& progress) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_.clear();
    }
    if (!model_path().empty())
        return true;
    std::wstring expected = L"ggml-tiny.bin";
    {
        std::lock_guard<std::mutex> lock(mu_);
        load_settings();
        if (!selected_model_file_.empty())
            expected = selected_model_file_;
    }
    const std::wstring dir = primary_models_dir();
    const std::wstring url = download_url_for_model(expected);
    std::wstring message =
        L"missing:" + expected + L"|" + dir + L"|" + url;
    {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = message;
    }
    if (progress)
        progress(message);
    return false;
}

std::string WhisperRuntime::last_detected_language() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_detected_language_;
}

bool WhisperRuntime::server_healthy() const {
    if (!server_process_ || server_port_ <= 0)
        return false;
    DWORD code = STILL_ACTIVE;
    if (!GetExitCodeProcess(server_process_, &code) || code != STILL_ACTIVE)
        return false;
    return HttpGetOk(server_port_, L"/");
}

void WhisperRuntime::stop_server() {
    HANDLE proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        proc = server_process_;
        server_process_ = nullptr;
        server_pid_ = 0;
        server_port_ = 0;
        server_model_.clear();
    }
    if (!proc)
        return;
    TerminateProcess(proc, 1);
    WaitForSingleObject(proc, 3000);
    CloseHandle(proc);
}

bool WhisperRuntime::ensure_server() {
    // A second pass picks up a model change that landed while the first start was in flight.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::wstring wanted = model_path();
        bool running_matches = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (server_failed_)
                return false;
            if (server_process_ && server_healthy() &&
                (wanted.empty() || SameText(server_model_, wanted)))
                running_matches = true;
        }
        if (running_matches)
            return true;

        stop_server();

        const std::wstring server = FindServer();
        const std::wstring model = model_path();
        if (server.empty() || model.empty())
            return false;
        if (!start_server(server, model)) {
            std::lock_guard<std::mutex> lock(mu_);
            if (server_failed_)
                return false;
            continue;
        }
        return true;
    }
    return false;
}

bool WhisperRuntime::start_server(const std::wstring& server, const std::wstring& model) {

    SYSTEM_INFO sysinfo{};
    GetSystemInfo(&sysinfo);
    int threads = static_cast<int>(sysinfo.dwNumberOfProcessors);
    if (threads < 2)
        threads = 2;
    if (threads > 16)
        threads = 16;

    const int port = 18765 + static_cast<int>(GetCurrentProcessId() % 2000);
    std::wstring cmd = Quote(server) + L" -m " + Quote(model) + L" -l auto -t " + std::to_wstring(threads) +
                       L" -bs 1 --host 127.0.0.1 --port " + std::to_wstring(port);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd;
    std::wstring cwd = ExeDirectory();
    const auto slash = server.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        cwd = server.substr(0, slash);

    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        std::lock_guard<std::mutex> lock(mu_);
        server_failed_ = true;
        last_error_ = L"server:Failed to start whisper-server";
        return false;
    }
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lock(mu_);
        server_process_ = pi.hProcess;
        server_pid_ = pi.dwProcessId;
        server_port_ = port;
        server_model_ = model;
    }

    if (!SameText(model_path(), model)) {
        stop_server();
        return false;
    }

    // Model load on Vulkan can take several seconds.
    for (int i = 0; i < 60; ++i) {
        if (server_healthy()) {
            if (!SameText(model_path(), model)) {
                stop_server();
                return false;
            }
            return true;
        }
        DWORD code = STILL_ACTIVE;
        if (!GetExitCodeProcess(pi.hProcess, &code) || code != STILL_ACTIVE)
            break;
        Sleep(250);
    }

    const bool superseded = !SameText(model_path(), model);
    stop_server();
    if (superseded)
        return false;
    std::lock_guard<std::mutex> lock(mu_);
    server_failed_ = true;
    last_error_ = L"server:whisper-server failed to become ready";
    return false;
}

bool WhisperRuntime::transcribe_via_server(const std::wstring& wav_path, const std::string& prompt,
                                           std::vector<WhisperSegment>& out, std::wstring* error,
                                           const std::string& language) {
    int port = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        port = server_port_;
    }
    if (port <= 0)
        return false;

    const std::string wav = ReadFileUtf8(wav_path);
    if (wav.empty()) {
        if (error)
            *error = L"Cannot read WAV for whisper-server";
        return false;
    }

    std::string lang = language.empty() ? "auto" : language;
    for (char& c : lang) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }

    const std::string boundary = "----upPlayerWhisper7e3c9a";
    std::string body;
    body.reserve(wav.size() + 512);
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"chunk.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body += wav;
    body += "\r\n";
    body += MultipartField(boundary, "response_format", "verbose_json");
    body += MultipartField(boundary, "temperature", "0.0");
    body += MultipartField(boundary, "temperature_inc", "0.0");
    if (!lang.empty())
        body += MultipartField(boundary, "language", lang);
    if (!prompt.empty())
        body += MultipartField(boundary, "prompt", prompt);
    body += "--" + boundary + "--\r\n";

    std::string response;
    std::wstring http_error;
    if (!HttpPostMultipart(port, "/inference", body, boundary, &response, &http_error)) {
        if (error)
            *error = http_error.empty() ? L"whisper-server inference failed" : http_error;
        return false;
    }

    CaptureDetectedLanguage(response, &last_detected_language_);
    if (!ParseAnyWhisperJson(response, out)) {
        // Empty speech is OK.
        return true;
    }
    return true;
}

bool WhisperRuntime::transcribe_via_cli(const std::wstring& wav_path, const std::string& prompt,
                                        std::vector<WhisperSegment>& out, std::wstring* error,
                                        const std::string& language) {
    const std::wstring cli = cli_path();
    const std::wstring model = model_path();
    if (cli.empty()) {
        last_error_ = L"Whisper CLI missing — run scripts\\fetch-whisper-deps.ps1 "
                      L"(or install open whisper.cpp next to upPlayer\\whisper\\)";
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
    if (threads > 16)
        threads = 16;

    std::string lang = language.empty() ? "auto" : language;
    for (char& c : lang) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }

    std::wstring cmd = Quote(cli) + L" -m " + Quote(model) + L" -f " + Quote(wav_path) + L" -l ";
    cmd += std::wstring(lang.begin(), lang.end());
    cmd += L" -t " + std::to_wstring(threads) + L" -bs 1 -oj -of " + Quote(out_base) + L" -np";
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
        last_error_ = L"Failed to start Whisper CLI";
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

    CaptureDetectedLanguage(json, &last_detected_language_);
    if (!ParseAnyWhisperJson(json, out)) {
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

bool WhisperRuntime::transcribe(const std::wstring& wav_path, const std::string& prompt,
                                std::vector<WhisperSegment>& out, std::wstring* error,
                                const std::string& language) {
    out.clear();
    last_error_.clear();
    if (model_path().empty()) {
        last_error_ = L"Whisper model missing";
        if (error)
            *error = last_error_;
        return false;
    }

    if (ensure_server()) {
        std::wstring server_error;
        if (transcribe_via_server(wav_path, prompt, out, &server_error, language))
            return true;
        // One transient failure: restart once, then fall back to CLI.
        stop_server();
        {
            std::lock_guard<std::mutex> lock(mu_);
            server_failed_ = false;
        }
        if (ensure_server() && transcribe_via_server(wav_path, prompt, out, &server_error, language))
            return true;
        last_error_ = server_error.empty() ? L"whisper-server failed; falling back to CLI" : server_error;
    }

    return transcribe_via_cli(wav_path, prompt, out, error, language);
}

void WhisperRuntime::cancel() {
    HANDLE proc = process_;
    if (proc)
        TerminateProcess(proc, 1);
}
