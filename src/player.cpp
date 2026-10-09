#include "player.hpp"

#include <mpv/client.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty())
        return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
        return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty())
        return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        return {};
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::wstring Trim(std::wstring text) {
    auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!text.empty() && space(text.front()))
        text.erase(text.begin());
    while (!text.empty() && space(text.back()))
        text.pop_back();
    return text;
}

std::wstring DisplayName(const std::wstring& target) {
    if (target.find(L"://") != std::wstring::npos)
        return target;
    const auto slash = target.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return target;
    return target.substr(slash + 1);
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string BuildCommand(const std::vector<std::string>& args, int request_id) {
    std::string json = "{\"command\":[";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i)
            json += ',';
        json += '"';
        json += JsonEscape(args[i]);
        json += '"';
    }
    json += ']';
    if (request_id > 0)
        json += ",\"request_id\":" + std::to_string(request_id);
    json += '}';
    return json;
}

size_t FindKey(const std::string& json, const char* key) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t start = 0;
    while (start < json.size()) {
        const size_t pos = json.find(pattern, start);
        if (pos == std::string::npos)
            return std::string::npos;
        size_t cursor = pos + pattern.size();
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t'))
            ++cursor;
        if (cursor < json.size() && json[cursor] == ':')
            return cursor + 1;
        start = pos + 1;
    }
    return std::string::npos;
}

void SkipWs(const std::string& json, size_t& index) {
    while (index < json.size() &&
           (json[index] == ' ' || json[index] == '\t' || json[index] == '\r' || json[index] == '\n')) {
        ++index;
    }
}

enum class JsonKind { Null, Bool, Number, String, Other };

struct JsonValue {
    JsonKind kind = JsonKind::Other;
    bool boolean = false;
    double number = 0;
    std::string text;
};

bool ParseString(const std::string& json, size_t& index, std::string& out) {
    if (index >= json.size() || json[index] != '"')
        return false;
    ++index;
    out.clear();
    while (index < json.size()) {
        const char c = json[index++];
        if (c == '"')
            return true;
        if (c != '\\' || index >= json.size()) {
            out.push_back(c);
            continue;
        }
        const char escaped = json[index++];
        switch (escaped) {
        case '"':
        case '\\':
        case '/': out.push_back(escaped); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            if (index + 4 > json.size())
                return false;
            unsigned code = 0;
            for (int n = 0; n < 4; ++n) {
                const char hex = json[index++];
                code <<= 4;
                if (hex >= '0' && hex <= '9')
                    code += static_cast<unsigned>(hex - '0');
                else if (hex >= 'a' && hex <= 'f')
                    code += static_cast<unsigned>(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F')
                    code += static_cast<unsigned>(hex - 'A' + 10);
                else
                    return false;
            }
            AppendUtf8(out, code);
            break;
        }
        default: out.push_back(escaped); break;
        }
    }
    return false;
}

bool ParseValue(const std::string& json, size_t index, JsonValue& out) {
    SkipWs(json, index);
    if (index >= json.size())
        return false;
    if (json.compare(index, 4, "null") == 0) {
        out.kind = JsonKind::Null;
        return true;
    }
    if (json.compare(index, 4, "true") == 0) {
        out.kind = JsonKind::Bool;
        out.boolean = true;
        return true;
    }
    if (json.compare(index, 5, "false") == 0) {
        out.kind = JsonKind::Bool;
        out.boolean = false;
        return true;
    }
    if (json[index] == '"') {
        out.kind = JsonKind::String;
        return ParseString(json, index, out.text);
    }
    if (json[index] == '-' || (json[index] >= '0' && json[index] <= '9')) {
        char* end = nullptr;
        out.number = std::strtod(json.c_str() + index, &end);
        if (end == json.c_str() + index)
            return false;
        out.kind = JsonKind::Number;
        return true;
    }
    out.kind = JsonKind::Other;
    return true;
}

bool JsonGet(const std::string& json, const char* key, JsonValue& out) {
    const size_t pos = FindKey(json, key);
    if (pos == std::string::npos)
        return false;
    return ParseValue(json, pos, out);
}

std::string JsonGetString(const std::string& json, const char* key) {
    JsonValue value;
    if (!JsonGet(json, key, value) || value.kind != JsonKind::String)
        return {};
    return value.text;
}

std::wstring ExeDirectory() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return L".";
    std::wstring path(buffer, length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L".";
    return path.substr(0, slash);
}

std::wstring FindOnPath(const wchar_t* file) {
    wchar_t buffer[MAX_PATH];
    const DWORD length = SearchPathW(nullptr, file, nullptr, MAX_PATH, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return buffer;
}

std::wstring ReadTail(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const LONGLONG keep = 4096;
    if (size.QuadPart > keep) {
        LARGE_INTEGER pos{};
        pos.QuadPart = size.QuadPart - keep;
        SetFilePointerEx(file, pos, nullptr, FILE_BEGIN);
    }
    std::string data;
    char buffer[1024];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        data.append(buffer, buffer + read);
        if (data.size() > 8192)
            break;
    }
    CloseHandle(file);
    return Utf8ToWide(data);
}

