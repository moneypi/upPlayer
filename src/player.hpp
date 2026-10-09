#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct PlayerState {
    std::wstring title;
    std::wstring last_error;
    double time_pos = -1.0;
    double duration = -1.0;
    bool paused = false;
    bool has_media = false;
};

// Embeds mpv. Prefers libmpv (headers from the mpv submodule); if libmpv-2.dll
// is not next to the program, falls back to mpv.exe on PATH with --wid and JSON IPC.
class Player {
public:
    static constexpr UINT WM_PLAYER_WAKEUP = WM_APP + 21;
    static constexpr UINT WM_PLAYER_STATE = WM_APP + 22;

    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool start(HWND video, HWND notify);
    void shutdown();

    bool load(const std::wstring& target);
    void append(const std::wstring& target);
    bool set_low_latency(bool enabled);
    bool set_untimed(bool enabled);
    void toggle_pause();
    void stop_playback();
    void seek_relative(double seconds);
    void seek_absolute(double seconds);

    void poll();
    void consume_notify();

    PlayerState state() const;
    bool low_latency() const;
    bool untimed() const;
    bool running() const;
    const std::wstring& backend_label() const;
    const std::wstring& start_error() const;

private:
    struct Api;

    bool start_libmpv(HWND video, std::wstring& error);
    bool start_exe(HWND video, std::wstring& error);
    void fail_libmpv();
    void fail_exe();

    bool exec(const std::vector<std::string>& args, std::wstring* error);
    void post(const std::vector<std::string>& args);
    bool write_line(const std::string& line);
    bool connect_pipe();
    void stop_io();
    void reader_main();
    void writer_main();
    void handle_ipc_line(const std::string& line);

    void load_utf8(const std::string& utf8, const std::wstring& display, bool preserve_seek);
    void on_file_loaded();
    void set_time(double t);
    void set_duration(double t);
    void set_title(std::wstring title);
    void set_paused(bool paused);
    void set_error(std::wstring error);
    void notify();

    bool begin_wait(int id);
    void resolve_wait(int id, const std::string& error);
    bool wait_pending(int timeout_ms, std::string& error);

    Api* api_ = nullptr;
    void* ctx_ = nullptr;
    HMODULE dll_ = nullptr;

    HWND notify_hwnd_ = nullptr;
    std::atomic<uintptr_t> notify_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> reader_run_{false};
    std::atomic<bool> low_latency_{false};
    std::atomic<bool> untimed_{false};
    std::atomic<bool> profile_applied_{false};
    std::atomic<bool> state_posted_{false};
    std::atomic<bool> wakeup_posted_{false};

    HANDLE process_ = nullptr;
    HANDLE process_thread_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::wstring pipe_name_;
    std::wstring log_path_;
    std::thread reader_;
    std::thread writer_;
    std::deque<std::string> write_queue_;
    std::condition_variable write_cv_;
    std::atomic<bool> writer_run_{false};

    mutable std::mutex mu_;
    PlayerState state_;
    std::string current_utf8_;
    std::wstring current_display_;
    double pending_seek_ = -1.0;

    std::mutex write_mu_;
    std::mutex wait_mu_;
    std::condition_variable wait_cv_;
    int next_id_ = 1;
    int pending_id_ = 0;
    bool pending_done_ = false;
    std::string pending_result_;

    std::wstring backend_label_;
    std::wstring start_error_;
    bool backend_exe_ = false;
};
