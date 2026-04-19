// process_utils.cpp
#include "process_utils.h"

#include "logging.h"

#include <Shellapi.h>
#include <objbase.h>
#include <urlmon.h>
#include <winerror.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string_view>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "urlmon.lib")

namespace kds::proc {

namespace {

class DownloadBindCallback final : public IBindStatusCallback {
    ULONG ref_ = 1;
    std::function<void(std::uint64_t, std::uint64_t)> on_progress_;

public:
    explicit DownloadBindCallback(std::function<void(std::uint64_t, std::uint64_t)> fn)
        : on_progress_(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *ppv = static_cast<IBindStatusCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) {
            delete this;
        }
        return r;
    }

    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetPriority(LONG* p) override {
        if (p) {
            *p = THREAD_PRIORITY_NORMAL;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnProgress(ULONG ulProgress, ULONG ulProgressMax, ULONG, LPCWSTR) override {
        if (on_progress_) {
            on_progress_(static_cast<std::uint64_t>(ulProgress), static_cast<std::uint64_t>(ulProgressMax));
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD* grfBINDF, BINDINFO* pbindinfo) override {
        if (grfBINDF) {
            *grfBINDF = BINDF_ASYNCHRONOUS | BINDF_GETNEWESTVERSION | BINDF_NOWRITECACHE;
        }
        if (pbindinfo) {
            const DWORD n = pbindinfo->cbSize;
            if (n >= sizeof(BINDINFO)) {
                std::memset(pbindinfo, 0, n);
                pbindinfo->cbSize = n;
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

bool IsUserAdminElevated() {
    BOOL is_admin = FALSE;
    PSID admin_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,
                                  0, 0, 0, 0, 0, &admin_group)) {
        return false;
    }
    if (!CheckTokenMembership(nullptr, admin_group, &is_admin)) {
        is_admin = FALSE;
    }
    FreeSid(admin_group);
    return is_admin == TRUE;
}

bool IsTokenElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD ret_len = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &ret_len);
    CloseHandle(token);
    if (!ok) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

} // namespace

bool IsProcessElevatedAdmin() {
    return IsUserAdminElevated() && IsTokenElevated();
}

bool RelaunchSelfElevated(const std::wstring& args) {
    std::wstring exe = GetModulePath();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        KDS_LOG(Error, ("ShellExecuteExW (runas) failed. Win32 error: " + std::to_string(GetLastError())).c_str());
        return false;
    }
    if (sei.hProcess) {
        CloseHandle(sei.hProcess);
    }
    return true;
}

std::wstring GetModulePath() {
    wchar_t path[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (n == 0 || n >= std::size(path)) {
        return {};
    }
    return std::wstring(path);
}

std::wstring ExpandEnvironmentStringsWrapper(const std::wstring& input) {
    DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (needed == 0) {
        return input;
    }
    std::wstring buf;
    buf.resize(needed);
    DWORD written = ExpandEnvironmentStringsW(input.c_str(), buf.data(), needed);
    if (written == 0 || written > needed) {
        return input;
    }
    buf.resize(written > 0 ? written - 1 : 0);
    return buf;
}

std::optional<std::wstring> WriteTempUtf8File(const std::wstring& prefix_three_chars,
                                              const std::string& utf8_content) {
    wchar_t temp_dir[MAX_PATH] = {};
    if (GetTempPathW(static_cast<DWORD>(std::size(temp_dir)), temp_dir) == 0) {
        return std::nullopt;
    }
    wchar_t temp_file[MAX_PATH] = {};
    const wchar_t* prefix = prefix_three_chars.empty() ? L"kds" : prefix_three_chars.c_str();
    UINT u = GetTempFileNameW(temp_dir, prefix, 0, temp_file);
    if (u == 0) {
        return std::nullopt;
    }
    // GetTempFileName creates zero-byte file; we overwrite with UTF-8 BOM optional - use plain UTF-8.
    int fd = -1;
    errno_t err = _wsopen_s(&fd, temp_file, _O_WRONLY | _O_TRUNC | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (err != 0 || fd < 0) {
        DeleteFileW(temp_file);
        return std::nullopt;
    }
    const char* data = utf8_content.data();
    size_t remaining = utf8_content.size();
    while (remaining > 0) {
        int w = _write(fd, data, static_cast<unsigned int>(std::min(remaining, static_cast<size_t>(UINT_MAX))));
        if (w <= 0) {
            _close(fd);
            DeleteFileW(temp_file);
            return std::nullopt;
        }
        data += w;
        remaining -= static_cast<size_t>(w);
    }
    _close(fd);
    return std::wstring(temp_file);
}

CommandResult RunCommand(const std::wstring& command_line, const std::wstring* working_directory,
                         DWORD timeout_ms, bool hide_window) {
    CommandResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Single combined pipe for stdout+stderr avoids child blocking when either stream fills.
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        result.error_message = "CreatePipe (stdout) failed";
        return result;
    }

    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = hide_window ? SW_HIDE : SW_SHOWNORMAL;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmdline = command_line;

    DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT;
    if (hide_window) {
        creation_flags |= CREATE_NO_WINDOW;
    }

    BOOL created =
        CreateProcessW(nullptr, mutable_cmdline.data(), nullptr, nullptr, TRUE, creation_flags, nullptr,
                       working_directory ? working_directory->c_str() : nullptr, &si, &pi);

    CloseHandle(stdout_write);

    if (!created) {
        CloseHandle(stdout_read);
        result.error_message = "CreateProcessW failed";
        return result;
    }

    result.started = true;
    CloseHandle(pi.hThread);

    std::string combined_output;
    char buffer[4096];

    auto drain_pipe = [&]() {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &avail, nullptr)) {
                break;
            }
            if (avail == 0) {
                break;
            }
            DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buffer)));
            DWORD read_bytes = 0;
            if (!ReadFile(stdout_read, buffer, to_read, &read_bytes, nullptr) || read_bytes == 0) {
                break;
            }
            combined_output.append(buffer, read_bytes);
        }
    };

    const auto start_time = std::chrono::steady_clock::now();

    for (;;) {
        DWORD child_exit = STILL_ACTIVE;
        GetExitCodeProcess(pi.hProcess, &child_exit);
        if (child_exit != STILL_ACTIVE) {
            break;
        }

        DWORD slice_ms = 250;
        if (timeout_ms != 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start_time)
                                     .count();
            if (elapsed >= static_cast<long long>(timeout_ms)) {
                result.timed_out = true;
                TerminateProcess(pi.hProcess, static_cast<UINT>(-1));
                WaitForSingleObject(pi.hProcess, 10000);
                break;
            }
            const auto remaining = static_cast<DWORD>(timeout_ms - elapsed);
            slice_ms = std::min<DWORD>(250, remaining);
        }

        WaitForSingleObject(pi.hProcess, slice_ms);
        drain_pipe();
    }

    drain_pipe();
    DWORD read_bytes = 0;
    while (ReadFile(stdout_read, buffer, sizeof(buffer), &read_bytes, nullptr) && read_bytes > 0) {
        combined_output.append(buffer, read_bytes);
    }

    CloseHandle(stdout_read);

    DWORD exit_code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);

    result.exit_code = exit_code;
    // Stderr was merged into the child's stdout handle for deadlock safety.
    result.stdout_text = std::move(combined_output);
    result.stderr_text.clear();
    return result;
}

