// package_manager.cpp
#include "package_manager.h"

#include "constants.h"
#include "logging.h"
#include "process_utils.h"
#include "progress_ui.h"

#include <ShlObj.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <vector>

#pragma comment(lib, "Shell32.lib")

namespace kds::pkg {

namespace {

std::wstring Utf8ToWide(std::string_view u8) {
    if (u8.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), out.data(), n);
    return out;
}

bool FileExists(const std::wstring& p) {
    return !p.empty() && std::filesystem::exists(std::filesystem::path(p));
}

// winget often returns non-zero when the package is already current; treat as success for automation.
bool IsWingetInstallAlreadySatisfied(std::string_view combined) {
    std::string lower(combined);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    static constexpr const char* kPhrases[] = {
        "already installed",
        "an existing package was found",
        "existing package already installed",
        "no available upgrade found",
        "no newer package versions are available",
        "no applicable upgrade found",
        "no applicable installer",
        "trying to upgrade the installed package",
        "a newer version was not found",
    };
    for (const char* p : kPhrases) {
        if (lower.find(p) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::optional<int> TryParseLastPercentUtf8(std::string_view text) {
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

bool TryWingetVersion(const std::wstring& exe_path, std::string& combined_output) {
    if (exe_path.empty() || !FileExists(exe_path)) {
        return false;
    }
    std::wstring cmd = L"\"" + exe_path + L"\" --version";
    auto res = proc::RunCommand(cmd, nullptr, 30000, true);
    combined_output = res.stdout_text + res.stderr_text;
    return res.started && res.exit_code == 0;
}

std::string TrimAsciiWhitespace(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

void CollectWingetCandidatePaths(std::vector<std::wstring>& out, std::set<std::wstring>& seen) {
    auto add = [&](const std::wstring& p) {
        if (p.empty() || seen.count(p)) {
            return;
        }
        seen.insert(p);
        out.push_back(p);
    };

    // 0) Registered package install location (fixes "winget not found" when App Execution Aliases never
    //    created %LocalAppData%\Microsoft\WindowsApps\winget.exe — Store still shows App Installer installed.)
    {
        const wchar_t* ps_cmd =
            L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
            L"-Command \"(Get-AppxPackage -Name Microsoft.DesktopAppInstaller).InstallLocation\"";
        auto ps_res = proc::RunCommand(ps_cmd, nullptr, 30000, true);
        if (ps_res.started && ps_res.exit_code == 0 && !ps_res.stdout_text.empty()) {
            std::string dir_utf8 = TrimAsciiWhitespace(ps_res.stdout_text);
            if (!dir_utf8.empty()) {
                std::wstring dir = Utf8ToWide(dir_utf8);
                while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
                    dir.pop_back();
                }
                if (!dir.empty()) {
                    add((std::filesystem::path(dir) / L"winget.exe").wstring());
                }
            }
        }
    }

    // 1) PATH resolution via where.exe
    auto where_res = proc::RunCommand(L"where.exe winget", nullptr, 10000, true);
    if (where_res.started && where_res.exit_code == 0 && !where_res.stdout_text.empty()) {
        std::string line = where_res.stdout_text;
        size_t pos = line.find('\r');
        if (pos != std::string::npos) {
            line = line.substr(0, pos);
        }
        pos = line.find('\n');
        if (pos != std::string::npos) {
            line = line.substr(0, pos);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (!line.empty()) {
            add(Utf8ToWide(line));
        }
    }

    // 2) Execution-alias stub (common when "App execution aliases" are enabled)
    wchar_t local_app_data[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local_app_data) == S_OK) {
        std::filesystem::path stub =
            std::filesystem::path(local_app_data) / L"Microsoft" / L"WindowsApps" / L"winget.exe";
        add(stub.wstring());

        // 3) Packaged Desktop App Installer: Microsoft.DesktopAppInstaller_*\\winget.exe
        std::filesystem::path wa = std::filesystem::path(local_app_data) / L"Microsoft" / L"WindowsApps";
        try {
            if (std::filesystem::exists(wa)) {
                for (const auto& entry : std::filesystem::directory_iterator(wa)) {
                    std::error_code ec;
                    if (!entry.is_directory(ec) || ec) {
                        continue;
                    }
                    const std::wstring name = entry.path().filename().wstring();
                    if (name.rfind(L"Microsoft.DesktopAppInstaller_", 0) != 0) {
                        continue;
                    }
                    std::filesystem::path candidate = entry.path() / L"winget.exe";
                    if (std::filesystem::exists(candidate)) {
                        add(candidate.wstring());
                    }
                }
            }
        } catch (...) {
        }
    }

    // 4) All-users WindowsApps (may require permissions; best-effort)
    wchar_t prog[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, SHGFP_TYPE_CURRENT, prog) == S_OK) {
        try {
            std::filesystem::path wa64 = std::filesystem::path(prog) / L"WindowsApps";
            if (std::filesystem::exists(wa64)) {
                for (const auto& entry : std::filesystem::directory_iterator(wa64)) {
                    std::error_code ec;
                    if (!entry.is_directory(ec) || ec) {
                        continue;
                    }
                    const std::wstring name = entry.path().filename().wstring();
                    if (name.rfind(L"Microsoft.DesktopAppInstaller_", 0) != 0) {
                        continue;
                    }
                    std::filesystem::path candidate = entry.path() / L"winget.exe";
                    if (std::filesystem::exists(candidate)) {
                        add(candidate.wstring());
                    }
                }
            }
        } catch (...) {
        }
    }
}

} // namespace

WingetInfo DetectWinget() {
    WingetInfo info;
    std::vector<std::wstring> candidates;
    std::set<std::wstring> seen;
    CollectWingetCandidatePaths(candidates, seen);

    for (const auto& path : candidates) {
        std::string ver_out;
        if (TryWingetVersion(path, ver_out)) {
            info.executable_path = path;
            info.version_stdout = std::move(ver_out);
            info.usable = true;
            return info;
        }
    }

    if (!candidates.empty()) {
        info.executable_path = candidates.front();
    }
    info.usable = false;
    return info;
}

std::string GetWingetRecoveryHints() {
    return R"(winget troubleshooting (official Windows Package Manager):
1) Install or update "App Installer" from the Microsoft Store (package that provides winget).
   Store link: https://www.microsoft.com/p/app-installer/9nblggh4nns1
2) If Store shows "Installed" but `winget` is not recognized: App Execution Aliases may be missing.
   Settings -> Apps -> Advanced app settings -> App execution aliases: turn ON winget.exe (and App Installer).
   This creates %LocalAppData%\Microsoft\WindowsApps\winget.exe stubs; some PCs have an empty WindowsApps folder until aliases are enabled.
3) Open Microsoft Store -> Library -> Get updates (update App Installer).
4) Repair: Settings -> Apps -> Installed apps -> App Installer -> Advanced options -> Repair.
5) If winget errors with sources: elevated PowerShell: winget source reset --force  then  winget source update
6) Corporate PCs: Group Policy may block Store or winget; ask IT or use offline/ISO installers instead.
)";
}

