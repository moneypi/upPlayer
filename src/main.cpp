#include "player.hpp"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <string>
#include <vector>

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
    void create_children();
    void layout();
    void update_title();
    void update_seek_bar();
    void seek_from_point(int x);
    void open_file();
    void open_url();
    void open_target(const std::wstring& target);
    void show_context_menu(int screen_x, int screen_y);
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
    double seek_preview_ = -1.0;
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
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
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
    if (message == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    if (message == WM_CONTEXTMENU) {
        HWND parent = GetParent(hwnd);
        auto* app = parent ? reinterpret_cast<App*>(GetWindowLongPtrW(parent, GWLP_USERDATA)) : nullptr;
        if (app) {
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

    context_menu_ = CreatePopupMenu();
    AppendMenuW(context_menu_, MF_STRING, ID_FILE_OPEN, L"Open(&O)...\tCtrl+O");
    AppendMenuW(context_menu_, MF_STRING, ID_FILE_OPEN_URL, L"Open Network Address(&N)...\tCtrl+U");
    AppendMenuW(context_menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(context_menu_, MF_STRING, ID_PLAY_PAUSE, L"Play/Pause(&P)\tSpace");
    AppendMenuW(context_menu_, MF_STRING, ID_STOP, L"Stop(&S)");
    AppendMenuW(context_menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(context_menu_, MF_STRING, ID_FILE_EXIT, L"Exit(&X)");

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
    if (caption != shown_title_) {
        shown_title_ = caption;
        SetWindowTextW(hwnd_, caption.c_str());
    }
}

void App::update_seek_bar() {
    if (seek_bar_)
        InvalidateRect(seek_bar_, nullptr, FALSE);
}

void App::seek_from_point(int x) {
    const PlayerState playback = player_.state();
    if (!(playback.duration > 0.0))
        return;
    RECT rect{};
    GetClientRect(seek_bar_, &rect);
    const int pad = MulDiv(12, dpi(), 96);
    const int time_width = MulDiv(54, dpi(), 96);
    const int left = pad + time_width;
    const int right = rect.right - pad - time_width;
    if (right <= left)
        return;
    double ratio = static_cast<double>(x - left) / static_cast<double>(right - left);
    if (ratio < 0.0)
        ratio = 0.0;
    if (ratio > 1.0)
        ratio = 1.0;
    seek_preview_ = ratio * playback.duration;
    player_.seek_absolute(seek_preview_);
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
        const int pad = MulDiv(12, dpi(), 96);
        const int time_width = MulDiv(54, dpi(), 96);
        const int track_height = MulDiv(4, dpi(), 96);
        const int track_y = (rect.bottom - track_height) / 2;
        RECT track{pad + time_width, track_y, rect.right - pad - time_width, track_y + track_height};
        if (track.right > track.left) {
            const HBRUSH track_brush = CreateSolidBrush(RGB(48, 52, 60));
            FillRect(dc, &track, track_brush);
            DeleteObject(track_brush);

            if (duration > 0.0) {
                double ratio = position / duration;
                if (ratio < 0.0)
                    ratio = 0.0;
                if (ratio > 1.0)
                    ratio = 1.0;
                const int filled = track.left + static_cast<int>((track.right - track.left) * ratio + 0.5);
                RECT progress{track.left, track.top, filled, track.bottom};
                const HBRUSH progress_brush = CreateSolidBrush(RGB(90, 200, 250));
                FillRect(dc, &progress, progress_brush);
                DeleteObject(progress_brush);

                const int knob = MulDiv(10, dpi(), 96);
                const RECT knob_rect{filled - knob / 2, track_y + track_height / 2 - knob / 2,
                                     filled + (knob + 1) / 2, track_y + track_height / 2 + (knob + 1) / 2};
                const HBRUSH knob_brush = CreateSolidBrush(RGB(230, 240, 250));
                FillRect(dc, &knob_rect, knob_brush);
                DeleteObject(knob_brush);
            }
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(180, 188, 196));
        HFONT font = CreateFontW(-MulDiv(9, dpi(), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        const std::wstring left = FormatTime(position);
        const std::wstring right = FormatTime(duration > 0.0 ? duration : -1.0);
        RECT left_rect{pad, 0, pad + time_width - MulDiv(6, dpi(), 96), rect.bottom};
        RECT right_rect{rect.right - pad - time_width + MulDiv(6, dpi(), 96), 0, rect.right - pad, rect.bottom};
        DrawTextW(dc, left.c_str(), -1, &left_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(dc, right.c_str(), -1, &right_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        seeking_ = true;
        seek_from_point(GET_X_LPARAM(lparam));
        return 0;
    case WM_MOUSEMOVE:
        if (seeking_ && (wparam & MK_LBUTTON))
            seek_from_point(GET_X_LPARAM(lparam));
        return 0;
    case WM_LBUTTONUP:
        if (seeking_) {
            seeking_ = false;
            seek_from_point(GET_X_LPARAM(lparam));
            seek_preview_ = -1.0;
            ReleaseCapture();
            update_seek_bar();
        }
        return 0;
    case WM_CAPTURECHANGED:
        seeking_ = false;
        seek_preview_ = -1.0;
        return 0;
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
    if (!player_.load(target))
        update_title();
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
            player_.seek_relative(-5.0);
            return 0;
        case ID_SEEK_FORWARD:
            player_.seek_relative(5.0);
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
    case WM_TIMER:
        if (wparam == kTimerStatus) {
            update_title();
            if (!seeking_)
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