std::wstring QuoteArg(const std::wstring& text) {
    std::wstring out = L"\"";
    for (wchar_t c : text) {
        if (c == L'"')
            out += L"\\\"";
        else
            out += c;
    }
    out += L'"';
    return out;
}

}  // namespace

struct Player::Api {
    using CreateFn = mpv_handle* (*)();
    using InitializeFn = int (*)(mpv_handle*);
    using TerminateFn = void (*)(mpv_handle*);
    using CommandFn = int (*)(mpv_handle*, const char**);
    using SetOptionFn = int (*)(mpv_handle*, const char*, mpv_format, void*);
    using SetOptionStringFn = int (*)(mpv_handle*, const char*, const char*);
    using SetPropertyFn = int (*)(mpv_handle*, const char*, mpv_format, void*);
    using SetPropertyStringFn = int (*)(mpv_handle*, const char*, const char*);
    using GetPropertyStringFn = char* (*)(mpv_handle*, const char*);
    using ObserveFn = int (*)(mpv_handle*, uint64_t, const char*, mpv_format);
    using WakeupFn = void (*)(mpv_handle*, void (*)(void*), void*);
    using WaitEventFn = mpv_event* (*)(mpv_handle*, double);
    using ErrorStringFn = const char* (*)(int);
    using FreeFn = void (*)(void*);

    CreateFn create = nullptr;
    InitializeFn initialize = nullptr;
    TerminateFn terminate_destroy = nullptr;
    CommandFn command = nullptr;
    SetOptionFn set_option = nullptr;
    SetOptionStringFn set_option_string = nullptr;
    SetPropertyFn set_property = nullptr;
    SetPropertyStringFn set_property_string = nullptr;
    GetPropertyStringFn get_property_string = nullptr;
    ObserveFn observe_property = nullptr;
    WakeupFn set_wakeup_callback = nullptr;
    WaitEventFn wait_event = nullptr;
    ErrorStringFn error_string = nullptr;
    FreeFn free_fn = nullptr;

    template <class T>
    bool load(HMODULE module, const char* name, T& fn) {
        fn = reinterpret_cast<T>(GetProcAddress(module, name));
        return fn != nullptr;
    }

    bool bind(HMODULE module) {
        return load(module, "mpv_create", create) &&
               load(module, "mpv_initialize", initialize) &&
               load(module, "mpv_terminate_destroy", terminate_destroy) &&
               load(module, "mpv_command", command) &&
               load(module, "mpv_set_property", set_property) &&
               load(module, "mpv_set_property_string", set_property_string) &&
               load(module, "mpv_get_property_string", get_property_string) &&
               load(module, "mpv_observe_property", observe_property) &&
               load(module, "mpv_set_wakeup_callback", set_wakeup_callback) &&
               load(module, "mpv_wait_event", wait_event) &&
               load(module, "mpv_error_string", error_string) &&
               load(module, "mpv_free", free_fn) &&
               load(module, "mpv_set_option", set_option) &&
               load(module, "mpv_set_option_string", set_option_string);
    }
};

Player::Player() = default;

Player::~Player() {
    shutdown();
    delete api_;
}

bool Player::start(HWND video, HWND notify) {
    shutdown();
    delete api_;
    api_ = nullptr;
    start_error_.clear();
    notify_hwnd_ = notify;
    notify_.store(reinterpret_cast<uintptr_t>(notify));

    std::wstring lib_error;
    std::wstring exe_error;
    if (start_libmpv(video, lib_error))
        return true;
    if (start_exe(video, exe_error))
        return true;

    start_error_ = L"无法启动播放内核。请把 libmpv-2.dll 放在程序旁边，或安装 mpv 并加入 PATH。";
    if (!lib_error.empty())
        start_error_ += L"\nlibmpv: " + lib_error;
    if (!exe_error.empty())
        start_error_ += L"\nmpv.exe: " + exe_error;
    return false;
}

void Player::shutdown() {
    const bool was_running = running_.exchange(false);
    if (!was_running && !process_ && !ctx_ && !reader_.joinable() && !writer_.joinable())
        return;
    notify_.store(0);

    if (ctx_ && api_ && api_->set_wakeup_callback)
        api_->set_wakeup_callback(static_cast<mpv_handle*>(ctx_), nullptr, nullptr);

    if (process_ && writer_run_.load())
        write_line("{\"command\":[\"quit\"]}");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(write_mu_);
            if (write_queue_.empty())
                break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        Sleep(10);
    }
    stop_io();
    if (process_ && WaitForSingleObject(process_, 400) != WAIT_OBJECT_0)
        TerminateProcess(process_, 0);

    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (process_) {
        WaitForSingleObject(process_, 1000);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (process_thread_) {
        CloseHandle(process_thread_);
        process_thread_ = nullptr;
    }
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
    if (!log_path_.empty()) {
        DeleteFileW(log_path_.c_str());
        log_path_.clear();
    }
    fail_libmpv();
    backend_exe_ = false;
    backend_label_.clear();
}

