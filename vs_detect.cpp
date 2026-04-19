// vs_detect.cpp
#include "vs_detect.h"

#include "constants.h"
#include "logging.h"
#include "process_utils.h"

#include <ShlObj.h>

#include <filesystem>
#include <sstream>
#include <vector>
#include <cwctype>

#pragma comment(lib, "Shell32.lib")

namespace kds::vs {

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

bool ParseVersionParts(const std::string& ver, int& major, int& minor) {
    major = 0;
    minor = 0;
    if (ver.empty()) {
        return false;
    }
    try {
        size_t dot = ver.find('.');
        if (dot == std::string::npos) {
            major = std::stoi(ver);
            return true;
        }
        major = std::stoi(ver.substr(0, dot));
        size_t dot2 = ver.find('.', dot + 1);
        if (dot2 == std::string::npos) {
            minor = std::stoi(ver.substr(dot + 1));
        } else {
            minor = std::stoi(ver.substr(dot + 1, dot2 - dot - 1));
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<VsInstallation> RunVsWhere(const std::wstring& vswhere,
                                           const std::vector<std::wstring>& extra_args) {
    std::wstring cmd = L"\"" + vswhere + L"\"";
    for (const auto& a : extra_args) {
        cmd.push_back(L' ');
        cmd += a;
    }

    auto res = proc::RunCommand(cmd, nullptr, kShortCommandTimeoutSec * 1000, true);
    if (!res.started || res.exit_code != 0) {
        return std::nullopt;
    }

    std::string path_utf8 = res.stdout_text;
    // Trim whitespace and newlines
    while (!path_utf8.empty() && (path_utf8.back() == '\r' || path_utf8.back() == '\n' || path_utf8.back() == ' ')) {
        path_utf8.pop_back();
    }
    size_t start = 0;
    while (start < path_utf8.size() && (path_utf8[start] == ' ' || path_utf8[start] == '\r' ||
                                        path_utf8[start] == '\n')) {
        ++start;
    }
    std::string trimmed = path_utf8.substr(start);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    VsInstallation vi{};
    vi.installation_path = Utf8ToWide(trimmed);

    std::wstring ver_cmd =
        L"\"" + vswhere + L"\" -nologo -latest -version \"[17.0,18.0)\" -products "
                         L"Microsoft.VisualStudio.Product.Professional -property installationVersion";
    auto ver_out = proc::RunCommand(ver_cmd, nullptr, kShortCommandTimeoutSec * 1000, true);
    if (ver_out.started && ver_out.exit_code == 0 && !ver_out.stdout_text.empty()) {
        std::string v = ver_out.stdout_text;
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) {
            v.pop_back();
        }
        vi.installation_version = v;
        ParseVersionParts(vi.installation_version, vi.major, vi.minor);
    }

    std::wstring complete_cmd =
        L"\"" + vswhere + L"\" -nologo -latest -version \"[17.0,18.0)\" -products "
                         L"Microsoft.VisualStudio.Product.Professional -property isComplete";
    auto comp_out = proc::RunCommand(complete_cmd, nullptr, kShortCommandTimeoutSec * 1000, true);
    if (comp_out.started && comp_out.exit_code == 0) {
        std::string c = comp_out.stdout_text;
        vi.is_complete = (c.find('1') != std::string::npos);
    }

    vi.product_id = L"Microsoft.VisualStudio.Product.Professional";
    return vi;
}

bool VsWhereRequires(const std::wstring& vswhere, const char* component_id) {
    std::wstring comp = Utf8ToWide(component_id);
    std::wstring cmd = L"\"" + vswhere + L"\" -nologo -latest -version \"[17.0,18.0)\" -products "
                                        L"Microsoft.VisualStudio.Product.Professional -requires " +
                       comp + L" -property installationPath";
    auto res = proc::RunCommand(cmd, nullptr, kShortCommandTimeoutSec * 1000, true);
    return res.started && res.exit_code == 0 && !res.stdout_text.empty();
}

bool VsWhereRequiresAtPath(const std::wstring& vswhere, const std::wstring& installation_path,
                           const char* component_id) {
    std::wstring comp = Utf8ToWide(component_id);
    std::wstring cmd = L"\"" + vswhere + L"\" -nologo -all -version \"[17.0,18.0)\" -products * -requires " + comp +
                       L" -property installationPath";
    auto res = proc::RunCommand(cmd, nullptr, kShortCommandTimeoutSec * 1000, true);
    if (!res.started || res.exit_code != 0 || res.stdout_text.empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::path expected = std::filesystem::weakly_canonical(std::filesystem::path(installation_path), ec);
    if (ec || expected.empty()) {
        expected = std::filesystem::path(installation_path);
    }
    std::wstring expected_norm = expected.wstring();
    for (auto& ch : expected_norm) {
        ch = static_cast<wchar_t>(towlower(ch));
    }

    std::istringstream iss(res.stdout_text);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim CR/LF and leading/trailing spaces.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        std::string trimmed = line.substr(start);
        if (trimmed.empty()) {
            continue;
        }
        std::wstring found = Utf8ToWide(trimmed);
        if (found.empty()) {
            continue;
        }
        std::filesystem::path found_path = std::filesystem::path(found);
        std::filesystem::path found_canon = std::filesystem::weakly_canonical(found_path, ec);
        if (ec || found_canon.empty()) {
            found_canon = found_path;
        }
        std::wstring found_norm = found_canon.wstring();
        for (auto& ch : found_norm) {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        if (found_norm == expected_norm) {
            return true;
        }
    }
    return false;
}

std::string ExtractJsonStringField(const std::string& object, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t pos = object.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const size_t start = pos + needle.size();
    const size_t end = object.find('"', start);
    if (end == std::string::npos || end <= start) {
        return {};
    }
    return object.substr(start, end - start);
}

bool ExtractJsonBoolField(const std::string& object, const char* key, bool& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = object.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    size_t i = pos + needle.size();
    while (i < object.size() && (object[i] == ' ' || object[i] == '\t' || object[i] == '\r' || object[i] == '\n')) {
        ++i;
    }
    if (object.compare(i, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (object.compare(i, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

std::vector<std::string> SplitTopLevelJsonObjects(const std::string& json_array_body) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < json_array_body.size()) {
        const size_t lb = json_array_body.find('{', i);
        if (lb == std::string::npos) {
            break;
        }
        int depth = 0;
        size_t j = lb;
        for (; j < json_array_body.size(); ++j) {
            if (json_array_body[j] == '{') {
                depth++;
            } else if (json_array_body[j] == '}') {
                depth--;
                if (depth == 0) {
                    ++j;
                    break;
                }
            }
        }
        out.push_back(json_array_body.substr(lb, j - lb));
        i = j;
    }
    return out;
}

} // namespace

std::wstring ResolveVsWherePath() {
    wchar_t pf86[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, SHGFP_TYPE_CURRENT, pf86) != S_OK) {
        // Fallback environment variable
        DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, static_cast<DWORD>(std::size(pf86)));
        if (n == 0 || n >= std::size(pf86)) {
            return {};
        }
    }
    std::filesystem::path p = std::filesystem::path(pf86) / kVsWhereRelativePath;
    if (std::filesystem::exists(p)) {
        return p.wstring();
    }
    return {};
}

std::optional<VsInstallation> QueryVs2022Professional() {
    std::wstring vswhere = ResolveVsWherePath();
    if (vswhere.empty()) {
        KDS_LOG(Warning, "vswhere.exe not found. Visual Studio may not be installed.");
        return std::nullopt;
    }

    std::vector<std::wstring> args = {
        L"-nologo", L"-latest", L"-version", L"[17.0,18.0)", L"-products",
        L"Microsoft.VisualStudio.Product.Professional", L"-property", L"installationPath"};
    return RunVsWhere(vswhere, args);
}

bool ProbeComponents(const VsInstallation& inst, VsComponentStatus& out) {
    if (!inst.installation_path.empty()) {
        return ProbeComponentsAtPath(inst.installation_path, out);
    }
    out = VsComponentStatus{};
    std::wstring vswhere = ResolveVsWherePath();
    if (vswhere.empty()) {
        return false;
    }

    out.has_native_desktop_workload = VsWhereRequires(vswhere, kVsComponentWorkloadNativeDesktop);
    out.has_vc_tools_x86_x64 = VsWhereRequires(vswhere, kVsComponentVcToolsX86X64);
    out.has_win11_sdk_26100 = VsWhereRequires(vswhere, kVsComponentWin11Sdk26100);
    out.has_driver_kit_component = VsWhereRequires(vswhere, kVsComponentWindowsDriverKit);
    out.has_kernel_mode_driver_toolset = HasKernelModeDriverToolset(inst.installation_path);
    return true;
}

bool ProbeComponentsAtPath(const std::wstring& installation_path, VsComponentStatus& out) {
    out = VsComponentStatus{};
    std::wstring vswhere = ResolveVsWherePath();
    if (vswhere.empty() || installation_path.empty()) {
        return false;
    }

    out.has_native_desktop_workload =
        VsWhereRequiresAtPath(vswhere, installation_path, kVsComponentWorkloadNativeDesktop);
    out.has_vc_tools_x86_x64 = VsWhereRequiresAtPath(vswhere, installation_path, kVsComponentVcToolsX86X64);
    out.has_win11_sdk_26100 = VsWhereRequiresAtPath(vswhere, installation_path, kVsComponentWin11Sdk26100);
    out.has_driver_kit_component = VsWhereRequiresAtPath(vswhere, installation_path, kVsComponentWindowsDriverKit);
    out.has_kernel_mode_driver_toolset = HasKernelModeDriverToolset(installation_path);
    return true;
}

std::vector<VsInstanceInfo> EnumerateVsInstances17() {
    std::vector<VsInstanceInfo> result;
    std::wstring vswhere = ResolveVsWherePath();
    if (vswhere.empty()) {
        return result;
    }

    std::wstring cmd =
        L"\"" + vswhere + L"\" -nologo -all -version \"[17.0,18.0)\" -format json -sort installationVersion";
    auto res = proc::RunCommand(cmd, nullptr, kShortCommandTimeoutSec * 1000, true);
    if (!res.started || res.exit_code != 0 || res.stdout_text.empty()) {
        return result;
    }

    const std::string& raw = res.stdout_text;
    const size_t lb = raw.find('[');
    const size_t rb = raw.rfind(']');
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb) {
        return result;
    }
    const std::string array_body = raw.substr(lb + 1, rb - lb - 1);
    const auto objects = SplitTopLevelJsonObjects(array_body);
    result.reserve(objects.size());

    for (const std::string& obj : objects) {
        VsInstanceInfo vi{};
        vi.installation_path = Utf8ToWide(ExtractJsonStringField(obj, "installationPath"));
        vi.product_id = Utf8ToWide(ExtractJsonStringField(obj, "productId"));
        vi.installation_version = ExtractJsonStringField(obj, "installationVersion");
        vi.display_name = Utf8ToWide(ExtractJsonStringField(obj, "displayName"));
        (void)ExtractJsonBoolField(obj, "isComplete", vi.is_complete);
        ParseVersionParts(vi.installation_version, vi.major, vi.minor);
        if (!vi.installation_path.empty()) {
            result.push_back(std::move(vi));
        }
    }
    return result;
}

bool NeedsWdkVsixSilently(const VsInstallation& inst) {
    if (inst.major < kVsMajorMin) {
        return true;
    }
    if (inst.major == kVsMajorMin && inst.minor < kVsMinorWdkVsixCutoff) {
        return true;
    }
    return false;
}

std::wstring FindWdkVsixPath() {
    wchar_t pf86[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, SHGFP_TYPE_CURRENT, pf86) != S_OK) {
        DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, static_cast<DWORD>(std::size(pf86)));
        if (n == 0 || n >= std::size(pf86)) {
            return {};
        }
    }
    const std::filesystem::path base = std::filesystem::path(pf86);
    for (const auto* rel : kWdkVsixSearchRelativePaths) {
        std::filesystem::path p = base / rel;
        if (std::filesystem::exists(p)) {
            return p.wstring();
        }
    }
    // Newer WDK builds may place WDK.vsix only under a versioned Vsix subtree — search shallow tree.
    const std::filesystem::path vsix_root = base / L"Windows Kits" / L"10" / L"Vsix";
    try {
        if (std::filesystem::exists(vsix_root)) {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(
                     vsix_root, std::filesystem::directory_options::skip_permission_denied)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (entry.path().filename() == L"WDK.vsix") {
                    return entry.path().wstring();
                }
            }
        }
    } catch (...) {
    }
    return {};
}

