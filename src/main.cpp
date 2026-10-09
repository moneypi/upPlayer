#include "player.hpp"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

struct UrlDialogResult {
    std::wstring url;
    bool low_latency = true;
    bool untimed = true;
};

INT_PTR CALLBACK UrlDlgProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        HWND edit = GetDlgItem(dialog, IDC_URL);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"https://example.com/video.mp4"));
        CheckDlgButton(dialog, IDC_LOW_LATENCY, BST_CHECKED);
        CheckDlgButton(dialog, IDC_UNTIMED, BST_CHECKED);
        SetFocus(edit);
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
                out->untimed = IsDlgButtonChecked(dialog, IDC_UNTIMED) == BST_CHECKED;
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
    void open_target(const std::wstring& target);
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

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND video_ = nullptr;
    HWND seek_bar_ = nullptr;
    HMENU context_menu_ = nullptr;
    Player player_;
    std::wstring shown_title_;
    std::vector<std::wstring> pending_files_;
    bool seeking_ = false;
    bool volume_dragging_ = false;
    double seek_preview_ = -1.0;
    double volume_preview_ = -1.0;
    double clip_in_ = -1.0;
    double clip_out_ = -1.0;
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
        case L'[':
        case L'{':
            player_.adjust_speed(-0.1);
            return true;
        case L']':
        case L'}':
            player_.adjust_speed(0.1);
            return true;
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
        default:
            return false;
        }
    }
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN)
        return false;
    switch (wparam) {
    case VK_OEM_4:
        player_.adjust_speed(-0.1);
        return true;
    case VK_OEM_6:
        player_.adjust_speed(0.1);
        return true;
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
    case VK_VOLUME_UP:
        player_.adjust_volume(2.0);
        return true;
    case VK_DOWN:
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
    AppendMenuW(speed_menu, MF_STRING, ID_SPEED_DOWN, L"Slower\t[");
    AppendMenuW(speed_menu, MF_STRING, ID_SPEED_UP, L"Faster\t]");
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

    context_menu_ = CreatePopupMenu();
    AppendMenuW(context_menu_, MF_STRING, ID_FILE_OPEN, L"Open...\tCtrl+O");
    AppendMenuW(context_menu_, MF_STRING, ID_FILE_OPEN_URL, L"Open Network Address...\tCtrl+U");
    AppendMenuW(context_menu_, MF_STRING, ID_SNAPSHOT, L"Take Snapshot\ts");
    AppendMenuW(context_menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(playback_menu), L"Playback");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(speed_menu), L"Speed");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(size_menu), L"Size");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(volume_menu), L"Volume");
    AppendMenuW(context_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(clip_menu), L"Clip");
    AppendMenuW(context_menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(context_menu_, MF_STRING, ID_FILE_EXIT, L"Exit");

    DragAcceptFiles(hwnd_, TRUE);
}

void App::show_context_menu(int screen_x, int screen_y) {
    if (!context_menu_ || !hwnd_)
        return;
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
    player_.stop_playback();
    player_.set_low_latency(result.low_latency);
    player_.set_untimed(result.untimed);
    open_target(result.url);
    update_title();
    SetFocus(video_);
}

void App::open_target(const std::wstring& target) {
    clip_in_ = -1.0;
    clip_out_ = -1.0;
    clip_active_ = ClipMark::None;
    clip_dragging_ = ClipMark::None;
    if (!player_.load(target))
        update_title();
    update_seek_bar();
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

    // Official Windows mpv builds do not expose --ovc=copy. Use stream-record to dump A–B.
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
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_FILE_OPEN:
            open_file();
            return 0;
        case ID_FILE_OPEN_URL:
            open_url();
            return 0;
        case ID_FILE_EXIT:
            DestroyWindow(hwnd);
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
        default:
            break;
        }
        break;
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
        return 0;
    case Player::WM_PLAYER_STATE:
        player_.consume_notify();
        update_title();
        update_seek_bar();
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
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimerStatus);
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
