// logging.cpp
#include "logging.h"

#include "constants.h"

#include <Windows.h>

#include <cstdarg>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace kds::log {

namespace {

std::filesystem::path GetLocalAppDataPath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) {
        return L".";
    }
    return std::filesystem::path(buf);
}

} // namespace

Logger::~Logger() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

bool Logger::Initialize(bool quiet_console) {
    quiet_console_ = quiet_console;

    std::error_code ec;
    std::filesystem::path base = GetLocalAppDataPath() / kAppDataFolderName / kLogsSubfolder;
    std::filesystem::create_directories(base, ec);
    if (ec) {
        return false;
    }

    // Single session log file with timestamp in name.
    auto t = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream name;
    name << "KernelDevSetup_" << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << ".log";

    log_file_path_ = base / name.str();
    file_.open(log_file_path_, std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
        return false;
    }

    Log(Level::Info, "Logger initialized.");
    Log(Level::Info, std::string("Log file: ") + log_file_path_.string());
    return true;
}

const char* Logger::LevelToString(Level level) {
    switch (level) {
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warning:
        return "WARN";
    case Level::Error:
        return "ERROR";
    default:
        return "INFO";
    }
}

std::string Logger::CurrentTimestampUtc() {
    auto t = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

void Logger::Log(Level level, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string line =
        std::string("[") + CurrentTimestampUtc() + "] [" + LevelToString(level) + "] " +
        std::string(message);

    if (file_.is_open()) {
        file_ << line << '\n';
        file_.flush();
    }

    if (!quiet_console_) {
        if (level == Level::Error) {
            std::cerr << line << std::endl;
        } else {
            std::cout << line << std::endl;
        }
    }
}

void Logger::Logf(Level level, const char* fmt, ...) {
    char buffer[8192];
    va_list args;
    va_start(args, fmt);
#ifdef _WIN32
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
#else
    vsnprintf(buffer, sizeof(buffer), fmt, args);
#endif
    va_end(args);
    Log(level, buffer);
}

Logger& GlobalLogger() {
    static Logger instance;
    return instance;
}

} // namespace kds::log
