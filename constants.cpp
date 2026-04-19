// constants.cpp
#include "constants.h"

#include <sstream>

namespace kds {

std::string BuildVsConfigComponentListForLog() {
    std::ostringstream oss;
    oss << kVsComponentWorkloadNativeDesktop << "\n"
        << kVsComponentVcToolsX86X64 << "\n"
        << kVsComponentSpectreLibs << "\n"
        << kVsComponentAtl << "\n"
        << kVsComponentMfc << "\n"
        << kVsComponentWin11Sdk26100 << "\n"
        << kVsComponentWindowsDriverKit;
    return oss.str();
}

} // namespace kds
