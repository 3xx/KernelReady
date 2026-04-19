// compatibility.h - Detect installed dev stack, classify compatibility, and plan minimal remediation.
#pragma once

#include "constants.h"
#include "os_detect.h"
#include "package_manager.h"
#include "vs_detect.h"

#include <string>
#include <vector>

namespace kds::compat {

enum class CompatibilityTier {
    FullyCompatible,
    PartiallyCompatible,
    Incompatible,
};

enum class RemediationAction {
    NoneReuse,
    ModifyOrAddComponents,
    RepairWithConfig,
    InstallFreshOrSideBySide,
    Blocked,
};

struct ComponentGapReport {
    bool missing_native_desktop = false;
    bool missing_vc_tools = false;
    bool missing_win11_sdk_26100 = false;
    bool missing_wdk_component = false;
    bool missing_kernel_mode_driver_toolset = false;
    bool incomplete_installation = false;
    bool edition_not_professional = false;
    bool wdk_integration_risk = false; // VS < 17.11 without DDK component (may need VSIX)
};

struct VsInstanceEvaluation {
    vs::VsInstanceInfo instance{};
    vs::VsComponentStatus components{};
    CompatibilityTier tier = CompatibilityTier::Incompatible;
    ComponentGapReport gaps{};
    std::vector<std::string> notes;
};

struct SdkWdkEvaluation {
    CompatibilityTier sdk = CompatibilityTier::PartiallyCompatible;
    CompatibilityTier wdk = CompatibilityTier::PartiallyCompatible;
    bool winget_sdk_installed = false;
    bool winget_wdk_installed = false;
    std::vector<std::string> notes;
};

/// Result of analyzing the machine against KernelDevSetup / driver-dev requirements.
struct CompatibilityPlan {
    std::vector<VsInstanceEvaluation> visual_studio_instances;
    SdkWdkEvaluation sdk_wdk{};

    RemediationAction vs_action = RemediationAction::InstallFreshOrSideBySide;
    bool run_vs_bootstrapper = true;
    bool prefer_repair_with_bootstrapper = false;
    std::string vs_decision_rationale;
    std::vector<std::string> vs_decision_detail;

    bool install_sdk_via_winget = true;
    bool install_wdk_via_winget = true;
    std::string sdk_decision_rationale;
    std::string wdk_decision_rationale;

    /// Policy: we do not silently remove Visual Studio products; manual steps only.
    std::vector<std::string> manual_cleanup_recommendations;
    std::vector<std::string> coexistence_notes;
};

/// Enumerates VS 2022 instances, evaluates components, and chooses reuse vs modify vs install.
CompatibilityPlan BuildPlan(const os::OsInfo& os, const RunOptions& options, const pkg::WingetInfo& winget);

void LogPlan(const CompatibilityPlan& plan);

} // namespace kds::compat
