// process_utils.h - Elevation, process execution with I/O capture, temp files.
#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <Windows.h>

namespace kds::proc {

struct CommandResult {
    DWORD exit_code = static_cast<DWORD>(-1);
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out = false;
    bool started = false;
    std::string error_message;
};

// Returns true if the current process token is elevated (UAC) and user is in Administrators.
bool IsProcessElevatedAdmin();

// Relaunches the current executable with "runas". Returns true if ShellExecute succeeded.
// The caller should exit immediately after a successful return.
bool RelaunchSelfElevated(const std::wstring& args);

// Runs a command line. Uses CreateProcessW; optionally sets working directory.
// Captures stdout and stderr separately. Enforces timeout (0 = infinite).
CommandResult RunCommand(const std::wstring& command_line,
                         const std::wstring* working_directory,
                         DWORD timeout_ms,
                         bool hide_window);

// Writes UTF-8 content to a new temp file. Returns full path on success.
// Note: GetTempFileNameW only uses the first three characters of the prefix.
std::optional<std::wstring> WriteTempUtf8File(const std::wstring& prefix_three_chars,
                                              const std::string& utf8_content);

// Returns the path to the current executable.
std::wstring GetModulePath();

// Expands environment strings like %ProgramFiles%
std::wstring ExpandEnvironmentStringsWrapper(const std::wstring& input);

// Downloads a URL to a local file (HTTPS). Uses URLDownloadToFile (WinINet/URL Moniker).
bool DownloadUrlToFile(const std::wstring& url, const std::wstring& dest_path, std::string& error_out);

// Same as DownloadUrlToFile but reports byte progress (total may be 0 when length is unknown).
bool DownloadUrlToFileWithProgress(const std::wstring& url, const std::wstring& dest_path,
                                   const std::function<void(std::uint64_t downloaded, std::uint64_t total)>& on_progress,
                                   std::string& error_out);

struct ProcessRunResult {
    DWORD exit_code = static_cast<DWORD>(-1);
    bool started = false;
    bool timed_out = false;
};

// Runs a process without capturing stdout/stderr (discarded to NUL). Invokes on_poll periodically while running.
ProcessRunResult RunProcessNoCaptureWithPoll(const std::wstring& command_line, DWORD timeout_ms, bool hide_window,
                                            const std::function<void()>& on_poll);

// Like RunCommand but streams merged stdout+stderr to on_chunk as data arrives (for progress parsing).
CommandResult RunCommandWithStreamingOutput(const std::wstring& command_line, const std::wstring* working_directory,
                                            DWORD timeout_ms, bool hide_window,
                                            const std::function<void(std::string_view chunk)>& on_chunk);

// Simple retry wrapper for transient failures (caller supplies lambda returning bool success).
template <typename Fn>
bool WithRetries(int max_attempts, int base_delay_ms, Fn&& fn) {
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (fn(attempt)) {
            return true;
        }
        if (attempt < max_attempts) {
            Sleep(static_cast<DWORD>(base_delay_ms * attempt));
        }
    }
    return false;
}

} // namespace kds::proc
