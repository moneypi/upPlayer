#include "caption_worker.hpp"
#include "whisper_runtime.hpp"
#include "player.hpp"
#include "resource.h"
#include "version.hpp"
#include "yolo_worker.hpp"
#include "lightfc_worker.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <imm.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "imm32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr int kTimerStatus = 1;
constexpr int kTimerPin = 2;
constexpr int kSeekBarId = 3001;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_BUTTON_BOUNDS
#define DWMWA_CAPTION_BUTTON_BOUNDS 5
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif

std::wstring Trim(std::wstring text) {
    auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!text.empty() && space(text.front()))
        text.erase(text.begin());
    while (!text.empty() && space(text.back()))
        text.pop_back();
    return text;
}

bool IsSubtitlePath(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return false;
    const std::wstring ext = path.substr(dot);
    return _wcsicmp(ext.c_str(), L".srt") == 0 || _wcsicmp(ext.c_str(), L".ass") == 0 ||
           _wcsicmp(ext.c_str(), L".ssa") == 0 || _wcsicmp(ext.c_str(), L".vtt") == 0 ||
           _wcsicmp(ext.c_str(), L".sub") == 0;
}

std::wstring FormatTime(double seconds) {
    if (!(seconds >= 0.0) || seconds > 86400.0 * 10.0)
        return L"--:--";
    const int total = static_cast<int>(seconds + 0.5);
    wchar_t buffer[32];
    if (total >= 3600)
        swprintf_s(buffer, L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    else
        swprintf_s(buffer, L"%02d:%02d", total / 60, total % 60);
    return buffer;
}

std::wstring QuoteCmd(const std::wstring& arg) {
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

std::wstring FindExecutable(const wchar_t* name) {
    wchar_t buffer[MAX_PATH];
    const DWORD length = SearchPathW(nullptr, name, nullptr, MAX_PATH, buffer, nullptr);
    if (length > 0 && length < MAX_PATH)
        return buffer;
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        std::wstring dir(buffer);
        const auto slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir.resize(slash);
        const std::wstring candidate = dir + L"\\" + name;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return {};
}

std::string FormatTimeAscii(double seconds) {
    if (!(seconds >= 0.0) || seconds > 86400.0 * 10.0)
        return "--:--";
    const int total = static_cast<int>(seconds + 0.5);
    char buffer[32];
    if (total >= 3600)
        snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    else
        snprintf(buffer, sizeof(buffer), "%02d:%02d", total / 60, total % 60);
    return buffer;
}

std::wstring SuggestClipName(const std::wstring& source, double start, double end) {
    std::wstring base = source;
    const auto q = base.find_first_of(L"?#");
    if (q != std::wstring::npos)
        base = base.substr(0, q);
    const auto slash = base.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        base = base.substr(slash + 1);
    const auto dot = base.find_last_of(L'.');
    std::wstring stem = (dot == std::wstring::npos) ? base : base.substr(0, dot);
    std::wstring ext = (dot == std::wstring::npos) ? L".mp4" : base.substr(dot);
    if (stem.empty())
        stem = L"clip";
    wchar_t name[MAX_PATH];
    swprintf_s(name, L"%s_%s-%s%s", stem.c_str(), FormatTime(start).c_str(), FormatTime(end).c_str(),
               ext.c_str());
    std::wstring out = name;
    for (wchar_t& c : out) {
        if (c == L':' || c == L'/' || c == L'\\' || c == L'?' || c == L'*' || c == L'"' || c == L'<' ||
            c == L'>' || c == L'|')
            c = L'-';
    }
    return out;
}

bool RunProcessWait(const std::wstring& command, DWORD& exit_code, std::wstring* log_out = nullptr) {
    wchar_t temp_dir[MAX_PATH] = {};
    wchar_t log_path[MAX_PATH] = {};
    HANDLE log = INVALID_HANDLE_VALUE;
    if (log_out) {
        GetTempPathW(MAX_PATH, temp_dir);
        if (GetTempFileNameW(temp_dir, L"upcl", 0, log_path)) {
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            log = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log != INVALID_HANDLE_VALUE)
                *log_out = log_path;
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = log;
        si.hStdError = log;
    }
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = command;
    const BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr,
                                   log != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                   &pi);
    if (log != INVALID_HANDLE_VALUE)
        CloseHandle(log);
    if (!ok)
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring ReadTextFileHead(const std::wstring& path, size_t max_chars = 800) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    std::string bytes(max_chars, '\0');
    DWORD read = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    bytes.resize(read);
    if (bytes.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring wide;
    if (needed > 0) {
        wide.resize(static_cast<size_t>(needed));
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(),
                            needed);
    } else {
        wide.assign(bytes.begin(), bytes.end());
    }
    return wide;
}

DWORD LoadRegDword(const wchar_t* name, DWORD fallback) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return fallback;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LONG ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (ok != ERROR_SUCCESS || type != REG_DWORD)
        return fallback;
    return value;
}

void SaveRegDword(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

std::wstring LoadAppRegString(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buffer[1024] = {};
    DWORD type = 0;
    DWORD size = sizeof(buffer);
    const LONG ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);
    if (ok != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return {};
    return buffer;
}

std::wstring StripTrailingSlashes(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

std::wstring UserKnownFolder(int csidl, const wchar_t* fallback_name) {
    wchar_t path[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, path) == S_OK && path[0])
        return path;
    wchar_t profile[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n > 0 && n < MAX_PATH && fallback_name && fallback_name[0])
        return std::wstring(profile) + L"\\" + fallback_name;
    return {};
}

std::wstring UserPicturesFolder() {
    return UserKnownFolder(CSIDL_MYPICTURES, L"Pictures");
}

std::wstring UserVideosFolder() {
    return UserKnownFolder(CSIDL_MYVIDEO, L"Videos");
}

std::wstring SavedDirectoryOrDefault(const wchar_t* reg_name, const std::wstring& fallback) {
    const std::wstring saved = StripTrailingSlashes(LoadAppRegString(reg_name));
    if (!saved.empty()) {
        const DWORD attr = GetFileAttributesW(saved.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            return saved;
    }
    return fallback;
}

std::wstring SnapshotDirectory() {
    return SavedDirectoryOrDefault(L"SnapshotDirectory", UserPicturesFolder());
}

std::wstring ClipOutputDirectory() {
    return SavedDirectoryOrDefault(L"ClipOutputDirectory", UserVideosFolder());
}

bool IsNetworkTarget(const std::wstring& path) {
    return path.find(L"://") != std::wstring::npos;
}

constexpr size_t kResumeLimit = 20;

std::vector<std::pair<std::wstring, double>> LoadResumePositions() {
    std::vector<std::pair<std::wstring, double>> items;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return items;
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, L"ResumePositions", nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_MULTI_SZ || size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return items;
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(key, L"ResumePositions", nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()),
                         &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return items;
    }
    RegCloseKey(key);
    size_t i = 0;
    while (i < buffer.size() && buffer[i] != L'\0') {
        const std::wstring item = &buffer[i];
        i += wcslen(&buffer[i]) + 1;
        const auto tab = item.find(L'\t');
        if (tab == std::wstring::npos || tab == 0)
            continue;
        const double pos = _wtof(item.c_str() + tab + 1);
        if (pos >= 1.0)
            items.emplace_back(item.substr(0, tab), pos);
        if (items.size() >= kResumeLimit)
            break;
    }
    return items;
}

void SaveResumePositions(const std::vector<std::pair<std::wstring, double>>& items) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    std::wstring blob;
    const size_t count = (std::min)(items.size(), kResumeLimit);
    for (size_t i = 0; i < count; ++i) {
        wchar_t pos[32];
        swprintf_s(pos, L"%.3f", items[i].second);
        blob += items[i].first;
        blob.push_back(L'\t');
        blob += pos;
        blob.push_back(L'\0');
    }
    blob.push_back(L'\0');
    RegSetValueExW(key, L"ResumePositions", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(blob.c_str()),
                   static_cast<DWORD>(blob.size() * sizeof(wchar_t)));
    RegCloseKey(key);
}

double LookupResume(const std::wstring& path) {
    if (path.empty() || IsNetworkTarget(path))
        return -1.0;
    for (const auto& item : LoadResumePositions()) {
        if (_wcsicmp(item.first.c_str(), path.c_str()) == 0)
            return item.second;
    }
    return -1.0;
}

void StoreResume(const std::wstring& path, double position, double duration) {
    if (path.empty() || path.find(L'\t') != std::wstring::npos || IsNetworkTarget(path))
        return;
    auto items = LoadResumePositions();
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const std::pair<std::wstring, double>& item) {
                                   return _wcsicmp(item.first.c_str(), path.c_str()) == 0;
                               }),
                items.end());
    const bool keep = position >= 3.0 && !(duration > 1.0 && position + 5.0 >= duration);
    if (keep)
        items.insert(items.begin(), {path, position});
    if (items.size() > kResumeLimit)
        items.resize(kResumeLimit);
    SaveResumePositions(items);
}

std::wstring PickFolder(HWND owner, const std::wstring& initial, const wchar_t* title) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE)
        return {};
    IFileDialog* dialog = nullptr;
    std::wstring result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) &&
        dialog) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title && title[0] ? title : L"Choose folder");
        if (!initial.empty()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (com == S_OK)
        CoUninitialize();
    return result;
}

void SaveAppRegString(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

enum class MenuLanguage { English, Chinese };

MenuLanguage g_menu_language = MenuLanguage::English;

void LoadMenuLanguage() {
    g_menu_language = LoadAppRegString(L"MenuLanguage") == L"zh" ? MenuLanguage::Chinese
                                                                 : MenuLanguage::English;
}

const wchar_t* Tr(const wchar_t* english) {
    if (g_menu_language != MenuLanguage::Chinese || !english)
        return english;
    struct Pair {
        const wchar_t* en;
        const wchar_t* zh;
    };
    static const Pair kText[] = {
        {L"Open...\tCtrl+O", L"打开...\tCtrl+O"},
        {L"Open Network Address...\tCtrl+U", L"打开网络地址...\tCtrl+U"},
        {L"Take Snapshot\ts", L"截图\ts"},
        {L"Subtitle", L"字幕"},
        {L"Load Subtitle...", L"加载字幕..."},
        {L"Unload Subtitle", L"卸载字幕"},
        {L"Playback", L"播放"},
        {L"Play/Pause\tSpace", L"播放/暂停\tSpace"},
        {L"Play\tSpace", L"播放\tSpace"},
        {L"Pause\tSpace", L"暂停\tSpace"},
        {L"Stop", L"停止"},
        {L"Seek -5s\tLeft", L"后退 5 秒\tLeft"},
        {L"Seek +5s\tRight", L"前进 5 秒\tRight"},
        {L"Frame Back\t,", L"上一帧\t,"},
        {L"Frame Forward\t.", L"下一帧\t."},
        {L"Stats\ti", L"统计\ti"},
        {L"Stats Toggle\tI", L"开关统计\tI"},
        {L"Speed", L"速度"},
        {L"Speed %.1fx", L"速度 %.1fx"},
        {L"Slower\tCtrl+Down", L"减慢\tCtrl+Down"},
        {L"Faster\tCtrl+Up", L"加快\tCtrl+Up"},
        {L"Reset 1x\tBackspace", L"恢复 1x\tBackspace"},
        {L"Size", L"尺寸"},
        {L"Toggle Fullscreen\tEnter / Double-click", L"切换全屏\tEnter / 双击"},
        {L"Fullscreen Keep Aspect\t5", L"全屏保持比例\t5"},
        {L"Fullscreen Stretch\t6", L"全屏拉伸\t6"},
        {L"Volume", L"音量"},
        {L"Volume Muted", L"音量 已静音"},
        {L"Volume %d", L"音量 %d"},
        {L"Down\tDown", L"减小\tDown"},
        {L"Up\tUp", L"增大\tUp"},
        {L"Mute\tm", L"静音\tm"},
        {L"Clip", L"片段"},
        {L"Clip In ", L"片段入点 "},
        {L"Clip Out ", L"片段出点 "},
        {L"Mark In\tz", L"标记入点\tz"},
        {L"Mark Out\tx", L"标记出点\tx"},
        {L"Export Clip...\tc", L"导出片段...\tc"},
        {L"Cancel Clip\tEsc", L"取消片段\tEsc"},
        {L"AI", L"AI"},
        {L"Auto Captions\ta", L"自动字幕\ta"},
        {L"Whisper Model", L"Whisper 模型"},
        {L"Language", L"字幕语言"},
        {L"Generate Subtitle...", L"生成字幕..."},
        {L"Cancel Generate Subtitle\tEsc", L"取消生成字幕\tEsc"},
        {L"YOLO26 Detect\ty", L"YOLO26 检测\ty"},
        {L"LightFC Track\tt", L"LightFC 跟踪\tt"},
        {L"Settings...", L"设置..."},
        {L"About upPlayer...", L"关于 upPlayer..."},
        {L"Exit", L"退出"},
        {L"Download model...", L"下载模型..."},
        {L"Settings", L"设置"},
        {L"Prefer low-latency for network streams", L"网络流优先低延迟"},
        {L"Recent URLs to keep:", L"保留的最近地址:"},
        {L"Snapshot folder:", L"截图文件夹:"},
        {L"Clip folder:", L"片段文件夹:"},
        {L"Menu language:", L"菜单语言:"},
        {L"Browse...", L"浏览..."},
        {L"OK", L"确定"},
        {L"Cancel", L"取消"},
        {L"Snapshot folder", L"截图文件夹"},
        {L"Clip folder", L"片段文件夹"},
        {L"Recent URLs to keep must be between 1 and 20.", L"保留的最近地址须在 1 到 20 之间。"},
        {L"Choose an existing snapshot folder.", L"请选择已有的截图文件夹。"},
        {L"Choose an existing clip folder.", L"请选择已有的片段文件夹。"},
        {L"Settings saved", L"设置已保存"},
        {L"Snapshot format:", L"截图格式:"},
        {L"Always on Top", L"窗口置顶"},
        {L"Audio", L"音轨"},
        {L"Off", L"关闭"},
        {L"Live", L"直播"},
        {L"Audio ", L"音轨 "},
        {L"Subtitle ", L"字幕 "},
        {L"Audio: ", L"音轨: "},
        {L"Subtitle: ", L"字幕: "},
        {L"Subtitles off", L"字幕已关闭"},
        {L"Whisper model load failed", L"Whisper 模型加载失败"},
        {L"Whisper model: ", L"Whisper 模型: "},
        {L"Caption language: ", L"字幕语言: "},
        {L"Clip In: ", L"入点: "},
        {L"Clip Out: ", L"出点: "},
        {L"Resumed ", L"已恢复到 "},
        {L"Captions on", L"自动字幕开"},
        {L"Captions off", L"自动字幕关"},
        {L"Cancelling subtitle...", L"正在取消字幕..."},
        {L"Generating subtitle...", L"正在生成字幕..."},
        {L"Loaded: ", L"已加载: "},
        {L"Unloaded subtitle", L"已卸载字幕"},
        {L"YOLO26 off", L"YOLO26 关"},
        {L"YOLO26 on (skip busy frames)", L"YOLO26 开（忙时跳帧）"},
        {L"LightFC off", L"LightFC 关"},
        {L"LightFC: click a target", L"LightFC：点击目标"},
        {L"LightFC tracking", L"LightFC 跟踪中"},
        {L"LightFC: locking...", L"LightFC：锁定中..."},
        {L"No media", L"没有媒体"},
        {L"Clip cancelled", L"已取消片段"},
        {L"Mark In (z) and Out (x) first", L"请先标记入点 (z) 和出点 (x)"},
        {L"Export needs a local file", L"导出需要本地文件"},
        {L"Exporting clip...", L"正在导出片段..."},
        {L"Clip exported. Start may move to the nearest keyframe", L"片段已导出。起点可能移到最近的关键帧"},
        {L"Subtitle loaded", L"字幕已加载"},
        {L"SRT saved", L"字幕已保存"},
        {L"Generate cancelled", L"已取消生成"},
        {L"SRT failed", L"字幕生成失败"},
        {L"Volume: ", L"音量: "},
        {L"Mute", L"静音"},
        {L"Snapshot saved", L"截图已保存"},
        {L"Snapshot saved to ", L"截图已保存到 "},
        {L"Open Network Address", L"打开网络地址"},
        {L"Address(&A):", L"地址(&A):"},
        {L"Low latency", L"低延迟"},
        {L"Clear", L"清除"},
        {L"Enter a network address.", L"请输入网络地址。"},
        {L"Recent list is already empty.", L"最近地址已经是空的。"},
        {L"Clear all recent network addresses?", L"清除全部最近的网络地址？"},
        {L"Open", L"打开"},
        {L"Media files", L"媒体文件"},
        {L"All files", L"所有文件"},
        {L"About upPlayer", L"关于 upPlayer"},
        {L"Lightweight Win32 front-end for mpv.", L"基于 mpv 的轻量 Win32 播放器。"},
        {L"Playback: ", L"播放后端: "},
        {L"License: MIT (see LICENSE / NOTICE)", L"许可：MIT（见 LICENSE / NOTICE）"},
        {L"Load Subtitle", L"加载字幕"},
        {L"Subtitle files", L"字幕文件"},
        {L"upPlayer - Load Subtitle", L"upPlayer - 加载字幕"},
        {L"Open a video first.", L"请先打开视频。"},
        {L"Failed to load subtitle.", L"加载字幕失败。"},
        {L"upPlayer - Unload Subtitle", L"upPlayer - 卸载字幕"},
        {L"Failed to unload subtitle.", L"卸载字幕失败。"},
        {L"Export Clip", L"导出片段"},
        {L"upPlayer - Generate Subtitle", L"upPlayer - 生成字幕"},
        {L"Cannot start Generate Subtitle.", L"无法开始生成字幕。"},
        {L"Generate Subtitle failed.", L"生成字幕失败。"},
        {L"Subtitle file saved:\r\n\r\n", L"字幕文件已保存:\r\n\r\n"},
    };
    for (const Pair& row : kText) {
        if (wcscmp(row.en, english) == 0)
            return row.zh;
    }
    return english;
}

void ApplySettingsDialogLanguage(HWND dialog) {
    SetWindowTextW(dialog, Tr(L"Settings"));
    SetDlgItemTextW(dialog, IDC_SETTINGS_LOW_LATENCY, Tr(L"Prefer low-latency for network streams"));
    SetDlgItemTextW(dialog, IDC_SETTINGS_RECENT_LABEL, Tr(L"Recent URLs to keep:"));
    SetDlgItemTextW(dialog, IDC_SETTINGS_SNAP_LABEL, Tr(L"Snapshot folder:"));
    SetDlgItemTextW(dialog, IDC_SETTINGS_SNAP_FORMAT_LABEL, Tr(L"Snapshot format:"));
    SetDlgItemTextW(dialog, IDC_SETTINGS_CLIP_LABEL, Tr(L"Clip folder:"));
    SetDlgItemTextW(dialog, IDC_SETTINGS_MENU_LANG_LABEL, Tr(L"Menu language:"));
    SetDlgItemTextW(dialog, IDC_SETTINGS_SNAPSHOT_BROWSE, Tr(L"Browse..."));
    SetDlgItemTextW(dialog, IDC_SETTINGS_CLIP_BROWSE, Tr(L"Browse..."));
    SetDlgItemTextW(dialog, IDOK, Tr(L"OK"));
    SetDlgItemTextW(dialog, IDCANCEL, Tr(L"Cancel"));
}

void Osd(Player& player, const wchar_t* english, int duration_ms) {
    player.show_osd(WideToUtf8(std::wstring(Tr(english))), duration_ms);
}

void Osd(Player& player, const std::wstring& text, int duration_ms) {
    player.show_osd(WideToUtf8(text), duration_ms);
}

std::string SnapshotFormat() {
    return LoadAppRegString(L"SnapshotFormat") == L"png" ? "png" : "jpg";
}

void ApplyUrlDialogLanguage(HWND dialog) {
    SetWindowTextW(dialog, Tr(L"Open Network Address"));
    SetDlgItemTextW(dialog, IDC_URL_ADDRESS_LABEL, Tr(L"Address(&A):"));
    SetDlgItemTextW(dialog, IDC_LOW_LATENCY, Tr(L"Low latency"));
    SetDlgItemTextW(dialog, IDOK, Tr(L"OK"));
    SetDlgItemTextW(dialog, IDCANCEL, Tr(L"Cancel"));
}

size_t MaxRecentUrls() {
    DWORD n = LoadRegDword(L"RecentUrlLimit", 10);
    if (n < 1)
        n = 1;
    if (n > 20)
        n = 20;
    return static_cast<size_t>(n);
}

bool DefaultNetworkLowLatency() {
    return LoadRegDword(L"NetworkLowLatency", 1) != 0;
}

std::vector<std::wstring> LoadRecentUrls() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, L"RecentUrls", nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_MULTI_SZ || size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(key, L"RecentUrls", nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &size) !=
        ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    std::vector<std::wstring> urls;
    size_t i = 0;
    while (i < buffer.size() && buffer[i] != L'\0') {
        const std::wstring item = Trim(&buffer[i]);
        i += wcslen(&buffer[i]) + 1;
        if (!item.empty())
            urls.push_back(item);
        if (urls.size() >= MaxRecentUrls())
            break;
    }
    return urls;
}