bool Player::start_libmpv(HWND video, std::wstring& error) {
    std::vector<std::wstring> candidates;
    candidates.push_back(ExeDirectory() + L"\\libmpv-2.dll");
    std::wstring walk = ExeDirectory();
    for (int i = 0; i < 6; ++i) {
        candidates.push_back(walk + L"\\third_party\\libmpv-win\\libmpv-2.dll");
        const auto slash = walk.find_last_of(L"\\/");
        if (slash == std::wstring::npos || slash == 0)
            break;
        walk.resize(slash);
    }
    wchar_t env[MAX_PATH];
    const DWORD env_len = GetEnvironmentVariableW(L"UPPLAYER_LIBMPV", env, MAX_PATH);
    if (env_len > 0 && env_len < MAX_PATH)
        candidates.insert(candidates.begin(), env);

    HMODULE module = nullptr;
    for (const std::wstring& path : candidates) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module)
            break;
        error = L"找到了 libmpv-2.dll，但加载失败。";
    }
    if (!module)
        return false;

    api_ = new Api();
    if (!api_->bind(module)) {
        error = L"libmpv-2.dll 缺少所需的导出函数。";
        FreeLibrary(module);
        delete api_;
        api_ = nullptr;
        return false;
    }
    dll_ = module;

    mpv_handle* ctx = api_->create();
    if (!ctx) {
        error = L"mpv_create 失败。";
        fail_libmpv();
        return false;
    }
    ctx_ = ctx;

    auto option_string = [&](const char* name, const char* value) {
        if (api_->set_option_string)
            api_->set_option_string(ctx, name, value);
    };
    option_string("config", "no");
    option_string("terminal", "no");
    option_string("idle", "yes");
    option_string("keep-open", "yes");
    option_string("force-window", "no");
    option_string("hwdec", "auto-safe");
    option_string("osc", "yes");
    option_string("input-vo-keyboard", "no");
    option_string("screenshot-directory", "~~desktop/");
    option_string("screenshot-template", "upPlayer-%F-%n");
    option_string("screenshot-format", "jpg");

    int64_t wid = static_cast<int64_t>(reinterpret_cast<intptr_t>(video));
    int rc = api_->set_option(ctx, "wid", MPV_FORMAT_INT64, &wid);
    if (rc < 0) {
        error = L"无法把画面嵌入窗口。";
        fail_libmpv();
        return false;
    }
    rc = api_->initialize(ctx);
    if (rc < 0) {
        error = Utf8ToWide(api_->error_string(rc));
        fail_libmpv();
        return false;
    }

    api_->set_wakeup_callback(ctx, [](void* userdata) {
        auto* self = static_cast<Player*>(userdata);
        HWND hwnd = reinterpret_cast<HWND>(self->notify_.load());
        if (!hwnd)
            return;
        bool expected = false;
        if (self->wakeup_posted_.compare_exchange_strong(expected, true))
            PostMessageW(hwnd, WM_PLAYER_WAKEUP, 0, 0);
    }, this);

    api_->observe_property(ctx, 1, "time-pos", MPV_FORMAT_DOUBLE);
    api_->observe_property(ctx, 2, "duration", MPV_FORMAT_DOUBLE);
    api_->observe_property(ctx, 3, "pause", MPV_FORMAT_FLAG);
    api_->observe_property(ctx, 4, "media-title", MPV_FORMAT_STRING);
    api_->observe_property(ctx, 5, "speed", MPV_FORMAT_DOUBLE);
    api_->observe_property(ctx, 6, "width", MPV_FORMAT_INT64);
    api_->observe_property(ctx, 7, "height", MPV_FORMAT_INT64);
    api_->observe_property(ctx, 8, "volume", MPV_FORMAT_DOUBLE);
    api_->observe_property(ctx, 9, "mute", MPV_FORMAT_FLAG);
    api_->observe_property(ctx, 10, "container-fps", MPV_FORMAT_DOUBLE);
    api_->observe_property(ctx, 11, "estimated-vf-fps", MPV_FORMAT_DOUBLE);
    running_.store(true);
    backend_exe_ = false;
    backend_label_ = L"libmpv";
    return true;
}

void Player::fail_libmpv() {
    if (ctx_ && api_ && api_->terminate_destroy) {
        if (api_->set_wakeup_callback)
            api_->set_wakeup_callback(static_cast<mpv_handle*>(ctx_), nullptr, nullptr);
        api_->terminate_destroy(static_cast<mpv_handle*>(ctx_));
    }
    ctx_ = nullptr;
    if (dll_) {
        FreeLibrary(dll_);
        dll_ = nullptr;
    }
}