bool HasKernelModeDriverToolset(const std::wstring& installation_path) {
    try {
        // Legacy integration path expected by older projects/toolsets.
        if (!installation_path.empty()) {
            const std::filesystem::path base =
                std::filesystem::path(installation_path) / L"MSBuild" / L"Microsoft" / L"VC" / L"v170" / L"Platforms";
            const std::filesystem::path x64_toolset =
                base / L"x64" / L"PlatformToolsets" / L"WindowsKernelModeDriver10.0" / L"Toolset.props";
            const std::filesystem::path arm64_toolset =
                base / L"ARM64" / L"PlatformToolsets" / L"WindowsKernelModeDriver10.0" / L"Toolset.props";
            if (std::filesystem::exists(x64_toolset) || std::filesystem::exists(arm64_toolset)) {
                return true;
            }
        }

        // Newer WDK layouts (26100+) may wire driver toolsets through Kits ImportAfter targets
        // without creating legacy WindowsKernelModeDriver10.0 toolset folders under VS\MSBuild.
        wchar_t pf86[MAX_PATH] = {};
        if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, SHGFP_TYPE_CURRENT, pf86) != S_OK) {
            return false;
        }
        const std::filesystem::path kits = std::filesystem::path(pf86) / L"Windows Kits" / L"10" / L"build";
        if (!std::filesystem::exists(kits)) {
            return false;
        }
        for (const auto& ver : std::filesystem::directory_iterator(kits)) {
            if (!ver.is_directory()) {
                continue;
            }
            const auto x64_importafter =
                ver.path() / L"x64" / L"ImportAfter" / L"WDK.x64.WindowsDriverCommonToolset.Platform.Targets";
            const auto arm64_importafter =
                ver.path() / L"ARM64" / L"ImportAfter" / L"WDK.arm64.WindowsDriverCommonToolset.Platform.Targets";
            const auto x64_kmd =
                ver.path() / L"x64" / L"ImportAfter" / L"WDK.x64.WindowsKernelModeDriver.Platform.props";
            if (std::filesystem::exists(x64_importafter) || std::filesystem::exists(arm64_importafter) ||
                std::filesystem::exists(x64_kmd)) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

} // namespace kds::vs
