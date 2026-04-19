// progress_ui.h - Thread-safe Win32 progress window (determinate bytes, indeterminate installs).
#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace kds::ui {

// Abstract sink: safe to call from worker threads; implementations marshal to the UI thread.
class IProgressSink {
public:
    virtual ~IProgressSink() = default;
    virtual void SetPhase(const wchar_t* title, const wchar_t* detail) = 0;
    /// total == 0 means length unknown (UI shows indeterminate for the download).
    virtual void SetDownloadBytes(std::uint64_t current, std::uint64_t total) = 0;
    /// Long-running install with no reliable percentage.
    virtual void SetInstallIndeterminate(const wchar_t* operation_label) = 0;
    /// When install machinery exposes a 0..100 value; omit or pass nullopt to stay indeterminate.
    virtual void SetInstallPercent(std::optional<int> percent_0_100) = 0;
    virtual void SetStepComplete(const wchar_t* step_title, bool skipped, bool success) = 0;

protected:
    IProgressSink() = default;
    IProgressSink(const IProgressSink&) = delete;
    IProgressSink& operator=(const IProgressSink&) = delete;
};

struct NullProgressSink final : IProgressSink {
    void SetPhase(const wchar_t*, const wchar_t*) override {}
    void SetDownloadBytes(std::uint64_t, std::uint64_t) override {}
    void SetInstallIndeterminate(const wchar_t*) override {}
    void SetInstallPercent(std::optional<int>) override {}
    void SetStepComplete(const wchar_t*, bool, bool) override {}
};

// Modal progress window: create on the thread that will call PumpMessagesUntilClosed().
class ProgressWindow final : public IProgressSink {
public:
    ProgressWindow();
    ~ProgressWindow();

    ProgressWindow(const ProgressWindow&) = delete;
    ProgressWindow& operator=(const ProgressWindow&) = delete;

    bool Create();
    void Destroy();
    HWND Hwnd() const { return hwnd_; }

    /// Drive the Win32 message pump until Destroy() or WM_CLOSE (call from UI thread).
    void PumpMessagesUntilClosed();

    // IProgressSink — thread-safe (posts to window thread).
    void SetPhase(const wchar_t* title, const wchar_t* detail) override;
    void SetDownloadBytes(std::uint64_t current, std::uint64_t total) override;
    void SetInstallIndeterminate(const wchar_t* operation_label) override;
    void SetInstallPercent(std::optional<int> percent_0_100) override;
    void SetStepComplete(const wchar_t* step_title, bool skipped, bool success) override;

private:
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void ApplyPendingSmoothProgress();
    void SetProgressInternal(std::uint64_t current, std::uint64_t total);
    void SetModeDeterminate(bool determinate, bool marquee);

    HWND hwnd_ = nullptr;
    HWND label_title_ = nullptr;
    HWND label_detail_ = nullptr;
    HWND label_state_ = nullptr;
    HWND progress_ = nullptr;

    UINT_PTR smooth_timer_ = 0;
    std::uint64_t pending_cur_ = 0;
    std::uint64_t pending_total_ = 0;
    std::uint64_t shown_cur_ = 0;
    bool has_pending_bytes_ = false;
};

/// Runs `work` on a worker thread while pumping messages for `window` (must be created first).
void RunWithProgressPump(ProgressWindow& window, const std::function<void(IProgressSink&)>& work);

} // namespace kds::ui