bool Player::start_exe(HWND video, std::wstring& error) {
    std::wstring mpv = FindOnPath(L"mpv.exe");
    if (mpv.empty())
        mpv = ExeDirectory() + L"\\mpv.exe";
    if (GetFileAttributesW(mpv.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"PATH 中没有 mpv.exe。";
        return false;
    }

    const DWORD pid = GetCurrentProcessId();
    pipe_name_ = L"\\\\.\\pipe\\upplayer-" + std::to_wstring(pid);
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    log_path_ = std::wstring(temp) + L"upplayer-" + std::to_wstring(pid) + L".log";

    std::wstring cmd = QuoteArg(mpv);
    cmd += L" --no-config --no-terminal --idle=yes --keep-open=yes --force-window=no";
    cmd += L" --hwdec=auto-safe --osc=yes --input-vo-keyboard=no --input-cursor=yes";
    cmd += L" --screenshot-directory=~~desktop/ --screenshot-template=upPlayer-%F-%n --screenshot-format=jpg";
    cmd += L" --wid=" + std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(video)));
    cmd += L" --input-ipc-server=" + QuoteArg(pipe_name_);
    cmd += L" --log-file=" + QuoteArg(log_path_);

    std::wstring work = mpv;
    const auto slash = work.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        work.resize(slash);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
    buffer.push_back(L'\0');
    if (!CreateProcessW(mpv.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, work.c_str(), &si, &pi)) {
        error = L"无法启动 mpv.exe。";
        return false;
    }
    process_ = pi.hProcess;
    process_thread_ = pi.hThread;

    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
        AssignProcessToJobObject(job_, process_);
    }

    if (!connect_pipe()) {
        const DWORD exit_code = WaitForSingleObject(process_, 0);
        error = L"无法连接 mpv IPC。";
        if (exit_code == WAIT_OBJECT_0) {
            const std::wstring log = ReadTail(log_path_);
            if (!log.empty())
                error += L"\n" + log;
        }
        fail_exe();
        return false;
    }

    writer_run_.store(true);
    writer_ = std::thread([this] { writer_main(); });
    reader_run_.store(true);
    reader_ = std::thread([this] { reader_main(); });
    write_line("{\"command\":[\"observe_property\",1,\"time-pos\"]}");
    write_line("{\"command\":[\"observe_property\",2,\"duration\"]}");
    write_line("{\"command\":[\"observe_property\",3,\"pause\"]}");
    write_line("{\"command\":[\"observe_property\",4,\"media-title\"]}");
    write_line("{\"command\":[\"observe_property\",5,\"speed\"]}");
    write_line("{\"command\":[\"observe_property\",6,\"width\"]}");
    write_line("{\"command\":[\"observe_property\",7,\"height\"]}");
    write_line("{\"command\":[\"observe_property\",8,\"volume\"]}");
    write_line("{\"command\":[\"observe_property\",9,\"mute\"]}");
    write_line("{\"command\":[\"observe_property\",10,\"container-fps\"]}");
    write_line("{\"command\":[\"observe_property\",11,\"estimated-vf-fps\"]}");

    running_.store(true);
    backend_exe_ = true;
    backend_label_ = L"mpv.exe";
    return true;
}

void Player::fail_exe() {
    stop_io();
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (process_) {
        TerminateProcess(process_, 0);
        WaitForSingleObject(process_, 1000);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (process_thread_) {
        CloseHandle(process_thread_);
        process_thread_ = nullptr;
    }
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
}

bool Player::connect_pipe() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0)
            return false;
        HANDLE pipe = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            pipe_ = pipe;
            return true;
        }
        // mpv sends window messages while attaching to --wid. Pump them or both sides wait forever.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                return false;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return false;
}

bool Player::write_line(const std::string& line) {
    std::string data = line;
    if (data.empty() || data.back() != '\n')
        data.push_back('\n');
    {
        std::lock_guard<std::mutex> lock(write_mu_);
        if (!writer_run_.load() || pipe_ == INVALID_HANDLE_VALUE)
            return false;
        write_queue_.push_back(std::move(data));
    }
    write_cv_.notify_one();
    return true;
}

void Player::stop_io() {
    reader_run_.store(false);
    {
        std::lock_guard<std::mutex> lock(write_mu_);
        writer_run_.store(false);
    }
    write_cv_.notify_all();
    if (pipe_ != INVALID_HANDLE_VALUE)
        CancelIoEx(pipe_, nullptr);
    if (writer_.joinable())
        writer_.join();
    if (reader_.joinable())
        reader_.join();
}

void Player::reader_main() {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string buffer;
    char chunk[4096];
    while (reader_run_.load() && overlapped.hEvent) {
        ResetEvent(overlapped.hEvent);
        DWORD read = 0;
        BOOL ok = ReadFile(pipe_, chunk, sizeof(chunk), &read, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(pipe_, &overlapped, &read, TRUE);
        if (!ok && GetLastError() != ERROR_MORE_DATA)
            break;
        if (read == 0)
            break;
        buffer.append(chunk, chunk + read);
        if (buffer.size() > 1024 * 1024)
            buffer.clear();
        size_t newline = std::string::npos;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                handle_ipc_line(line);
        }
    }
    if (overlapped.hEvent)
        CloseHandle(overlapped.hEvent);
}

