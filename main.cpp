// main.cpp - Entry point, elevation handling, and command-line parsing for KernelDevSetup.
#include "constants.h"
#include "installer.h"
#include "logging.h"
#include "process_utils.h"

#include <Windows.h>

#include <cstdio>
#include <io.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

void PrintBanner() {
    std::wcout << L"KernelDevSetup - Windows kernel/driver development environment bootstrap\n";
    std::wcout << L"For legitimate development use only.\n\n";
}

void PrintUsage() {
    std::wcout << L"Usage: KernelDevSetup.exe [options]\n";
    std::wcout << L"Options:\n";
    std::wcout << L"  --dry-run        Log planned actions without running installers.\n";
    std::wcout << L"  --repair         Prefer repair-style reinstalls where supported.\n";
    std::wcout << L"  --force          Force reinstall attempts for winget packages.\n";
    std::wcout << L"  --quiet          Reduce console output (logs are still written).\n";
    std::wcout << L"  --verify-only    Run verification checks only.\n";
    std::wcout << L"  --no-pause       Do not wait for Enter before closing the console window.\n";
    std::wcout << L"  --help           Show this help.\n";
}

bool ParseArgs(int argc, wchar_t** argv, kds::RunOptions& out, std::wstring& error) {
    out = kds::RunOptions{};
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--dry-run") {
            out.dry_run = true;
        } else if (a == L"--repair") {
            out.repair = true;
        } else if (a == L"--force") {
            out.force_reinstall = true;
        } else if (a == L"--quiet") {
            out.quiet = true;
        } else if (a == L"--verify-only") {
            out.verify_only = true;
        } else if (a == L"--no-pause") {
            out.no_pause = true;
        } else {
            error = L"Unknown argument: " + a;
            return false;
        }
    }
    if (out.verify_only && out.dry_run) {
        error = L"--verify-only cannot be combined with --dry-run.";
        return false;
    }
    return true;
}

// Wait so users can read output when launching from Explorer (double-click).
void PauseBeforeExitIfNeeded(const kds::RunOptions& opt) {
    if (opt.quiet || opt.no_pause) {
        return;
    }
    if (GetConsoleWindow() == nullptr) {
        return;
    }
    if (_isatty(_fileno(stdin)) == 0) {
        return;
    }
    fflush(stdout);
    fflush(stderr);
    std::wcout << L"\nPress Enter to close this window..." << std::endl;
    std::wstring line;
    std::getline(std::wcin, line);
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--help" || a == L"-h" || a == L"/?") {
            PrintBanner();
            PrintUsage();
            PauseBeforeExitIfNeeded(kds::RunOptions{});
            return static_cast<int>(kds::ExitCode::Success);
        }
    }

    kds::RunOptions options{};
    std::wstring parse_error;
    if (!ParseArgs(argc, argv, options, parse_error)) {
        std::wcerr << parse_error << L"\n";
        PrintUsage();
        PauseBeforeExitIfNeeded(options);
        return static_cast<int>(kds::ExitCode::InvalidArguments);
    }

    if (!kds::log::GlobalLogger().Initialize(options.quiet)) {
        std::wcerr << L"Failed to initialize logging.\n";
        PauseBeforeExitIfNeeded(options);
        return static_cast<int>(kds::ExitCode::InternalError);
    }

    if (!options.quiet) {
        PrintBanner();
    }

    if (!kds::proc::IsProcessElevatedAdmin()) {
        KDS_LOG(Warning, "Not elevated. Relaunching with Administrator rights...");
        std::wstring args;
        for (int i = 1; i < argc; ++i) {
            if (!args.empty()) {
                args.push_back(L' ');
            }
            args += argv[i];
        }
        if (!kds::proc::RelaunchSelfElevated(args)) {
            KDS_LOG(Error, "Failed to relaunch elevated. Please run as Administrator.");
            PauseBeforeExitIfNeeded(options);
            return static_cast<int>(kds::ExitCode::ElevationFailed);
        }
        // Non-elevated parent exits immediately; give the user time to read the log line.
        if (!options.quiet && !options.no_pause) {
            std::wcout
                << L"\nIf a UAC prompt appeared, approve it to continue in the new window.\n"
                << L"This window will close in 5 seconds...\n"
                << std::flush;
            Sleep(5000);
        }
        return static_cast<int>(kds::ExitCode::Success);
    }

    const kds::ExitCode code = kds::install::Run(options);
    KDS_LOG(Info, ("Exit code: " + std::to_string(static_cast<int>(code))).c_str());
    const int ret = static_cast<int>(code);
    PauseBeforeExitIfNeeded(options);
    return ret;
}
