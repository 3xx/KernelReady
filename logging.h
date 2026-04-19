// logging.h - Thread-safe file + console logging.
#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace kds::log {

enum class Level { Debug, Info, Warning, Error };

class Logger {
public:
    Logger() = default;
    ~Logger();

    // Initializes log file under %LOCALAPPDATA%\KernelDevSetup\Logs.
    // Creates directories as needed. Returns false if file cannot be opened.
    bool Initialize(bool quiet_console);

    void SetQuietConsole(bool quiet) { quiet_console_ = quiet; }

    void Log(Level level, std::string_view message);
    void Logf(Level level, const char* fmt, ...);

    const std::filesystem::path& LogFilePath() const { return log_file_path_; }

private:
    std::mutex mutex_;
    std::ofstream file_;
    std::filesystem::path log_file_path_;
    bool quiet_console_ = false;

    static const char* LevelToString(Level level);
    static std::string CurrentTimestampUtc();
};

// Global logger used by the application.
Logger& GlobalLogger();

} // namespace kds::log

#define KDS_LOG(level, msg) ::kds::log::GlobalLogger().Log(::kds::log::Level::level, msg)
