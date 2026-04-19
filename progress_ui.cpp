// progress_ui.cpp
#include "progress_ui.h"

#include <CommCtrl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#pragma comment(lib, "Comctl32.lib")

namespace kds::ui {

namespace {

struct StepDonePack {
    std::wstring title;
    bool skipped = false;
    bool success = true;
};

constexpr UINT kMsgPhase = WM_APP + 10;
constexpr UINT kMsgBytes = WM_APP + 11;
constexpr UINT kMsgInstall = WM_APP + 12;
constexpr UINT kMsgPercent = WM_APP + 13;
constexpr UINT kMsgStepDone = WM_APP + 14;
constexpr UINT kMsgThreadDone = WM_APP + 99;
constexpr UINT_PTR kTimerSmooth = 1;

constexpr int kProgressRange = 10000; // smooth sub-percent updates

void SetChildFont(HWND child) {
    if (!child) {
        return;
    }
    if (HFONT hf = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(hf), TRUE);
    }
}

LONG ProgressStyleWithMarquee(bool marquee) {
    LONG s = PBS_SMOOTH;
    if (marquee) {
        s |= PBS_MARQUEE;
    }
    return s;
}

} // namespace

ProgressWindow::ProgressWindow() = default;

ProgressWindow::~ProgressWindow() {
    Destroy();
}

