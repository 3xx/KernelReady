// os_detect.cpp
#include "os_detect.h"

#include "logging.h"

#include <Windows.h>
#include <VersionHelpers.h>
#include <winternl.h>

#include <sstream>

#pragma comment(lib, "ntdll.lib")

using RtlGetVersionFn = NTSTATUS(WINAPI*)(PRTL_OSVERSIONINFOW);

namespace kds::os {

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

Architecture DetectArch() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return Architecture::X64;
    case PROCESSOR_ARCHITECTURE_ARM64:
        return Architecture::Arm64;
    case PROCESSOR_ARCHITECTURE_INTEL:
        return Architecture::X86;
    default:
        return Architecture::Unknown;
    }
}

bool DetectSMode() {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy", 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (rc != ERROR_SUCCESS) {
        return false;
    }
    DWORD sku = 0;
    DWORD size = sizeof(sku);
    DWORD type = 0;
    rc = RegQueryValueExW(key, L"Sku", nullptr, &type, reinterpret_cast<BYTE*>(&sku), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        return false;
    }
    // 0x2 is commonly reported for S mode; this is best-effort.
    return sku == 2;
}

std::string ProductTypeToLabel(DWORD product_type) {
    switch (product_type) {
    case PRODUCT_PROFESSIONAL:
        return "Professional";
    case PRODUCT_PROFESSIONAL_N:
        return "Professional N";
    case PRODUCT_CORE:
        return "Home";
    case PRODUCT_CORE_N:
        return "Home N";
    case PRODUCT_EDUCATION:
        return "Education";
    case PRODUCT_ENTERPRISE:
        return "Enterprise";
    case PRODUCT_ENTERPRISE_N:
        return "Enterprise N";
    case PRODUCT_STANDARD_SERVER:
        return "Standard Server";
    case PRODUCT_DATACENTER_SERVER:
        return "Datacenter Server";
    default:
        return "Unknown/Other";
    }
}

} // namespace

bool DetectOs(OsInfo& out) {
    out = OsInfo{};

    // RtlGetVersion is not always declared with lean Windows headers; load from ntdll explicitly.
    const RtlGetVersionFn pRtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!pRtlGetVersion) {
        KDS_LOG(Error, "RtlGetVersion not available.");
        return false;
    }

    RTL_OSVERSIONINFOW ver{};
    ver.dwOSVersionInfoSize = sizeof(ver);
    if (pRtlGetVersion(&ver) != 0) {
        KDS_LOG(Error, "RtlGetVersion failed.");
        return false;
    }

    out.major = ver.dwMajorVersion;
    out.minor = ver.dwMinorVersion;
    out.build = ver.dwBuildNumber;

    // UBR from registry (build revision)
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        DWORD ubr = 0;
        DWORD sz = sizeof(ubr);
        DWORD tp = 0;
        if (RegQueryValueExW(hk, L"UBR", nullptr, &tp, reinterpret_cast<BYTE*>(&ubr), &sz) == ERROR_SUCCESS &&
            tp == REG_DWORD) {
            out.ubr = ubr;
        }

        wchar_t pname[256] = {};
        sz = sizeof(pname);
        tp = 0;
        if (RegQueryValueExW(hk, L"ProductName", nullptr, &tp, reinterpret_cast<BYTE*>(pname), &sz) ==
                ERROR_SUCCESS &&
            (tp == REG_SZ || tp == REG_EXPAND_SZ)) {
            out.product_name = WideToUtf8(pname);
        }
        RegCloseKey(hk);
    }

    if (out.product_name.empty()) {
        std::ostringstream oss;
        oss << "Windows " << out.major << "." << out.minor << " Build " << out.build;
        out.product_name = oss.str();
    }

    out.arch = DetectArch();
    out.is_s_mode = DetectSMode();

    DWORD product_type = 0;
    // RTL_OSVERSIONINFOW has no wServicePack*; service pack fields are not required for GetProductInfo on Win10+.
    if (GetProductInfo(ver.dwMajorVersion, ver.dwMinorVersion, 0, 0, &product_type)) {
        out.product_type = product_type;
        out.product_type_label = ProductTypeToLabel(product_type);
    }

    out.is_server = IsWindowsServer();

    // Windows 11 reports major 10 with build >= 22000.
    out.is_windows_11 = (out.major == 10 && out.build >= 22000);
    out.is_windows_10 = (out.major == 10 && out.build < 22000);

    return true;
}

bool EvaluateVisualStudio2022Support(OsInfo& io) {
    io.vs2022_supported = true;
    io.vs2022_support_detail.clear();

    // Visual Studio 2022 requires a 64-bit or ARM64 OS host for the IDE toolchain in typical setups.
    if (io.arch == Architecture::X86) {
        io.vs2022_supported = false;
        io.vs2022_support_detail =
            "32-bit (x86) Windows is not a supported host for Visual Studio 2022 (64-bit IDE).";
        return false;
    }

    if (io.arch != Architecture::X64 && io.arch != Architecture::Arm64) {
        io.vs2022_supported = false;
        io.vs2022_support_detail = "Unknown or unsupported processor architecture for Visual Studio 2022.";
        return false;
    }

    // Windows 10 1909+ is commonly cited (build 18363+). We use a conservative minimum of 19041 (20H1).
    if (io.major < 10) {
        io.vs2022_supported = false;
        io.vs2022_support_detail = "Windows version is older than Windows 10.";
        return false;
    }

    if (io.major == 10 && io.build < 19041) {
        io.vs2022_supported = false;
        io.vs2022_support_detail =
            "Windows 10 build is below 19041 (20H1). Visual Studio 2022 requires a newer Windows 10/11 build.";
        return false;
    }

    if (io.is_s_mode) {
        io.vs2022_supported = false;
        io.vs2022_support_detail =
            "Windows S mode detected. Switch out of S mode to install desktop development tools.";
        return false;
    }

    // Server: VS is supported on Server 2019/2022 with Desktop Experience; headless SKU may be problematic.
    if (io.is_server && io.build < 17763) {
        io.vs2022_supported = false;
        io.vs2022_support_detail = "Windows Server build is too old for Visual Studio 2022.";
        return false;
    }

    io.vs2022_support_detail = "This OS build appears suitable for Visual Studio 2022 (heuristic check).";
    return true;
}

} // namespace kds::os
