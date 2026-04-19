// compatibility.cpp
#include "compatibility.h"

#include "logging.h"

#include <Windows.h>

#include <filesystem>
#include <sstream>

namespace kds::compat {

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

bool IsProfessionalProduct(const std::wstring& product_id) {
    return product_id == L"Microsoft.VisualStudio.Product.Professional";
}

vs::VsInstallation ToVsInstallation(const vs::VsInstanceInfo& i) {
    vs::VsInstallation v{};
    v.installation_path = i.installation_path;
    v.installation_version = i.installation_version;
    v.major = i.major;
    v.minor = i.minor;
    v.is_complete = i.is_complete;
    v.product_id = i.product_id;
    return v;
}

void FillGaps(const vs::VsInstanceInfo& inst, const vs::VsComponentStatus& cs, ComponentGapReport& g) {
    g = {};
    g.edition_not_professional = !IsProfessionalProduct(inst.product_id);
    g.incomplete_installation = !inst.is_complete;
    g.missing_native_desktop = !cs.has_native_desktop_workload;
    g.missing_vc_tools = !cs.has_vc_tools_x86_x64;
    g.missing_win11_sdk_26100 = !cs.has_win11_sdk_26100;
    g.missing_wdk_component = !cs.has_driver_kit_component;
    g.missing_kernel_mode_driver_toolset = !cs.has_kernel_mode_driver_toolset;
    const vs::VsInstallation conv = ToVsInstallation(inst);
    g.wdk_integration_risk = vs::NeedsWdkVsixSilently(conv) && !cs.has_driver_kit_component;
}

void ClassifyInstance(VsInstanceEvaluation& ev) {
    FillGaps(ev.instance, ev.components, ev.gaps);

    namespace fs = std::filesystem;
    if (!ev.instance.installation_path.empty() && !fs::exists(fs::path(ev.instance.installation_path))) {
        ev.tier = CompatibilityTier::Incompatible;
        ev.notes.push_back("vswhere reported an installationPath that does not exist on disk (partial uninstall?).");
        return;
    }

    const bool stacks_ok =
        ev.components.has_native_desktop_workload && ev.components.has_vc_tools_x86_x64 &&
        ev.components.has_win11_sdk_26100 && ev.components.has_driver_kit_component &&
        ev.components.has_kernel_mode_driver_toolset;
    const vs::VsInstallation conv = ToVsInstallation(ev.instance);
    const bool wdk_line_ok = !vs::NeedsWdkVsixSilently(conv) || ev.components.has_driver_kit_component;

    if (IsProfessionalProduct(ev.instance.product_id) && ev.instance.is_complete && stacks_ok && wdk_line_ok) {
        ev.tier = CompatibilityTier::FullyCompatible;
        ev.notes.push_back("Visual Studio 2022 Professional has required workloads/components for this project.");
        return;
    }

    if (IsProfessionalProduct(ev.instance.product_id)) {
        ev.tier = CompatibilityTier::PartiallyCompatible;
        if (!ev.instance.is_complete) {
            ev.notes.push_back("Professional installation is not marked complete; modification or repair may be required.");
        }
        if (!stacks_ok) {
            ev.notes.push_back(
                "One or more required components are missing; the generated .vsconfig can add them in modify mode.");
        }
        if (!ev.components.has_kernel_mode_driver_toolset) {
            ev.notes.push_back(
                "Kernel toolset 'WindowsKernelModeDriver10.0' is not integrated into Visual Studio MSBuild paths.");
        }
        if (!wdk_line_ok) {
            ev.notes.push_back("Visual Studio is below 17.11 and WDK integration may still require a separate VSIX step.");
        }
        return;
    }

    // Other editions (Community, Enterprise, BuildTools, ...): can often coexist; target remains Professional.
    if (!ev.instance.product_id.empty()) {
        ev.tier = CompatibilityTier::PartiallyCompatible;
        ev.notes.push_back(
            "A non-Professional Visual Studio 2022 edition is present. KernelDevSetup targets Professional; a "
            "side-by-side Professional install is preferred rather than removing other editions.");
    } else {
        ev.tier = CompatibilityTier::Incompatible;
    }
}

const VsInstanceEvaluation* FindFirst(const std::vector<VsInstanceEvaluation>& list, CompatibilityTier tier,
                                      bool professional_only) {
    for (const auto& e : list) {
        if (e.tier != tier) {
            continue;
        }
        if (professional_only && !IsProfessionalProduct(e.instance.product_id)) {
            continue;
        }
        return &e;
    }
    return nullptr;
}

} // namespace