void SaveRecentUrls(const std::vector<std::wstring>& urls) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    std::wstring blob;
    const size_t count = (std::min)(urls.size(), MaxRecentUrls());
    for (size_t i = 0; i < count; ++i) {
        blob += urls[i];
        blob.push_back(L'\0');
    }
    blob.push_back(L'\0');
    RegSetValueExW(key, L"RecentUrls", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(blob.c_str()),
                   static_cast<DWORD>(blob.size() * sizeof(wchar_t)));
    RegCloseKey(key);
}

void RememberRecentUrl(const std::wstring& url) {
    const std::wstring trimmed = Trim(url);
    if (trimmed.empty())
        return;
    std::vector<std::wstring> urls = LoadRecentUrls();
    urls.erase(std::remove_if(urls.begin(), urls.end(),
                              [&](const std::wstring& item) {
                                  return _wcsicmp(item.c_str(), trimmed.c_str()) == 0;
                              }),
               urls.end());
    urls.insert(urls.begin(), trimmed);
    if (urls.size() > MaxRecentUrls())
        urls.resize(MaxRecentUrls());
    SaveRecentUrls(urls);
}

bool RemoveRecentUrl(const std::wstring& url) {
    const std::wstring trimmed = Trim(url);
    if (trimmed.empty())
        return false;
    std::vector<std::wstring> urls = LoadRecentUrls();
    const size_t before = urls.size();
    urls.erase(std::remove_if(urls.begin(), urls.end(),
                              [&](const std::wstring& item) {
                                  return _wcsicmp(item.c_str(), trimmed.c_str()) == 0;
                              }),
               urls.end());
    if (urls.size() == before)
        return false;
    SaveRecentUrls(urls);
    return true;
}

void ClearRecentUrls() {
    SaveRecentUrls({});
}

constexpr wchar_t kClearRecentItem[] = L"\u0001clear";
std::wstring url_edit_backup;

bool IsClearRecentItem(const wchar_t* text) {
    return text && wcscmp(text, kClearRecentItem) == 0;
}

void RememberUrlEdit(HWND dialog) {
    wchar_t text[4096] = {};
    GetDlgItemTextW(dialog, IDC_URL, text, 4096);
    if (!IsClearRecentItem(text))
        url_edit_backup = text;
}

void RestoreUrlEditIfClear(HWND dialog) {
    const auto sel = static_cast<int>(SendDlgItemMessageW(dialog, IDC_URL, CB_GETCURSEL, 0, 0));
    wchar_t item[64] = {};
    if (sel >= 0)
        SendDlgItemMessageW(dialog, IDC_URL, CB_GETLBTEXT, static_cast<WPARAM>(sel),
                            reinterpret_cast<LPARAM>(item));
    wchar_t text[4096] = {};
    GetDlgItemTextW(dialog, IDC_URL, text, 4096);
    if (IsClearRecentItem(item) || IsClearRecentItem(text))
        SetDlgItemTextW(dialog, IDC_URL, url_edit_backup.c_str());
}

void FillRecentUrlCombo(HWND combo) {
    if (!combo)
        return;
    wchar_t keep[4096] = {};
    GetWindowTextW(combo, keep, 4096);
    const bool keep_edit = keep[0] != L'\0' && !IsClearRecentItem(keep);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const std::vector<std::wstring> recent = LoadRecentUrls();
    for (const std::wstring& item : recent)
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    if (!recent.empty())
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kClearRecentItem));
    if (keep_edit)
        SetWindowTextW(combo, keep);
    else if (!recent.empty())
        SetWindowTextW(combo, recent.front().c_str());
    else
        SetWindowTextW(combo, L"");
}

int UrlRowMetric(HWND hwnd, int px_at_96dpi) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    return MulDiv(px_at_96dpi, dpi ? static_cast<int>(dpi) : 96, 96);
}

constexpr wchar_t kUrlListProcProp[] = L"upPlayerUrlListProc";
constexpr wchar_t kUrlListComboProp[] = L"upPlayerUrlCombo";
constexpr wchar_t kUrlListDialogProp[] = L"upPlayerUrlDialog";

void DropClearRowIfAlone(HWND combo) {
    if (!combo)
        return;
    if (SendMessageW(combo, CB_GETCOUNT, 0, 0) != 1)
        return;
    wchar_t text[64] = {};
    SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(text));
    if (IsClearRecentItem(text))
        SendMessageW(combo, CB_DELETESTRING, 0, 0);
}

void ClearRecentFromDropdown(HWND list) {
    HWND dialog = reinterpret_cast<HWND>(GetPropW(list, kUrlListDialogProp));
    HWND combo = reinterpret_cast<HWND>(GetPropW(list, kUrlListComboProp));
    if (LoadRecentUrls().empty()) {
        MessageBoxW(dialog, Tr(L"Recent list is already empty."), Tr(L"Open Network Address"),
                    MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(dialog, Tr(L"Clear all recent network addresses?"), Tr(L"Open Network Address"),
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    wchar_t keep[4096] = {};
    if (combo)
        GetWindowTextW(combo, keep, 4096);
    if (IsClearRecentItem(keep))
        keep[0] = L'\0';
    ClearRecentUrls();
    if (combo) {
        SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SetWindowTextW(combo, keep);
    }
}

LRESULT CALLBACK UrlRecentListProc(HWND list, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto old = reinterpret_cast<WNDPROC>(GetPropW(list, kUrlListProcProp));
    if (message == WM_LBUTTONDOWN) {
        const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
        const int index = static_cast<int>(LOWORD(hit));
        if (HIWORD(hit) == 0 && index >= 0) {
            wchar_t text[4096] = {};
            HWND combo = reinterpret_cast<HWND>(GetPropW(list, kUrlListComboProp));
            if (combo)
                SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(text));
            if (IsClearRecentItem(text)) {
                ClearRecentFromDropdown(list);
                return 0;
            }
            RECT item{};
            SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
            if (pt.x >= item.right - UrlRowMetric(list, 22)) {
                if (text[0] != L'\0' && RemoveRecentUrl(text) && combo) {
                    SendMessageW(combo, CB_DELETESTRING, static_cast<WPARAM>(index), 0);
                    DropClearRowIfAlone(combo);
                }
                return 0;
            }
        }
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(list, kUrlListProcProp);
        RemovePropW(list, kUrlListComboProp);
        RemovePropW(list, kUrlListDialogProp);
    }
    return old ? CallWindowProcW(old, list, message, wparam, lparam)
               : DefWindowProcW(list, message, wparam, lparam);
}

void HookUrlDropdownList(HWND dialog) {
    HWND combo = GetDlgItem(dialog, IDC_URL);
    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(combo, &info) || !info.hwndList)
        return;
    if (GetPropW(info.hwndList, kUrlListProcProp))
        return;
    const auto old = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(info.hwndList, GWLP_WNDPROC));
    SetPropW(info.hwndList, kUrlListComboProp, combo);
    SetPropW(info.hwndList, kUrlListDialogProp, dialog);
    SetPropW(info.hwndList, kUrlListProcProp, reinterpret_cast<HANDLE>(old));
    SetWindowLongPtrW(info.hwndList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(UrlRecentListProc));
}

struct UrlDialogResult {
    std::wstring url;
    bool low_latency = true;
};