void Player::writer_main() {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (overlapped.hEvent) {
        std::string data;
        {
            std::unique_lock<std::mutex> lock(write_mu_);
            write_cv_.wait(lock, [&] { return !write_queue_.empty() || !writer_run_.load(); });
            if (write_queue_.empty())
                break;
            data = std::move(write_queue_.front());
            write_queue_.pop_front();
        }
        write_cv_.notify_all();
        if (pipe_ == INVALID_HANDLE_VALUE)
            break;
        ResetEvent(overlapped.hEvent);
        DWORD written = 0;
        BOOL ok = WriteFile(pipe_, data.data(), static_cast<DWORD>(data.size()), &written, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(pipe_, &overlapped, &written, TRUE);
        if (!ok || written != data.size())
            break;
    }
    if (overlapped.hEvent)
        CloseHandle(overlapped.hEvent);
}

void Player::handle_ipc_line(const std::string& line) {
    const std::string event = JsonGetString(line, "event");
    if (!event.empty()) {
        if (event == "property-change") {
            const std::string name = JsonGetString(line, "name");
            JsonValue data;
            if (!JsonGet(line, "data", data))
                return;
            if (name == "time-pos")
                set_time(data.kind == JsonKind::Number ? data.number : -1.0);
            else if (name == "duration")
                set_duration(data.kind == JsonKind::Number ? data.number : -1.0);
            else if (name == "pause" && data.kind == JsonKind::Bool)
                set_paused(data.boolean);
            else if (name == "media-title" && data.kind == JsonKind::String)
                set_title(Utf8ToWide(data.text));
            else if (name == "speed")
                set_speed_state(data.kind == JsonKind::Number ? data.number : 1.0);
            else if (name == "width")
                set_video_size(data.kind == JsonKind::Number ? static_cast<int>(data.number) : 0, -1);
            else if (name == "height")
                set_video_size(-1, data.kind == JsonKind::Number ? static_cast<int>(data.number) : 0);
            else if (name == "volume")
                set_volume_state(data.kind == JsonKind::Number ? data.number : 100.0);
            else if (name == "mute" && data.kind == JsonKind::Bool)
                set_mute_state(data.boolean);
            else if (name == "container-fps" || name == "estimated-vf-fps")
                set_fps(data.kind == JsonKind::Number ? data.number : 0.0);
        } else if (event == "file-loaded") {
            on_file_loaded();
        } else if (event == "end-file") {
            if (JsonGetString(line, "reason") == "error") {
                std::string message = JsonGetString(line, "file_error");
                if (message.empty())
                    message = "failed to open";
                set_error(Utf8ToWide(message));
            }
        }
        return;
    }

    JsonValue id;
    if (!JsonGet(line, "request_id", id) || id.kind != JsonKind::Number)
        return;
    std::string error = JsonGetString(line, "error");
    if (error.empty())
        error = "success";
    resolve_wait(static_cast<int>(id.number), error);
}

bool Player::load(const std::wstring& target) {
    if (!running_.load())
        return false;
    const std::wstring trimmed = Trim(target);
    if (trimmed.empty() || trimmed.find_first_of(L"\r\n") != std::wstring::npos)
        return false;
    load_utf8(WideToUtf8(trimmed), DisplayName(trimmed), false);
    return true;
}

void Player::append(const std::wstring& target) {
    if (!running_.load())
        return;
    const std::wstring trimmed = Trim(target);
    if (trimmed.empty())
        return;
    post({"loadfile", WideToUtf8(trimmed), "append"});
}

void Player::load_utf8(const std::string& utf8, const std::wstring& display, bool preserve_seek) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!preserve_seek)
            pending_seek_ = -1.0;
        current_utf8_ = utf8;
        current_display_ = display;
        state_.title = display;
        state_.has_media = true;
        state_.last_error.clear();
        state_.time_pos = -1.0;
        state_.duration = -1.0;
        state_.paused = false;
        state_.width = 0;
        state_.height = 0;
    }
    notify();
    post({"loadfile", utf8});
}

bool Player::set_low_latency(bool enabled) {
    if (!running_.load())
        return false;
    if (enabled == low_latency_.load())
        return true;

    std::wstring error;
    bool ok = true;
    if (enabled)
        ok = exec({"apply-profile", "low-latency"}, &error);
    else if (profile_applied_.load())
        ok = exec({"apply-profile", "low-latency", "restore"}, &error);
    if (!ok) {
        if (error.empty())
            error = L"无法切换 low-latency";
        set_error(error);
        return false;
    }

    low_latency_.store(enabled);
    profile_applied_.store(enabled);

    std::string path;
    std::wstring display;
    double seek = -1.0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        path = current_utf8_;
        display = current_display_;
        state_.last_error.clear();
        if (state_.duration > 1.0 && state_.time_pos > 0.5 && state_.time_pos + 0.5 < state_.duration)
            seek = state_.time_pos;
        pending_seek_ = seek;
    }
    notify();
    if (!path.empty())
        load_utf8(path, display.empty() ? Utf8ToWide(path) : display, true);
    return true;
}