bool DownloadUrlToFile(const std::wstring& url, const std::wstring& dest_path, std::string& error_out) {
    error_out.clear();
    if (url.empty() || dest_path.empty()) {
        error_out = "DownloadUrlToFile: empty URL or destination path.";
        return false;
    }

    const HRESULT coinit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (coinit != S_OK && coinit != S_FALSE && coinit != RPC_E_CHANGED_MODE) {
        error_out = "CoInitializeEx failed for URL download.";
        return false;
    }
    const bool com_initialized_here = (coinit == S_OK || coinit == S_FALSE);

    DeleteFileW(dest_path.c_str());

    const HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), dest_path.c_str(), 0, nullptr);

    if (com_initialized_here) {
        CoUninitialize();
    }

    if (FAILED(hr)) {
        char buf[96];
        sprintf_s(buf, sizeof(buf), "URLDownloadToFile failed. HRESULT: 0x%08lX",
                  static_cast<unsigned long>(hr));
        error_out = buf;
        return false;
    }
    return true;
}

bool DownloadUrlToFileWithProgress(const std::wstring& url, const std::wstring& dest_path,
                                   const std::function<void(std::uint64_t, std::uint64_t)>& on_progress,
                                   std::string& error_out) {
    error_out.clear();
    if (url.empty() || dest_path.empty()) {
        error_out = "DownloadUrlToFileWithProgress: empty URL or destination path.";
        return false;
    }

    const HRESULT coinit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (coinit != S_OK && coinit != S_FALSE && coinit != RPC_E_CHANGED_MODE) {
        error_out = "CoInitializeEx failed for URL download.";
        return false;
    }
    const bool com_initialized_here = (coinit == S_OK || coinit == S_FALSE);

    DeleteFileW(dest_path.c_str());

    auto* cb = new DownloadBindCallback(on_progress);
    const HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), dest_path.c_str(), 0, cb);
    cb->Release();

    if (com_initialized_here) {
        CoUninitialize();
    }

    if (FAILED(hr)) {
        char buf[96];
        sprintf_s(buf, sizeof(buf), "URLDownloadToFile failed. HRESULT: 0x%08lX",
                  static_cast<unsigned long>(hr));
        error_out = buf;
        return false;
    }
    return true;
}

