// installer.cpp
#include "installer.h"

#include <Windows.h>

#include "compatibility.h"
#include "constants.h"
#include "logging.h"
#include "os_detect.h"
#include "package_manager.h"
#include "process_utils.h"
#include "progress_ui.h"
#include "verification.h"
#include "vs_detect.h"

#include <ShlObj.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <vector>

#pragma comment(lib, "Shell32.lib")

namespace kds::install {

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string BuildVsConfigJson() {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"version\": \"" << kVsConfigVersion << "\",\n";
    oss << "  \"components\": [\n";
    oss << "    \"" << kVsComponentWorkloadNativeDesktop << "\",\n";
    oss << "    \"" << kVsComponentVcToolsX86X64 << "\",\n";
    oss << "    \"" << kVsComponentSpectreLibs << "\",\n";
    oss << "    \"" << kVsComponentAtl << "\",\n";
    oss << "    \"" << kVsComponentMfc << "\",\n";
    oss << "    \"" << kVsComponentWin11Sdk26100 << "\",\n";
    oss << "    \"" << kVsComponentWindowsDriverKit << "\"\n";
    oss << "  ]\n";
    oss << "}\n";
    return oss.str();
}

std::wstring BuildVsBootstrapperArgs(const std::wstring& vsconfig_path, bool prefer_repair) {
    // Passed to vs_professional.exe (official bootstrapper).
    std::wstring o = L"--quiet --wait --norestart ";
    if (prefer_repair) {
        o += L"--repair ";
    }
    o += L"--config \"";
    o += vsconfig_path;
    o += L"\"";
    return o;
}

void FileTimeSubtractSeconds(FILETIME& ft, ULONGLONG seconds) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= seconds * 10000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
}

std::optional<int> ParseLastPercentFromUtf8Log(std::string_view text) {
    try {
        static const std::regex re(R"((\d{1,3})\s*%)");
        std::optional<int> last;
        auto begin = std::cregex_iterator(text.data(), text.data() + text.size(), re);
        const auto end = std::cregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const int v = std::stoi((*it)[1].str());
            if (v <= 100) {
                last = v;
            }
        }
        return last;
    } catch (...) {
        return std::nullopt;
    }
}

