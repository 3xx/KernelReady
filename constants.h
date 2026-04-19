// constants.h - Central configuration, package IDs, paths, and exit codes for KernelDevSetup.
#pragma once

#include <cstdint>
#include <string>

namespace kds {

// ---------------------------------------------------------------------------
// Exit codes (process return value). Documented for automation callers.
// ---------------------------------------------------------------------------
enum class ExitCode : int {
    Success = 0,
    UnsupportedOs = 1,
    UnsupportedArchitecture = 2,
    ElevationFailed = 3,
    WingetUnavailable = 4,
    VisualStudioInstallFailed = 5,
    SdkInstallFailed = 6,
    WdkInstallFailed = 7,
    VerificationFailed = 8,
    PartialSuccess = 9,
    UserCancelled = 10,
    NetworkOrTransientFailure = 11,
    RebootRequired = 12,
    InvalidArguments = 13,
    InternalError = 99
};

// ---------------------------------------------------------------------------
// Runtime options (set from command line).
// ---------------------------------------------------------------------------
struct RunOptions {
    bool dry_run = false;
    bool repair = false;
    bool force_reinstall = false;
    bool quiet = false;
    bool verify_only = false;
    /// When true, do not wait for a key before exit (for scripts / piped runs).
    bool no_pause = false;
};

// ---------------------------------------------------------------------------
// Visual Studio 2022 Professional — official bootstrapper (direct download; not winget).
// ---------------------------------------------------------------------------
inline constexpr const wchar_t* kVsProfessionalBootstrapperUrl =
    L"https://aka.ms/vs/17/release/vs_professional.exe";

// ---------------------------------------------------------------------------
// Official winget package identifiers for SDK/WDK (optional; VS uses bootstrapper URL above).
// ---------------------------------------------------------------------------
inline constexpr const char* kWingetIdWindowsSdk = "Microsoft.WindowsSDK.10.0.26100";
inline constexpr const char* kWingetIdWindowsWdk = "Microsoft.WindowsWDK.10.0.26100";

// ---------------------------------------------------------------------------
// Visual Studio / WDK version thresholds.
// ---------------------------------------------------------------------------
inline constexpr int kVsMajorMin = 17;
inline constexpr int kVsMinorWdkVsixCutoff = 11; // Below 17.11 may need separate WDK VSIX.

// ---------------------------------------------------------------------------
// Timeouts and retries (installers can run for a long time).
// ---------------------------------------------------------------------------
inline constexpr int kDefaultCommandTimeoutSec = 7200; // 2 hours for VS bootstrapper.
inline constexpr int kShortCommandTimeoutSec = 600;
inline constexpr int kMaxRetries = 3;
inline constexpr int kRetryBaseDelayMs = 2000;

// ---------------------------------------------------------------------------
// Known folder names under %LOCALAPPDATA% and log file naming.
// ---------------------------------------------------------------------------
inline constexpr const wchar_t* kAppDataFolderName = L"KernelDevSetup";
inline constexpr const wchar_t* kLogsSubfolder = L"Logs";
inline constexpr const char* kVsConfigFileName = "kernel_driver_dev.vsconfig";

// ---------------------------------------------------------------------------
// vswhere default path (64-bit OS).
// ---------------------------------------------------------------------------
inline constexpr const wchar_t* kVsWhereRelativePath =
    L"Microsoft Visual Studio\\Installer\\vswhere.exe";

// ---------------------------------------------------------------------------
// WDK VSIX search paths (relative to Program Files (x86)) when VS < 17.11.
// Multiple entries for different WDK releases; first match wins.
// ---------------------------------------------------------------------------
inline constexpr const wchar_t* kWdkVsixSearchRelativePaths[] = {
    L"Windows Kits\\10\\Vsix\\VS2022\\WDK.vsix",
    L"Windows Kits\\10\\Vsix\\WDK.vsix"
};

// ---------------------------------------------------------------------------
// Post-install verification: substrings/paths under Windows Kits.
// ---------------------------------------------------------------------------
inline constexpr const wchar_t* kWindowsKitsRootRelative = L"Windows Kits\\10";
inline constexpr const wchar_t* kWdkBinRelative =
    L"Windows Kits\\10\\bin"; // Versioned subfolders expected under bin.

// ---------------------------------------------------------------------------
// Visual Studio .vsconfig component IDs (Workload + common driver tooling).
// Adjust here when Microsoft publishes new component IDs for newer SDKs.
// ---------------------------------------------------------------------------
// Workload: Desktop development with C++
inline constexpr const char* kVsComponentWorkloadNativeDesktop =
    "Microsoft.VisualStudio.Workload.NativeDesktop";
// MSVC v143 toolset (latest channel in VS 2022)
inline constexpr const char* kVsComponentVcToolsX86X64 =
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64";
// Spectre-mitigated libs for x64/x86
inline constexpr const char* kVsComponentSpectreLibs =
    "Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre";
// ATL for x64/x86
inline constexpr const char* kVsComponentAtl =
    "Microsoft.VisualStudio.Component.VC.ATL";
// MFC for x64/x86 (optional compatibility for samples)
inline constexpr const char* kVsComponentMfc =
    "Microsoft.VisualStudio.Component.VC.ATLMFC";
// Windows 11 SDK 10.0.26100 (matches winget SDK 26100)
inline constexpr const char* kVsComponentWin11Sdk26100 =
    "Microsoft.VisualStudio.Component.Windows11SDK.26100";
// Windows Driver Kit Visual Studio extension (when available in installer catalog)
inline constexpr const char* kVsComponentWindowsDriverKit =
    "Microsoft.VisualStudio.Component.Windows.DriverKit";

// JSON schema version for generated .vsconfig
inline constexpr const char* kVsConfigVersion = "1.0";

// Build a newline-separated list of component IDs for documentation/logging.
std::string BuildVsConfigComponentListForLog();

} // namespace kds
