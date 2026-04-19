// verification.h - Post-install environment validation for driver development.
#pragma once

#include <string>
#include <vector>

namespace kds::ui {
class IProgressSink;
}

namespace kds::verify {

struct CheckItem {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct VerificationReport {
    std::vector<CheckItem> items;
    bool all_critical_passed = false;
};

// Runs a series of best-effort checks (filesystem, vswhere, registry).
// When progress is non-null, long-running verification updates the progress UI (indeterminate + labels).
VerificationReport RunVerification(kds::ui::IProgressSink* progress = nullptr);

// Serializes the report as JSON (escaped strings).
std::string ReportToJson(const VerificationReport& r);

// Human-readable multi-line summary.
std::string ReportToText(const VerificationReport& r);

} // namespace kds::verify