// Best-effort: Visual Studio setup writes dd_*.log under %TEMP%. We only trust logs touched after install_window_start.
std::optional<int> PollVisualStudioInstallerPercent(const FILETIME& install_window_start) {
    wchar_t temp_dir[MAX_PATH] = {};
    if (GetTempPathW(static_cast<DWORD>(std::size(temp_dir)), temp_dir) == 0) {
        return std::nullopt;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path newest_path;
    FILETIME newest_time{};
    bool have = false;

    for (const auto& entry : fs::directory_iterator(fs::path(temp_dir), ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::wstring name = entry.path().filename().wstring();
        if (name.size() < 7 || name.rfind(L"dd_", 0) != 0) {
            continue;
        }
        if (name.find(L".log") == std::wstring::npos) {
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        const std::wstring p = entry.path().wstring();
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) {
            continue;
        }
        if (CompareFileTime(&fad.ftLastWriteTime, &install_window_start) < 0) {
            continue;
        }
        if (!have || CompareFileTime(&fad.ftLastWriteTime, &newest_time) > 0) {
            newest_time = fad.ftLastWriteTime;
            newest_path = entry.path();
            have = true;
        }
    }

    if (!have) {
        return std::nullopt;
    }

    std::ifstream f(newest_path, std::ios::binary | std::ios::ate);
    if (!f) {
        return std::nullopt;
    }
    const auto sz = f.tellg();
    if (sz <= 0) {
        return std::nullopt;
    }
    const auto read_sz = std::min<std::streamoff>(sz, static_cast<std::streamoff>(262144));
    f.seekg(sz - read_sz);
    std::string chunk(static_cast<size_t>(read_sz), '\0');
    f.read(chunk.data(), read_sz);
    return ParseLastPercentFromUtf8Log(chunk);
}

// Downloads vs_professional.exe and runs it with the generated .vsconfig (no winget).
bool RunVisualStudioProfessionalBootstrapper(const std::wstring& vsconfig_path, bool prefer_repair,
                                              kds::ui::IProgressSink* sink) {
    wchar_t temp_dir[MAX_PATH] = {};
    if (GetTempPathW(static_cast<DWORD>(std::size(temp_dir)), temp_dir) == 0) {
        KDS_LOG(Error, "GetTempPathW failed; cannot place bootstrapper.");
        return false;
    }
    const std::wstring dest = std::wstring(temp_dir) + L"kds_vs_professional.exe";

    if (sink) {
        sink->SetPhase(L"Visual Studio 2022 Professional",
                       L"Downloading official bootstrapper (vs_professional.exe) — byte progress when the server "
                       L"reports size.");
    }
    KDS_LOG(Info, "Downloading Visual Studio 2022 Professional bootstrapper from Microsoft (official URL)...");

    std::string dl_err;
    const bool dl_ok =
        sink
            ? proc::DownloadUrlToFileWithProgress(
                  kVsProfessionalBootstrapperUrl, dest,
                  [sink](std::uint64_t cur, std::uint64_t total) {
                      if (sink) {
                          sink->SetDownloadBytes(cur, total);
                      }
                  },
                  dl_err)
            : proc::DownloadUrlToFile(kVsProfessionalBootstrapperUrl, dest, dl_err);
    if (!dl_ok) {
        KDS_LOG(Error, dl_err.c_str());
        if (sink) {
            sink->SetStepComplete(L"Visual Studio 2022 Professional", false, false);
        }
        return false;
    }

    FILETIME install_window_start{};
    GetSystemTimeAsFileTime(&install_window_start);
    FileTimeSubtractSeconds(install_window_start, 90);

    const std::wstring args = BuildVsBootstrapperArgs(vsconfig_path, prefer_repair);
    const std::wstring cmd = L"\"" + dest + L"\" " + args;
    if (sink) {
        sink->SetPhase(L"Visual Studio 2022 Professional",
                       L"Running Visual Studio Installer — percentage is read from dd_*.log when present (otherwise "
                       L"indeterminate).");
        sink->SetInstallIndeterminate(L"Visual Studio is applying your .vsconfig (silent). This may take a long time.");
    }
    KDS_LOG(Info, "Starting Visual Studio installer (silent, .vsconfig-driven). This may take a long time.");

    const auto pr = proc::RunProcessNoCaptureWithPoll(
        cmd, kDefaultCommandTimeoutSec * 1000, true,
        sink ? std::function<void()>{[sink, install_window_start]() {
                 if (const auto pct = PollVisualStudioInstallerPercent(install_window_start)) {
                     sink->SetInstallPercent(*pct);
                 }
             }}
             : std::function<void()>{});

    DeleteFileW(dest.c_str());

    if (!pr.started) {
        KDS_LOG(Error, "Failed to start the Visual Studio bootstrapper process.");
        if (sink) {
            sink->SetStepComplete(L"Visual Studio 2022 Professional", false, false);
        }
        return false;
    }
    if (pr.timed_out) {
        KDS_LOG(Error, "Visual Studio installer timed out.");
        if (sink) {
            sink->SetStepComplete(L"Visual Studio 2022 Professional", false, false);
        }
        return false;
    }

    // 3010 = ERROR_SUCCESS_REBOOT_REQUIRED (common for VS / MSI chains).
    if (pr.exit_code != 0 && pr.exit_code != 3010) {
        KDS_LOG(Error, ("Visual Studio installer failed. Exit code: " + std::to_string(pr.exit_code)).c_str());
        if (sink) {
            sink->SetStepComplete(L"Visual Studio 2022 Professional", false, false);
        }
        return false;
    }
    if (pr.exit_code == 3010) {
        KDS_LOG(Warning, "Visual Studio installer reported success with reboot pending (exit code 3010).");
    } else {
        KDS_LOG(Info, "Visual Studio installer process completed successfully.");
    }
    if (sink) {
        sink->SetInstallPercent(std::nullopt);
        sink->SetStepComplete(L"Visual Studio 2022 Professional", false, true);
    }
    return true;
}

std::wstring ResolveVsixInstallerNearVs(const std::wstring& vs_install_path) {
    if (vs_install_path.empty()) {
        return {};
    }
    try {
        std::filesystem::path p =
            std::filesystem::path(vs_install_path) / L"Common7" / L"IDE" / L"VSIXInstaller.exe";
        if (std::filesystem::exists(p)) {
            return p.wstring();
        }
    } catch (...) {
    }
    return {};
}

bool InstallWdkVsixIfPossible(const vs::VsInstallation& vs, bool dry_run, kds::ui::IProgressSink* sink) {
    if (dry_run) {
        KDS_LOG(Info, "Dry-run: would attempt WDK VSIX installation if needed.");
        return true;
    }
    if (sink) {
        sink->SetPhase(L"WDK Visual Studio integration", L"Installing WDK VSIX when required...");
        sink->SetInstallIndeterminate(L"VSIXInstaller.exe — no reliable percentage; waiting for completion.");
    }
    std::wstring vsix = vs::FindWdkVsixPath();
    if (vsix.empty()) {
        KDS_LOG(Warning,
                "WDK VSIX not found under default paths. Install WDK from winget or mount the WDK ISO.");
        return false;
    }
    std::wstring vsix_installer = ResolveVsixInstallerNearVs(vs.installation_path);
    if (vsix_installer.empty()) {
        KDS_LOG(Warning, "VSIXInstaller.exe not found next to the Visual Studio installation.");
        KDS_LOG(Warning, "Manual: install the WDK VSIX from Visual Studio or run VSIXInstaller.exe on the WDK VSIX.");
        return false;
    }

    std::wstring cmd = L"\"" + vsix_installer + L"\" /quiet /admin \"" + vsix + L"\"";
    auto res = proc::RunCommand(cmd, nullptr, kDefaultCommandTimeoutSec * 1000, true);
    if (!res.started) {
        KDS_LOG(Error, "Failed to launch VSIXInstaller.exe.");
        return false;
    }
    if (res.exit_code != 0) {
        KDS_LOG(Warning, ("VSIXInstaller returned non-zero exit code: " + std::to_string(res.exit_code)).c_str());
        KDS_LOG(Info, res.stdout_text.c_str());
        if (sink) {
            sink->SetStepComplete(L"WDK VSIX", false, false);
        }
        return false;
    }
    KDS_LOG(Info, "WDK VSIX installation completed (exit code 0).");
    if (sink) {
        sink->SetInstallPercent(std::nullopt);
        sink->SetStepComplete(L"WDK VSIX", false, true);
    }
    return true;
}

bool EnsureLegacyKernelToolsetShim(const std::wstring& vs_install_path) {
    if (vs_install_path.empty()) {
        return false;
    }
    try {
        namespace fs = std::filesystem;
        const fs::path platforms =
            fs::path(vs_install_path) / L"MSBuild" / L"Microsoft" / L"VC" / L"v170" / L"Platforms";
        const std::vector<std::wstring> plats = {L"x64", L"ARM64", L"Win32"};

        bool all_ok = true;
        bool created_any = false;
        for (const auto& p : plats) {
            const fs::path legacy_dir = platforms / p / L"PlatformToolsets" / L"WindowsKernelModeDriver10.0";
            const fs::path legacy_props = legacy_dir / L"Toolset.props";
            const fs::path legacy_targets = legacy_dir / L"Toolset.targets";
            if (fs::exists(legacy_props) && fs::exists(legacy_targets)) {
                continue;
            }
            const fs::path v143_dir = platforms / p / L"PlatformToolsets" / L"v143";
            const fs::path v143_props = v143_dir / L"Toolset.props";
            const fs::path v143_targets = v143_dir / L"Toolset.targets";
            if (!fs::exists(v143_props) || !fs::exists(v143_targets)) {
                all_ok = false;
                continue;
            }
            fs::create_directories(legacy_dir);
            {
                std::ofstream f(legacy_props, std::ios::binary | std::ios::trunc);
                f << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
                f << "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n";
                f << "  <Import Project=\"$(VCTargetsPath)\\Platforms\\$(Platform)\\PlatformToolsets\\v143\\Toolset.props\" />\n";
                f << "</Project>\n";
            }
            {
                std::ofstream f(legacy_targets, std::ios::binary | std::ios::trunc);
                f << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
                f << "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n";
                f << "  <Import Project=\"$(VCTargetsPath)\\Platforms\\$(Platform)\\PlatformToolsets\\v143\\Toolset.targets\" />\n";
                f << "</Project>\n";
            }
            created_any = true;
        }
        if (created_any) {
            KDS_LOG(Info, "Created legacy WindowsKernelModeDriver10.0 toolset shim (v143-backed) for compatibility.");
        }
        return all_ok;
    } catch (...) {
        return false;
    }
}

void WriteSummaryFiles(const std::string& json_text, const std::string& text_summary) {
    wchar_t local[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local) != S_OK) {
        return;
    }
    std::filesystem::path dir = std::filesystem::path(local) / kAppDataFolderName;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return;
    }
    const auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    char stamp[64] = {};
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_buf);
    try {
        std::ofstream j(dir / (std::string("summary_") + stamp + ".json"));
        j << json_text;
        std::ofstream ttxt(dir / (std::string("summary_") + stamp + ".txt"));
        ttxt << text_summary;
    } catch (...) {
    }
}

} // namespace