bool ProgressWindow::Create() {
    if (hwnd_) {
        return true;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    static ATOM class_atom = 0;
    if (class_atom == 0) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ProgressWindow::WndProcStatic;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"KdsProgressWindow";
        class_atom = RegisterClassExW(&wc);
        if (class_atom == 0) {
            return false;
        }
    }

    const DWORD frame = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const int w = 520;
    const int h = 220;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, L"KdsProgressWindow", L"KernelDevSetup",
                              frame, (sx - w) / 2, (sy - h) / 2, w, h, nullptr, nullptr,
                              GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        return false;
    }

    label_title_ =
        CreateWindowW(L"STATIC", L"KernelDevSetup", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 16, w - 48, 22,
                      hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    label_detail_ =
        CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 42, w - 48, 44, hwnd_, nullptr,
                      GetModuleHandleW(nullptr), nullptr);
    label_state_ =
        CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 90, w - 48, 20, hwnd_, nullptr,
                      GetModuleHandleW(nullptr), nullptr);

    progress_ =
        CreateWindowW(PROGRESS_CLASS, L"", WS_CHILD | WS_VISIBLE | ProgressStyleWithMarquee(false), 24, 118,
                      w - 48, 24, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);

    SetChildFont(label_title_);
    SetChildFont(label_detail_);
    SetChildFont(label_state_);

    SendMessageW(progress_, PBM_SETRANGE32, 0, kProgressRange);
    SendMessageW(progress_, PBM_SETBARCOLOR, 0, RGB(0, 122, 204));
    SendMessageW(progress_, PBM_SETBKCOLOR, 0, RGB(236, 239, 244));

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void ProgressWindow::Destroy() {
    if (smooth_timer_) {
        KillTimer(hwnd_, smooth_timer_);
        smooth_timer_ = 0;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    label_title_ = nullptr;
    label_detail_ = nullptr;
    label_state_ = nullptr;
    progress_ = nullptr;
}

void ProgressWindow::PumpMessagesUntilClosed() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void ProgressWindow::SetModeDeterminate(bool determinate, bool marquee) {
    if (!progress_) {
        return;
    }
    if (marquee) {
        LONG ex = GetWindowLongW(progress_, GWL_STYLE);
        SetWindowLongW(progress_, GWL_STYLE, ex | PBS_MARQUEE);
        SetWindowPos(progress_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
        LONG ex = GetWindowLongW(progress_, GWL_STYLE);
        SetWindowLongW(progress_, GWL_STYLE, ex & ~PBS_MARQUEE);
        SetWindowPos(progress_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        SendMessageW(progress_, PBM_SETRANGE32, 0, kProgressRange);
        if (determinate) {
            SendMessageW(progress_, PBM_SETPOS, 0, 0);
        }
    }
}

void ProgressWindow::SetProgressInternal(std::uint64_t current, std::uint64_t total) {
    if (!progress_ || total == 0) {
        return;
    }
    const double frac = static_cast<double>(current) / static_cast<double>(total);
    const int pos = static_cast<int>(std::llround(frac * static_cast<double>(kProgressRange)));
    SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(std::clamp(pos, 0, kProgressRange)), 0);
}

void ProgressWindow::ApplyPendingSmoothProgress() {
    if (!has_pending_bytes_ || pending_total_ == 0 || !progress_) {
        return;
    }
    const std::uint64_t target = (pending_cur_ * static_cast<std::uint64_t>(kProgressRange)) / pending_total_;
    if (shown_cur_ < target) {
        const std::uint64_t gap = target - shown_cur_;
        shown_cur_ += std::max<std::uint64_t>(1, gap / 6 + 1);
        shown_cur_ = std::min(shown_cur_, target);
    } else {
        shown_cur_ = target;
    }
    SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(shown_cur_), 0);
}

LRESULT CALLBACK ProgressWindow::WndProcStatic(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    ProgressWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<ProgressWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ProgressWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT ProgressWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_DESTROY:
        if (smooth_timer_) {
            KillTimer(hwnd, smooth_timer_);
            smooth_timer_ = 0;
        }
        return 0;

    case WM_TIMER:
        if (wparam == kTimerSmooth) {
            ApplyPendingSmoothProgress();
        }
        return 0;

    case kMsgPhase: {
        auto* t = reinterpret_cast<std::wstring*>(wparam);
        auto* d = reinterpret_cast<std::wstring*>(lparam);
        if (label_title_ && t) {
            SetWindowTextW(label_title_, t->c_str());
        }
        if (label_detail_ && d) {
            SetWindowTextW(label_detail_, d->c_str());
        }
        delete t;
        delete d;
        return 0;
    }

    case kMsgBytes: {
        auto* pack = reinterpret_cast<std::uint64_t*>(wparam);
        if (!pack) {
            return 0;
        }
        const std::uint64_t cur = pack[0];
        const std::uint64_t total = pack[1];
        delete[] pack;

        if (total == 0) {
            SetWindowTextW(label_state_, L"Download size unknown — showing activity only.");
            SetModeDeterminate(false, true);
            has_pending_bytes_ = false;
            return 0;
        }

        SetModeDeterminate(true, false);
        pending_cur_ = cur;
        pending_total_ = total;
        has_pending_bytes_ = true;
        if (shown_cur_ == 0) {
            shown_cur_ = 0;
        }

        wchar_t line[128];
        const double pct = (100.0 * static_cast<double>(cur)) / static_cast<double>(total);
        swprintf_s(line, L"Downloaded %llu of %llu bytes (%.1f%%)", static_cast<unsigned long long>(cur),
                   static_cast<unsigned long long>(total), pct);
        SetWindowTextW(label_state_, line);

        if (!smooth_timer_) {
            smooth_timer_ = SetTimer(hwnd, kTimerSmooth, 40, nullptr);
        }
        ApplyPendingSmoothProgress();
        return 0;
    }

    case kMsgInstall: {
        if (smooth_timer_) {
            KillTimer(hwnd, smooth_timer_);
            smooth_timer_ = 0;
        }
        auto* s = reinterpret_cast<std::wstring*>(wparam);
        if (label_detail_ && s) {
            SetWindowTextW(label_detail_, s->c_str());
        }
        delete s;
        SetWindowTextW(label_state_, L"Running installer — percentage may be unavailable; this can take a long time.");
        SetModeDeterminate(false, true);
        has_pending_bytes_ = false;
        return 0;
    }

    case kMsgPercent: {
        if (smooth_timer_) {
            KillTimer(hwnd, smooth_timer_);
            smooth_timer_ = 0;
        }
        const int known = static_cast<int>(wparam);
        if (known >= 0 && known <= 100) {
            SetModeDeterminate(true, false);
            const int pos = (known * kProgressRange) / 100;
            SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(pos), 0);
            wchar_t line[96];
            swprintf_s(line, L"Setup progress: %d%% (from Visual Studio installer logs)", known);
            SetWindowTextW(label_state_, line);
        } else {
            SetWindowTextW(label_state_, L"Running installer — percentage may be unavailable; this can take a long time.");
            SetModeDeterminate(false, true);
        }
        return 0;
    }

    case kMsgThreadDone:
        return 0;

    case kMsgStepDone: {
        auto* p = reinterpret_cast<StepDonePack*>(wparam);
        if (label_state_ && p) {
            std::wstring t;
            if (p->skipped) {
                t = L"Skipped: ";
            } else if (p->success) {
                t = L"Completed: ";
            } else {
                t = L"Finished with issues: ";
            }
            t += p->title;
            SetWindowTextW(label_state_, t.c_str());
        }
        delete p;
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void ProgressWindow::SetPhase(const wchar_t* title, const wchar_t* detail) {
    if (!hwnd_) {
        return;
    }
    auto* t = new std::wstring(title ? title : L"");
    auto* d = new std::wstring(detail ? detail : L"");
    PostMessageW(hwnd_, kMsgPhase, reinterpret_cast<WPARAM>(t), reinterpret_cast<LPARAM>(d));
}

void ProgressWindow::SetDownloadBytes(std::uint64_t current, std::uint64_t total) {
    if (!hwnd_) {
        return;
    }
    auto* pack = new std::uint64_t[2];
    pack[0] = current;
    pack[1] = total;
    PostMessageW(hwnd_, kMsgBytes, reinterpret_cast<WPARAM>(pack), 0);
}

void ProgressWindow::SetInstallIndeterminate(const wchar_t* operation_label) {
    if (!hwnd_) {
        return;
    }
    auto* s = new std::wstring(operation_label ? operation_label : L"");
    PostMessageW(hwnd_, kMsgInstall, reinterpret_cast<WPARAM>(s), 0);
}

void ProgressWindow::SetInstallPercent(std::optional<int> percent_0_100) {
    if (!hwnd_) {
        return;
    }
    const WPARAM w = static_cast<WPARAM>(percent_0_100.has_value() ? percent_0_100.value() : -1);
    PostMessageW(hwnd_, kMsgPercent, w, 0);
}

void ProgressWindow::SetStepComplete(const wchar_t* step_title, bool skipped, bool success) {
    if (!hwnd_) {
        return;
    }
    auto* p = new StepDonePack{};
    p->title = step_title ? step_title : L"";
    p->skipped = skipped;
    p->success = success;
    PostMessageW(hwnd_, kMsgStepDone, reinterpret_cast<WPARAM>(p), 0);
}

void RunWithProgressPump(ProgressWindow& window, const std::function<void(IProgressSink&)>& work) {
    if (!window.Hwnd()) {
        if (!window.Create()) {
            NullProgressSink null_sink;
            work(null_sink);
            return;
        }
    }

    std::atomic<bool> worker_finished{false};
    std::thread worker([&]() {
        work(window);
        worker_finished = true;
        if (window.Hwnd()) {
            PostMessageW(window.Hwnd(), kMsgThreadDone, 0, 0);
        }
    });

    MSG msg;
    bool exit_pump = false;
    while (!exit_pump) {
        const BOOL gm = PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
        if (gm) {
            if (msg.message == kMsgThreadDone) {
                exit_pump = true;
                continue;
            }
            if (msg.message == WM_QUIT) {
                exit_pump = true;
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            if (worker_finished.load()) {
                exit_pump = true;
            } else {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            }
        }
    }

    worker.join();
    window.Destroy();
}

} // namespace kds::ui