bool Player::set_untimed(bool enabled) {
    if (!running_.load())
        return false;
    if (enabled == untimed_.load())
        return true;

    std::wstring error;
    bool ok = false;
    if (!backend_exe_) {
        int flag = enabled ? 1 : 0;
        const int rc = api_->set_property(static_cast<mpv_handle*>(ctx_), "untimed", MPV_FORMAT_FLAG, &flag);
        ok = rc >= 0;
        if (!ok && api_->error_string)
            error = Utf8ToWide(api_->error_string(rc));
    } else {
        const int id = next_id_++;
        const std::string json = std::string("{\"command\":[\"set_property\",\"untimed\",") +
                                 (enabled ? "true" : "false") + "],\"request_id\":" + std::to_string(id) + "}";
        begin_wait(id);
        if (!write_line(json)) {
            error = L"无法写入 mpv IPC。";
        } else {
            std::string result;
            ok = wait_pending(3000, result);
            if (!ok)
                error = result == "timeout" ? L"等待 mpv 响应超时。" : Utf8ToWide(result);
        }
    }
    if (!ok) {
        if (error.empty())
            error = L"无法切换 untimed";
        set_error(error);
        return false;
    }
    untimed_.store(enabled);
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_.last_error.clear();
    }
    notify();
    return true;
}

void Player::toggle_pause() {
    post({"cycle", "pause"});
}

void Player::set_pause(bool paused) {
    if (!running_.load())
        return;
    if (!backend_exe_ && ctx_ && api_ && api_->set_property) {
        int flag = paused ? 1 : 0;
        api_->set_property(static_cast<mpv_handle*>(ctx_), "pause", MPV_FORMAT_FLAG, &flag);
    } else {
        write_line(std::string("{\"command\":[\"set_property\",\"pause\",") +
                   (paused ? "true" : "false") + "]}");
    }
    set_paused(paused);
}

void Player::stop_playback() {
    post({"stop"});
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_utf8_.clear();
        current_display_.clear();
        pending_seek_ = -1.0;
        state_.title.clear();
        state_.has_media = false;
        state_.time_pos = -1.0;
        state_.duration = -1.0;
        state_.paused = false;
        state_.width = 0;
        state_.height = 0;
    }
    notify();
}

void Player::seek_relative(double seconds) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    post({"seek", buf, "relative"});
}

void Player::adjust_speed(double delta) {
    if (delta == 0.0)
        return;
    double current = 1.0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_.speed > 0.0)
            current = state_.speed;
    }
    apply_speed(std::round((current + delta) * 10.0) / 10.0);
}

void Player::reset_speed() {
    apply_speed(1.0);
}

void Player::set_keepaspect(bool keep) {
    if (!running_.load())
        return;
    if (!backend_exe_ && ctx_ && api_ && api_->set_property) {
        int flag = keep ? 1 : 0;
        api_->set_property(static_cast<mpv_handle*>(ctx_), "keepaspect", MPV_FORMAT_FLAG, &flag);
    } else {
        write_line(std::string("{\"command\":[\"set_property\",\"keepaspect\",") +
                   (keep ? "true" : "false") + "]}");
    }
}

void Player::show_stats() {
    post({"script-binding", "stats/display-stats"});
}

void Player::toggle_stats() {
    post({"script-binding", "stats/display-stats-toggle"});
}

void Player::adjust_volume(double delta) {
    if (delta == 0.0)
        return;
    double current = 100.0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_.volume >= 0.0)
            current = state_.volume;
    }
    apply_volume(std::round(current + delta), true);
}

void Player::set_volume(double volume, bool show_osd) {
    apply_volume(volume, show_osd);
}

void Player::take_snapshot() {
    post({"screenshot"});
    show_osd("Snapshot saved to Desktop", 1200);
}

void Player::show_osd(const std::string& text, int duration_ms) {
    if (text.empty())
        return;
    if (duration_ms < 100)
        duration_ms = 100;
    char ms[32];
    snprintf(ms, sizeof(ms), "%d", duration_ms);
    post({"show-text", text, ms});
}

void Player::toggle_mute() {
    bool muted = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        muted = !state_.mute;
    }
    if (!running_.load())
        return;
    if (!backend_exe_ && ctx_ && api_ && api_->set_property) {
        int flag = muted ? 1 : 0;
        api_->set_property(static_cast<mpv_handle*>(ctx_), "mute", MPV_FORMAT_FLAG, &flag);
    } else {
        write_line(std::string("{\"command\":[\"set_property\",\"mute\",") +
                   (muted ? "true" : "false") + "]}");
    }
    set_mute_state(muted);
    if (muted) {
        post({"show-text", "Mute", "800"});
    } else {
        char osd[64];
        double volume = 100.0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            volume = state_.volume;
        }
        snprintf(osd, sizeof(osd), "Volume: %.0f%%", volume);
        post({"show-text", osd, "800"});
    }
}

void Player::apply_volume(double volume, bool show_osd) {
    if (!running_.load())
        return;
    if (volume < 0.0)
        volume = 0.0;
    if (volume > 100.0)
        volume = 100.0;
    bool was_muted = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        was_muted = state_.mute;
    }
    if (!backend_exe_ && ctx_ && api_ && api_->set_property) {
        api_->set_property(static_cast<mpv_handle*>(ctx_), "volume", MPV_FORMAT_DOUBLE, &volume);
        if (was_muted && volume > 0.0) {
            int flag = 0;
            api_->set_property(static_cast<mpv_handle*>(ctx_), "mute", MPV_FORMAT_FLAG, &flag);
            set_mute_state(false);
        }
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f", volume);
        write_line(std::string("{\"command\":[\"set_property\",\"volume\",") + buf + "]}");
        if (was_muted && volume > 0.0) {
            write_line("{\"command\":[\"set_property\",\"mute\",false]}");
            set_mute_state(false);
        }
    }
    if (show_osd) {
        char osd[64];
        snprintf(osd, sizeof(osd), "Volume: %.0f%%", volume);
        post({"show-text", osd, "800"});
    }
    set_volume_state(volume);
}