INT_PTR CALLBACK UrlDlgProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        HWND combo = GetDlgItem(dialog, IDC_URL);
        FillRecentUrlCombo(combo);
        HWND edit = FindWindowExW(combo, nullptr, L"Edit", nullptr);
        if (edit)
            SendMessageW(edit, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"https://example.com/video.mp4"));
        CheckDlgButton(dialog, IDC_LOW_LATENCY, DefaultNetworkLowLatency() ? BST_CHECKED : BST_UNCHECKED);
        ApplyUrlDialogLanguage(dialog);
        SetFocus(combo);
        return FALSE;
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (measure->CtlID != IDC_URL)
            break;
        measure->itemHeight = static_cast<UINT>(UrlRowMetric(dialog, 18));
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (draw->CtlID != IDC_URL)
            break;
        const bool in_edit = (draw->itemState & ODS_COMBOBOXEDIT) != 0;
        const bool selected = !in_edit && (draw->itemState & ODS_SELECTED) != 0;
        const COLORREF bg = GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW);
        const COLORREF fg = GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(draw->hDC, &draw->rcItem, brush);
        DeleteObject(brush);

        wchar_t text[4096] = {};
        if (draw->itemID != static_cast<UINT>(-1))
            SendMessageW(draw->hwndItem, CB_GETLBTEXT, draw->itemID, reinterpret_cast<LPARAM>(text));
        else
            GetWindowTextW(draw->hwndItem, text, 4096);
        const bool clear_row = !in_edit && IsClearRecentItem(text);

        RECT text_rc = draw->rcItem;
        text_rc.left += 4;
        if (!in_edit && !clear_row)
            text_rc.right -= UrlRowMetric(dialog, 22);
        SetBkMode(draw->hDC, TRANSPARENT);
        SetTextColor(draw->hDC, fg);
        if (clear_row) {
            HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
            const HGDIOBJ old_pen = SelectObject(draw->hDC, pen);
            MoveToEx(draw->hDC, draw->rcItem.left, draw->rcItem.top, nullptr);
            LineTo(draw->hDC, draw->rcItem.right, draw->rcItem.top);
            SelectObject(draw->hDC, old_pen);
            DeleteObject(pen);
        }
        DrawTextW(draw->hDC, clear_row ? Tr(L"Clear") : text, -1, &text_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!in_edit && !clear_row && draw->itemID != static_cast<UINT>(-1)) {
            RECT cross_rc = draw->rcItem;
            cross_rc.left = cross_rc.right - UrlRowMetric(dialog, 22);
            SetTextColor(draw->hDC, selected ? fg : RGB(176, 64, 64));
            DrawTextW(draw->hDC, L"\u00D7", 1, &cross_rc,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }
        if (!in_edit && (draw->itemState & ODS_FOCUS))
            DrawFocusRect(draw->hDC, &draw->rcItem);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_URL:
            if (HIWORD(wparam) == CBN_DROPDOWN) {
                RememberUrlEdit(dialog);
                HookUrlDropdownList(dialog);
            } else if (HIWORD(wparam) == CBN_EDITCHANGE) {
                RememberUrlEdit(dialog);
            } else if (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == CBN_SELENDOK) {
                RestoreUrlEditIfClear(dialog);
            }
            return TRUE;
        case IDOK: {
            wchar_t buffer[4096] = {};
            GetDlgItemTextW(dialog, IDC_URL, buffer, 4096);
            const std::wstring url = Trim(buffer);
            if (url.empty() || IsClearRecentItem(url.c_str())) {
                MessageBoxW(dialog, Tr(L"Enter a network address."), Tr(L"Open Network Address"),
                            MB_ICONINFORMATION);
                return TRUE;
            }
            auto* out = reinterpret_cast<UrlDialogResult*>(GetWindowLongPtrW(dialog, DWLP_USER));
            if (out) {
                out->url = url;
                out->low_latency = IsDlgButtonChecked(dialog, IDC_LOW_LATENCY) == BST_CHECKED;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}




INT_PTR CALLBACK SettingsDlgProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID(reinterpret_cast<HWND>(lparam)) == IDC_SETTINGS_RECENT_HINT) {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    case WM_INITDIALOG: {
        LoadMenuLanguage();
        ApplySettingsDialogLanguage(dialog);
        CheckDlgButton(dialog, IDC_SETTINGS_LOW_LATENCY,
                       DefaultNetworkLowLatency() ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(dialog, IDC_SETTINGS_RECENT_COUNT, static_cast<UINT>(MaxRecentUrls()), FALSE);
        SetDlgItemTextW(dialog, IDC_SETTINGS_SNAPSHOT, SnapshotDirectory().c_str());
        SetDlgItemTextW(dialog, IDC_SETTINGS_CLIP_DIR, ClipOutputDirectory().c_str());
        HWND languages = GetDlgItem(dialog, IDC_SETTINGS_MENU_LANG);
        SendMessageW(languages, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(languages, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"中文"));
        SendMessageW(languages, CB_SETCURSEL, g_menu_language == MenuLanguage::Chinese ? 1 : 0, 0);
        HWND formats = GetDlgItem(dialog, IDC_SETTINGS_SNAP_FORMAT);
        SendMessageW(formats, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"JPEG"));
        SendMessageW(formats, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PNG"));
        SendMessageW(formats, CB_SETCURSEL, SnapshotFormat() == "png" ? 1 : 0, 0);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_SETTINGS_SNAPSHOT_BROWSE: {
            wchar_t current[1024] = {};
            GetDlgItemTextW(dialog, IDC_SETTINGS_SNAPSHOT, current, 1024);
            const std::wstring picked = PickFolder(dialog, Trim(current), Tr(L"Snapshot folder"));
            if (!picked.empty())
                SetDlgItemTextW(dialog, IDC_SETTINGS_SNAPSHOT, picked.c_str());
            return TRUE;
        }
        case IDC_SETTINGS_CLIP_BROWSE: {
            wchar_t current[1024] = {};
            GetDlgItemTextW(dialog, IDC_SETTINGS_CLIP_DIR, current, 1024);
            const std::wstring picked = PickFolder(dialog, Trim(current), Tr(L"Clip folder"));
            if (!picked.empty())
                SetDlgItemTextW(dialog, IDC_SETTINGS_CLIP_DIR, picked.c_str());
            return TRUE;
        }
        case IDOK: {
            BOOL translated = FALSE;
            UINT recent = GetDlgItemInt(dialog, IDC_SETTINGS_RECENT_COUNT, &translated, FALSE);
            if (!translated || recent < 1 || recent > 20) {
                MessageBoxW(dialog, Tr(L"Recent URLs to keep must be between 1 and 20."), Tr(L"Settings"),
                            MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            wchar_t snapshot[1024] = {};
            GetDlgItemTextW(dialog, IDC_SETTINGS_SNAPSHOT, snapshot, 1024);
            const std::wstring folder = StripTrailingSlashes(Trim(snapshot));
            if (!folder.empty()) {
                const DWORD attr = GetFileAttributesW(folder.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    MessageBoxW(dialog, Tr(L"Choose an existing snapshot folder."), Tr(L"Settings"),
                                MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
            }
            wchar_t clip_dir[1024] = {};
            GetDlgItemTextW(dialog, IDC_SETTINGS_CLIP_DIR, clip_dir, 1024);
            const std::wstring clip_folder = StripTrailingSlashes(Trim(clip_dir));
            if (!clip_folder.empty()) {
                const DWORD attr = GetFileAttributesW(clip_folder.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    MessageBoxW(dialog, Tr(L"Choose an existing clip folder."), Tr(L"Settings"),
                                MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
            }
            const std::wstring pictures = StripTrailingSlashes(UserPicturesFolder());
            SaveAppRegString(L"SnapshotDirectory",
                             (!folder.empty() && _wcsicmp(folder.c_str(), pictures.c_str()) != 0) ? folder
                                                                                                  : L"");
            const std::wstring videos = StripTrailingSlashes(UserVideosFolder());
            SaveAppRegString(L"ClipOutputDirectory",
                             (!clip_folder.empty() && _wcsicmp(clip_folder.c_str(), videos.c_str()) != 0)
                                 ? clip_folder
                                 : L"");
            SaveRegDword(L"NetworkLowLatency",
                         IsDlgButtonChecked(dialog, IDC_SETTINGS_LOW_LATENCY) == BST_CHECKED ? 1 : 0);
            SaveRegDword(L"RecentUrlLimit", recent);
            const LRESULT language =
                SendMessageW(GetDlgItem(dialog, IDC_SETTINGS_MENU_LANG), CB_GETCURSEL, 0, 0);
            SaveAppRegString(L"MenuLanguage", language == 1 ? L"zh" : L"en");
            const LRESULT format =
                SendMessageW(GetDlgItem(dialog, IDC_SETTINGS_SNAP_FORMAT), CB_GETCURSEL, 0, 0);
            SaveAppRegString(L"SnapshotFormat", format == 1 ? L"png" : L"jpg");
            {
                std::vector<std::wstring> urls = LoadRecentUrls();
                if (urls.size() > recent)
                    urls.resize(recent);
                SaveRecentUrls(urls);
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

bool CopyDlgItemTextToClipboard(HWND dialog, int control_id) {
    const int len = GetWindowTextLengthW(GetDlgItem(dialog, control_id));
    if (len <= 0)
        return false;
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    const int got = GetDlgItemTextW(dialog, control_id, text.data(), len + 1);
    if (got <= 0)
        return false;
    text.resize(static_cast<size_t>(got));
    if (!OpenClipboard(dialog))
        return false;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    void* locked = GlobalLock(mem);
    if (!locked) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    memcpy(locked, text.c_str(), bytes);
    GlobalUnlock(mem);
    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    if (!ok)
        GlobalFree(mem);
    CloseClipboard();
    return ok;
}
struct WhisperModelHelp {
    std::wstring summary;
    std::wstring filename;
    std::wstring directory;
    std::wstring url;
};

INT_PTR CALLBACK WhisperModelHelpDlgProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_INITDIALOG: {
        const auto* help = reinterpret_cast<const WhisperModelHelp*>(lparam);
        SetWindowTextW(dialog, L"upPlayer - Whisper Model");
        if (help) {
            SetDlgItemTextW(dialog, IDC_WHISPER_SUMMARY, help->summary.c_str());
            SetDlgItemTextW(dialog, IDC_WHISPER_FILE, help->filename.c_str());
            SetDlgItemTextW(dialog, IDC_WHISPER_DIR, help->directory.c_str());
            SetDlgItemTextW(dialog, IDC_WHISPER_URL, help->url.c_str());
        }
        const HWND url = GetDlgItem(dialog, IDC_WHISPER_URL);
        if (url) {
            const HFONT base = reinterpret_cast<HFONT>(SendMessageW(url, WM_GETFONT, 0, 0));
            LOGFONTW lf{};
            if (base && GetObjectW(base, sizeof(lf), &lf)) {
                lf.lfUnderline = TRUE;
                const HFONT link = CreateFontIndirectW(&lf);
                if (link) {
                    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(link));
                    SendMessageW(url, WM_SETFONT, reinterpret_cast<WPARAM>(link), TRUE);
                }
            }
        }
        return TRUE;
    }
    case WM_DESTROY: {
        const HFONT link = reinterpret_cast<HFONT>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (link)
            DeleteObject(link);
        SetWindowLongPtrW(dialog, DWLP_USER, 0);
        break;
    }
    case WM_SETCURSOR: {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(dialog, &pt);
        const HWND hit = ChildWindowFromPointEx(dialog, pt, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
        if (hit && GetDlgCtrlID(hit) == IDC_WHISPER_URL) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        const HWND child = reinterpret_cast<HWND>(lparam);
        const int id = GetDlgCtrlID(child);
        const HDC dc = reinterpret_cast<HDC>(wparam);
        if (id == IDC_WHISPER_URL) {
            SetTextColor(dc, RGB(0, 102, 204));
            SetBkColor(dc, GetSysColor(COLOR_3DFACE));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
        }
        if (id == IDC_WHISPER_DIR) {
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_3DFACE));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_WHISPER_DIR:
            if (HIWORD(wparam) == STN_CLICKED || HIWORD(wparam) == STN_DBLCLK) {
                if (CopyDlgItemTextToClipboard(dialog, IDC_WHISPER_DIR))
                    SetWindowTextW(dialog, L"upPlayer - Whisper Model (directory copied)");
                return TRUE;
            }
            break;
        case IDC_WHISPER_URL:
            if (HIWORD(wparam) == STN_CLICKED || HIWORD(wparam) == STN_DBLCLK) {
                if (CopyDlgItemTextToClipboard(dialog, IDC_WHISPER_URL))
                    SetWindowTextW(dialog, L"upPlayer - Whisper Model (URL copied)");
                return TRUE;
            }
            break;
        case IDC_WHISPER_OPEN_DIR: {
            wchar_t dir[MAX_PATH] = {};
            GetDlgItemTextW(dialog, IDC_WHISPER_DIR, dir, MAX_PATH);
            if (dir[0] == L'\0')
                return TRUE;
            CreateDirectoryW(dir, nullptr);  // no-op if it already exists
            const HINSTANCE opened = ShellExecuteW(dialog, L"explore", dir, nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(opened) <= 32)
                ShellExecuteW(dialog, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        case IDOK:
        case IDCANCEL:
            EndDialog(dialog, LOWORD(wparam));
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}
void ShowWhisperModelLoadHelp(HWND owner, const std::wstring& reason, const std::wstring& expected_filename) {
    WhisperRuntime whisper;
    const std::wstring filename = expected_filename.empty() ? L"ggml-tiny.bin" : expected_filename;
    const std::wstring dir = whisper.primary_models_dir();
    const std::wstring url = WhisperRuntime::download_url_for_model(filename);
    WhisperModelHelp help;
    help.summary = L"Whisper model load failed.\r\nReason: ";
    help.summary += reason.empty() ? L"unknown error" : reason;
    help.filename = L"Expected file: " + filename;
    help.directory = dir;
    help.url = url;
    const HINSTANCE instance = owner ? reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE))
                                     : GetModuleHandleW(nullptr);
    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_WHISPER_MODEL), owner,
                                            WhisperModelHelpDlgProc, reinterpret_cast<LPARAM>(&help));
    if (result == -1) {
        std::wstring body = help.summary;
        body += L"\r\n";
        body += help.filename;
        body += L"\r\n\r\nModel directory:\r\n";
        body += dir;
        body += L"\r\n\r\nHugging Face download URL:\r\n";
        body += url;
        body += L"\r\n\r\nSave the .bin into the model directory, then pick Whisper Model again.";
        MessageBoxW(owner, body.c_str(), L"upPlayer 闂?Whisper Model", MB_OK | MB_ICONWARNING);
    }
}

bool MenuCommandPresent(HMENU menu, UINT id) {
    if (!menu)
        return false;
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_ID;
    return GetMenuItemInfoW(menu, id, FALSE, &info) != FALSE;
}

// Show a command when it applies. Remove it otherwise so the menu never shows a gray item.
void SetMenuCommandVisible(HMENU menu, UINT id, bool visible, const wchar_t* text, int insert_pos) {
    if (!menu)
        return;
    const bool present = MenuCommandPresent(menu, id);
    if (visible) {
        if (!present) {
            const int count = GetMenuItemCount(menu);
            if (insert_pos < 0 || insert_pos > count)
                insert_pos = count;
            InsertMenuW(menu, static_cast<UINT>(insert_pos), MF_BYPOSITION | MF_STRING, id, text);
        } else if (text) {
            ModifyMenuW(menu, id, MF_BYCOMMAND | MF_STRING, id, text);
        }
    } else if (present) {
        RemoveMenu(menu, id, MF_BYCOMMAND);
    }
}

int PopupMenuPosition(HMENU parent, HMENU popup) {
    if (!parent || !popup)
        return -1;
    const int count = GetMenuItemCount(parent);
    for (int i = 0; i < count; ++i) {
        if (GetSubMenu(parent, i) == popup)
            return i;
    }
    return -1;
}

int SeparatorBeforePopup(HMENU menu, HMENU popup) {
    const int pos = PopupMenuPosition(menu, popup);
    if (pos < 0)
        return 2;
    for (int i = pos - 1; i >= 0; --i) {
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE;
        if (GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) && (info.fType & MFT_SEPARATOR))
            return i;
    }
    return pos;
}

std::wstring ModuleDirectory() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    std::wstring dir(buffer, length);
    const auto slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    dir.resize(slash);
    return dir;
}

bool PathExists(const std::wstring& path) {
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool AiBundlePresent() {
    const std::wstring root = ModuleDirectory();
    if (root.empty())
        return false;
    const wchar_t* markers[] = {
        L"\\onnxruntime.dll",
        L"\\yolo26\\yolo26n.onnx",
        L"\\lightfc\\lightfc_z.onnx",
        L"\\whisper\\whisper-cli.exe",
        L"\\whisper\\whisper-server.exe",
        L"\\whisper\\main.exe",
        L"\\whisper\\whisper.exe",
    };
    for (const wchar_t* marker : markers) {
        if (PathExists(root + marker))
            return true;
    }
    return false;
}

int MenuCommandPosition(HMENU menu, UINT id) {
    if (!menu)
        return -1;
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        if (GetMenuItemID(menu, i) == id)
            return i;
    }
    return -1;
}

int InsertBeforeAiOrSettings(HMENU menu, HMENU ai_menu) {
    const int ai = PopupMenuPosition(menu, ai_menu);
    if (ai >= 0)
        return ai;
    const int settings = MenuCommandPosition(menu, ID_SETTINGS);
    if (settings >= 0)
        return settings;
    return GetMenuItemCount(menu);
}

void SetPopupVisible(HMENU parent, HMENU popup, const wchar_t* text, int insert_pos, bool visible) {
    if (!parent || !popup)
        return;
    const int found = PopupMenuPosition(parent, popup);
    if (visible) {
        if (found < 0) {
            const int count = GetMenuItemCount(parent);
            if (insert_pos < 0 || insert_pos > count)
                insert_pos = count;
            InsertMenuW(parent, static_cast<UINT>(insert_pos), MF_BYPOSITION | MF_POPUP,
                        reinterpret_cast<UINT_PTR>(popup), text);
        } else if (text) {
            ModifyMenuW(parent, static_cast<UINT>(found), MF_BYPOSITION | MF_POPUP,
                        reinterpret_cast<UINT_PTR>(popup), text);
        }
    } else if (found >= 0) {
        RemoveMenu(parent, static_cast<UINT>(found), MF_BYPOSITION);
    }
}

int MenuSeparatorPosition(HMENU menu) {
    if (!menu)
        return -1;
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE;
        if (GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) && (info.fType & MFT_SEPARATOR))
            return i;
    }
    return -1;
}

void SetSeparatorVisible(HMENU menu, int insert_pos, bool visible) {
    if (!menu)
        return;
    const int found = MenuSeparatorPosition(menu);
    if (visible) {
        if (found < 0) {
            const int count = GetMenuItemCount(menu);
            if (insert_pos < 0 || insert_pos > count)
                insert_pos = count;
            InsertMenuW(menu, static_cast<UINT>(insert_pos), MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        }
    } else if (found >= 0) {
        RemoveMenu(menu, static_cast<UINT>(found), MF_BYPOSITION);
    }
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    bool create(int show, const std::vector<std::wstring>& files);
    int loop();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK VideoProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK SeekBarProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK PinProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_seek_bar(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    bool handle_playback_key(UINT message, WPARAM wparam);
    void disable_ime(HWND hwnd);
    void claim_keyboard_focus();
    void create_children();
    void apply_menu_language();
    void refresh_track_menus();
    void set_always_on_top(bool enabled, bool save);
    void place_pin_button();
    void update_pin_tooltip();
    void layout();
    void update_title();
    void update_seek_bar();
    void seek_from_point(int x);
    void volume_from_point(int x, bool show_osd);
    bool hit_volume_slider(int x) const;
    void bar_metrics(int width, int& pad, int& time_width, int& seek_left, int& seek_right,
                     int& vol_left, int& vol_right) const;
    void open_file();
    void open_url();
    void open_target(const std::wstring& target, bool auto_live_for_network = true);
    void show_context_menu(int screen_x, int screen_y);
    void resize_to_video_scale(double scale);
    void enter_fullscreen(bool stretch);
    void exit_fullscreen();
    void toggle_fullscreen();
    enum class ClipMark { None, In, Out };

    void mark_clip_in();
    void mark_clip_out();
    void clear_clip_marks();
    void export_clip();
    void nudge_clip_or_frame(int direction);
    void handle_escape();
    bool clip_session_active() const;
    void set_clip_mark_time(ClipMark mark, double time, bool announce);
    void normalize_clip_marks();
    ClipMark hit_clip_marker(int x) const;
    void clip_mark_from_point(int x);
    double time_from_seek_x(int x) const;
    int dpi() const;
    int seek_bar_height() const;
    void toggle_captions();
    void generate_srt();
    void load_srt();
    void load_subtitle_file(const std::wstring& path);
    void clear_srt();
    void refresh_caption_menus();
    void select_caption_model(UINT index);
    void select_caption_language(UINT index);
    void sync_captions_media();
    void refresh_caption_osd(bool force_status);
    void cycle_yolo_mode();
    void set_yolo_mode(YoloMode mode);
    void tick_yolo();
    void refresh_yolo_osd();
    void cycle_lightfc_mode();
    void set_lightfc_mode(LightFcMode mode);
    void tick_lightfc();
    void refresh_lightfc_osd();
    bool lightfc_click_select(int client_x, int client_y);
    void note_playback_time(double time_pos);
    void show_about();
    void show_settings();
    void restore_saved_volume();
    void remember_volume();
    void remember_resume();
    bool live_playback() const;
    std::wstring track_label(const MediaTrack& track) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND video_ = nullptr;
    HWND seek_bar_ = nullptr;
    HWND pin_ = nullptr;
    HWND pin_tip_ = nullptr;
    HMENU context_menu_ = nullptr;
    HMENU playback_menu_ = nullptr;
    HMENU speed_menu_ = nullptr;
    HMENU size_menu_ = nullptr;
    HMENU volume_menu_ = nullptr;
    HMENU clip_menu_ = nullptr;
    HMENU overlay_menu_ = nullptr;
    HMENU caption_model_menu_ = nullptr;
    HMENU caption_lang_menu_ = nullptr;
    HMENU subtitle_menu_ = nullptr;
    HMENU audio_menu_ = nullptr;
    std::vector<int> audio_track_ids_;
    std::vector<int> sub_track_ids_;
    bool always_on_top_ = false;
    bool pin_hover_ = false;
    bool pin_pressed_ = false;
    bool external_subtitle_loaded_ = false;
    std::vector<std::wstring> caption_model_ids_;
    std::vector<std::wstring> caption_lang_ids_;
    Player player_;
    std::unique_ptr<CaptionWorker> captions_;
    std::unique_ptr<YoloWorker> yolo_;
    std::unique_ptr<LightFcWorker> lightfc_;
    std::wstring shown_title_;
    std::wstring last_caption_osd_;
    std::vector<std::wstring> pending_files_;
    bool seeking_ = false;
    bool volume_dragging_ = false;
    bool volume_restored_ = false;
    int saved_volume_ = 100;
    bool saved_mute_ = false;
    std::wstring last_resume_path_;
    double last_resume_pos_ = -1.0;
    double seek_preview_ = -1.0;
    double volume_preview_ = -1.0;
    double clip_in_ = -1.0;
    double clip_out_ = -1.0;
    double last_caption_time_ = -1.0;
    ULONGLONG last_caption_osd_tick_ = 0;
    ClipMark clip_active_ = ClipMark::None;
    ClipMark clip_dragging_ = ClipMark::None;
    bool fullscreen_ = false;
    bool suppress_click_pause_ = false;
    RECT windowed_rect_{};
    LONG windowed_style_ = 0;
};

int App::dpi() const {
    const int value = GetDpiForWindow(hwnd_);
    return value > 0 ? value : 96;
}

int App::seek_bar_height() const {
    return MulDiv(34, dpi(), 96);
}

bool App::create(int show, const std::vector<std::wstring>& files) {
    pending_files_ = files;

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSW video_class{};
    video_class.style = CS_DBLCLKS;
    video_class.lpfnWndProc = VideoProc;
    video_class.hInstance = instance_;
    video_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    video_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    video_class.lpszClassName = L"upPlayerVideo";
    RegisterClassW(&video_class);

    WNDCLASSW seek_class{};
    seek_class.lpfnWndProc = SeekBarProc;
    seek_class.hInstance = instance_;
    seek_class.hCursor = LoadCursorW(nullptr, IDC_HAND);
    seek_class.lpszClassName = L"upPlayerSeekBar";
    RegisterClassW(&seek_class);

    WNDCLASSW pin_class{};
    pin_class.lpfnWndProc = PinProc;
    pin_class.hInstance = instance_;
    pin_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pin_class.lpszClassName = L"upPlayerPin";
    RegisterClassW(&pin_class);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WndProc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = L"upPlayerMain";
    window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    window_class.hIconSm = reinterpret_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&window_class);

    const int scale = GetDpiForSystem();
    const int width = MulDiv(960, scale, 96);
    const int height = MulDiv(540, scale, 96);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    hwnd_ = CreateWindowExW(0, L"upPlayerMain", L"upPlayer",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            x, y, width, height, nullptr, nullptr, instance_, this);
    if (!hwnd_)
        return false;

    const HICON icon_big = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    const HICON icon_small = reinterpret_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (icon_big)
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big));
    if (icon_small)
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small));

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    disable_ime(hwnd_);

    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    layout();

    captions_ = std::make_unique<CaptionWorker>(hwnd_);
    yolo_ = std::make_unique<YoloWorker>(hwnd_);
    lightfc_ = std::make_unique<LightFcWorker>(hwnd_);

    player_.set_screenshot_directory(SnapshotDirectory());
    player_.set_screenshot_format(SnapshotFormat());
    if (!player_.start(video_, hwnd_)) {
        MessageBoxW(hwnd_, player_.start_error().c_str(), L"upPlayer", MB_ICONERROR);
    } else {
        restore_saved_volume();
        if (!pending_files_.empty()) {
            open_target(pending_files_[0]);
            for (size_t i = 1; i < pending_files_.size(); ++i)
                player_.append(pending_files_[i]);
            update_title();
        }
    }
    SetTimer(hwnd_, kTimerStatus, 100, nullptr);
    SetTimer(hwnd_, kTimerPin, 200, nullptr);
    update_seek_bar();
    return true;
}

int App::loop() {
    const HACCEL accel = LoadAcceleratorsW(instance_, MAKEINTRESOURCEW(IDC_ACCEL));
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (accel && TranslateAcceleratorW(hwnd_, accel, &message))
            continue;
        if (handle_playback_key(message.message, message.wParam))
            continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void App::disable_ime(HWND hwnd) {
    if (!hwnd)
        return;
    ImmAssociateContext(hwnd, nullptr);
}

void App::claim_keyboard_focus() {
    if (!hwnd_ || GetForegroundWindow() != hwnd_)
        return;
    HWND focus = GetFocus();
    if (focus == hwnd_ || focus == video_ || focus == seek_bar_)
        return;
    if (video_)
        SetFocus(video_);
}

bool App::handle_playback_key(UINT message, WPARAM wparam) {
    if (message == WM_CHAR) {
        switch (wparam) {
        case L'1':
            resize_to_video_scale(0.5);
            return true;
        case L'2':
            resize_to_video_scale(1.0);
            return true;
        case L'3':
            resize_to_video_scale(1.5);
            return true;
        case L'4':
            resize_to_video_scale(2.0);
            return true;
        case L'5':
            enter_fullscreen(false);
            return true;
        case L'6':
            enter_fullscreen(true);
            return true;
        case L'i':
            player_.show_stats();
            return true;
        case L'I':
            player_.toggle_stats();
            return true;
        case L'm':
        case L'M':
            player_.toggle_mute();
            return true;
        case L's':
        case L'S':
            player_.take_snapshot();
            return true;
        case L'a':
        case L'A':
            if (AiBundlePresent())
                toggle_captions();
            return true;
        case L'y':
        case L'Y':
            if (AiBundlePresent())
                cycle_yolo_mode();
            return true;
        case L't':
        case L'T':
            if (AiBundlePresent())
                cycle_lightfc_mode();
            return true;
        default:
            return false;
        }
    }
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN)
        return false;
    switch (wparam) {
    case VK_BACK:
        player_.reset_speed();
        return true;
    case '1':
    case VK_NUMPAD1:
        resize_to_video_scale(0.5);
        return true;
    case '2':
    case VK_NUMPAD2:
        resize_to_video_scale(1.0);
        return true;
    case '3':
    case VK_NUMPAD3:
        resize_to_video_scale(1.5);
        return true;
    case '4':
    case VK_NUMPAD4:
        resize_to_video_scale(2.0);
        return true;
    case '5':
    case VK_NUMPAD5:
        enter_fullscreen(false);
        return true;
    case '6':
    case VK_NUMPAD6:
        enter_fullscreen(true);
        return true;
    case 'I':
        if (GetKeyState(VK_SHIFT) & 0x8000)
            player_.toggle_stats();
        else
            player_.show_stats();
        return true;
    case VK_UP:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            player_.adjust_speed(0.1);
            return true;
        }
        player_.adjust_volume(2.0);
        return true;
    case VK_VOLUME_UP:
        player_.adjust_volume(2.0);
        return true;
    case VK_DOWN:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            player_.adjust_speed(-0.1);
            return true;
        }
        player_.adjust_volume(-2.0);
        return true;
    case VK_VOLUME_DOWN:
        player_.adjust_volume(-2.0);
        return true;
    case 'M':
    case VK_VOLUME_MUTE:
        player_.toggle_mute();
        return true;
    case 'S':
        player_.take_snapshot();
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* app = static_cast<App*>(created->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app)
            app->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app)
        return DefWindowProcW(hwnd, message, wparam, lparam);
    return app->handle(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK App::VideoProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    HWND parent = GetParent(hwnd);
    auto* app = parent ? reinterpret_cast<App*>(GetWindowLongPtrW(parent, GWLP_USERDATA)) : nullptr;
    if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN)
        SetFocus(hwnd);
    if (message == WM_PARENTNOTIFY &&
        (LOWORD(wparam) == WM_LBUTTONDOWN || LOWORD(wparam) == WM_RBUTTONDOWN))
        SetFocus(hwnd);
    if (message == WM_LBUTTONUP && app) {
        SetFocus(hwnd);
        if (app->suppress_click_pause_) {
            app->suppress_click_pause_ = false;
        } else if (app->lightfc_ && app->lightfc_->enabled()) {
            // Arm or Tracking: click (re)selects target; outside letterbox still pauses.
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            if (app->lightfc_click_select(x, y))
                return 0;
            if (app->player_.state().has_media)
                app->player_.toggle_pause();
        } else if (app->player_.state().has_media) {
            app->player_.toggle_pause();
        }
        return 0;
    }
    if (message == WM_LBUTTONDBLCLK && app) {
        SetFocus(hwnd);
        // Undo the pause toggle from the first click of the double-click, then
        // ignore the trailing LBUTTONUP so play state stays unchanged.
        if (app->player_.state().has_media)
            app->player_.toggle_pause();
        app->suppress_click_pause_ = true;
        app->toggle_fullscreen();
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    if (message == WM_MOUSEWHEEL && app) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        if (delta > 0)
            app->player_.adjust_volume(2.0);
        else if (delta < 0)
            app->player_.adjust_volume(-2.0);
        SetFocus(hwnd);
        return 0;
    }
    if (message == WM_CONTEXTMENU && app) {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pt.x == -1 && pt.y == -1) {
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            pt.x = rect.left + (rect.right - rect.left) / 2;
            pt.y = rect.top + (rect.bottom - rect.top) / 2;
        }
        app->show_context_menu(pt.x, pt.y);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK App::SeekBarProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app)
        return DefWindowProcW(hwnd, message, wparam, lparam);
    return app->handle_seek_bar(hwnd, message, wparam, lparam);
}