ExitCode RunBody(const RunOptions& options, kds::ui::IProgressSink* ui) {
    KDS_LOG(Info, "KernelDevSetup starting.");
    if (ui) {
        ui->SetPhase(L"KernelDevSetup", L"Detecting operating system...");
    }

    os::OsInfo os_info{};
    if (!os::DetectOs(os_info)) {
        KDS_LOG(Error, "OS detection failed.");
        return ExitCode::InternalError;
    }

    KDS_LOG(Info, ("Product: " + os_info.product_name).c_str());
    KDS_LOG(Info,
            ("Build: " + std::to_string(os_info.major) + "." + std::to_string(os_info.minor) + "." +
             std::to_string(os_info.build) + "." + std::to_string(os_info.ubr))
                .c_str());

    if (os_info.arch == os::Architecture::X86) {
        KDS_LOG(Error, "32-bit Windows is not supported as a host for Visual Studio 2022.");
        return ExitCode::UnsupportedArchitecture;
    }

    if (!os::EvaluateVisualStudio2022Support(os_info)) {
        KDS_LOG(Error, os_info.vs2022_support_detail.c_str());
        return ExitCode::UnsupportedOs;
    }

    if (options.verify_only) {
        KDS_LOG(Info, "Verify-only mode: skipping installation steps.");
        if (ui) {
            ui->SetPhase(L"Verify only", L"No installs — running environment checks.");
        }
        auto winget_v = ::kds::pkg::DetectWinget();
        const auto plan_v = compat::BuildPlan(os_info, options, winget_v);
        compat::LogPlan(plan_v);
        auto report = verify::RunVerification(ui);
        const std::string json = verify::ReportToJson(report);
        const std::string txt = verify::ReportToText(report);
        KDS_LOG(Info, txt.c_str());
        WriteSummaryFiles(json, txt);
        return report.all_critical_passed ? ExitCode::Success : ExitCode::VerificationFailed;
    }

    if (ui) {
        ui->SetPhase(L"Bootstrap", L"Detecting Windows Package Manager (winget) and preparing configuration...");
    }

    // winget presence (informational if missing; installs may fail gracefully).
    auto winget = ::kds::pkg::DetectWinget();
    if (!winget.usable) {
        KDS_LOG(Warning,
                "winget is not available. Automatic installation will be limited. Install App Installer / winget.");
        KDS_LOG(Warning, ::kds::pkg::GetWingetRecoveryHints().c_str());
    } else {
        KDS_LOG(Info, ("winget detected: " + WideToUtf8(winget.executable_path)).c_str());
    }

    const auto plan = compat::BuildPlan(os_info, options, winget);
    compat::LogPlan(plan);

    if (plan.vs_action == compat::RemediationAction::Blocked) {
        KDS_LOG(Error, plan.vs_decision_rationale.c_str());
        return ExitCode::UnsupportedOs;
    }

    const bool can_winget_sdk_wdk = winget.usable && !options.dry_run;

    // Build VS configuration file.
    const std::string vsconfig_body = BuildVsConfigJson();
    KDS_LOG(Info, "Generated .vsconfig component list (see log).");
    KDS_LOG(Info, BuildVsConfigComponentListForLog().c_str());

    std::optional<std::wstring> vsconfig_path = proc::WriteTempUtf8File(L"kds", vsconfig_body);
    if (!vsconfig_path.has_value()) {
        KDS_LOG(Error, "Failed to write temporary .vsconfig file.");
        return ExitCode::InternalError;
    }

    ExitCode stage = ExitCode::Success;

    const bool need_vs_bootstrapper = plan.run_vs_bootstrapper;
    const bool vs_repair = plan.prefer_repair_with_bootstrapper;

    if (options.dry_run) {
        KDS_LOG(Info, "Dry-run: planned actions logged; no installers executed.");
        if (ui) {
            ui->SetPhase(L"Dry run", L"Reviewing planned actions (no installers will run).");
            ui->SetInstallIndeterminate(L"Planning only — no download or install activity.");
        }
        if (need_vs_bootstrapper) {
            KDS_LOG(Info, "Would download vs_professional.exe from Microsoft and run the bootstrapper with .vsconfig.");
            if (vs_repair) {
                KDS_LOG(Info, "Repair mode: would pass --repair to the bootstrapper when applicable.");
            }
            KDS_LOG(Info, (std::string("URL: ") + WideToUtf8(std::wstring(kVsProfessionalBootstrapperUrl))).c_str());
        } else {
            KDS_LOG(Info, "Would skip Visual Studio bootstrapper (reuse compatible installation per compatibility plan).");
        }
        if (plan.install_sdk_via_winget) {
            KDS_LOG(Info, "Would install/ensure Windows SDK 10.0.26100 via winget when available.");
        }
        if (plan.install_wdk_via_winget) {
            KDS_LOG(Info, "Would install/ensure Windows WDK 10.0.26100 via winget when available.");
        }
        if (ui) {
            ui->SetStepComplete(L"Dry run plan", false, true);
        }
    } else {
        // Visual Studio: official bootstrapper only (no winget).
        if (need_vs_bootstrapper) {
            if (!RunVisualStudioProfessionalBootstrapper(*vsconfig_path, vs_repair, ui)) {
                stage = ExitCode::VisualStudioInstallFailed;
            }
        } else {
            KDS_LOG(Info, "Skipping Visual Studio bootstrapper (compatibility plan: reuse existing installation).");
            if (ui) {
                ui->SetPhase(L"Visual Studio 2022 Professional",
                             L"Compatible installation reused — skipping bootstrapper.");
                ui->SetInstallPercent(std::nullopt);
                ui->SetStepComplete(L"Visual Studio 2022 Professional", true, true);
            }
        }

        // Windows SDK / WDK: winget when plan selects install/ensure.
        if (can_winget_sdk_wdk) {
            if (plan.install_sdk_via_winget) {
                KDS_LOG(Info, "Installing Windows SDK package via winget (compatibility plan).");
                auto r2 = ::kds::pkg::WingetInstallPackage(kWingetIdWindowsSdk, options.force_reinstall, nullptr,
                                                           kDefaultCommandTimeoutSec * 1000, ui);
                if (r2.outcome != pkg::InstallOutcome::Success) {
                    KDS_LOG(Warning, "Windows SDK winget install did not succeed; SDK may still ship with VS.");
                    KDS_LOG(Warning, r2.detail.c_str());
                    if (stage == ExitCode::Success) {
                        stage = ExitCode::SdkInstallFailed;
                    }
                }
            } else {
                KDS_LOG(Info, "Skipping Windows SDK winget install (already satisfied or not required).");
                if (ui) {
                    ui->SetPhase(L"Windows SDK (winget)", L"Already satisfied — skipping package install.");
                    ui->SetStepComplete(L"Windows SDK (winget)", true, true);
                }
            }

            if (plan.install_wdk_via_winget) {
                KDS_LOG(Info, "Installing Windows WDK package via winget (compatibility plan).");
                auto r3 = ::kds::pkg::WingetInstallPackage(kWingetIdWindowsWdk, options.force_reinstall, nullptr,
                                                           kDefaultCommandTimeoutSec * 1000, ui);
                if (r3.outcome != pkg::InstallOutcome::Success) {
                    KDS_LOG(Warning, "WDK winget install did not succeed.");
                    KDS_LOG(Warning, r3.detail.c_str());
                    if (stage == ExitCode::Success) {
                        stage = ExitCode::WdkInstallFailed;
                    }
                }
            } else {
                KDS_LOG(Info, "Skipping Windows WDK winget install (already satisfied or not required).");
                if (ui) {
                    ui->SetPhase(L"Windows WDK (winget)", L"Already satisfied — skipping package install.");
                    ui->SetStepComplete(L"Windows WDK (winget)", true, true);
                }
            }
        } else {
            KDS_LOG(Warning,
                    "winget is not available; skipping automated Windows SDK / WDK installs. Install App Installer or "
                    "add SDK/WDK manually.");
        }
    }

    // Refresh VS detection after installs.
    auto vs_after = vs::QueryVs2022Professional();
    if (vs_after.has_value()) {
        if (!vs::HasKernelModeDriverToolset(vs_after->installation_path) && !options.dry_run) {
            KDS_LOG(Warning,
                    "Kernel Mode Driver toolset is missing from Visual Studio. Starting forced WDK/VS integration repair.");
            if (winget.usable) {
                auto repair_wdk = ::kds::pkg::WingetInstallPackage(kWingetIdWindowsWdk, true, nullptr,
                                                                   kDefaultCommandTimeoutSec * 1000, ui);
                if (repair_wdk.outcome != pkg::InstallOutcome::Success) {
                    KDS_LOG(Warning, "Forced WDK repair via winget did not succeed.");
                    KDS_LOG(Warning, repair_wdk.detail.c_str());
                }
            } else {
                KDS_LOG(Warning, "winget unavailable; cannot run forced WDK repair package install.");
            }
            // --repair + --config is not consistently accepted by all VS bootstrapper versions.
            // Use config-driven modify pass here for broader compatibility.
            if (!RunVisualStudioProfessionalBootstrapper(*vsconfig_path, false, ui)) {
                KDS_LOG(Warning, "Visual Studio repair pass failed while trying to restore kernel toolset integration.");
                if (stage == ExitCode::Success) {
                    stage = ExitCode::VisualStudioInstallFailed;
                }
            }
            vs_after = vs::QueryVs2022Professional();
        }
        if (!vs::HasKernelModeDriverToolset(vs_after->installation_path) && !options.dry_run) {
            if (!EnsureLegacyKernelToolsetShim(vs_after->installation_path)) {
                KDS_LOG(Warning, "Failed to create legacy WindowsKernelModeDriver10.0 shim files.");
            }
        }
        if (vs::NeedsWdkVsixSilently(*vs_after)) {
            KDS_LOG(Warning,
                    "Visual Studio is below 17.11; attempting silent WDK VSIX install when available.");
            InstallWdkVsixIfPossible(*vs_after, options.dry_run, ui);
        } else {
            KDS_LOG(Info, "Visual Studio is 17.11+; separate WDK VSIX step typically not required.");
        }
    }

    if (ui) {
        ui->SetPhase(L"Verification", L"Validating toolchain, SDK, and WDK...");
        ui->SetInstallIndeterminate(L"Running verification — no single percentage for this phase.");
    }
    auto report = verify::RunVerification(ui);
    const std::string json = verify::ReportToJson(report);
    const std::string txt = verify::ReportToText(report);
    KDS_LOG(Info, txt.c_str());
    WriteSummaryFiles(json, txt);

    if (!report.all_critical_passed) {
        if (stage == ExitCode::Success) {
            return ExitCode::VerificationFailed;
        }
        return ExitCode::PartialSuccess;
    }

    if (stage != ExitCode::Success) {
        return ExitCode::PartialSuccess;
    }
    return ExitCode::Success;
}

ExitCode Run(const RunOptions& options) {
    if (options.quiet) {
        return RunBody(options, nullptr);
    }
    kds::ui::ProgressWindow window;
    ExitCode code = ExitCode::InternalError;
    kds::ui::RunWithProgressPump(window, [&](kds::ui::IProgressSink& sink) { code = RunBody(options, &sink); });
    return code;
}

} // namespace kds::install
