// package_manager.h - winget discovery, availability checks, and install wrappers.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace kds::ui {
class IProgressSink;
}

namespace kds::pkg {

struct WingetInfo {
    std::wstring executable_path;
    std::string version_stdout;
    bool usable = false;
};

// Resolves winget.exe and validates with "winget --version".
WingetInfo DetectWinget();

// Plain-text steps when winget is missing or broken (for logs / UI).
std::string GetWingetRecoveryHints();

enum class InstallOutcome { Success, Failed, Skipped };

struct InstallResult {
    InstallOutcome outcome = InstallOutcome::Failed;
    std::string detail;
    std::uint32_t last_exit_code = 0;
};

// winget install with official package id. Uses silent flags where supported.
// When progress_ui is non-null, output is streamed for best-effort percentage detection; otherwise a single capture.
InstallResult WingetInstallPackage(const char* winget_id, bool force, const std::wstring* override_arguments,
                                   std::uint32_t timeout_ms, kds::ui::IProgressSink* progress_ui = nullptr);

// Returns true if winget reports the package as installed (best-effort text/json parsing).
bool WingetIsPackageInstalled(const char* winget_id);

} // namespace kds::pkg