CompatibilityPlan BuildPlan(const os::OsInfo& os, const RunOptions& options, const pkg::WingetInfo& winget) {
    CompatibilityPlan plan;
    plan.coexistence_notes.push_back(
        "Multiple Visual Studio 2022 editions can coexist. KernelDevSetup never uninstalls an existing Visual Studio "
        "edition automatically.");
    plan.manual_cleanup_recommendations.push_back(
        "If an old or broken installation must be removed, use Visual Studio Installer or Settings -> Apps manually.");

    if (!os.vs2022_supported) {
        plan.vs_action = RemediationAction::Blocked;
        plan.run_vs_bootstrapper = false;
        plan.vs_decision_rationale = "Host OS is not suitable for Visual Studio 2022; see OS detection detail.";
        plan.vs_decision_detail.push_back(os.vs2022_support_detail);
        return plan;
    }

    std::vector<vs::VsInstanceInfo> raw = vs::EnumerateVsInstances17();
    plan.visual_studio_instances.reserve(raw.size());
    for (auto& ri : raw) {
        VsInstanceEvaluation ev;
        ev.instance = std::move(ri);
        vs::ProbeComponentsAtPath(ev.instance.installation_path, ev.components);
        ClassifyInstance(ev);
        plan.visual_studio_instances.push_back(std::move(ev));
    }

    // SDK / WDK via winget IDs (matches constants / verification expectations).
    plan.sdk_wdk.winget_sdk_installed = pkg::WingetIsPackageInstalled(kds::kWingetIdWindowsSdk);
    plan.sdk_wdk.winget_wdk_installed = pkg::WingetIsPackageInstalled(kds::kWingetIdWindowsWdk);
    if (plan.sdk_wdk.winget_sdk_installed) {
        plan.sdk_wdk.sdk = CompatibilityTier::FullyCompatible;
        plan.sdk_wdk.notes.push_back("winget reports Microsoft.WindowsSDK.10.0.26100 as installed.");
    } else {
        plan.sdk_wdk.sdk = CompatibilityTier::PartiallyCompatible;
        plan.sdk_wdk.notes.push_back("Windows SDK 10.0.26100 (winget id) not reported as installed.");
    }
    if (plan.sdk_wdk.winget_wdk_installed) {
        plan.sdk_wdk.wdk = CompatibilityTier::FullyCompatible;
        plan.sdk_wdk.notes.push_back("winget reports Microsoft.WindowsWDK.10.0.26100 as installed.");
    } else {
        plan.sdk_wdk.wdk = CompatibilityTier::PartiallyCompatible;
        plan.sdk_wdk.notes.push_back("Windows WDK 10.0.26100 (winget id) not reported as installed.");
    }

    const bool winget_ok = winget.usable;
    plan.install_sdk_via_winget = winget_ok && (!plan.sdk_wdk.winget_sdk_installed || options.force_reinstall);
    plan.install_wdk_via_winget = winget_ok && (!plan.sdk_wdk.winget_wdk_installed || options.force_reinstall);

    if (!plan.sdk_wdk.winget_sdk_installed || options.force_reinstall) {
        plan.sdk_decision_rationale =
            options.force_reinstall
                ? "--force: ensure/repair Windows SDK 10.0.26100 via winget when available."
                : "Install or upgrade Windows SDK 10.0.26100 via winget when the package is missing.";
    } else {
        plan.sdk_decision_rationale = "Reuse: Windows SDK winget package already present (no install step).";
    }

    if (!plan.sdk_wdk.winget_wdk_installed || options.force_reinstall) {
        plan.wdk_decision_rationale =
            options.force_reinstall
                ? "--force: ensure/repair Windows WDK 10.0.26100 via winget when available."
                : "Install or upgrade Windows WDK 10.0.26100 via winget when the package is missing.";
    } else {
        plan.wdk_decision_rationale = "Reuse: Windows WDK winget package already present (no install step).";
    }

    if (options.force_reinstall) {
        plan.vs_action = RemediationAction::RepairWithConfig;
        plan.run_vs_bootstrapper = true;
        plan.prefer_repair_with_bootstrapper = options.repair;
        plan.vs_decision_rationale =
            "--force: run the Visual Studio Professional bootstrapper to modify/repair installation state even if a "
            "compatible instance exists.";
        return plan;
    }

    const VsInstanceEvaluation* full_pro =
        FindFirst(plan.visual_studio_instances, CompatibilityTier::FullyCompatible, true);
    if (full_pro != nullptr) {
        plan.vs_action = RemediationAction::NoneReuse;
        plan.run_vs_bootstrapper = false;
        plan.prefer_repair_with_bootstrapper = false;
        std::ostringstream oss;
        oss << "Reuse: Visual Studio 2022 Professional at " << WideToUtf8(full_pro->instance.installation_path)
            << " is complete and reports required components.";
        plan.vs_decision_rationale = oss.str();
        return plan;
    }

    const VsInstanceEvaluation* partial_pro =
        FindFirst(plan.visual_studio_instances, CompatibilityTier::PartiallyCompatible, true);
    if (partial_pro != nullptr) {
        plan.vs_action = options.repair ? RemediationAction::RepairWithConfig : RemediationAction::ModifyOrAddComponents;
        plan.run_vs_bootstrapper = true;
        plan.prefer_repair_with_bootstrapper = options.repair && !partial_pro->instance.is_complete;
        plan.vs_decision_rationale =
            "Adapt: existing Visual Studio 2022 Professional is incomplete or missing required components; run the "
            "official bootstrapper with the generated .vsconfig to add/repair workloads (minimal change).";
        for (const auto& n : partial_pro->notes) {
            plan.vs_decision_detail.push_back(n);
        }
        return plan;
    }

    const VsInstanceEvaluation* any_partial = FindFirst(plan.visual_studio_instances,
                                                        CompatibilityTier::PartiallyCompatible, false);
    if (any_partial != nullptr) {
        plan.vs_action = RemediationAction::InstallFreshOrSideBySide;
        plan.run_vs_bootstrapper = true;
        plan.prefer_repair_with_bootstrapper = false;
        plan.vs_decision_rationale =
            "Install: no fully-compatible Professional instance found; install Visual Studio 2022 Professional "
            "alongside existing edition(s). Other editions are left intact.";
        for (const auto& n : any_partial->notes) {
            plan.vs_decision_detail.push_back(n);
        }
        return plan;
    }

    // No instances or only incompatible records.
    plan.vs_action = RemediationAction::InstallFreshOrSideBySide;
    plan.run_vs_bootstrapper = true;
    plan.prefer_repair_with_bootstrapper = false;
    plan.vs_decision_rationale =
        "Install: no Visual Studio 2022 instance was enumerated (or none usable); perform a fresh Professional install "
        "using the bootstrapper.";
    return plan;
}