void PaintPinButton(HWND hwnd, bool pinned, bool hover, bool pressed) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        EndPaint(hwnd, &paint);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ old_bitmap = SelectObject(mem, bitmap);

    COLORREF background = RGB(26, 32, 49);
    COLORREF edge = background;
    RECT pin_rect{};
    if (GetWindowRect(hwnd, &pin_rect)) {
        HDC screen = GetDC(nullptr);
        if (screen) {
            const int sample_x = pin_rect.left - 8;
            background = GetPixel(screen, sample_x, pin_rect.top + height / 2);
            if (background == CLR_INVALID || GetRValue(background) > 90)
                background = GetPixel(screen, sample_x, pin_rect.top + 4);
            edge = GetPixel(screen, sample_x, pin_rect.top);
            if (background == CLR_INVALID)
                background = RGB(26, 32, 49);
            if (edge == CLR_INVALID)
                edge = background;
            ReleaseDC(nullptr, screen);
        }
    }
    HBRUSH fill = CreateSolidBrush(background);
    FillRect(mem, &rect, fill);
    DeleteObject(fill);
    if (edge != background) {
        HPEN edge_pen = CreatePen(PS_SOLID, 1, edge);
        HGDIOBJ previous = SelectObject(mem, edge_pen);
        MoveToEx(mem, 0, 0, nullptr);
        LineTo(mem, width, 0);
        SelectObject(mem, previous);
        DeleteObject(edge_pen);
    }

    if (hover || pressed) {
        const int lift = pressed ? 46 : 28;
        const COLORREF hot = RGB(std::min(255, GetRValue(background) + lift),
                                 std::min(255, GetGValue(background) + lift),
                                 std::min(255, GetBValue(background) + lift));
        const int inset = std::max(2, height / 8);
        RECT hot_rect{inset, inset, width - inset, height - inset};
        HBRUSH hot_brush = CreateSolidBrush(hot);
        const int radius = std::max(4, height / 6);
        HRGN region = CreateRoundRectRgn(hot_rect.left, hot_rect.top, hot_rect.right + 1, hot_rect.bottom + 1,
                                         radius, radius);
        FillRgn(mem, region, hot_brush);
        DeleteObject(region);
        DeleteObject(hot_brush);
    }

    const COLORREF icon = pinned ? RGB(110, 200, 255) : RGB(255, 255, 255);
    const int cx = width / 2;
    const int head_r = std::max(3, height / 8);
    const int head_cy = height / 2 - head_r / 3;
    const int thickness = std::max(2, height / 28);
    HPEN pen = CreatePen(PS_SOLID, thickness, icon);
    HGDIOBJ old_pen = SelectObject(mem, pen);
    HBRUSH head = pinned ? CreateSolidBrush(icon) : static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ old_brush = SelectObject(mem, head);
    Ellipse(mem, cx - head_r, head_cy - head_r, cx + head_r, head_cy + head_r);
    const int shaft_top = head_cy + head_r - thickness;
    const int shaft_bottom = std::min(height - std::max(3, height / 8), shaft_top + head_r * 2);
    MoveToEx(mem, cx, shaft_top, nullptr);
    LineTo(mem, cx, shaft_bottom);
    SelectObject(mem, old_brush);
    SelectObject(mem, old_pen);
    if (pinned)
        DeleteObject(head);
    DeleteObject(pen);

    BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
    EndPaint(hwnd, &paint);
}

