// os_detect.h - Operating system capability and support checks for Visual Studio 2022.
#pragma once

#include <cstdint>
#include <string>

namespace kds::os {

enum class Architecture { Unknown, X86, X64, Arm64 };

struct OsInfo {
    std::string product_name;
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0;
    uint32_t ubr = 0; // Update Build Revision
    Architecture arch = Architecture::Unknown;
    bool is_server = false;
    uint32_t product_type = 0; // PRODUCT_* from GetProductInfo
    std::string product_type_label;
    bool is_windows_11 = false;
    bool is_windows_10 = false;
    bool is_s_mode = false;
    bool vs2022_supported = false;
    std::string vs2022_support_detail;
};

// Fills OsInfo using RtlGetVersion, GetNativeSystemInfo, GetProductInfo, and registry probes.
bool DetectOs(OsInfo& out);

// Returns false if the OS cannot reasonably host VS 2022 (sets vs2022_supported and detail).
bool EvaluateVisualStudio2022Support(OsInfo& io);

} // namespace kds::os