void Player::apply_speed(double speed) {
    if (!running_.load() || !(speed > 0.0))
        return;
    if (speed < 0.1)
        speed = 0.1;
    if (speed > 20.0)
        speed = 20.0;
    if (!backend_exe_ && ctx_ && api_ && api_->set_property) {
        api_->set_property(static_cast<mpv_handle*>(ctx_), "speed", MPV_FORMAT_DOUBLE, &speed);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.4f", speed);
        write_line(std::string("{\"command\":[\"set_property\",\"speed\",") + buf + "]}");
    }
    char osd[64];
    snprintf(osd, sizeof(osd), "%.1fx", speed);
    post({"show-text", osd, "800"});
    set_speed_state(speed);
}

void Player::seek_absolute(double seconds) {
    if (!(seconds >= 0.0))
        return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    post({"seek", buf, "absolute"});
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_.time_pos = seconds;
    }
    notify();
}

void Player::frame_step(bool forward) {
    post({forward ? "frame-step" : "frame-back-step"});
}

double Player::frame_duration() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_.fps > 1.0 && state_.fps <= 240.0)
        return 1.0 / state_.fps;
    return 1.0 / 30.0;
}

void Player::post(const std::vector<std::string>& args) {
    if (!running_.load() || args.empty())
        return;
    if (!backend_exe_) {
        if (!ctx_ || !api_ || !api_->command)
            return;
        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args)
            argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        api_->command(static_cast<mpv_handle*>(ctx_), argv.data());
        return;
    }
    write_line(BuildCommand(args, 0));
}

bool Player::exec(const std::vector<std::string>& args, std::wstring* error) {
    if (!running_.load())
        return false;
    if (!backend_exe_) {
        if (!ctx_ || !api_ || !api_->command)
            return false;
        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args)
            argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        const int rc = api_->command(static_cast<mpv_handle*>(ctx_), argv.data());
        if (rc < 0) {
            if (error && api_->error_string)
                *error = Utf8ToWide(api_->error_string(rc));
            return false;
        }
        return true;
    }

    const int id = next_id_++;
    begin_wait(id);
    if (!write_line(BuildCommand(args, id))) {
        if (error)
            *error = L"无法写入 mpv IPC。";
        return false;
    }
    std::string result;
    if (!wait_pending(3000, result)) {
        if (error)
            *error = result == "timeout" ? L"等待 mpv 响应超时。" : Utf8ToWide(result);
        return false;
    }
    return true;
}

bool Player::begin_wait(int id) {
    std::lock_guard<std::mutex> lock(wait_mu_);
    pending_id_ = id;
    pending_done_ = false;
    pending_result_.clear();
    return true;
}

void Player::resolve_wait(int id, const std::string& error) {
    std::lock_guard<std::mutex> lock(wait_mu_);
    if (pending_done_ || id != pending_id_)
        return;
    pending_result_ = error;
    pending_done_ = true;
    wait_cv_.notify_one();
}

bool Player::wait_pending(int timeout_ms, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(wait_mu_);
            if (pending_done_) {
                error = pending_result_;
                return error == "success";
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::lock_guard<std::mutex> lock(wait_mu_);
            if (pending_done_) {
                error = pending_result_;
                return error == "success";
            }
            pending_id_ = 0;
            error = "timeout";
            return false;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now())
                        .count();
        if (left < 1)
            left = 1;
        const DWORD slice = static_cast<DWORD>(left > 15 ? 15 : left);
        MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                std::lock_guard<std::mutex> lock(wait_mu_);
                pending_id_ = 0;
                error = "timeout";
                return false;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void Player::on_file_loaded() {
    double seek = -1.0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_.has_media = true;
        if (pending_seek_ >= 0.0) {
            seek = pending_seek_;
            pending_seek_ = -1.0;
        }
    }
    notify();
    if (seek >= 0.0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.3f", seek);
        post({"seek", buf, "absolute"});
    }
}

void Player::set_time(double time_pos) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Refresh UI about every 100ms so the seek bar moves smoothly.
        const int previous = state_.time_pos >= 0.0 ? static_cast<int>(state_.time_pos * 10.0) : -1;
        const int next = time_pos >= 0.0 ? static_cast<int>(time_pos * 10.0) : -1;
        state_.time_pos = time_pos;
        changed = previous != next;
    }
    if (changed)
        notify();
}

void Player::set_duration(double duration) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_.duration = duration;
    }
    notify();
}

void Player::set_title(std::wstring title) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_.title == title)
            return;
        state_.title = std::move(title);
    }
    notify();
}

void Player::set_paused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_.paused == paused)
            return;
        state_.paused = paused;
    }
    notify();
}

