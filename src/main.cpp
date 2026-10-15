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
constexpr int kSeekBarId = 3001;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef MFS_HIDDEN
#define MFS_HIDDEN 0x00000008L
#endif

std::wstring Trim(std::wstring text) {
    auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!text.empty() && space(text.front()))
        text.erase(text.begin());
    while (!text.empty() && space(text.back()))
        text.pop_back();
    return text;
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

void SaveAppRegString(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\upPlayer", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
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

void FillRecentUrlCombo(HWND combo) {
    if (!combo)
        return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const std::vector<std::wstring> recent = LoadRecentUrls();
    for (const std::wstring& item : recent)
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    if (!recent.empty())
        SetWindowTextW(combo, recent.front().c_str());
    else
        SetWindowTextW(combo, L"");
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
        SetFocus(combo);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDOK: {
            wchar_t buffer[4096] = {};
            GetDlgItemTextW(dialog, IDC_URL, buffer, 4096);
            const std::wstring url = Trim(buffer);
            if (url.empty()) {
                MessageBoxW(dialog, L"Enter a network address.", L"Open Network Address", MB_ICONINFORMATION);
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
        case IDC_URL_REMOVE: {
            wchar_t buffer[4096] = {};
            GetDlgItemTextW(dialog, IDC_URL, buffer, 4096);
            const std::wstring url = Trim(buffer);
            if (url.empty()) {
                MessageBoxW(dialog, L"Nothing to remove.", L"Open Network Address", MB_ICONINFORMATION);
                return TRUE;
            }
            if (!RemoveRecentUrl(url)) {
                MessageBoxW(dialog, L"That address is not in the recent list.", L"Open Network Address",
                            MB_ICONINFORMATION);
                return TRUE;
            }
            FillRecentUrlCombo(GetDlgItem(dialog, IDC_URL));
            return TRUE;
        }
        case IDC_URL_CLEAR: {
            if (LoadRecentUrls().empty()) {
                MessageBoxW(dialog, L"Recent list is already empty.", L"Open Network Address",
                            MB_ICONINFORMATION);
                return TRUE;
            }
            if (MessageBoxW(dialog, L"Clear all recent network addresses?", L"Open Network Address",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return TRUE;
            ClearRecentUrls();
            FillRecentUrlCombo(GetDlgItem(dialog, IDC_URL));
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




struct SettingsDialogState {
    std::vector<std::wstring> model_ids;
    std::vector<std::wstring> lang_ids;
};

INT_PTR CALLBACK SettingsDlgProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_INITDIALOG: {
        auto* state = new SettingsDialogState();
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));

        HWND model_cb = GetDlgItem(dialog, IDC_SETTINGS_MODEL);
        WhisperRuntime whisper;
        const std::vector<WhisperModel> models = whisper.models();
        std::wstring current_model = LoadAppRegString(L"WhisperModel");
        if (current_model.empty())
            current_model = whisper.model_filename();
        int select_model = 0;
        if (models.empty()) {
            const std::wstring fallback = current_model.empty() ? L"ggml-tiny.bin" : current_model;
            SendMessageW(model_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fallback.c_str()));
            state->model_ids.push_back(fallback);
        } else {
            for (size_t i = 0; i < models.size(); ++i) {
                SendMessageW(model_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(models[i].filename.c_str()));
                state->model_ids.push_back(models[i].filename);
                if (!current_model.empty() && _wcsicmp(models[i].filename.c_str(), current_model.c_str()) == 0)
                    select_model = static_cast<int>(i);
            }
        }
        SendMessageW(model_cb, CB_SETCURSEL, select_model, 0);

        HWND lang_cb = GetDlgItem(dialog, IDC_SETTINGS_LANG);
        const std::vector<CaptionOption> languages = CaptionWorker::languages();
        std::wstring current_lang = LoadAppRegString(L"WhisperLanguage");
        if (current_lang.empty())
            current_lang = L"auto";
        int select_lang = 0;
        for (size_t i = 0; i < languages.size(); ++i) {
            SendMessageW(lang_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(languages[i].label.c_str()));
            state->lang_ids.push_back(languages[i].id);
            if (_wcsicmp(languages[i].id.c_str(), current_lang.c_str()) == 0)
                select_lang = static_cast<int>(i);
        }
        SendMessageW(lang_cb, CB_SETCURSEL, select_lang, 0);

        CheckDlgButton(dialog, IDC_SETTINGS_LOW_LATENCY,
                       DefaultNetworkLowLatency() ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(dialog, IDC_SETTINGS_RECENT_COUNT, static_cast<UINT>(MaxRecentUrls()), FALSE);
        return TRUE;
    }
    case WM_DESTROY: {
        auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
        delete state;
        SetWindowLongPtrW(dialog, DWLP_USER, 0);
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDOK: {
            auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
            const int model_sel =
                static_cast<int>(SendDlgItemMessageW(dialog, IDC_SETTINGS_MODEL, CB_GETCURSEL, 0, 0));
            const int lang_sel =
                static_cast<int>(SendDlgItemMessageW(dialog, IDC_SETTINGS_LANG, CB_GETCURSEL, 0, 0));
            BOOL translated = FALSE;
            UINT recent = GetDlgItemInt(dialog, IDC_SETTINGS_RECENT_COUNT, &translated, FALSE);
            if (!translated || recent < 1 || recent > 20) {
                MessageBoxW(dialog, L"Recent URLs to keep must be between 1 and 20.", L"Settings",
                            MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            if (state && model_sel >= 0 && static_cast<size_t>(model_sel) < state->model_ids.size())
                SaveAppRegString(L"WhisperModel", state->model_ids[static_cast<size_t>(model_sel)]);
            if (state && lang_sel >= 0 && static_cast<size_t>(lang_sel) < state->lang_ids.size())
                SaveAppRegString(L"WhisperLanguage", state->lang_ids[static_cast<size_t>(lang_sel)]);
            SaveRegDword(L"NetworkLowLatency",
                         IsDlgButtonChecked(dialog, IDC_SETTINGS_LOW_LATENCY) == BST_CHECKED ? 1 : 0);
            SaveRegDword(L"RecentUrlLimit", recent);
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

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    bool create(int show, const std::vector<std::wstring>& files);
    int loop();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK VideoProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK SeekBarProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_seek_bar(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    bool handle_playback_key(UINT message, WPARAM wparam);
    void disable_ime(HWND hwnd);
    void claim_keyboard_focus();
    void create_children();
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

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND video_ = nullptr;
    HWND seek_bar_ = nullptr;
    HMENU context_menu_ = nullptr;
    HMENU caption_model_menu_ = nullptr;
    HMENU caption_lang_menu_ = nullptr;
    HMENU subtitle_menu_ = nullptr;
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

    if (!player_.start(video_, hwnd_)) {
        MessageBoxW(hwnd_, player_.start_error().c_str(), L"upPlayer", MB_ICONERROR);
    } else if (!pending_files_.empty()) {
        open_target(pending_files_[0]);
        for (size_t i = 1; i < pending_files_.size(); ++i)
            player_.append(pending_files_[i]);
        update_title();
    }
    SetTimer(hwnd_, kTimerStatus, 100, nullptr);
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
            toggle_captions();
            return true;
        case L'y':
        case L'Y':
            cycle_yolo_mode();
            return true;
        case L't':
        case L'T':
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

    HMENU playback_menu = CreatePopupMenu();
    AppendMenuW(playback_menu, MF_STRING, ID_PLAY_PAUSE, L"Play/Pause\tSpace");
    AppendMenuW(playback_menu, MF_STRING, ID_STOP, L"Stop");
    AppendMenuW(playback_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playback_menu, MF_STRING, ID_SEEK_BACK, L"Seek -5s\tLeft");
    AppendMenuW(playback_menu, MF_STRING, ID_SEEK_FORWARD, L"Seek +5s\tRight");
    AppendMenuW(playback_menu, MF_STRING, ID_FRAME_BACK, L"Frame Back\t,");
    AppendMenuW(playback_menu, MF_STRING, ID_FRAME_FORWARD, L"Frame Forward\t.");
    AppendMenuW(playback_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playback_menu, MF_STRING, ID_STATS_SHOW, L"Stats\ti");
    AppendMenuW(playback_menu, MF_STRING, ID_STATS_TOGGLE, L"Stats Toggle\tI");

    HMENU speed_menu = CreatePopupMenu();
    AppendMenuW(speed_menu, MF_STRING, ID_SPEED_DOWN, L"Slower\tCtrl+Down");
    AppendMenuW(speed_menu, MF_STRING, ID_SPEED_UP, L"Faster\tCtrl+Up");
    AppendMenuW(speed_menu, MF_STRING, ID_SPEED_RESET, L"Reset 1x\tBackspace");

    HMENU size_menu = CreatePopupMenu();
    AppendMenuW(size_menu, MF_STRING, ID_SIZE_05, L"0.5x\t1");
    AppendMenuW(size_menu, MF_STRING, ID_SIZE_10, L"1x\t2");
    AppendMenuW(size_menu, MF_STRING, ID_SIZE_15, L"1.5x\t3");
    AppendMenuW(size_menu, MF_STRING, ID_SIZE_20, L"2x\t4");
    AppendMenuW(size_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(size_menu, MF_STRING, ID_FULLSCREEN_TOGGLE, L"Toggle Fullscreen\tEnter / Double-click");
    AppendMenuW(size_menu, MF_STRING, ID_FULLSCREEN_NORMAL, L"Fullscreen\t5");
    AppendMenuW(size_menu, MF_STRING, ID_FULLSCREEN_STRETCH, L"Fullscreen Stretch\t6");

    HMENU volume_menu = CreatePopupMenu();
    AppendMenuW(volume_menu, MF_STRING, ID_VOLUME_DOWN, L"Down\tDown");
    AppendMenuW(volume_menu, MF_STRING, ID_VOLUME_UP, L"Up\tUp");
    AppendMenuW(volume_menu, MF_STRING, ID_VOLUME_MUTE, L"Mute\tm");

    HMENU clip_menu = CreatePopupMenu();
    AppendMenuW(clip_menu, MF_STRING, ID_CLIP_MARK_IN, L"Mark In\tz");
    AppendMenuW(clip_menu, MF_STRING, ID_CLIP_MARK_OUT, L"Mark Out\tx");
    AppendMenuW(clip_menu, MF_STRING, ID_FRAME_BACK, L"Nudge Mark / Frame -\t,");
    AppendMenuW(clip_menu, MF_STRING, ID_FRAME_FORWARD, L"Nudge Mark / Frame +\t.");
    AppendMenuW(clip_menu, MF_STRING, ID_CLIP_EXPORT, L"Export Clip...\tc");
    AppendMenuW(clip_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(clip_menu, MF_STRING, ID_CLIP_CLEAR, L"Cancel Clip\tEsc");

    HMENU overlay_menu = CreatePopupMenu();
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
    AppendMenuW(subtitle_menu_, MF_STRING, ID_CLEAR_SRT, L"Unload Subtitle");
    {
        MENUITEMINFOW hide{};
        hide.cbSize = sizeof(hide);
        hide.fMask = MIIM_STATE;
        hide.fState = MFS_HIDDEN | MFS_DISABLED;
        SetMenuItemInfoW(subtitle_menu_, ID_CLEAR_SRT, FALSE, &hide);
    }
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
        AppendMenuW(caption_model_menu_, MF_STRING, ID_WHISPER_GET_MODEL, L"Download model...");
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
        player_.show_osd("Whisper model load failed", 1800);
        return;
    }
    player_.show_osd(WideToUtf8(L"Whisper model: " + label), 1400);
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
    player_.show_osd(WideToUtf8(L"Caption language: " + label), 1400);
}

void App::show_context_menu(int screen_x, int screen_y) {
    if (!context_menu_ || !hwnd_)
        return;
    refresh_caption_menus();
    CheckMenuItem(context_menu_, ID_CAPTIONS_TOGGLE,
                  MF_BYCOMMAND | ((captions_ && captions_->enabled()) ? MF_CHECKED : MF_UNCHECKED));
    {
        const bool srt_busy = captions_ && captions_->srt_busy();
        const bool can_srt = captions_ && !srt_busy && player_.state().has_media;
        ModifyMenuW(context_menu_, ID_GENERATE_SRT, MF_BYCOMMAND | MF_STRING, ID_GENERATE_SRT,
                    srt_busy ? L"Cancel Generate Subtitle\tEsc" : L"Generate Subtitle...");
        EnableMenuItem(context_menu_, ID_GENERATE_SRT,
                       MF_BYCOMMAND | ((srt_busy || can_srt) ? MF_ENABLED : MF_GRAYED));
        const bool can_load = player_.state().has_media;
        EnableMenuItem(context_menu_, ID_LOAD_SRT,
                       MF_BYCOMMAND | (can_load ? MF_ENABLED : MF_GRAYED));
        if (subtitle_menu_) {
            MENUITEMINFOW unload_info{};
            unload_info.cbSize = sizeof(unload_info);
            unload_info.fMask = MIIM_STATE;
            unload_info.fState = external_subtitle_loaded_
                                    ? MFS_ENABLED
                                    : (MFS_HIDDEN | MFS_DISABLED);
            SetMenuItemInfoW(subtitle_menu_, ID_CLEAR_SRT, FALSE, &unload_info);
        }
    }
    CheckMenuItem(context_menu_, ID_YOLO_TOGGLE,
                  MF_BYCOMMAND | ((yolo_ && yolo_->enabled()) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(context_menu_, ID_LIGHTFC_TOGGLE,
                  MF_BYCOMMAND | ((lightfc_ && lightfc_->enabled()) ? MF_CHECKED : MF_UNCHECKED));
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(context_menu_, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                   screen_x, screen_y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
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
    SetWindowPos(hwnd_, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
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
        player_.show_osd((clip_active_ == ClipMark::In ? "Clip In: " : "Clip Out: ") +
                             FormatTimeAscii(shown),
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

        if (duration > 0.0)
            draw_track(seek_left, seek_right, position / duration, RGB(90, 200, 250));
        else
            draw_track(seek_left, seek_right, 0.0, RGB(90, 200, 250));

        if (duration > 0.0 && seek_right > seek_left) {
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
        const std::wstring left = FormatTime(position);
        const std::wstring right = FormatTime(duration > 0.0 ? duration : -1.0);
        RECT left_rect{pad, 0, pad + time_width - MulDiv(6, dpi(), 96), rect.bottom};
        RECT right_rect{seek_right + MulDiv(6, dpi(), 96), 0, vol_left - MulDiv(6, dpi(), 96), rect.bottom};
        DrawTextW(dc, left.c_str(), -1, &left_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(dc, right.c_str(), -1, &right_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
                player_.show_osd("Clip In: " + FormatTimeAscii(clip_in_), 900);
            else if (clip_active_ == ClipMark::Out)
                player_.show_osd("Clip Out: " + FormatTimeAscii(clip_out_), 900);
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
    dialog.lpstrFilter =
        L"Media files\0*.mp4;*.mkv;*.avi;*.mov;*.webm;*.flv;*.ts;*.m2ts;*.m4v;*.wmv;*.mpg;*.mpeg;*.mp3;*.flac;*.wav;*.aac;*.ogg;*.opus;*.m4a;*.m3u;*.m3u8\0"
        L"All files\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Open";
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
    if (!player_.load(target))
        update_title();
    sync_captions_media();
    update_seek_bar();
}

void App::show_about() {
    std::wstring text = L"upPlayer ";
    text += UPPLAYER_VERSION_STRINGW;
    text += L"\n\nLightweight Win32 front-end for mpv.";
    text += L"\nPlayback: ";
    text += player_.backend_label().empty() ? L"n/a" : player_.backend_label();
    text += L"\n\nLicense: MIT (see LICENSE / NOTICE)";
    MessageBoxW(hwnd_, text.c_str(), L"About upPlayer", MB_OK | MB_ICONINFORMATION);
}
void App::show_settings() {
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd_, SettingsDlgProc, 0) != IDOK)
        return;
    if (captions_) {
        const std::wstring model = LoadAppRegString(L"WhisperModel");
        if (!model.empty())
            captions_->set_model(model);
        const std::wstring lang = LoadAppRegString(L"WhisperLanguage");
        if (!lang.empty())
            captions_->set_language(lang);
        refresh_caption_menus();
    }
    player_.show_osd("Settings saved", 1400);
}


void App::toggle_captions() {
    if (!captions_)
        return;
    const bool on = !captions_->enabled();
    captions_->set_enabled(on);
    if (on) {
        sync_captions_media();
        player_.show_osd("Captions on", 1000);
    } else {
        last_caption_osd_.clear();
        last_caption_osd_tick_ = 0;
        player_.clear_caption();
        player_.show_osd("Captions off", 1000);
    }
}

void App::generate_srt() {
    if (!captions_)
        return;
    if (captions_->srt_busy()) {
        captions_->cancel_generate_srt();
        player_.show_osd("Cancelling subtitle...", 1600);
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
        MessageBoxW(hwnd_, error.empty() ? L"Cannot start Generate Subtitle." : error.c_str(),
                    L"upPlayer - Generate Subtitle", MB_OK | MB_ICONINFORMATION);
        return;
    }
    player_.show_osd("Generating subtitle...", 2000);
    update_title();
}


void App::load_srt() {
    if (!player_.state().has_media) {
        MessageBoxW(hwnd_, L"Open a video first.", L"upPlayer - Load Subtitle",
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
    dialog.lpstrFilter =
        L"Subtitle files\0*.srt;*.ass;*.ssa;*.vtt;*.sub\0"
        L"SRT\0*.srt\0"
        L"All files\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Load Subtitle";
    dialog.lpstrDefExt = L"srt";
    if (!initial_dir.empty())
        dialog.lpstrInitialDir = initial_dir.c_str();
    if (!GetOpenFileNameW(&dialog))
        return;

    const std::wstring path = buffer.data();
    std::wstring error;
    if (!player_.add_external_subtitle(path, &error)) {
        MessageBoxW(hwnd_, error.empty() ? L"Failed to load subtitle." : error.c_str(),
                    L"upPlayer - Load Subtitle", MB_OK | MB_ICONWARNING);
        return;
    }

    const auto slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    external_subtitle_loaded_ = true;
    player_.show_osd(WideToUtf8(L"Loaded: " + name), 1600);
}



void App::clear_srt() {
    if (!player_.state().has_media) {
        MessageBoxW(hwnd_, L"Open a video first.", L"upPlayer - Unload Subtitle",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring error;
    if (!player_.clear_external_subtitle(&error)) {
        MessageBoxW(hwnd_, error.empty() ? L"Failed to unload subtitle." : error.c_str(),
                    L"upPlayer - Unload Subtitle", MB_OK | MB_ICONWARNING);
        return;
    }
    external_subtitle_loaded_ = false;
    player_.show_osd("Unloaded subtitle", 1200);
}

void App::set_yolo_mode(YoloMode mode) {
    if (!yolo_)
        return;
    yolo_->set_mode(mode);
    if (mode == YoloMode::Off) {
        player_.clear_yolo_overlay();
        player_.show_osd("YOLO26 off", 1000);
    } else {
        player_.show_osd("YOLO26 on (skip busy frames)", 1400);
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
    lightfc_->set_mode(mode);
    if (mode == LightFcMode::Off) {
        player_.clear_track_overlay();
        player_.show_osd("LightFC off", 1000);
    } else if (mode == LightFcMode::Arm) {
        player_.show_osd("LightFC: click a target", 1600);
    } else {
        player_.show_osd("LightFC tracking", 1200);
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
    player_.show_osd("LightFC: locking...", 1000);
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
        player_.show_osd("No media", 1000);
        return;
    }
    player_.set_pause(true);
    set_clip_mark_time(ClipMark::In, playback.time_pos, true);
}

void App::mark_clip_out() {
    const PlayerState playback = player_.state();
    if (!playback.has_media || !(playback.time_pos >= 0.0)) {
        player_.show_osd("No media", 1000);
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
    player_.show_osd("Clip cancelled", 1000);
    update_seek_bar();
}

void App::handle_escape() {
    if (captions_ && captions_->srt_busy()) {
        captions_->cancel_generate_srt();
        player_.show_osd("Cancelling subtitle...", 1600);
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
        player_.show_osd("Mark In (z) and Out (x) first", 1600);
        return;
    }
    const std::wstring source = player_.current_path();
    if (source.empty()) {
        player_.show_osd("No source path", 1200);
        return;
    }
    const std::wstring mpv = FindExecutable(L"mpv.exe");
    if (mpv.empty()) {
        MessageBoxW(hwnd_,
                    L"mpv.exe was not found.\n\n"
                    L"Place mpv.exe next to upPlayer.exe (release package includes it), or add it to PATH.",
                    L"Export Clip", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const double start = clip_in_;
    const double end = clip_out_;
    std::wstring suggested = SuggestClipName(source, start, end);
    std::vector<wchar_t> buffer(32768, L'\0');
    wcsncpy_s(buffer.data(), buffer.size(), suggested.c_str(), _TRUNCATE);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"Media files\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Export Clip";
    dialog.lpstrDefExt = L"mp4";
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

    player_.show_osd("Exporting clip...", 2000);
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
        MessageBoxW(hwnd_, message.c_str(), L"Export Clip", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!log_path.empty())
        DeleteFileW(log_path.c_str());
    player_.show_osd("Clip exported", 1400);
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
            clip_active_ = ClipMark::None;
            player_.seek_relative(-5.0);
            return 0;
        case ID_SEEK_FORWARD:
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
            player_.show_osd("SRT saved", 1800);
            std::wstring body = L"Subtitle file saved:\r\n\r\n";
            body += path.empty() ? status : path;
            MessageBoxW(hwnd_, body.c_str(), L"upPlayer - Generate Subtitle", MB_OK | MB_ICONINFORMATION);
        } else if (code == 3 || status == L"Cancelled") {
            player_.show_osd("Generate cancelled", 1600);
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
                MessageBoxW(hwnd_, status.empty() ? L"Generate Subtitle failed." : status.c_str(),
                            L"upPlayer - Generate Subtitle", MB_OK | MB_ICONWARNING);
            }
            player_.show_osd("SRT failed", 1600);
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
        if (count > 0) {
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            std::vector<wchar_t> path(static_cast<size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, 0, path.data(), length + 1);
            open_target(path.data());
            for (UINT index = 1; index < count; ++index) {
                const UINT item_length = DragQueryFileW(drop, index, nullptr, 0);
                std::vector<wchar_t> item(static_cast<size_t>(item_length) + 1, L'\0');
                DragQueryFileW(drop, index, item.data(), item_length + 1);
                player_.append(item.data());
            }
        }
        DragFinish(drop);
        return 0;
    }
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
        note_playback_time(player_.state().time_pos);
        refresh_caption_osd(false);
        return 0;
    case Player::WM_PLAYER_STATE:
        player_.consume_notify();
        update_title();
        update_seek_bar();
        note_playback_time(player_.state().time_pos);
        refresh_caption_osd(false);
        return 0;
    case WM_PARENTNOTIFY:
        if (LOWORD(wparam) == WM_LBUTTONDOWN || LOWORD(wparam) == WM_RBUTTONDOWN)
            claim_keyboard_focus();
        break;
    case WM_TIMER:
        if (wparam == kTimerStatus) {
            claim_keyboard_focus();
            update_title();
            if (!seeking_ && clip_dragging_ == ClipMark::None)
                update_seek_bar();
            note_playback_time(player_.state().time_pos);
            refresh_caption_osd(false);
            tick_yolo();
            tick_lightfc();
        }
        return 0;
    case WM_DESTROY:
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
