// verification.cpp
#include "verification.h"

#include "constants.h"
#include "logging.h"
#include "package_manager.h"
#include "process_utils.h"
#include "progress_ui.h"
#include "vs_detect.h"

#include <ShlObj.h>

#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>

#pragma comment(lib, "Shell32.lib")

namespace kds::verify {

namespace {

std::string WideToUtf8Local(const std::wstring& w) {
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

void Add(std::vector<CheckItem>& items, std::string name, bool ok, std::string detail) {
    items.push_back(CheckItem{std::move(name), ok, std::move(detail)});
}

std::string JsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                std::ostringstream esc;
                esc << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                out += esc.str();
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::wstring ReadRegistrySz(HKEY root, const wchar_t* subkey, const wchar_t* value_name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buf[2048] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(key, value_name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    return std::wstring(buf);
}

bool FileExistsWide(const std::wstring& p) {
    try {
        return !p.empty() && std::filesystem::exists(std::filesystem::path(p));
    } catch (...) {
        return false;
    }
}

// WDK installs kernel-mode headers under Kits\10\Include\<build>\km (folder "km" exists).
bool WdkKernelHeadersPresent() {
    wchar_t pf86[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, SHGFP_TYPE_CURRENT, pf86) != S_OK) {
        return false;
    }
    try {
        std::filesystem::path inc = std::filesystem::path(pf86) / L"Windows Kits" / L"10" / L"Include";
        if (!std::filesystem::exists(inc)) {
            return false;
        }
        for (const auto& entry : std::filesystem::directory_iterator(inc)) {
            if (!entry.is_directory()) {
                continue;
            }
            if (std::filesystem::exists(entry.path() / L"km")) {
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

} // namespace

VerificationReport RunVerification(kds::ui::IProgressSink* progress) {
    VerificationReport report;

    if (progress) {
        progress->SetPhase(L"Verification", L"Checking winget, Visual Studio, SDK, WDK, and tools...");
        progress->SetInstallIndeterminate(L"Running checks — no single percentage; this may take a minute.");
    }

    // winget
    {
        auto wg = ::kds::pkg::DetectWinget();
        Add(report.items, "winget_available", wg.usable,
            wg.usable ? ("version output: " + wg.version_stdout) : "winget not found or not working.");
    }

    if (progress) {
        progress->SetPhase(L"Verification", L"Inspecting Visual Studio 2022 Professional...");
    }

    // VS 2022 Professional
    {
        auto vs = vs::QueryVs2022Professional();
        if (vs.has_value()) {
            std::string detail = "Path: " + WideToUtf8Local(vs->installation_path) +
                                 "; Version: " + vs->installation_version;
            Add(report.items, "visual_studio_2022_professional", true, detail);
            vs::VsComponentStatus cs{};
            vs::ProbeComponents(*vs, cs);
            const bool wdk_headers = WdkKernelHeadersPresent();
            Add(report.items, "vs_cpp_workload_native_desktop", cs.has_native_desktop_workload,
                cs.has_native_desktop_workload ? "vswhere -requires workload present."
                                               : "Workload not detected via vswhere -requires.");
            Add(report.items, "vs_vc_tools_x86_x64", cs.has_vc_tools_x86_x64,
                cs.has_vc_tools_x86_x64 ? "VC tools component present." : "VC tools component not detected.");
            Add(report.items, "vs_win11_sdk_26100_component", cs.has_win11_sdk_26100,
                cs.has_win11_sdk_26100 ? "Windows 11 SDK 26100 component present."
                                       : "26100 SDK component not detected (may still be installed separately).");
            Add(report.items, "vs_kernel_mode_driver_toolset", cs.has_kernel_mode_driver_toolset,
                cs.has_kernel_mode_driver_toolset
                    ? "Kernel driver toolset integration detected (legacy VS toolset path or modern WDK ImportAfter wiring)."
                    : "Kernel driver toolset integration not detected (legacy toolset path missing and no modern WDK "
                      "ImportAfter wiring found). KMDF/driver projects can fail with MSB8020.");
            {
                const bool driver_ok = cs.has_driver_kit_component || wdk_headers;
                std::string driver_detail;
                if (cs.has_driver_kit_component) {
                    driver_detail = "VS WDK component detected via vswhere.";
                } else if (wdk_headers) {
                    driver_detail = "Optional VS WDK component not registered in vswhere, but WDK kernel headers "
                                    "(km) are present under Windows Kits (driver build should work).";
                } else {
                    driver_detail = "VS WDK component not detected via vswhere and no km headers found under Kits.";
                }
                Add(report.items, "vs_windows_driver_kit_component", driver_ok, driver_detail);
            }
        } else {
            Add(report.items, "visual_studio_2022_professional", false,
                "No VS 2022 Professional installation found.");
        }
    }

    if (progress) {
        progress->SetPhase(L"Verification", L"Locating compiler and MSBuild via vswhere...");
    }

    // vswhere cl.exe / MSBuild
    {
        std::wstring vswhere = vs::ResolveVsWherePath();
        if (!vswhere.empty()) {
            std::wstring cmd_cl =
                L"\"" + vswhere + L"\" -nologo -latest -version \"[17.0,18.0)\" -products "
                                L"Microsoft.VisualStudio.Product.Professional -find **\\cl.exe";
            auto r = proc::RunCommand(cmd_cl, nullptr, 60000, true);
            bool ok = r.started && r.exit_code == 0 && !r.stdout_text.empty();
            Add(report.items, "cl_exe_discoverable", ok,
                ok ? ("First match output:\n" + r.stdout_text) : "vswhere did not return cl.exe paths.");

            std::wstring cmd_msb =
                L"\"" + vswhere + L"\" -nologo -latest -version \"[17.0,18.0)\" -products "
                                 L"Microsoft.VisualStudio.Product.Professional -find **\\MSBuild.exe";
            auto r2 = proc::RunCommand(cmd_msb, nullptr, 60000, true);
            bool ok2 = r2.started && r2.exit_code == 0 && !r2.stdout_text.empty();
            Add(report.items, "msbuild_discoverable", ok2,
                ok2 ? ("First match output:\n" + r2.stdout_text) : "vswhere did not return MSBuild.exe paths.");
        } else {
            Add(report.items, "cl_exe_discoverable", false, "vswhere missing.");
            Add(report.items, "msbuild_discoverable", false, "vswhere missing.");
        }
    }

    if (progress) {
        progress->SetPhase(L"Verification", L"Checking Windows SDK / Kits layout...");
    }

    // Windows Kits root (SDK)
    {
        std::wstring kits10 = ReadRegistrySz(HKEY_LOCAL_MACHINE,
                                             L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots",
                                             L"KitsRoot10");
        if (kits10.empty()) {
            wchar_t pf[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, SHGFP_TYPE_CURRENT, pf);
            kits10 = std::wstring(pf) + L"\\" + kWindowsKitsRootRelative;
        }
        bool ok = FileExistsWide(kits10);
        Add(report.items, "windows_kits_root10", ok,
            ok ? ("KitsRoot10: " + WideToUtf8Local(kits10)) : "KitsRoot10 not found in registry/filesystem.");
        if (ok) {
            try {
                bool has_include = false;
                for (auto& e : std::filesystem::directory_iterator(std::filesystem::path(kits10) / L"Include")) {
                    (void)e;
                    has_include = true;
                    break;
                }
                Add(report.items, "windows_sdk_include_present", has_include,
                    has_include ? "Include directory exists under Kits\\10."
                                : "Include directory not found (SDK may be incomplete).");
            } catch (...) {
                Add(report.items, "windows_sdk_include_present", false, "Failed enumerating Include directory.");
            }
        }
    }

    if (progress) {
        progress->SetPhase(L"Verification", L"Checking WDK binaries and VSIX...");
    }

    // WDK presence (best-effort)
    {
        wchar_t pf86[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, SHGFP_TYPE_CURRENT, pf86);
        std::wstring wdk_bin = std::wstring(pf86) + L"\\" + kWdkBinRelative;
        bool ok = FileExistsWide(wdk_bin);
        Add(report.items, "wdk_bin_folder", ok,
            ok ? ("Found: " + WideToUtf8Local(wdk_bin)) : "Expected WDK bin folder not found.");

        std::wstring vsix = vs::FindWdkVsixPath();
        const bool wdk_km = WdkKernelHeadersPresent();
        const bool vsix_ok = !vsix.empty() || wdk_km;
        std::string vsix_detail;
        if (!vsix.empty()) {
            vsix_detail = "Found: " + WideToUtf8Local(vsix);
        } else if (wdk_km) {
            vsix_detail = "WDK.vsix not found on disk; VS 17.11+ often integrates WDK without a loose VSIX file. "
                          "Kernel headers (km) are present.";
        } else {
            vsix_detail = "WDK.vsix not found under Windows Kits\\10\\Vsix (optional on newer VS/WDK).";
        }
        Add(report.items, "wdk_vsix_present", vsix_ok, vsix_detail);
    }

    // Kernel-mode build capability is inferred from toolchain + kits + WDK paths.
    bool critical = true;
    for (const auto& it : report.items) {
        if (it.name == "visual_studio_2022_professional" || it.name == "windows_kits_root10" ||
            it.name == "wdk_bin_folder" || it.name == "cl_exe_discoverable" ||
            it.name == "vs_kernel_mode_driver_toolset") {
            if (!it.ok) {
                critical = false;
            }
        }
    }
    report.all_critical_passed = critical;

    if (progress) {
        progress->SetInstallPercent(std::nullopt);
        progress->SetStepComplete(L"Verification", false, critical);
    }

    return report;
}

std::string ReportToJson(const VerificationReport& r) {
    std::ostringstream oss;
    oss << "{\"verification\":{";
    oss << "\"all_critical_passed\":" << (r.all_critical_passed ? "true" : "false") << ",";
    oss << "\"items\":[";
    for (size_t i = 0; i < r.items.size(); ++i) {
        const auto& it = r.items[i];
        if (i > 0) {
            oss << ",";
        }
        oss << "{";
        oss << "\"name\":\"" << JsonEscape(it.name) << "\",";
        oss << "\"ok\":" << (it.ok ? "true" : "false") << ",";
        oss << "\"detail\":\"" << JsonEscape(it.detail) << "\"";
        oss << "}";
    }
    oss << "]}}";
    return oss.str();
}

std::string ReportToText(const VerificationReport& r) {
    std::ostringstream oss;
    oss << "Verification summary\n";
    oss << "all_critical_passed: " << (r.all_critical_passed ? "true" : "false") << "\n";
    for (const auto& it : r.items) {
        oss << "- " << it.name << ": " << (it.ok ? "OK" : "FAIL") << "\n";
        oss << "  " << it.detail << "\n";
    }
    return oss.str();
}

} // namespace kds::verify
