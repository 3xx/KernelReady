// vs_detect.h - Visual Studio 2022 discovery via vswhere and filesystem probes.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace kds::vs {

struct VsInstallation {
    std::wstring installation_path;
    std::string installation_version; // e.g. 17.11.35222.181
    int major = 0;
    int minor = 0;
    bool is_complete = false;
    std::wstring product_id; // e.g. Microsoft.VisualStudio.Product.Professional
};

/// One Visual Studio 2022 (17.x) instance reported by vswhere -all (any product).
struct VsInstanceInfo {
    std::wstring installation_path;
    std::wstring product_id;
    std::string installation_version;
    std::wstring display_name;
    int major = 0;
    int minor = 0;
    bool is_complete = false;
};

struct VsComponentStatus {
    bool has_native_desktop_workload = false;
    bool has_vc_tools_x86_x64 = false;
    bool has_win11_sdk_26100 = false;
    bool has_driver_kit_component = false;
    bool has_kernel_mode_driver_toolset = false;
};

// Locates vswhere.exe under Program Files (x86).
std::wstring ResolveVsWherePath();

// Queries the latest VS 2022 Professional installation. Empty optional if not found.
std::optional<VsInstallation> QueryVs2022Professional();

// Uses vswhere -requires (multiple invocations) to infer key components when possible.
bool ProbeComponents(const VsInstallation& inst, VsComponentStatus& out);

// Probes components for a specific installation directory (any VS 2022 product).
bool ProbeComponentsAtPath(const std::wstring& installation_path, VsComponentStatus& out);

// All VS 17.x instances (Community, Professional, Enterprise, BuildTools, ...). Empty if vswhere missing or none.
std::vector<VsInstanceInfo> EnumerateVsInstances17();

// Returns true if minor version is below the WDK VSIX threshold (see constants.h).
bool NeedsWdkVsixSilently(const VsInstallation& inst);

// Searches default WDK VSIX locations under Program Files (x86).
std::wstring FindWdkVsixPath();

// Verifies VS-integrated Kernel Mode Driver platform toolset files are present.
bool HasKernelModeDriverToolset(const std::wstring& installation_path);

} // namespace kds::vs