ProcessRunResult RunProcessNoCaptureWithPoll(const std::wstring& command_line, DWORD timeout_ms, bool hide_window,
                                             const std::function<void()>& on_poll) {
    ProcessRunResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE nul_write = INVALID_HANDLE_VALUE;
    nul_write = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul_write == INVALID_HANDLE_VALUE) {
        return result;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = hide_window ? SW_HIDE : SW_SHOWNORMAL;
    si.hStdOutput = nul_write;
    si.hStdError = nul_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmdline = command_line;

    DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT;
    if (hide_window) {
        creation_flags |= CREATE_NO_WINDOW;
    }

    const BOOL created =
        CreateProcessW(nullptr, mutable_cmdline.data(), nullptr, nullptr, TRUE, creation_flags, nullptr, nullptr, &si,
                       &pi);

    CloseHandle(nul_write);

    if (!created) {
        return result;
    }

    result.started = true;
    CloseHandle(pi.hThread);

    const auto start_time = std::chrono::steady_clock::now();

    for (;;) {
        DWORD child_exit = STILL_ACTIVE;
        GetExitCodeProcess(pi.hProcess, &child_exit);
        if (child_exit != STILL_ACTIVE) {
            break;
        }

        DWORD slice_ms = 250;
        if (timeout_ms != 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start_time)
                                     .count();
            if (elapsed >= static_cast<long long>(timeout_ms)) {
                result.timed_out = true;
                TerminateProcess(pi.hProcess, static_cast<UINT>(-1));
                WaitForSingleObject(pi.hProcess, 10000);
                break;
            }
            const auto remaining = static_cast<DWORD>(timeout_ms - elapsed);
            slice_ms = std::min<DWORD>(250, remaining);
        }

        if (on_poll) {
            on_poll();
        }

        WaitForSingleObject(pi.hProcess, slice_ms);
    }

    DWORD exit_code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);

    result.exit_code = exit_code;
    return result;
}

CommandResult RunCommandWithStreamingOutput(const std::wstring& command_line, const std::wstring* working_directory,
                                            DWORD timeout_ms, bool hide_window,
                                            const std::function<void(std::string_view chunk)>& on_chunk) {
    CommandResult result;
    std::string combined_output;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        result.error_message = "CreatePipe (stdout) failed";
        return result;
    }

    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = hide_window ? SW_HIDE : SW_SHOWNORMAL;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmdline = command_line;

    DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT;
    if (hide_window) {
        creation_flags |= CREATE_NO_WINDOW;
    }

    BOOL created =
        CreateProcessW(nullptr, mutable_cmdline.data(), nullptr, nullptr, TRUE, creation_flags, nullptr,
                       working_directory ? working_directory->c_str() : nullptr, &si, &pi);

    CloseHandle(stdout_write);

    if (!created) {
        CloseHandle(stdout_read);
        result.error_message = "CreateProcessW failed";
        return result;
    }

    result.started = true;
    CloseHandle(pi.hThread);

    char buffer[4096];

    auto drain_pipe = [&]() {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &avail, nullptr)) {
                break;
            }
            if (avail == 0) {
                break;
            }
            DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buffer)));
            DWORD read_bytes = 0;
            if (!ReadFile(stdout_read, buffer, to_read, &read_bytes, nullptr) || read_bytes == 0) {
                break;
            }
            const std::string_view sv(buffer, read_bytes);
            combined_output.append(sv.data(), sv.size());
            if (on_chunk) {
                on_chunk(sv);
            }
        }
    };

    const auto start_time = std::chrono::steady_clock::now();

    for (;;) {
        DWORD child_exit = STILL_ACTIVE;
        GetExitCodeProcess(pi.hProcess, &child_exit);
        if (child_exit != STILL_ACTIVE) {
            break;
        }

        DWORD slice_ms = 250;
        if (timeout_ms != 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start_time)
                                     .count();
            if (elapsed >= static_cast<long long>(timeout_ms)) {
                result.timed_out = true;
                TerminateProcess(pi.hProcess, static_cast<UINT>(-1));
                WaitForSingleObject(pi.hProcess, 10000);
                break;
            }
            const auto remaining = static_cast<DWORD>(timeout_ms - elapsed);
            slice_ms = std::min<DWORD>(250, remaining);
        }

        WaitForSingleObject(pi.hProcess, slice_ms);
        drain_pipe();
    }

    drain_pipe();
    DWORD read_bytes = 0;
    while (ReadFile(stdout_read, buffer, sizeof(buffer), &read_bytes, nullptr) && read_bytes > 0) {
        const std::string_view sv(buffer, read_bytes);
        combined_output.append(sv.data(), sv.size());
        if (on_chunk) {
            on_chunk(sv);
        }
    }

    CloseHandle(stdout_read);

    DWORD exit_code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);

    result.exit_code = exit_code;
    result.stdout_text = std::move(combined_output);
    result.stderr_text.clear();
    return result;
}

} // namespace kds::proc
