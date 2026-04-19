// installer.h - High-level installation orchestration for the driver development stack.
#pragma once

#include "constants.h"

namespace kds::install {

// Performs detection, optional installs, and verification according to RunOptions.
ExitCode Run(const RunOptions& options);

} // namespace kds::install