void Player::set_speed_state(double speed) {
    if (!(speed > 0.0))
        speed = 1.0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (std::fabs(state_.speed - speed) < 0.0005)
            return;
        state_.speed = speed;
    }
    notify();
}

void Player::set_video_size(int width, int height) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (width >= 0 && state_.width != width) {
            state_.width = width;
            changed = true;
        }
        if (height >= 0 && state_.height != height) {
            state_.height = height;
            changed = true;
        }
    }
    if (changed)
        notify();
}

void Player::set_volume_state(double volume) {
    if (!(volume >= 0.0))
        volume = 0.0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (std::fabs(state_.volume - volume) < 0.05)
            return;
        state_.volume = volume;
    }
    notify();
}

void Player::set_mute_state(bool mute) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_.mute == mute)
            return;
        state_.mute = mute;
    }
    notify();
}

void Player::set_fps(double fps) {
    if (!(fps > 1.0) || fps > 240.0)
        return;
    std::lock_guard<std::mutex> lock(mu_);
    if (std::fabs(state_.fps - fps) < 0.05)
        return;
    state_.fps = fps;
}

void Player::set_error(std::wstring error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_.last_error = std::move(error);
    }
    notify();
}

void Player::notify() {
    const HWND hwnd = reinterpret_cast<HWND>(notify_.load());
    if (!hwnd)
        return;
    bool expected = false;
    if (state_posted_.compare_exchange_strong(expected, true))
        PostMessageW(hwnd, WM_PLAYER_STATE, 0, 0);
}

void Player::consume_notify() {
    state_posted_.store(false);
}

void Player::poll() {
    wakeup_posted_.store(false);
    if (!ctx_ || !api_ || !api_->wait_event)
        return;
    auto* ctx = static_cast<mpv_handle*>(ctx_);
    for (;;) {
        mpv_event* event = api_->wait_event(ctx, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && event->data) {
            auto* prop = static_cast<mpv_event_property*>(event->data);
            if (prop->name && prop->data && prop->format != MPV_FORMAT_NONE) {
                if (std::strcmp(prop->name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE)
                    set_time(*static_cast<double*>(prop->data));
                else if (std::strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE)
                    set_duration(*static_cast<double*>(prop->data));
                else if (std::strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG)
                    set_paused(*static_cast<int*>(prop->data) != 0);
                else if (std::strcmp(prop->name, "speed") == 0 && prop->format == MPV_FORMAT_DOUBLE)
                    set_speed_state(*static_cast<double*>(prop->data));
                else if (std::strcmp(prop->name, "width") == 0 && prop->format == MPV_FORMAT_INT64)
                    set_video_size(static_cast<int>(*static_cast<int64_t*>(prop->data)), -1);
                else if (std::strcmp(prop->name, "height") == 0 && prop->format == MPV_FORMAT_INT64)
                    set_video_size(-1, static_cast<int>(*static_cast<int64_t*>(prop->data)));
                else if (std::strcmp(prop->name, "volume") == 0 && prop->format == MPV_FORMAT_DOUBLE)
                    set_volume_state(*static_cast<double*>(prop->data));
                else if (std::strcmp(prop->name, "mute") == 0 && prop->format == MPV_FORMAT_FLAG)
                    set_mute_state(*static_cast<int*>(prop->data) != 0);
                else if ((std::strcmp(prop->name, "container-fps") == 0 ||
                          std::strcmp(prop->name, "estimated-vf-fps") == 0) &&
                         prop->format == MPV_FORMAT_DOUBLE)
                    set_fps(*static_cast<double*>(prop->data));
                else if (std::strcmp(prop->name, "media-title") == 0 && api_->get_property_string && api_->free_fn) {
                    char* title = api_->get_property_string(ctx, "media-title");
                    if (title) {
                        set_title(Utf8ToWide(title));
                        api_->free_fn(title);
                    }
                }
            } else if (prop->name && std::strcmp(prop->name, "time-pos") == 0) {
                set_time(-1.0);
            }
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            if (api_->get_property_string && api_->free_fn) {
                char* title = api_->get_property_string(ctx, "media-title");
                if (title) {
                    set_title(Utf8ToWide(title));
                    api_->free_fn(title);
                }
            }
            on_file_loaded();
        } else if (event->event_id == MPV_EVENT_END_FILE && event->data) {
            auto* end = static_cast<mpv_event_end_file*>(event->data);
            if (end->reason == MPV_END_FILE_REASON_ERROR) {
                const char* message = api_->error_string ? api_->error_string(end->error) : "playback failed";
                set_error(Utf8ToWide(message ? message : "playback failed"));
            }
        }
    }
}

PlayerState Player::state() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

std::wstring Player::current_path() const {
    std::lock_guard<std::mutex> lock(mu_);
    return Utf8ToWide(current_utf8_);
}

bool Player::low_latency() const { return low_latency_.load(); }
bool Player::untimed() const { return untimed_.load(); }
bool Player::running() const { return running_.load(); }
const std::wstring& Player::backend_label() const { return backend_label_; }
const std::wstring& Player::start_error() const { return start_error_; }