void LogPlan(const CompatibilityPlan& plan) {
    KDS_LOG(Info, "=== Compatibility plan (driver development stack) ===");
    KDS_LOG(Info, ("Visual Studio decision: " + plan.vs_decision_rationale).c_str());
    for (const auto& line : plan.vs_decision_detail) {
        KDS_LOG(Info, ("  detail: " + line).c_str());
    }
    KDS_LOG(Info, (std::string("  run_vs_bootstrapper: ") + (plan.run_vs_bootstrapper ? "yes" : "no")).c_str());
    KDS_LOG(Info,
            (std::string("  prefer_repair_with_bootstrapper: ") + (plan.prefer_repair_with_bootstrapper ? "yes" : "no"))
                .c_str());

    for (const auto& e : plan.visual_studio_instances) {
        std::ostringstream head;
        head << "  VS instance: " << WideToUtf8(e.instance.display_name) << " ["
             << WideToUtf8(e.instance.product_id) << "] " << WideToUtf8(e.instance.installation_path);
        KDS_LOG(Info, head.str().c_str());
        const char* tier = "Incompatible";
        if (e.tier == CompatibilityTier::FullyCompatible) {
            tier = "FullyCompatible";
        } else if (e.tier == CompatibilityTier::PartiallyCompatible) {
            tier = "PartiallyCompatible";
        }
        KDS_LOG(Info, (std::string("    tier: ") + tier).c_str());
        for (const auto& n : e.notes) {
            KDS_LOG(Info, ("    note: " + n).c_str());
        }
    }

    KDS_LOG(Info, ("SDK: " + plan.sdk_decision_rationale).c_str());
    KDS_LOG(Info, ("WDK: " + plan.wdk_decision_rationale).c_str());
    KDS_LOG(Info, (std::string("  install_sdk_via_winget: ") + (plan.install_sdk_via_winget ? "yes" : "no")).c_str());
    KDS_LOG(Info, (std::string("  install_wdk_via_winget: ") + (plan.install_wdk_via_winget ? "yes" : "no")).c_str());

    for (const auto& n : plan.coexistence_notes) {
        KDS_LOG(Info, ("Policy: " + n).c_str());
    }
    for (const auto& n : plan.manual_cleanup_recommendations) {
        KDS_LOG(Info, ("Manual cleanup: " + n).c_str());
    }
    KDS_LOG(Info,
            "Policy: KernelDevSetup does not uninstall Visual Studio products or other development environments "
            "automatically; it prefers reuse, then modify/repair via the official bootstrapper, then side-by-side "
            "installs.");
    KDS_LOG(Info, "=== End compatibility plan ===");
}

} // namespace kds::compat