InstallResult WingetInstallPackage(const char* winget_id, bool force, const std::wstring* override_arguments,
                                   std::uint32_t timeout_ms, kds::ui::IProgressSink* progress_ui) {
    InstallResult r;
    WingetInfo wg = DetectWinget();
    if (!wg.usable) {
        r.detail = "winget is not available or did not respond to --version.\n";
        r.detail += GetWingetRecoveryHints();
        r.outcome = InstallOutcome::Failed;
        return r;
    }

    // Refresh package index (best-effort; helps when sources are stale or after Store updates).
    {
        std::wstring src_cmd = L"\"" + wg.executable_path + L"\" source update";
        auto src_res = proc::RunCommand(src_cmd, nullptr, 120000, true);
        if (src_res.exit_code != 0) {
            KDS_LOG(Warning, "winget source update returned non-zero; continuing anyway.");
        }
    }

    std::wstring id = Utf8ToWide(winget_id);
    std::wstring cmd = L"\"" + wg.executable_path + L"\" install -e --id " + id +
                         L" --accept-package-agreements --accept-source-agreements "
                         L"--disable-interactivity ";

    if (force) {
        cmd += L"--force ";
    }

    // Silent modes: winget uses --silent when supported by the package installer.
    cmd += L"--silent ";

    if (override_arguments && !override_arguments->empty()) {
        cmd += L"--override \"";
        cmd += *override_arguments;
        cmd += L"\" ";
    }

    const bool ok = proc::WithRetries(kMaxRetries, kRetryBaseDelayMs, [&](int attempt) {
        KDS_LOG(Info, (std::string("winget install attempt ") + std::to_string(attempt) + " for " + winget_id)
                          .c_str());
        if (progress_ui) {
            const std::wstring title = L"Package install (winget)";
            const std::wstring detail = L"Package: " + id + L" — attempt " + std::to_wstring(attempt);
            progress_ui->SetPhase(title.c_str(), detail.c_str());
            progress_ui->SetInstallIndeterminate(
                L"Running winget — a numeric percentage appears only if the installer prints one.");
        }

        std::string recent_for_percent;
        auto run_streaming = [&]() {
            return proc::RunCommandWithStreamingOutput(
                cmd, nullptr, timeout_ms, true, [&](std::string_view chunk) {
                    if (!progress_ui) {
                        return;
                    }
                    recent_for_percent.append(chunk.data(), chunk.size());
                    if (recent_for_percent.size() > 16384) {
                        recent_for_percent.erase(0, recent_for_percent.size() - 16384);
                    }
                    if (const auto pct = TryParseLastPercentUtf8(recent_for_percent)) {
                        progress_ui->SetInstallPercent(*pct);
                    }
                });
        };
        const auto res = progress_ui ? run_streaming() : proc::RunCommand(cmd, nullptr, timeout_ms, true);
        r.last_exit_code = res.exit_code;
        if (res.timed_out) {
            r.detail = "winget install timed out.";
            return false;
        }
        if (!res.started) {
            r.detail = "Failed to start winget.";
            return false;
        }
        // Some installers return reboot codes; winget may still return 0.
        r.detail = res.stdout_text;
        if (!res.stderr_text.empty()) {
            r.detail += "\n";
            r.detail += res.stderr_text;
        }
        const std::string merged = res.stdout_text + "\n" + res.stderr_text;
        if (res.exit_code == 0) {
            return true;
        }
        return IsWingetInstallAlreadySatisfied(merged);
    });

    if (progress_ui) {
        progress_ui->SetStepComplete(L"winget package install", false, ok);
    }
    r.outcome = ok ? InstallOutcome::Success : InstallOutcome::Failed;
    return r;
}

bool WingetIsPackageInstalled(const char* winget_id) {
    WingetInfo wg = DetectWinget();
    if (!wg.usable) {
        return false;
    }

    std::wstring id = Utf8ToWide(winget_id);
    std::wstring cmd = L"\"" + wg.executable_path + L"\" list --id " + id + L" --exact --output json";
    auto res = proc::RunCommand(cmd, nullptr, 60000, true);
    if (!res.started) {
        return false;
    }

    const std::string& out = res.stdout_text;
    // Minimal heuristic: look for exact id string and a version-like field in JSON output.
    if (out.find(winget_id) != std::string::npos && out.find("\"Version\"") != std::string::npos) {
        return true;
    }

    // Fallback: non-json list
    std::wstring cmd2 = L"\"" + wg.executable_path + L"\" list --id " + id + L" --exact";
    auto res2 = proc::RunCommand(cmd2, nullptr, 60000, true);
    if (!res2.started || res2.exit_code != 0) {
        return false;
    }
    std::string text = res2.stdout_text;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(c); });
    // If list returns rows, winget typically shows name and id.
    return text.find(winget_id) != std::string::npos;
}

} // namespace kds::pkg