LRESULT CALLBACK App::PinProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app)
        return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintPinButton(hwnd, app->always_on_top_, app->pin_hover_, app->pin_pressed_);
        return 0;
    case WM_MOUSEMOVE:
        if (!app->pin_hover_) {
            app->pin_hover_ = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        app->pin_hover_ = false;
        app->pin_pressed_ = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        app->pin_pressed_ = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        RECT rect{};
        GetClientRect(hwnd, &rect);
        const bool inside = PtInRect(&rect, pt) != FALSE;
        app->pin_pressed_ = false;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (inside)
            app->set_always_on_top(!app->always_on_top_, true);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void App::apply_menu_language() {
    auto item = [](HMENU menu, UINT id, const wchar_t* english) {
        if (MenuCommandPresent(menu, id))
            ModifyMenuW(menu, id, MF_BYCOMMAND | MF_STRING, id, Tr(english));
    };
    auto popup = [](HMENU parent, HMENU child, const wchar_t* english) {
        const int pos = PopupMenuPosition(parent, child);
        if (pos >= 0)
            ModifyMenuW(parent, static_cast<UINT>(pos), MF_BYPOSITION | MF_POPUP,
                        reinterpret_cast<UINT_PTR>(child), Tr(english));
    };

    item(context_menu_, ID_FILE_OPEN, L"Open...\tCtrl+O");
    item(context_menu_, ID_FILE_OPEN_URL, L"Open Network Address...\tCtrl+U");
    item(context_menu_, ID_SNAPSHOT, L"Take Snapshot\ts");
    item(context_menu_, ID_SETTINGS, L"Settings...");
    item(context_menu_, ID_HELP_ABOUT, L"About upPlayer...");
    item(context_menu_, ID_FILE_EXIT, L"Exit");
    popup(context_menu_, subtitle_menu_, L"Subtitle");
    popup(context_menu_, playback_menu_, L"Playback");
    popup(context_menu_, speed_menu_, L"Speed");
    popup(context_menu_, size_menu_, L"Size");
    popup(context_menu_, volume_menu_, L"Volume");
    popup(context_menu_, clip_menu_, L"Clip");
    popup(context_menu_, overlay_menu_, L"AI");

    item(playback_menu_, ID_PLAY_PAUSE, L"Play/Pause\tSpace");
    item(playback_menu_, ID_STOP, L"Stop");
    item(playback_menu_, ID_SEEK_BACK, L"Seek -5s\tLeft");
    item(playback_menu_, ID_SEEK_FORWARD, L"Seek +5s\tRight");
    item(playback_menu_, ID_FRAME_BACK, L"Frame Back\t,");
    item(playback_menu_, ID_FRAME_FORWARD, L"Frame Forward\t.");
    popup(playback_menu_, audio_menu_, L"Audio");
    item(playback_menu_, ID_STATS_SHOW, L"Stats\ti");
    item(playback_menu_, ID_STATS_TOGGLE, L"Stats Toggle\tI");

    item(speed_menu_, ID_SPEED_DOWN, L"Slower\tCtrl+Down");
    item(speed_menu_, ID_SPEED_UP, L"Faster\tCtrl+Up");
    item(speed_menu_, ID_SPEED_RESET, L"Reset 1x\tBackspace");

    item(size_menu_, ID_SIZE_05, L"0.5x\t1");
    item(size_menu_, ID_SIZE_10, L"1x\t2");
    item(size_menu_, ID_SIZE_15, L"1.5x\t3");
    item(size_menu_, ID_SIZE_20, L"2x\t4");
    item(size_menu_, ID_FULLSCREEN_TOGGLE, L"Toggle Fullscreen\tEnter / Double-click");
    item(size_menu_, ID_FULLSCREEN_NORMAL, L"Fullscreen Keep Aspect\t5");
    item(size_menu_, ID_FULLSCREEN_STRETCH, L"Fullscreen Stretch\t6");

    item(volume_menu_, ID_VOLUME_DOWN, L"Down\tDown");
    item(volume_menu_, ID_VOLUME_UP, L"Up\tUp");
    item(volume_menu_, ID_VOLUME_MUTE, L"Mute\tm");

    item(clip_menu_, ID_CLIP_MARK_IN, L"Mark In\tz");
    item(clip_menu_, ID_CLIP_MARK_OUT, L"Mark Out\tx");
    item(clip_menu_, ID_CLIP_EXPORT, L"Export Clip...\tc");
    item(clip_menu_, ID_CLIP_CLEAR, L"Cancel Clip\tEsc");

    item(subtitle_menu_, ID_LOAD_SRT, L"Load Subtitle...");
    item(subtitle_menu_, ID_CLEAR_SRT, L"Unload Subtitle");

    item(overlay_menu_, ID_CAPTIONS_TOGGLE, L"Auto Captions\ta");
    item(overlay_menu_, ID_GENERATE_SRT, L"Generate Subtitle...");
    item(overlay_menu_, ID_YOLO_TOGGLE, L"YOLO26 Detect\ty");
    item(overlay_menu_, ID_LIGHTFC_TOGGLE, L"LightFC Track\tt");
    popup(overlay_menu_, caption_model_menu_, L"Whisper Model");
    popup(overlay_menu_, caption_lang_menu_, L"Language");
    update_pin_tooltip();
}

void App::create_children() {
    video_ = CreateWindowExW(0, L"upPlayerVideo", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                             0, 0, 10, 10, hwnd_, nullptr, instance_, nullptr);
    seek_bar_ = CreateWindowExW(0, L"upPlayerSeekBar", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                0, 0, 10, 10, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSeekBarId)),
                                instance_, this);
    disable_ime(video_);
    disable_ime(seek_bar_);

    playback_menu_ = CreatePopupMenu();
    HMENU playback_menu = playback_menu_;
    AppendMenuW(playback_menu, MF_STRING, ID_PLAY_PAUSE, L"Play/Pause\tSpace");
    AppendMenuW(playback_menu, MF_STRING, ID_STOP, L"Stop");
    AppendMenuW(playback_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playback_menu, MF_STRING, ID_SEEK_BACK, L"Seek -5s\tLeft");
    AppendMenuW(playback_menu, MF_STRING, ID_SEEK_FORWARD, L"Seek +5s\tRight");
    AppendMenuW(playback_menu, MF_STRING, ID_FRAME_BACK, L"Frame Back\t,");
    AppendMenuW(playback_menu, MF_STRING, ID_FRAME_FORWARD, L"Frame Forward\t.");
    AppendMenuW(playback_menu, MF_SEPARATOR, 0, nullptr);
    audio_menu_ = CreatePopupMenu();
    AppendMenuW(playback_menu, MF_STRING, ID_STATS_SHOW, L"Stats\ti");
    AppendMenuW(playback_menu, MF_STRING, ID_STATS_TOGGLE, L"Stats Toggle\tI");

    speed_menu_ = CreatePopupMenu();
    HMENU speed_menu = speed_menu_;
    AppendMenuW(speed_menu, MF_STRING, ID_SPEED_DOWN, L"Slower\tCtrl+Down");
    AppendMenuW(speed_menu, MF_STRING, ID_SPEED_UP, L"Faster\tCtrl+Up");
    AppendMenuW(speed_menu, MF_STRING, ID_SPEED_RESET, L"Reset 1x\tBackspace");

    size_menu_ = CreatePopupMenu();
    HMENU size_menu = size_menu_;
    AppendMenuW(size_menu, MF_STRING, ID_SIZE_05, L"0.5x\t1");
    AppendMenuW(size_menu, MF_STRING, ID_SIZE_10, L"1x\t2");
    AppendMenuW(size_menu, MF_STRING, ID_SIZE_15, L"1.5x\t3");
    AppendMenuW(size_menu, MF_STRING, ID_SIZE_20, L"2x\t4");
    AppendMenuW(size_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(size_menu, MF_STRING, ID_FULLSCREEN_TOGGLE, L"Toggle Fullscreen\tEnter / Double-click");
    AppendMenuW(size_menu, MF_STRING, ID_FULLSCREEN_NORMAL, L"Fullscreen Keep Aspect\t5");
    AppendMenuW(size_menu, MF_STRING, ID_FULLSCREEN_STRETCH, L"Fullscreen Stretch\t6");

    volume_menu_ = CreatePopupMenu();
    HMENU volume_menu = volume_menu_;
    AppendMenuW(volume_menu, MF_STRING, ID_VOLUME_DOWN, L"Down\tDown");
    AppendMenuW(volume_menu, MF_STRING, ID_VOLUME_UP, L"Up\tUp");
    AppendMenuW(volume_menu, MF_STRING, ID_VOLUME_MUTE, L"Mute\tm");

    clip_menu_ = CreatePopupMenu();
    HMENU clip_menu = clip_menu_;
    AppendMenuW(clip_menu, MF_STRING, ID_CLIP_MARK_IN, L"Mark In\tz");
    AppendMenuW(clip_menu, MF_STRING, ID_CLIP_MARK_OUT, L"Mark Out\tx");
    AppendMenuW(clip_menu, MF_STRING, ID_CLIP_EXPORT, L"Export Clip...\tc");
    AppendMenuW(clip_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(clip_menu, MF_STRING, ID_CLIP_CLEAR, L"Cancel Clip\tEsc");

    overlay_menu_ = CreatePopupMenu();
    HMENU overlay_menu = overlay_menu_;
    AppendMenuW(overlay_menu, MF_STRING, ID_CAPTIONS_TOGGLE, L"Auto Captions\ta");
    caption_model_menu_ = CreatePopupMenu();
    caption_lang_menu_ = CreatePopupMenu();
    AppendMenuW(overlay_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(caption_model_menu_), L"Whisper Model");
    AppendMenuW(overlay_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(caption_lang_menu_), L"Language");
    AppendMenuW(overlay_menu, MF_STRING, ID_GENERATE_SRT, L"Generate Subtitle...");
    AppendMenuW(overlay_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(overlay_menu, MF_STRING, ID_YOLO_TOGGLE, L"YOLO26 Detect\ty");
    AppendMenuW(overlay_menu, MF_STRING, ID_LIGHTFC_TOGGLE, L"LightFC Track\tt");

    context_menu_ = CreatePopupMenu();
    AppendMenuW(context_menu_, MF_STRING, ID_FILE_OPEN, L"Open...\tCtrl+O");
    AppendMenuW(context_menu_, MF_STRING, ID_FILE_OPEN_URL, L"Open Network Address...\tCtrl+U");

    AppendMenuW(context_menu_, MF_STRING, ID_SNAPSHOT, L"Take Snapshot\ts");
    subtitle_menu_ = CreatePopupMenu();
    AppendMenuW(subtitle_menu_, MF_STRING, ID_LOAD_SRT, L"Load Subtitle...");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(subtitle_menu_), L"Subtitle");
    AppendMenuW(context_menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(playback_menu), L"Playback");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(speed_menu), L"Speed");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(size_menu), L"Size");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(volume_menu), L"Volume");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(clip_menu), L"Clip");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(overlay_menu), L"AI");
    AppendMenuW(context_menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(context_menu_, MF_STRING, ID_SETTINGS, L"Settings...");
    AppendMenuW(context_menu_, MF_STRING, ID_HELP_ABOUT, L"About upPlayer...");
    AppendMenuW(context_menu_, MF_STRING, ID_FILE_EXIT, L"Exit");

    pin_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY, L"upPlayerPin", nullptr, WS_POPUP,
                           0, 0, 0, 0, hwnd_, nullptr, instance_, this);
    pin_tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
                               WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, pin_, nullptr, instance_, nullptr);
    if (pin_tip_) {
        TOOLINFOW info{};
        info.cbSize = sizeof(info);
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS | TTF_TRANSPARENT;
        info.hwnd = pin_;
        info.uId = reinterpret_cast<UINT_PTR>(pin_);
        info.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(pin_tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
    LoadMenuLanguage();
    apply_menu_language();
    set_always_on_top(LoadRegDword(L"AlwaysOnTop", 0) != 0, false);
    place_pin_button();
    DragAcceptFiles(hwnd_, TRUE);
}

void App::refresh_caption_menus() {
    auto clear = [](HMENU menu) {
        if (!menu)
            return;
        while (GetMenuItemCount(menu) > 0)
            DeleteMenu(menu, 0, MF_BYPOSITION);
    };
    clear(caption_model_menu_);
    clear(caption_lang_menu_);
    caption_model_ids_.clear();
    caption_lang_ids_.clear();
    if (!captions_ || !caption_model_menu_ || !caption_lang_menu_)
        return;

    const std::vector<CaptionOption> models = captions_->models();
    const std::wstring current_model = captions_->model_id();
    if (models.empty()) {
        AppendMenuW(caption_model_menu_, MF_STRING, ID_WHISPER_GET_MODEL, Tr(L"Download model..."));
    } else {
        const size_t count = (std::min)(models.size(), static_cast<size_t>(ID_CAPTION_MODEL_LAST - ID_CAPTION_MODEL_FIRST + 1));
        for (size_t i = 0; i < count; ++i) {
            UINT flags = MF_STRING;
            if (_wcsicmp(models[i].id.c_str(), current_model.c_str()) == 0)
                flags |= MF_CHECKED;
            AppendMenuW(caption_model_menu_, flags, ID_CAPTION_MODEL_FIRST + static_cast<UINT>(i),
                        models[i].label.c_str());
            caption_model_ids_.push_back(models[i].id);
        }
    }

    std::vector<CaptionOption> languages = CaptionWorker::languages();
    const std::wstring current_lang = captions_->language();
    bool listed = false;
    for (const CaptionOption& option : languages) {
        if (_wcsicmp(option.id.c_str(), current_lang.c_str()) == 0)
            listed = true;
    }
    if (!current_lang.empty() && !listed)
        languages.push_back(CaptionOption{current_lang, current_lang});
    const size_t lang_count =
        (std::min)(languages.size(), static_cast<size_t>(ID_CAPTION_LANG_LAST - ID_CAPTION_LANG_FIRST + 1));
    for (size_t i = 0; i < lang_count; ++i) {
        UINT flags = MF_STRING;
        if (_wcsicmp(languages[i].id.c_str(), current_lang.c_str()) == 0)
            flags |= MF_CHECKED;
        AppendMenuW(caption_lang_menu_, flags, ID_CAPTION_LANG_FIRST + static_cast<UINT>(i),
                    languages[i].label.c_str());
        caption_lang_ids_.push_back(languages[i].id);
    }
}

void App::select_caption_model(UINT index) {
    if (!captions_ || index >= caption_model_ids_.size())
        return;
    const std::wstring id = caption_model_ids_[index];
    std::wstring label = id;
    for (const CaptionOption& option : captions_->models()) {
        if (_wcsicmp(option.id.c_str(), id.c_str()) == 0) {
            label = option.label;
            break;
        }
    }
    if (!captions_->set_model(id)) {
        ShowWhisperModelLoadHelp(hwnd_,
            L"Model file is missing from the model directory (or could not be loaded).",
            id);
        Osd(player_, L"Whisper model load failed", 1800);
        return;
    }
    Osd(player_, std::wstring(Tr(L"Whisper model: ")) + label, 1400);
}

void App::select_caption_language(UINT index) {
    if (!captions_ || index >= caption_lang_ids_.size())
        return;
    const std::wstring id = caption_lang_ids_[index];
    std::wstring label = id;
    for (const CaptionOption& option : CaptionWorker::languages()) {
        if (_wcsicmp(option.id.c_str(), id.c_str()) == 0) {
            label = option.label;
            break;
        }
    }
    captions_->set_language(id);
    Osd(player_, std::wstring(Tr(L"Caption language: ")) + label, 1400);
}

void App::show_context_menu(int screen_x, int screen_y) {
    if (!context_menu_ || !hwnd_)
        return;
    refresh_caption_menus();
    CheckMenuItem(context_menu_, ID_CAPTIONS_TOGGLE,
                  MF_BYCOMMAND | ((captions_ && captions_->enabled()) ? MF_CHECKED : MF_UNCHECKED));
    {
        const PlayerState playback = player_.state();
        const bool has_media = playback.has_media;
        const bool srt_busy = captions_ && captions_->srt_busy();
        const wchar_t* generate_text =
            srt_busy ? Tr(L"Cancel Generate Subtitle\tEsc") : Tr(L"Generate Subtitle...");
        const bool show_picture_actions = has_media;
        SetMenuCommandVisible(overlay_menu_, ID_CAPTIONS_TOGGLE, show_picture_actions,
                              Tr(L"Auto Captions\ta"), 0);
        const int after_language = PopupMenuPosition(overlay_menu_, caption_lang_menu_) + 1;
        SetMenuCommandVisible(overlay_menu_, ID_GENERATE_SRT, srt_busy || has_media, generate_text,
                              after_language);
        const bool show_detectors = has_media;
        const int after_generate = MenuCommandPresent(overlay_menu_, ID_GENERATE_SRT) ? after_language + 1
                                                                                      : after_language;
        SetSeparatorVisible(overlay_menu_, after_generate, show_detectors);
        const int after_separator = MenuSeparatorPosition(overlay_menu_) + 1;
        SetMenuCommandVisible(overlay_menu_, ID_YOLO_TOGGLE, show_detectors, Tr(L"YOLO26 Detect\ty"),
                              after_separator > 0 ? after_separator : GetMenuItemCount(overlay_menu_));
        SetMenuCommandVisible(overlay_menu_, ID_LIGHTFC_TOGGLE, show_detectors, Tr(L"LightFC Track\tt"),
                              GetMenuItemCount(overlay_menu_));
        SetMenuCommandVisible(subtitle_menu_, ID_LOAD_SRT, has_media, Tr(L"Load Subtitle..."), 0);
        SetMenuCommandVisible(subtitle_menu_, ID_CLEAR_SRT, external_subtitle_loaded_,
                              Tr(L"Unload Subtitle"), GetMenuItemCount(subtitle_menu_));
        const bool subtitle_has_items = subtitle_menu_ && GetMenuItemCount(subtitle_menu_) > 0;
        const bool clip_marked = clip_in_ >= 0.0 || clip_out_ >= 0.0;
        const bool local_file = !IsNetworkTarget(player_.current_path());
        const bool clip_exportable = local_file && clip_in_ >= 0.0 && clip_out_ > clip_in_;
        if (clip_marked) {
            if (!MenuCommandPresent(clip_menu_, ID_CLIP_CLEAR)) {
                const int count = GetMenuItemCount(clip_menu_);
                bool trailing_sep = false;
                if (count > 0) {
                    MENUITEMINFOW info{};
                    info.cbSize = sizeof(info);
                    info.fMask = MIIM_FTYPE;
                    trailing_sep = GetMenuItemInfoW(clip_menu_, static_cast<UINT>(count - 1), TRUE, &info) &&
                                   (info.fType & MFT_SEPARATOR);
                }
                if (!trailing_sep)
                    AppendMenuW(clip_menu_, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(clip_menu_, MF_STRING, ID_CLIP_CLEAR, Tr(L"Cancel Clip\tEsc"));
            }
        } else {
            SetMenuCommandVisible(clip_menu_, ID_CLIP_CLEAR, false, Tr(L"Cancel Clip\tEsc"), 0);
            const int count = GetMenuItemCount(clip_menu_);
            if (count > 0) {
                MENUITEMINFOW info{};
                info.cbSize = sizeof(info);
                info.fMask = MIIM_FTYPE;
                if (GetMenuItemInfoW(clip_menu_, static_cast<UINT>(count - 1), TRUE, &info) &&
                    (info.fType & MFT_SEPARATOR))
                    RemoveMenu(clip_menu_, static_cast<UINT>(count - 1), MF_BYPOSITION);
            }
        }
        SetMenuCommandVisible(clip_menu_, ID_CLIP_EXPORT, clip_exportable, Tr(L"Export Clip...\tc"), 2);
        SetMenuCommandVisible(playback_menu_, ID_PLAY_PAUSE, true,
                              playback.paused ? Tr(L"Play\tSpace") : Tr(L"Pause\tSpace"), 0);
        const bool show_seek = has_media && !player_.low_latency();
        SetMenuCommandVisible(playback_menu_, ID_SEEK_BACK, show_seek, Tr(L"Seek -5s\tLeft"), 3);
        SetMenuCommandVisible(playback_menu_, ID_SEEK_FORWARD, show_seek, Tr(L"Seek +5s\tRight"), 4);

        std::wstring speed_label = Tr(L"Speed");
        if (playback.speed > 0.0 && std::fabs(playback.speed - 1.0) >= 0.05) {
            wchar_t buf[32];
            swprintf_s(buf, Tr(L"Speed %.1fx"), playback.speed);
            speed_label = buf;
        }
        std::wstring volume_label = Tr(L"Volume");
        if (playback.mute) {
            volume_label = Tr(L"Volume Muted");
        } else {
            int level = static_cast<int>(std::lround(playback.volume));
            if (level < 0)
                level = 0;
            if (level > 100)
                level = 100;
            wchar_t buf[32];
            swprintf_s(buf, Tr(L"Volume %d"), level);
            volume_label = buf;
        }

        const bool ai_available = AiBundlePresent();
        const int before_ai = InsertBeforeAiOrSettings(context_menu_, overlay_menu_);
        SetPopupVisible(context_menu_, speed_menu_, speed_label.c_str(),
                        PopupMenuPosition(context_menu_, volume_menu_), has_media);
        SetPopupVisible(context_menu_, volume_menu_, volume_label.c_str(), before_ai, true);
        const int before_speed = SeparatorBeforePopup(context_menu_, speed_menu_);
        SetMenuCommandVisible(context_menu_, ID_SNAPSHOT, has_media, Tr(L"Take Snapshot\ts"), before_speed);
        SetPopupVisible(context_menu_, subtitle_menu_, Tr(L"Subtitle"),
                        SeparatorBeforePopup(context_menu_, speed_menu_), subtitle_has_items);
        SetPopupVisible(context_menu_, playback_menu_, Tr(L"Playback"),
                        PopupMenuPosition(context_menu_, speed_menu_), has_media);
        SetPopupVisible(context_menu_, size_menu_, Tr(L"Size"),
                        PopupMenuPosition(context_menu_, volume_menu_), has_media);
        std::wstring clip_label = Tr(L"Clip");
        if (clip_active_ == ClipMark::In && clip_in_ >= 0.0)
            clip_label = std::wstring(Tr(L"Clip In ")) + FormatTime(clip_in_);
        else if (clip_active_ == ClipMark::Out && clip_out_ >= 0.0)
            clip_label = std::wstring(Tr(L"Clip Out ")) + FormatTime(clip_out_);
        else if (clip_in_ >= 0.0 && clip_out_ >= 0.0)
            clip_label = std::wstring(Tr(L"Clip")) + L" " + FormatTime(clip_in_) + L"\u2013" + FormatTime(clip_out_);
        SetPopupVisible(context_menu_, clip_menu_, clip_label.c_str(),
                        InsertBeforeAiOrSettings(context_menu_, overlay_menu_), has_media);
        SetPopupVisible(context_menu_, overlay_menu_, Tr(L"AI"),
                        MenuCommandPosition(context_menu_, ID_SETTINGS), ai_available);
    }
    refresh_track_menus();
    CheckMenuItem(context_menu_, ID_YOLO_TOGGLE,
                  MF_BYCOMMAND | ((yolo_ && yolo_->enabled()) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(context_menu_, ID_LIGHTFC_TOGGLE,
                  MF_BYCOMMAND | ((lightfc_ && lightfc_->enabled()) ? MF_CHECKED : MF_UNCHECKED));
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(context_menu_, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                   screen_x, screen_y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
}

std::wstring App::track_label(const MediaTrack& track) const {
    if (!track.title.empty())
        return track.title;
    if (!track.lang.empty())
        return track.lang;
    const wchar_t* prefix = track.type == L"audio" ? L"Audio " : L"Subtitle ";
    return std::wstring(Tr(prefix)) + std::to_wstring(track.id);
}

void App::set_always_on_top(bool enabled, bool save) {
    always_on_top_ = enabled;
    const HWND z = enabled ? HWND_TOPMOST : HWND_NOTOPMOST;
    if (hwnd_) {
        SetWindowPos(hwnd_, z, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (pin_) {
        SetWindowPos(pin_, z, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(pin_, nullptr, FALSE);
    }
    if (save)
        SaveRegDword(L"AlwaysOnTop", enabled ? 1 : 0);
}

void App::update_pin_tooltip() {
    if (!pin_tip_ || !pin_)
        return;
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = pin_;
    info.uId = reinterpret_cast<UINT_PTR>(pin_);
    info.lpszText = const_cast<wchar_t*>(Tr(L"Always on Top"));
    SendMessageW(pin_tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
}

void App::place_pin_button() {
    if (!pin_ || !hwnd_)
        return;
    if (fullscreen_ || IsIconic(hwnd_)) {
        ShowWindow(pin_, SW_HIDE);
        return;
    }
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    RECT buttons{};
    const bool have_buttons =
        SUCCEEDED(DwmGetWindowAttribute(hwnd_, DWMWA_CAPTION_BUTTON_BOUNDS, &buttons, sizeof(buttons))) &&
        buttons.right > buttons.left && buttons.bottom > buttons.top;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (have_buttons) {
        h = buttons.bottom - buttons.top;
        w = h;
        const int slot = (buttons.right - buttons.left) / 3;
        if (slot > 8 && w > slot)
            w = slot;
        x = wr.left + buttons.left - w;
        y = wr.top + buttons.top;
    } else {
        const UINT dpi = GetDpiForWindow(hwnd_);
        w = GetSystemMetricsForDpi(SM_CXSIZE, dpi);
        h = GetSystemMetricsForDpi(SM_CYSIZE, dpi);
        const int frame_x = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        x = wr.right - frame_x - w * 4;
        y = wr.top + frame_y;
    }
    if (w < 8 || h < 8)
        return;
    SetWindowPos(pin_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(pin_, nullptr, FALSE);
}

void App::refresh_track_menus() {
    if (audio_menu_) {
        while (GetMenuItemCount(audio_menu_) > 0)
            DeleteMenu(audio_menu_, 0, MF_BYPOSITION);
    }
    audio_track_ids_.clear();
    sub_track_ids_.clear();
    if (subtitle_menu_) {
        for (UINT id = ID_SUB_TRACK_FIRST; id <= ID_SUB_TRACK_LAST; ++id)
            RemoveMenu(subtitle_menu_, id, MF_BYCOMMAND);
        RemoveMenu(subtitle_menu_, ID_SUB_TRACK_OFF, MF_BYCOMMAND);
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE;
        if (GetMenuItemCount(subtitle_menu_) > 0 &&
            GetMenuItemInfoW(subtitle_menu_, 0, TRUE, &info) && (info.fType & MFT_SEPARATOR))
            RemoveMenu(subtitle_menu_, 0, MF_BYPOSITION);
    }

    const bool has_media = player_.state().has_media;
    const std::vector<MediaTrack> tracks = has_media ? player_.media_tracks() : std::vector<MediaTrack>{};
    int audio_count = 0;
    bool any_sub_selected = false;
    std::vector<MediaTrack> embedded_subs;
    for (const MediaTrack& track : tracks) {
        if (track.type == L"audio")
            ++audio_count;
        else if (track.type == L"sub") {
            if (track.selected)
                any_sub_selected = true;
            if (!track.external)
                embedded_subs.push_back(track);
        }
    }

    if (audio_menu_ && audio_count >= 2) {
        UINT index = 0;
        for (const MediaTrack& track : tracks) {
            if (track.type != L"audio")
                continue;
            if (index > ID_AUDIO_TRACK_LAST - ID_AUDIO_TRACK_FIRST)
                break;
            const UINT id = ID_AUDIO_TRACK_FIRST + index;
            UINT flags = MF_STRING;
            if (track.selected)
                flags |= MF_CHECKED;
            AppendMenuW(audio_menu_, flags, id, track_label(track).c_str());
            audio_track_ids_.push_back(track.id);
            ++index;
        }
    }
    int stats_pos = -1;
    if (playback_menu_) {
        const int count = GetMenuItemCount(playback_menu_);
        for (int i = 0; i < count; ++i) {
            if (GetMenuItemID(playback_menu_, i) == ID_STATS_SHOW) {
                stats_pos = i;
                break;
            }
        }
    }
    SetPopupVisible(playback_menu_, audio_menu_, Tr(L"Audio"),
                    stats_pos >= 0 ? stats_pos : GetMenuItemCount(playback_menu_), audio_count >= 2);

    if (!embedded_subs.empty() && subtitle_menu_) {
        int at = -1;
        const int count = GetMenuItemCount(subtitle_menu_);
        for (int i = 0; i < count; ++i) {
            if (GetMenuItemID(subtitle_menu_, i) == ID_LOAD_SRT) {
                at = i;
                break;
            }
        }
        if (at < 0)
            at = GetMenuItemCount(subtitle_menu_);
        UINT index = 0;
        for (const MediaTrack& track : embedded_subs) {
            if (index > ID_SUB_TRACK_LAST - ID_SUB_TRACK_FIRST)
                break;
            const UINT id = ID_SUB_TRACK_FIRST + index;
            UINT flags = MF_BYPOSITION | MF_STRING;
            if (track.selected)
                flags |= MF_CHECKED;
            InsertMenuW(subtitle_menu_, static_cast<UINT>(at), flags, id, track_label(track).c_str());
            sub_track_ids_.push_back(track.id);
            ++at;
            ++index;
        }
        InsertMenuW(subtitle_menu_, static_cast<UINT>(at),
                    MF_BYPOSITION | MF_STRING | (any_sub_selected ? 0 : MF_CHECKED), ID_SUB_TRACK_OFF,
                    Tr(L"Off"));
        ++at;
        InsertMenuW(subtitle_menu_, static_cast<UINT>(at), MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }
}

void App::exit_fullscreen() {
    if (!fullscreen_ || !hwnd_)
        return;
    SetWindowLongW(hwnd_, GWL_STYLE, windowed_style_);
    SetWindowPos(hwnd_, nullptr, windowed_rect_.left, windowed_rect_.top,
                 windowed_rect_.right - windowed_rect_.left,
                 windowed_rect_.bottom - windowed_rect_.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED);
    fullscreen_ = false;
    layout();
    place_pin_button();
}

void App::toggle_fullscreen() {
    if (fullscreen_)
        exit_fullscreen();
    else
        enter_fullscreen(false);
}

void App::enter_fullscreen(bool stretch) {
    if (!hwnd_)
        return;
    player_.set_keepaspect(!stretch);
    if (!fullscreen_) {
        GetWindowRect(hwnd_, &windowed_rect_);
        windowed_style_ = GetWindowLongW(hwnd_, GWL_STYLE);
        SetWindowLongW(hwnd_, GWL_STYLE, (windowed_style_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        fullscreen_ = true;
    }
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        return;
    SetWindowPos(hwnd_, always_on_top_ ? HWND_TOPMOST : HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right - info.rcMonitor.left,
                 info.rcMonitor.bottom - info.rcMonitor.top,
                 SWP_FRAMECHANGED);
    layout();
    claim_keyboard_focus();
}

void App::resize_to_video_scale(double scale) {
    if (!hwnd_ || !(scale > 0.0))
        return;
    const PlayerState playback = player_.state();
    if (playback.width <= 0 || playback.height <= 0)
        return;

    player_.set_keepaspect(true);
    if (fullscreen_)
        exit_fullscreen();

    const int video_w = static_cast<int>(playback.width * scale + 0.5);
    const int video_h = static_cast<int>(playback.height * scale + 0.5);
    const int client_w = (std::max)(video_w, MulDiv(320, dpi(), 96));
    const int client_h = video_h + seek_bar_height();

    RECT frame{0, 0, client_w, client_h};
    const DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&frame, style, FALSE, ex_style, dpi());

    int width = frame.right - frame.left;
    int height = frame.bottom - frame.top;

    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info)) {
        const int work_w = info.rcWork.right - info.rcWork.left;
        const int work_h = info.rcWork.bottom - info.rcWork.top;
        if (width > work_w)
            width = work_w;
        if (height > work_h)
            height = work_h;
        RECT current{};
        GetWindowRect(hwnd_, &current);
        int x = current.left + (current.right - current.left - width) / 2;
        int y = current.top + (current.bottom - current.top - height) / 2;
        if (x < info.rcWork.left)
            x = info.rcWork.left;
        if (y < info.rcWork.top)
            y = info.rcWork.top;
        if (x + width > info.rcWork.right)
            x = info.rcWork.right - width;
        if (y + height > info.rcWork.bottom)
            y = info.rcWork.bottom - height;
        SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    layout();
}

void App::layout() {
    if (!hwnd_ || !video_ || !seek_bar_)
        return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int bar = seek_bar_height();
    int video_height = client.bottom - bar;
    if (video_height < 0)
        video_height = 0;
    MoveWindow(video_, 0, 0, client.right, video_height, TRUE);
    MoveWindow(seek_bar_, 0, video_height, client.right, bar, TRUE);
}

void App::update_title() {
    const PlayerState playback = player_.state();
    std::wstring caption = L"upPlayer";
    if (!playback.title.empty() && playback.has_media)
        caption += L" - " + playback.title;
    if (playback.has_media && playback.speed > 0.0 && std::fabs(playback.speed - 1.0) >= 0.05) {
        wchar_t speed[32];
        swprintf_s(speed, L" [%.1fx]", playback.speed);
        caption += speed;
    }
    if (lightfc_ && lightfc_->enabled())
        caption += L" [Track on]";
    if (captions_ && captions_->srt_busy()) {
        const std::wstring st = captions_->srt_status();
        const std::wstring gen_prefix = L"Generating SRT... ";
        if (st.rfind(gen_prefix, 0) == 0) {
            caption += L" [Generating subtitle ";
            caption += st.substr(gen_prefix.size());  // e.g. "42%"
            caption += L"]";
        } else if (!st.empty()) {
            caption += L" [";
            caption += st;
            caption += L"]";
        } else {
            caption += L" [Generating subtitle...]";
        }
    }
    if (caption != shown_title_) {
        shown_title_ = caption;
        SetWindowTextW(hwnd_, caption.c_str());
    }
}

void App::update_seek_bar() {
    if (seek_bar_)
        InvalidateRect(seek_bar_, nullptr, FALSE);
}

void App::bar_metrics(int width, int& pad, int& time_width, int& seek_left, int& seek_right,
                      int& vol_left, int& vol_right) const {
    pad = MulDiv(12, dpi(), 96);
    time_width = MulDiv(54, dpi(), 96);
    const int vol_width = MulDiv(84, dpi(), 96);
    const int vol_gap = MulDiv(10, dpi(), 96);
    vol_right = width - pad;
    vol_left = vol_right - vol_width;
    seek_left = pad + time_width;
    seek_right = vol_left - vol_gap - time_width;
    if (seek_right < seek_left)
        seek_right = seek_left;
}

bool App::hit_volume_slider(int x) const {
    if (!seek_bar_)
        return false;
    RECT rect{};
    GetClientRect(seek_bar_, &rect);
    int pad = 0, time_width = 0, seek_left = 0, seek_right = 0, vol_left = 0, vol_right = 0;
    bar_metrics(rect.right, pad, time_width, seek_left, seek_right, vol_left, vol_right);
    return x >= vol_left - MulDiv(4, dpi(), 96) && x <= vol_right + MulDiv(4, dpi(), 96);
}

double App::time_from_seek_x(int x) const {
    const PlayerState playback = player_.state();
    if (!(playback.duration > 0.0) || !seek_bar_)
        return -1.0;
    RECT rect{};
    GetClientRect(seek_bar_, &rect);
    int pad = 0, time_width = 0, seek_left = 0, seek_right = 0, vol_left = 0, vol_right = 0;
    bar_metrics(rect.right, pad, time_width, seek_left, seek_right, vol_left, vol_right);
    if (seek_right <= seek_left)
        return -1.0;
    double ratio = static_cast<double>(x - seek_left) / static_cast<double>(seek_right - seek_left);
    if (ratio < 0.0)
        ratio = 0.0;
    if (ratio > 1.0)
        ratio = 1.0;
    return ratio * playback.duration;
}

App::ClipMark App::hit_clip_marker(int x) const {
    const PlayerState playback = player_.state();
    if (!(playback.duration > 0.0) || !seek_bar_)
        return ClipMark::None;
    RECT rect{};
    GetClientRect(seek_bar_, &rect);
    int pad = 0, time_width = 0, seek_left = 0, seek_right = 0, vol_left = 0, vol_right = 0;
    bar_metrics(rect.right, pad, time_width, seek_left, seek_right, vol_left, vol_right);
    if (seek_right <= seek_left)
        return ClipMark::None;
    const int slop = MulDiv(10, dpi(), 96);
    auto marker_x = [&](double t) {
        return seek_left +
               static_cast<int>((seek_right - seek_left) * (t / playback.duration) + 0.5);
    };
    int best_dist = slop + 1;
    ClipMark hit = ClipMark::None;
    auto consider = [&](ClipMark mark, double t) {
        if (!(t >= 0.0))
            return;
        const int dist = std::abs(x - marker_x(t));
        if (dist <= slop && dist < best_dist) {
            best_dist = dist;
            hit = mark;
        }
    };
    consider(ClipMark::In, clip_in_);
    consider(ClipMark::Out, clip_out_);
    return hit;
}

void App::normalize_clip_marks() {
    const double frame = player_.frame_duration();
    if (!(clip_in_ >= 0.0 && clip_out_ >= 0.0) || clip_out_ > clip_in_)
        return;
    const ClipMark which =
        clip_dragging_ != ClipMark::None ? clip_dragging_ : clip_active_;
    if (which == ClipMark::In)
        clip_in_ = (std::max)(0.0, clip_out_ - frame);
    else
        clip_out_ = clip_in_ + frame;
}

void App::set_clip_mark_time(ClipMark mark, double time, bool announce) {
    const PlayerState playback = player_.state();
    if (!(time >= 0.0))
        return;
    if (playback.duration > 0.0 && time > playback.duration)
        time = playback.duration;
    if (mark == ClipMark::In) {
        clip_in_ = time;
        if (clip_dragging_ == ClipMark::None && clip_out_ >= 0.0 && clip_out_ <= clip_in_)
            clip_out_ = -1.0;
    } else if (mark == ClipMark::Out) {
        clip_out_ = time;
        if (clip_dragging_ == ClipMark::None && clip_in_ >= 0.0 && clip_out_ <= clip_in_) {
            const double tmp = clip_in_;
            clip_in_ = clip_out_;
            clip_out_ = tmp;
        }
    } else {
        return;
    }
    clip_active_ = mark;
    normalize_clip_marks();
    const double shown = (clip_active_ == ClipMark::In) ? clip_in_ : clip_out_;
    player_.seek_absolute(shown);
    if (announce)
        Osd(player_,
            std::wstring(Tr(clip_active_ == ClipMark::In ? L"Clip In: " : L"Clip Out: ")) + FormatTime(shown),
            900);
    update_seek_bar();
}

void App::clip_mark_from_point(int x) {
    if (clip_dragging_ == ClipMark::None)
        return;
    const double time = time_from_seek_x(x);
    if (!(time >= 0.0))
        return;
    set_clip_mark_time(clip_dragging_, time, false);
}

void App::seek_from_point(int x) {
    const double time = time_from_seek_x(x);
    if (!(time >= 0.0))
        return;
    clip_active_ = ClipMark::None;
    seek_preview_ = time;
    player_.seek_absolute(seek_preview_);
    last_caption_time_ = time;
    if (captions_ && captions_->enabled())
        captions_->on_seek(time);
    update_seek_bar();
}

void App::volume_from_point(int x, bool show_osd) {
    RECT rect{};
    GetClientRect(seek_bar_, &rect);
    int pad = 0, time_width = 0, seek_left = 0, seek_right = 0, vol_left = 0, vol_right = 0;
    bar_metrics(rect.right, pad, time_width, seek_left, seek_right, vol_left, vol_right);
    if (vol_right <= vol_left)
        return;
    double ratio = static_cast<double>(x - vol_left) / static_cast<double>(vol_right - vol_left);
    if (ratio < 0.0)
        ratio = 0.0;
    if (ratio > 1.0)
        ratio = 1.0;
    volume_preview_ = std::round(ratio * 100.0);
    player_.set_volume(volume_preview_, show_osd);
    update_seek_bar();
}

LRESULT App::handle_seek_bar(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        const HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
        FillRect(dc, &rect, background);
        DeleteObject(background);

        const PlayerState playback = player_.state();
        const double duration = playback.duration;
        const double position = seeking_ && seek_preview_ >= 0.0
                                    ? seek_preview_
                                    : (playback.time_pos >= 0.0 ? playback.time_pos : 0.0);
        const double volume = volume_dragging_ && volume_preview_ >= 0.0
                                  ? volume_preview_
                                  : (playback.volume >= 0.0 ? playback.volume : 100.0);
        int pad = 0, time_width = 0, seek_left = 0, seek_right = 0, vol_left = 0, vol_right = 0;
        bar_metrics(rect.right, pad, time_width, seek_left, seek_right, vol_left, vol_right);
        const int track_height = MulDiv(4, dpi(), 96);
        const int track_y = (rect.bottom - track_height) / 2;
        const int knob = MulDiv(10, dpi(), 96);

        auto draw_track = [&](int left, int right, double ratio, COLORREF fill) {
            if (right <= left)
                return;
            RECT track{left, track_y, right, track_y + track_height};
            const HBRUSH track_brush = CreateSolidBrush(RGB(48, 52, 60));
            FillRect(dc, &track, track_brush);
            DeleteObject(track_brush);
            if (ratio < 0.0)
                ratio = 0.0;
            if (ratio > 1.0)
                ratio = 1.0;
            const int filled = left + static_cast<int>((right - left) * ratio + 0.5);
            RECT progress{left, track_y, filled, track_y + track_height};
            const HBRUSH progress_brush = CreateSolidBrush(fill);
            FillRect(dc, &progress, progress_brush);
            DeleteObject(progress_brush);
            const RECT knob_rect{filled - knob / 2, track_y + track_height / 2 - knob / 2,
                                 filled + (knob + 1) / 2, track_y + track_height / 2 + (knob + 1) / 2};
            const HBRUSH knob_brush = CreateSolidBrush(RGB(230, 240, 250));
            FillRect(dc, &knob_rect, knob_brush);
            DeleteObject(knob_brush);
        };

        const bool live = live_playback();
        if (!live) {
            if (duration > 0.0)
                draw_track(seek_left, seek_right, position / duration, RGB(90, 200, 250));
            else
                draw_track(seek_left, seek_right, 0.0, RGB(90, 200, 250));
        }

        if (!live && duration > 0.0 && seek_right > seek_left) {
            auto marker_x = [&](double t) {
                double ratio = t / duration;
                if (ratio < 0.0)
                    ratio = 0.0;
                if (ratio > 1.0)
                    ratio = 1.0;
                return seek_left + static_cast<int>((seek_right - seek_left) * ratio + 0.5);
            };
            if (clip_in_ >= 0.0 && clip_out_ > clip_in_) {
                const int left = marker_x(clip_in_);
                const int right = marker_x(clip_out_);
                RECT range{left, track_y - MulDiv(2, dpi(), 96), right,
                           track_y + track_height + MulDiv(2, dpi(), 96)};
                const HBRUSH range_brush = CreateSolidBrush(RGB(250, 180, 60));
                FillRect(dc, &range, range_brush);
                DeleteObject(range_brush);
            }
            auto draw_marker = [&](double t, ClipMark mark, COLORREF color) {
                if (!(t >= 0.0))
                    return;
                const int x = marker_x(t);
                const bool active = clip_active_ == mark || clip_dragging_ == mark;
                const int half = MulDiv(active ? 3 : 2, dpi(), 96);
                const int top = track_y - MulDiv(active ? 8 : 6, dpi(), 96);
                const int bottom = track_y + track_height + MulDiv(active ? 8 : 6, dpi(), 96);
                RECT mark_rect{x - half, top, x + half + 1, bottom};
                const HBRUSH brush = CreateSolidBrush(color);
                FillRect(dc, &mark_rect, brush);
                DeleteObject(brush);
            };
            draw_marker(clip_in_, ClipMark::In, RGB(80, 210, 120));
            draw_marker(clip_out_, ClipMark::Out, RGB(255, 140, 40));
        }

        const COLORREF vol_color = playback.mute ? RGB(120, 120, 128) : RGB(120, 210, 150);
        draw_track(vol_left, vol_right, playback.mute ? 0.0 : (volume / 100.0), vol_color);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(180, 188, 196));
        HFONT font = CreateFontW(-MulDiv(9, dpi(), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        if (!live) {
            const std::wstring left = FormatTime(position);
            const std::wstring right = FormatTime(duration > 0.0 ? duration : -1.0);
            RECT left_rect{pad, 0, pad + time_width - MulDiv(6, dpi(), 96), rect.bottom};
            RECT right_rect{seek_right + MulDiv(6, dpi(), 96), 0, vol_left - MulDiv(6, dpi(), 96),
                            rect.bottom};
            DrawTextW(dc, left.c_str(), -1, &left_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(dc, right.c_str(), -1, &right_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            const wchar_t* live_text = Tr(L"Live");
            RECT live_rect{pad, 0, vol_left - MulDiv(6, dpi(), 96), rect.bottom};
            DrawTextW(dc, live_text, -1, &live_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lparam);
        SetCapture(hwnd);
        if (hit_volume_slider(x)) {
            volume_dragging_ = true;
            volume_from_point(x, false);
        } else if (live_playback()) {
            ReleaseCapture();
        } else if (const ClipMark mark = hit_clip_marker(x); mark != ClipMark::None) {
            clip_dragging_ = mark;
            clip_active_ = mark;
            clip_mark_from_point(x);
        } else {
            seeking_ = true;
            seek_from_point(x);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (volume_dragging_ && (wparam & MK_LBUTTON))
            volume_from_point(GET_X_LPARAM(lparam), false);
        else if (clip_dragging_ != ClipMark::None && (wparam & MK_LBUTTON))
            clip_mark_from_point(GET_X_LPARAM(lparam));
        else if (seeking_ && (wparam & MK_LBUTTON))
            seek_from_point(GET_X_LPARAM(lparam));
        return 0;
    case WM_LBUTTONUP:
        if (volume_dragging_) {
            volume_dragging_ = false;
            volume_from_point(GET_X_LPARAM(lparam), true);
            volume_preview_ = -1.0;
            ReleaseCapture();
            update_seek_bar();
        } else if (clip_dragging_ != ClipMark::None) {
            clip_mark_from_point(GET_X_LPARAM(lparam));
            clip_dragging_ = ClipMark::None;
            if (clip_active_ == ClipMark::In)
                Osd(player_, std::wstring(Tr(L"Clip In: ")) + FormatTime(clip_in_), 900);
            else if (clip_active_ == ClipMark::Out)
                Osd(player_, std::wstring(Tr(L"Clip Out: ")) + FormatTime(clip_out_), 900);
            ReleaseCapture();
            update_seek_bar();
        } else if (seeking_) {
            seeking_ = false;
            seek_from_point(GET_X_LPARAM(lparam));
            seek_preview_ = -1.0;
            ReleaseCapture();
            update_seek_bar();
        }
        return 0;
    case WM_CAPTURECHANGED:
        seeking_ = false;
        volume_dragging_ = false;
        clip_dragging_ = ClipMark::None;
        seek_preview_ = -1.0;
        volume_preview_ = -1.0;
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        if (delta > 0)
            player_.adjust_volume(2.0);
        else if (delta < 0)
            player_.adjust_volume(-2.0);
        update_seek_bar();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void App::open_file() {
    std::vector<wchar_t> buffer(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    const std::wstring filter = std::wstring(Tr(L"Media files")) + L'\0' +
        L"*.mp4;*.mkv;*.avi;*.mov;*.webm;*.flv;*.ts;*.m2ts;*.m4v;*.wmv;*.mpg;*.mpeg;*.mp3;*.flac;*.wav;*.aac;*.ogg;*.opus;*.m4a;*.m3u;*.m3u8" +
        L'\0' + std::wstring(Tr(L"All files")) + L'\0' + L"*.*" + L'\0';
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = Tr(L"Open");
    if (!GetOpenFileNameW(&dialog))
        return;
    open_target(buffer.data());
    SetFocus(video_);
}

void App::open_url() {
    UrlDialogResult result;
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_URL), hwnd_, UrlDlgProc,
                        reinterpret_cast<LPARAM>(&result)) != IDOK)
        return;
    RememberRecentUrl(result.url);
    player_.stop_playback();
    if (!player_.prepare_network_latency(result.low_latency))
        update_title();
    open_target(result.url, false);
    update_title();
    SetFocus(video_);
}

void App::open_target(const std::wstring& target, bool auto_live_for_network) {
    remember_resume();
    clip_in_ = -1.0;
    clip_out_ = -1.0;
    clip_active_ = ClipMark::None;
    clip_dragging_ = ClipMark::None;
    last_caption_time_ = -1.0;
    last_caption_osd_.clear();
    external_subtitle_loaded_ = false;
    player_.clear_caption();
    if (auto_live_for_network) {
        std::wstring lower = target;
        for (auto& ch : lower) {
            if (ch >= L'A' && ch <= L'Z')
                ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
        const bool network = lower.rfind(L"rtsp://", 0) == 0 || lower.rfind(L"rtsps://", 0) == 0 ||
                             lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0 ||
                             lower.rfind(L"rtmp://", 0) == 0 || lower.rfind(L"rtp://", 0) == 0 ||
                             lower.rfind(L"udp://", 0) == 0 || lower.rfind(L"tcp://", 0) == 0;
        player_.prepare_network_latency(network);
    }
    std::wstring lower = target;
    for (auto& ch : lower) {
        if (ch >= L'A' && ch <= L'Z')
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    const bool network = IsNetworkTarget(lower);
    const double resume = network ? -1.0 : LookupResume(Trim(target));
    if (!player_.load(target, network ? -1.0 : resume))
        update_title();
    else if (!network && resume >= 3.0)
        Osd(player_, std::wstring(Tr(L"Resumed ")) + FormatTime(resume), 1600);
    last_resume_path_.clear();
    last_resume_pos_ = -1.0;
    sync_captions_media();
    update_seek_bar();
}

void App::show_about() {
    std::wstring text = L"upPlayer ";
    text += UPPLAYER_VERSION_STRINGW;
    text += L"\n\n";
    text += Tr(L"Lightweight Win32 front-end for mpv.");
    text += L"\n";
    text += Tr(L"Playback: ");
    text += player_.backend_label().empty() ? L"n/a" : player_.backend_label();
    text += L"\n\n";
    text += Tr(L"License: MIT (see LICENSE / NOTICE)");
    MessageBoxW(hwnd_, text.c_str(), Tr(L"About upPlayer"), MB_OK | MB_ICONINFORMATION);
}
void App::show_settings() {
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd_, SettingsDlgProc, 0) != IDOK)
        return;
    LoadMenuLanguage();
    apply_menu_language();
    player_.set_screenshot_directory(SnapshotDirectory());
    player_.set_screenshot_format(SnapshotFormat());
    Osd(player_, L"Settings saved", 1400);
}

void App::restore_saved_volume() {
    DWORD volume = LoadRegDword(L"Volume", 100);
    if (volume > 100)
        volume = 100;
    const bool mute = LoadRegDword(L"Muted", 0) != 0;
    player_.set_volume(static_cast<double>(volume), false);
    if (mute)
        player_.set_mute(true);
    saved_volume_ = static_cast<int>(volume);
    saved_mute_ = mute;
    volume_restored_ = true;
}

void App::remember_volume() {
    if (!volume_restored_)
        return;
    const PlayerState playback = player_.state();
    int volume = static_cast<int>(std::lround(playback.volume));
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    if (volume == saved_volume_ && playback.mute == saved_mute_)
        return;
    saved_volume_ = volume;
    saved_mute_ = playback.mute;
    SaveRegDword(L"Volume", static_cast<DWORD>(volume));
    SaveRegDword(L"Muted", playback.mute ? 1 : 0);
}

bool App::live_playback() const {
    return player_.low_latency() && player_.state().has_media;
}

void App::remember_resume() {
    if (live_playback())
        return;
    const PlayerState playback = player_.state();
    const std::wstring path = player_.current_path();
    if (!playback.has_media || path.empty() || IsNetworkTarget(path) || !(playback.time_pos >= 0.0))
        return;
    const bool near_end = playback.duration > 1.0 && playback.time_pos + 5.0 >= playback.duration;
    if (!near_end && last_resume_path_ == path && std::fabs(playback.time_pos - last_resume_pos_) < 2.0)
        return;
    StoreResume(path, playback.time_pos, playback.duration);
    last_resume_path_ = path;
    last_resume_pos_ = playback.time_pos;
}


void App::toggle_captions() {
    if (!captions_)
        return;
    const bool on = !captions_->enabled();
    captions_->set_enabled(on);
    if (on) {
        sync_captions_media();
        Osd(player_, L"Captions on", 1000);
    } else {
        last_caption_osd_.clear();
        last_caption_osd_tick_ = 0;
        player_.clear_caption();
        Osd(player_, L"Captions off", 1000);
    }
}

void App::generate_srt() {
    if (!captions_)
        return;
    if (captions_->srt_busy()) {
        captions_->cancel_generate_srt();
        Osd(player_, L"Cancelling subtitle...", 1600);
        update_title();
        return;
    }
    if (!captions_->has_whisper_model()) {
        std::wstring expected = captions_->model_id();
        if (expected.empty())
            expected = L"ggml-tiny.bin";
        ShowWhisperModelLoadHelp(hwnd_, L"No usable ggml model was found in the model directory.", expected);
        return;
    }
    sync_captions_media();
    std::wstring error;
    if (!captions_->request_generate_srt(&error)) {
        MessageBoxW(hwnd_, error.empty() ? Tr(L"Cannot start Generate Subtitle.") : error.c_str(),
                    Tr(L"upPlayer - Generate Subtitle"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    Osd(player_, L"Generating subtitle...", 2000);
    update_title();
}


void App::load_srt() {
    if (!player_.state().has_media) {
        MessageBoxW(hwnd_, Tr(L"Open a video first."), Tr(L"upPlayer - Load Subtitle"),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::vector<wchar_t> buffer(32768, L'\0');
    std::wstring initial_dir;
    const std::wstring media = player_.current_path();
    // Local/UNC paths (not http/rtsp): seed dialog dir + sidecar <media>.srt.
    const bool file_like = !media.empty() && media.find(L"://") == std::wstring::npos;
    if (file_like) {
        const auto slash = media.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            initial_dir = media.substr(0, slash);

        std::wstring sidecar = media;
        const auto dot = sidecar.find_last_of(L'.');
        const auto slash2 = sidecar.find_last_of(L"\\/");
        if (dot != std::wstring::npos && (slash2 == std::wstring::npos || dot > slash2))
            sidecar = sidecar.substr(0, dot) + L".srt";
        else
            sidecar += L".srt";
        if (GetFileAttributesW(sidecar.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const auto name_slash = sidecar.find_last_of(L"\\/");
            const std::wstring name =
                name_slash == std::wstring::npos ? sidecar : sidecar.substr(name_slash + 1);
            wcsncpy_s(buffer.data(), buffer.size(), name.c_str(), _TRUNCATE);
        }
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    const std::wstring filter = std::wstring(Tr(L"Subtitle files")) + L'\0' +
                                L"*.srt;*.ass;*.ssa;*.vtt;*.sub" + L'\0' + L"SRT" + L'\0' + L"*.srt" +
                                L'\0' + std::wstring(Tr(L"All files")) + L'\0' + L"*.*" + L'\0';
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = Tr(L"Load Subtitle");
    dialog.lpstrDefExt = L"srt";
    if (!initial_dir.empty())
        dialog.lpstrInitialDir = initial_dir.c_str();
    if (!GetOpenFileNameW(&dialog))
        return;

    load_subtitle_file(buffer.data());
}

void App::load_subtitle_file(const std::wstring& path) {
    if (!player_.state().has_media) {
        MessageBoxW(hwnd_, Tr(L"Open a video first."), Tr(L"upPlayer - Load Subtitle"),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring error;
    if (!player_.add_external_subtitle(path, &error)) {
        MessageBoxW(hwnd_, error.empty() ? Tr(L"Failed to load subtitle.") : error.c_str(),
                    Tr(L"upPlayer - Load Subtitle"), MB_OK | MB_ICONWARNING);
        return;
    }

    const auto slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    external_subtitle_loaded_ = true;
    Osd(player_, std::wstring(Tr(L"Loaded: ")) + name, 1600);
}



void App::clear_srt() {
    if (!player_.state().has_media) {
        MessageBoxW(hwnd_, Tr(L"Open a video first."), Tr(L"upPlayer - Unload Subtitle"),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring error;
    if (!player_.clear_external_subtitle(&error)) {
        MessageBoxW(hwnd_, error.empty() ? Tr(L"Failed to unload subtitle.") : error.c_str(),
                    Tr(L"upPlayer - Unload Subtitle"), MB_OK | MB_ICONWARNING);
        return;
    }
    external_subtitle_loaded_ = false;
    Osd(player_, L"Unloaded subtitle", 1200);
}

void App::set_yolo_mode(YoloMode mode) {
    if (!yolo_)
        return;
    if (mode != YoloMode::Off && lightfc_ && lightfc_->enabled()) {
        lightfc_->set_mode(LightFcMode::Off);
        player_.clear_track_overlay();
        update_title();
    }
    yolo_->set_mode(mode);
    if (mode == YoloMode::Off) {
        player_.clear_yolo_overlay();
        Osd(player_, L"YOLO26 off", 1000);
    } else {
        Osd(player_, L"YOLO26 on (skip busy frames)", 1400);
        tick_yolo();
    }
}

void App::cycle_yolo_mode() {
    if (!yolo_)
        return;
    set_yolo_mode(yolo_->enabled() ? YoloMode::Off : YoloMode::On);
}

void App::set_lightfc_mode(LightFcMode mode) {
    if (!lightfc_)
        return;
    if (mode != LightFcMode::Off && yolo_ && yolo_->enabled()) {
        yolo_->set_mode(YoloMode::Off);
        player_.clear_yolo_overlay();
    }
    lightfc_->set_mode(mode);
    if (mode == LightFcMode::Off) {
        player_.clear_track_overlay();
        Osd(player_, L"LightFC off", 1000);
    } else if (mode == LightFcMode::Arm) {
        Osd(player_, L"LightFC: click a target", 1600);
    } else {
        Osd(player_, L"LightFC tracking", 1200);
        tick_lightfc();
    }
    update_title();
}

void App::cycle_lightfc_mode() {
    if (!lightfc_)
        return;
    if (lightfc_->enabled())
        set_lightfc_mode(LightFcMode::Off);
    else
        set_lightfc_mode(LightFcMode::Arm);
}

bool App::lightfc_click_select(int client_x, int client_y) {
    // Arm = first select; Tracking = click again to reselect.
    if (!lightfc_ || !lightfc_->enabled() || !video_)
        return false;
    const PlayerState st = player_.state();
    if (!st.has_media || st.width <= 0 || st.height <= 0)
        return false;

    RECT rc{};
    GetClientRect(video_, &rc);
    const int vo_w = (std::max)(1, static_cast<int>(rc.right - rc.left));
    const int vo_h = (std::max)(1, static_cast<int>(rc.bottom - rc.top));
    const double scale =
        (std::min)(static_cast<double>(vo_w) / st.width, static_cast<double>(vo_h) / st.height);
    const int dw = (std::max)(1, static_cast<int>(std::lround(st.width * scale)));
    const int dh = (std::max)(1, static_cast<int>(std::lround(st.height * scale)));
    const int ml = (vo_w - dw) / 2;
    const int mt = (vo_h - dh) / 2;
    if (client_x < ml || client_y < mt || client_x >= ml + dw || client_y >= mt + dh)
        return false;

    const float fx = static_cast<float>((client_x - ml) / scale);
    const float fy = static_cast<float>((client_y - mt) / scale);
    const float side = static_cast<float>((std::min)(st.width, st.height)) * 0.14f;
    LightFcBox box;
    box.w = side;
    box.h = side;
    box.x = fx - 0.5f * side;
    box.y = fy - 0.5f * side;
    box.score = 1.f;
    if (!lightfc_->init_at(box))
        return false;
    Osd(player_, L"LightFC: locking...", 1000);
    tick_lightfc();
    return true;
}

void App::tick_lightfc() {
    if (!lightfc_ || !lightfc_->enabled())
        return;
    const PlayerState playback = player_.state();
    if (!playback.has_media || playback.paused)
        return;
    lightfc_->pump();
    const std::wstring path = lightfc_->try_begin_capture();
    if (path.empty())
        return;
    DeleteFileW(path.c_str());
    player_.screenshot_to_file(path);
    lightfc_->notify_capture_requested(path);
}

void App::refresh_lightfc_osd() {
    if (!lightfc_)
        return;
    if (!lightfc_->enabled()) {
        player_.clear_track_overlay();
        return;
    }
    if (lightfc_->arming()) {
        player_.clear_track_overlay();
        const std::wstring status = lightfc_->status_text();
        if (!status.empty())
            player_.show_osd(WideToUtf8(status), 1800);
        return;
    }
    const int ow = lightfc_->overlay_width();
    const int oh = lightfc_->overlay_height();
    const std::wstring bgra = lightfc_->overlay_bgra_path();
    if (!bgra.empty() && ow > 0 && oh > 0 && video_) {
        RECT rc{};
        GetClientRect(video_, &rc);
        const int vo_w = (std::max)(1, static_cast<int>(rc.right - rc.left));
        const int vo_h = (std::max)(1, static_cast<int>(rc.bottom - rc.top));
        const PlayerState st = player_.state();
        const int iw = st.width > 0 ? st.width : ow;
        const int ih = st.height > 0 ? st.height : oh;
        const double scale =
            (std::min)(static_cast<double>(vo_w) / iw, static_cast<double>(vo_h) / ih);
        const int dw = (std::max)(1, static_cast<int>(std::lround(iw * scale)));
        const int dh = (std::max)(1, static_cast<int>(std::lround(ih * scale)));
        const int ml = (vo_w - dw) / 2;
        const int mt = (vo_h - dh) / 2;
        player_.show_track_boxes_bgra(bgra, ow, oh, lightfc_->overlay_stride(), ml, mt, dw, dh);
    } else {
        player_.show_track_boxes_bgra({}, 0, 0, 0, 0, 0, 0, 0);
    }
    const std::wstring status = lightfc_->status_text();
    if (!status.empty())
        player_.show_osd(WideToUtf8(status), 1400);
}

void App::tick_yolo() {
    if (!yolo_ || !yolo_->enabled())
        return;
    const PlayerState playback = player_.state();
    if (!playback.has_media || playback.paused)
        return;
    // Promote finished JPEGs into the infer queue; never block playback.
    yolo_->pump();
    const std::wstring path = yolo_->try_begin_capture();
    if (path.empty())
        return;
    DeleteFileW(path.c_str());
    player_.screenshot_to_file(path);
    yolo_->notify_capture_requested(path);
}

void App::refresh_yolo_osd() {
    if (!yolo_)
        return;
    if (!yolo_->enabled()) {
        player_.clear_yolo_overlay();
        return;
    }
    // Clear old ASS YOLO text layer; labels/HUD are drawn into the BGRA bitmap.
    player_.show_yolo_overlay({}, 0, 0);

    const int ow = yolo_->overlay_width();
    const int oh = yolo_->overlay_height();
    const std::wstring bgra = yolo_->overlay_bgra_path();
    if (!bgra.empty() && ow > 0 && oh > 0 && video_) {
        RECT rc{};
        GetClientRect(video_, &rc);
        const int vo_w = (std::max)(1, static_cast<int>(rc.right - rc.left));
        const int vo_h = (std::max)(1, static_cast<int>(rc.bottom - rc.top));
        // keepaspect letterbox inside the VO window (matches mpv default).
        const double scale = (std::min)(static_cast<double>(vo_w) / ow, static_cast<double>(vo_h) / oh);
        const int dw = (std::max)(1, static_cast<int>(std::lround(ow * scale)));
        const int dh = (std::max)(1, static_cast<int>(std::lround(oh * scale)));
        const int ml = (vo_w - dw) / 2;
        const int mt = (vo_h - dh) / 2;
        player_.show_yolo_boxes_bgra(bgra, ow, oh, yolo_->overlay_stride(), ml, mt, dw, dh);
    } else {
        player_.show_yolo_boxes_bgra({}, 0, 0, 0, 0, 0, 0, 0);
    }

    const std::wstring status = yolo_->status_text();
    if (!status.empty())
        player_.show_osd(WideToUtf8(status), 1600);
}

void App::sync_captions_media() {
    if (!captions_)
        return;
    const PlayerState playback = player_.state();
    const std::wstring path = player_.current_path();
    captions_->set_media(path, playback.duration);
    if (playback.time_pos >= 0.0)
        captions_->on_seek(playback.time_pos);
}

void App::note_playback_time(double time_pos) {
    if (!captions_ || !captions_->enabled() || !(time_pos >= 0.0))
        return;
    if (last_caption_time_ >= 0.0 && std::fabs(time_pos - last_caption_time_) > 1.5)
        captions_->on_seek(time_pos);
    last_caption_time_ = time_pos;
    captions_->tick(time_pos);
}

void App::refresh_caption_osd(bool force_status) {
    if (!captions_ || !captions_->enabled())
        return;
    const PlayerState playback = player_.state();
    if (!(playback.time_pos >= 0.0))
        return;
    const ULONGLONG now = GetTickCount64();
    const std::wstring cue = captions_->active_text(playback.time_pos);
    if (!cue.empty()) {
        if (cue != last_caption_osd_) {
            last_caption_osd_ = cue;
            last_caption_osd_tick_ = now;
            player_.show_caption(WideToUtf8(cue));
        }
        return;
    }
    // No active cue: clear bottom caption so stale lines don't linger.
    if (!last_caption_osd_.empty()) {
        last_caption_osd_.clear();
        player_.clear_caption();
    }
    if (!force_status)
        return;
    const std::wstring status = captions_->status_text();
    if (!status.empty() && now - last_caption_osd_tick_ > 1500) {
        last_caption_osd_tick_ = now;
        std::wstring osd = status;
        if (osd.rfind(L"missing:", 0) == 0)
            osd = L"Whisper model missing - see dialog for download URL";
        else if (osd.rfind(L"server:", 0) == 0)
            osd = L"Whisper server failed 闂?see dialog";
        player_.show_osd(WideToUtf8(osd), 1500);
    }
}

bool App::clip_session_active() const {
    return clip_in_ >= 0.0 || clip_out_ >= 0.0 || clip_active_ != ClipMark::None ||
           clip_dragging_ != ClipMark::None;
}

void App::mark_clip_in() {
    const PlayerState playback = player_.state();
    if (!playback.has_media || !(playback.time_pos >= 0.0)) {
        Osd(player_, L"No media", 1000);
        return;
    }
    player_.set_pause(true);
    set_clip_mark_time(ClipMark::In, playback.time_pos, true);
}

void App::mark_clip_out() {
    const PlayerState playback = player_.state();
    if (!playback.has_media || !(playback.time_pos >= 0.0)) {
        Osd(player_, L"No media", 1000);
        return;
    }
    player_.set_pause(true);
    set_clip_mark_time(ClipMark::Out, playback.time_pos, true);
}

void App::clear_clip_marks() {
    if (!clip_session_active())
        return;
    clip_in_ = -1.0;
    clip_out_ = -1.0;
    clip_active_ = ClipMark::None;
    clip_dragging_ = ClipMark::None;
    Osd(player_, L"Clip cancelled", 1000);
    update_seek_bar();
}

void App::handle_escape() {
    if (captions_ && captions_->srt_busy()) {
        captions_->cancel_generate_srt();
        Osd(player_, L"Cancelling subtitle...", 1600);
        update_title();
        return;
    }
    if (clip_session_active()) {
        clear_clip_marks();
        return;
    }
    if (fullscreen_)
        exit_fullscreen();
}

void App::nudge_clip_or_frame(int direction) {
    if (direction == 0)
        return;
    if (clip_active_ == ClipMark::In || clip_active_ == ClipMark::Out) {
        const double current = clip_active_ == ClipMark::In ? clip_in_ : clip_out_;
        if (!(current >= 0.0)) {
            player_.frame_step(direction > 0);
            return;
        }
        set_clip_mark_time(clip_active_, current + direction * player_.frame_duration(), true);
        return;
    }
    player_.frame_step(direction > 0);
}

void App::export_clip() {
    if (!(clip_in_ >= 0.0) || !(clip_out_ > clip_in_)) {
        Osd(player_, L"Mark In (z) and Out (x) first", 1600);
        return;
    }
    const std::wstring source = player_.current_path();
    if (source.empty() || IsNetworkTarget(source)) {
        Osd(player_, L"Export needs a local file", 1400);
        return;
    }
    const std::wstring mpv = FindExecutable(L"mpv.exe");
    if (mpv.empty()) {
        MessageBoxW(hwnd_,
                    L"mpv.exe was not found.\n\n"
                    L"Place mpv.exe next to upPlayer.exe (release package includes it), or add it to PATH.",
                    Tr(L"Export Clip"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    const double start = clip_in_;
    const double end = clip_out_;
    const std::wstring clip_dir = ClipOutputDirectory();
    std::wstring suggested = SuggestClipName(source, start, end);
    if (!clip_dir.empty())
        suggested = clip_dir + L"\\" + suggested;
    std::vector<wchar_t> buffer(32768, L'\0');
    wcsncpy_s(buffer.data(), buffer.size(), suggested.c_str(), _TRUNCATE);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    const std::wstring export_filter = std::wstring(Tr(L"Media files")) + L'\0' + L"*.*" + L'\0';
    dialog.lpstrFilter = export_filter.c_str();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = Tr(L"Export Clip");
    dialog.lpstrDefExt = L"mp4";
    if (!clip_dir.empty())
        dialog.lpstrInitialDir = clip_dir.c_str();
    if (!GetSaveFileNameW(&dialog))
        return;

    wchar_t start_arg[64];
    wchar_t end_arg[64];
    swprintf_s(start_arg, L"%.3f", start);
    swprintf_s(end_arg, L"%.3f", end);

    // Official Windows mpv builds do not expose --ovc=copy. Use stream-record to dump A闂傚倷鑳堕崑銊╁磿閹绘帩鍤?
    const std::wstring output = buffer.data();
    const std::wstring command =
        QuoteCmd(mpv) + L" --no-config --vo=null --ao=null --sid=no --start=" + start_arg +
        L" --end=" + end_arg + L" --stream-record=" + QuoteCmd(output) + L" -- " + QuoteCmd(source);

    Osd(player_, L"Exporting clip...", 2000);
    DWORD exit_code = 1;
    std::wstring log_path;
    const bool started = RunProcessWait(command, exit_code, &log_path);
    WIN32_FILE_ATTRIBUTE_DATA out_info{};
    const bool wrote = GetFileAttributesExW(output.c_str(), GetFileExInfoStandard, &out_info) &&
                       (out_info.nFileSizeLow > 0 || out_info.nFileSizeHigh > 0);

    if (!started || exit_code != 0 || !wrote) {
        std::wstring message =
            L"mpv failed to export the clip.\n\n"
            L"Try a slightly earlier In mark (keyframe), or keep the same extension as the source.";
        if (!log_path.empty()) {
            const std::wstring log = ReadTextFileHead(log_path);
            if (!log.empty()) {
                message += L"\n\n";
                message += log;
            }
            DeleteFileW(log_path.c_str());
        }
        MessageBoxW(hwnd_, message.c_str(), Tr(L"Export Clip"), MB_OK | MB_ICONWARNING);
        return;
    }
    if (!log_path.empty())
        DeleteFileW(log_path.c_str());
    Osd(player_, L"Clip exported. Start may move to the nearest keyframe", 2200);
}

LRESULT App::handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        create_children();
        return 0;
    case WM_CONTEXTMENU: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pt.x == -1 && pt.y == -1) {
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            pt.x = rect.left + (rect.right - rect.left) / 2;
            pt.y = rect.top + (rect.bottom - rect.top) / 2;
        }
        show_context_menu(pt.x, pt.y);
        return 0;
    }
    case WM_COMMAND: {
        const UINT cmd = LOWORD(wparam);
        if (cmd >= ID_CAPTION_MODEL_FIRST && cmd <= ID_CAPTION_MODEL_LAST) {
            select_caption_model(cmd - ID_CAPTION_MODEL_FIRST);
            return 0;
        }
        if (cmd >= ID_CAPTION_LANG_FIRST && cmd <= ID_CAPTION_LANG_LAST) {
            select_caption_language(cmd - ID_CAPTION_LANG_FIRST);
            return 0;
        }
        if (cmd >= ID_AUDIO_TRACK_FIRST && cmd <= ID_AUDIO_TRACK_LAST) {
            const size_t index = cmd - ID_AUDIO_TRACK_FIRST;
            if (index < audio_track_ids_.size()) {
                player_.select_track("aid", audio_track_ids_[index]);
                wchar_t label[256] = {};
                if (GetMenuStringW(audio_menu_, cmd, label, 256, MF_BYCOMMAND) > 0)
                    Osd(player_, std::wstring(Tr(L"Audio: ")) + label, 1000);
            }
            return 0;
        }
        if (cmd >= ID_SUB_TRACK_FIRST && cmd <= ID_SUB_TRACK_LAST) {
            const size_t index = cmd - ID_SUB_TRACK_FIRST;
            if (index < sub_track_ids_.size()) {
                player_.select_track("sid", sub_track_ids_[index]);
                wchar_t label[256] = {};
                if (GetMenuStringW(subtitle_menu_, cmd, label, 256, MF_BYCOMMAND) > 0)
                    Osd(player_, std::wstring(Tr(L"Subtitle: ")) + label, 1000);
            }
            return 0;
        }
        switch (cmd) {
        case ID_FILE_OPEN:
            open_file();
            return 0;
        case ID_FILE_OPEN_URL:
            open_url();
            return 0;
        case ID_FILE_EXIT:
            DestroyWindow(hwnd);
            return 0;
        case ID_ALWAYS_ON_TOP:
            set_always_on_top(!always_on_top_, true);
            return 0;
        case ID_SUB_TRACK_OFF:
            player_.disable_subtitles();
            Osd(player_, L"Subtitles off", 1000);
            return 0;
        case ID_SETTINGS:
            show_settings();
            return 0;
        case ID_HELP_ABOUT:
            show_about();
            return 0;
        case ID_PLAY_PAUSE:
            player_.toggle_pause();
            return 0;
        case ID_STOP:
            player_.stop_playback();
            update_title();
            update_seek_bar();
            return 0;
        case ID_SEEK_BACK:
            if (live_playback())
                return 0;
            clip_active_ = ClipMark::None;
            player_.seek_relative(-5.0);
            return 0;
        case ID_SEEK_FORWARD:
            if (live_playback())
                return 0;
            clip_active_ = ClipMark::None;
            player_.seek_relative(5.0);
            return 0;
        case ID_FRAME_BACK:
            nudge_clip_or_frame(-1);
            return 0;
        case ID_FRAME_FORWARD:
            nudge_clip_or_frame(1);
            return 0;
        case ID_SPEED_DOWN:
            player_.adjust_speed(-0.1);
            return 0;
        case ID_SPEED_UP:
            player_.adjust_speed(0.1);
            return 0;
        case ID_SPEED_RESET:
            player_.reset_speed();
            return 0;
        case ID_SIZE_05:
            resize_to_video_scale(0.5);
            return 0;
        case ID_SIZE_10:
            resize_to_video_scale(1.0);
            return 0;
        case ID_SIZE_15:
            resize_to_video_scale(1.5);
            return 0;
        case ID_SIZE_20:
            resize_to_video_scale(2.0);
            return 0;
        case ID_FULLSCREEN_TOGGLE:
            toggle_fullscreen();
            return 0;
        case ID_FULLSCREEN_NORMAL:
            enter_fullscreen(false);
            return 0;
        case ID_FULLSCREEN_STRETCH:
            enter_fullscreen(true);
            return 0;
        case ID_STATS_SHOW:
            player_.show_stats();
            return 0;
        case ID_STATS_TOGGLE:
            player_.toggle_stats();
            return 0;
        case ID_VOLUME_DOWN:
            player_.adjust_volume(-2.0);
            return 0;
        case ID_VOLUME_UP:
            player_.adjust_volume(2.0);
            return 0;
        case ID_VOLUME_MUTE:
            player_.toggle_mute();
            return 0;
        case ID_SNAPSHOT:
            player_.take_snapshot();
            return 0;
        case ID_CLIP_MARK_IN:
            mark_clip_in();
            return 0;
        case ID_CLIP_MARK_OUT:
            mark_clip_out();
            return 0;
        case ID_CLIP_CLEAR:
            clear_clip_marks();
            return 0;
        case ID_ESCAPE:
            handle_escape();
            return 0;
        case ID_CLIP_EXPORT:
            export_clip();
            return 0;
        case ID_CAPTIONS_TOGGLE:
            toggle_captions();
            return 0;
        case ID_GENERATE_SRT:
            generate_srt();
            return 0;
        case ID_WHISPER_GET_MODEL: {
            std::wstring expected = captions_ ? captions_->model_id() : L"";
            if (expected.empty())
                expected = L"ggml-tiny.bin";
            ShowWhisperModelLoadHelp(hwnd_, L"No usable ggml model was found in the model directory.", expected);
            return 0;
        }
        case ID_LOAD_SRT:
            load_srt();
            return 0;
        case ID_CLEAR_SRT:
            clear_srt();
            return 0;
        case ID_YOLO_TOGGLE:
            cycle_yolo_mode();
            return 0;
        case ID_LIGHTFC_TOGGLE:
            cycle_lightfc_mode();
            return 0;
        default:
            break;
        }
        break;
    }
    case CaptionWorker::WM_CAPTION_UPDATE: {
        refresh_caption_osd(true);
        if (captions_ && captions_->enabled()) {
            const std::wstring status = captions_->status_text();
            // Tagged missing:/server: errors from WhisperRuntime 闂?show a one-shot help popup.
            if (status.rfind(L"missing:", 0) == 0 || status.find(L"No Whisper model") != std::wstring::npos ||
                status.find(L"Whisper model missing") != std::wstring::npos ||
                status.find(L"place a ggml") != std::wstring::npos) {
                static std::wstring last_help;
                if (status != last_help) {
                    last_help = status;
                    std::wstring expected = L"ggml-tiny.bin";
                    std::wstring reason = L"No usable ggml model was found.";
                    if (status.rfind(L"missing:", 0) == 0) {
                        // missing:filename|dir|url  OR missing:filename
                        const std::wstring payload = status.substr(8);
                        const size_t bar = payload.find(L'|');
                        expected = bar == std::wstring::npos ? payload : payload.substr(0, bar);
                        reason = L"Model file is missing from the model directory.";
                    }
                    ShowWhisperModelLoadHelp(hwnd_, reason, expected);
                }
            } else if (status.rfind(L"server:", 0) == 0) {
                static std::wstring last_server_help;
                if (status != last_server_help) {
                    last_server_help = status;
                    ShowWhisperModelLoadHelp(hwnd_, status.substr(7), captions_->model_id());
                }
            }
        }
        return 0;
    }

    case CaptionWorker::WM_SRT_DONE: {
        if (!captions_)
            return 0;
        const WPARAM code = wparam;
        const std::wstring status = captions_->srt_status();
        update_title();
        if (code == 1) {
            const std::wstring path = captions_->last_srt_path();
            std::wstring load_error;
            const bool loaded = !path.empty() && player_.add_external_subtitle(path, &load_error);
            if (loaded) {
                external_subtitle_loaded_ = true;
                Osd(player_, L"Subtitle loaded", 1800);
            } else {
                Osd(player_, L"SRT saved", 1800);
                std::wstring body = Tr(L"Subtitle file saved:\r\n\r\n");
                body += path.empty() ? status : path;
                if (!load_error.empty()) {
                    body += L"\r\n\r\n";
                    body += load_error;
                }
                MessageBoxW(hwnd_, body.c_str(), Tr(L"upPlayer - Generate Subtitle"),
                            MB_OK | MB_ICONINFORMATION);
            }
        } else if (code == 3 || status == L"Cancelled") {
            Osd(player_, L"Generate cancelled", 1600);
        } else if (code == 2) {
            if (status.rfind(L"missing:", 0) == 0 ||
                status.find(L"Whisper model missing") != std::wstring::npos ||
                status.find(L"No Whisper model") != std::wstring::npos) {
                std::wstring expected = captions_->model_id();
                std::wstring reason = L"No usable ggml model was found.";
                if (status.rfind(L"missing:", 0) == 0) {
                    const size_t pipe = status.find(L'|', 8);
                    expected = (pipe == std::wstring::npos) ? status.substr(8)
                                                            : status.substr(8, pipe - 8);
                    reason = L"Model file is missing from the model directory.";
                }
                ShowWhisperModelLoadHelp(hwnd_, reason, expected);
            } else {
                MessageBoxW(hwnd_, status.empty() ? Tr(L"Generate Subtitle failed.") : status.c_str(),
                            Tr(L"upPlayer - Generate Subtitle"), MB_OK | MB_ICONWARNING);
            }
            Osd(player_, L"SRT failed", 1600);
        } else if (!status.empty()) {
            std::string osd = WideToUtf8(status);
            if (osd.size() > 80)
                osd = osd.substr(0, 77) + "...";
            player_.show_osd(osd, 1500);
        }
        return 0;
    }
    case YoloWorker::WM_YOLO_UPDATE:
        refresh_yolo_osd();
        return 0;
    case LightFcWorker::WM_LIGHTFC_UPDATE:
        refresh_lightfc_osd();
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> media;
        std::vector<std::wstring> subtitles;
        for (UINT index = 0; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::vector<wchar_t> path(static_cast<size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, index, path.data(), length + 1);
            const std::wstring item = path.data();
            if (IsSubtitlePath(item))
                subtitles.push_back(item);
            else if (!item.empty())
                media.push_back(item);
        }
        DragFinish(drop);
        if (!media.empty()) {
            open_target(media.front());
            for (size_t index = 1; index < media.size(); ++index)
                player_.append(media[index]);
        }
        if (!subtitles.empty()) {
            if (!player_.state().has_media) {
                MessageBoxW(hwnd_, Tr(L"Open a video first."), Tr(L"upPlayer - Load Subtitle"),
                            MB_OK | MB_ICONINFORMATION);
            } else {
                for (const std::wstring& path : subtitles)
                    load_subtitle_file(path);
            }
        }
        return 0;
    }
    case WM_WINDOWPOSCHANGED:
        place_pin_button();
        break;
    case WM_ACTIVATE:
        if (pin_ && LOWORD(wparam) != WA_INACTIVE)
            InvalidateRect(pin_, nullptr, FALSE);
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        if (pin_)
            InvalidateRect(pin_, nullptr, FALSE);
        break;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
            layout();
        return 0;
    case WM_DPICHANGED: {
        auto* rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const int scale = hwnd_ ? dpi() : GetDpiForSystem();
        info->ptMinTrackSize.x = MulDiv(480, scale, 96);
        info->ptMinTrackSize.y = MulDiv(270, scale, 96);
        return 0;
    }
    case Player::WM_PLAYER_WAKEUP:
        player_.poll();
        update_title();
        update_seek_bar();
        remember_volume();
        note_playback_time(player_.state().time_pos);
        refresh_caption_osd(false);
        return 0;
    case Player::WM_PLAYER_STATE:
        player_.consume_notify();
        update_title();
        update_seek_bar();
        remember_volume();
        note_playback_time(player_.state().time_pos);
        refresh_caption_osd(false);
        return 0;
    case WM_PARENTNOTIFY:
        if (LOWORD(wparam) == WM_LBUTTONDOWN || LOWORD(wparam) == WM_RBUTTONDOWN)
            claim_keyboard_focus();
        break;
    case WM_TIMER:
        if (wparam == kTimerPin) {
            KillTimer(hwnd, kTimerPin);
            if (pin_)
                InvalidateRect(pin_, nullptr, FALSE);
            return 0;
        }
        if (wparam == kTimerStatus) {
            claim_keyboard_focus();
            update_title();
            if (!seeking_ && clip_dragging_ == ClipMark::None)
                update_seek_bar();
            note_playback_time(player_.state().time_pos);
            remember_resume();
            refresh_caption_osd(false);
            tick_yolo();
            tick_lightfc();
        }
        return 0;
    case WM_DESTROY:
        remember_resume();
        KillTimer(hwnd, kTimerStatus);
        lightfc_.reset();
        yolo_.reset();
        captions_.reset();
        player_.shutdown();
        if (context_menu_) {
            DestroyMenu(context_menu_);
            context_menu_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

std::string MenuTextUtf8(const wchar_t* english) {
    return WideToUtf8(std::wstring(Tr(english)));
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> files;
    if (argv) {
        for (int i = 1; i < argc; ++i)
            files.emplace_back(argv[i]);
        LocalFree(argv);
    }

    App app(instance);
    if (!app.create(show, files))
        return 1;
    return app.loop();
}
