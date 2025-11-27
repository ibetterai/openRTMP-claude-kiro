// OpenRTMP - Cross-platform RTMP Server
// Windows Log PAL Implementation

#if defined(_WIN32)

#include "openrtmp/pal/windows/windows_log_pal.hpp"

#include <sstream>

namespace openrtmp {
namespace pal {
namespace windows {

// =============================================================================
// WindowsLogPAL Implementation
// =============================================================================

WindowsLogPAL::WindowsLogPAL() = default;

WindowsLogPAL::~WindowsLogPAL() {
    flush();
}

const char* WindowsLogPAL::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::Trace:    return "TRACE";
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warning:  return "WARN";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRIT";
        default:                 return "UNKNOWN";
    }
}

std::wstring WindowsLogPAL::utf8ToWide(const std::string& utf8) const {
    if (utf8.empty()) {
        return std::wstring();
    }

    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                       static_cast<int>(utf8.length()),
                                       nullptr, 0);
    if (wideLen <= 0) {
        return std::wstring();
    }

    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                       static_cast<int>(utf8.length()),
                       &wide[0], wideLen);

    return wide;
}

void WindowsLogPAL::log(
    LogLevel level,
    const std::string& message,
    const std::string& category,
    const LogContext& context)
{
    // Check minimum level
    if (static_cast<int>(level) < static_cast<int>(minLevel_.load())) {
        return;
    }

    // Log to debug output
    logToDebugOutput(level, message, category, context);

    // Log to sinks
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        for (auto& sink : sinks_) {
            sink->write(level, message, category, context);
        }
    }
}

void WindowsLogPAL::logToDebugOutput(
    LogLevel level,
    const std::string& message,
    const std::string& category,
    const LogContext& context)
{
    std::ostringstream oss;

    // Format: [LEVEL] [Category] message (file:line)
    oss << "[" << levelToString(level) << "]";

    if (!category.empty()) {
        oss << " [" << category << "]";
    }

    oss << " " << message;

    // Add context if available
    if (context.file != nullptr && context.line > 0) {
        // Extract just the filename from full path
        const char* filename = context.file;
        const char* lastSlash = filename;
        while (*filename) {
            if (*filename == '/' || *filename == '\\') {
                lastSlash = filename + 1;
            }
            filename++;
        }
        oss << " (" << lastSlash << ":" << context.line << ")";
    }

    oss << "\n";

    // Convert to wide string and output
    std::wstring wideMsg = utf8ToWide(oss.str());
    OutputDebugStringW(wideMsg.c_str());
}

void WindowsLogPAL::setMinLevel(LogLevel level) {
    minLevel_.store(level);
}

LogLevel WindowsLogPAL::getMinLevel() const {
    return minLevel_.load();
}

void WindowsLogPAL::flush() {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    for (auto& sink : sinks_) {
        sink->flush();
    }
}

void WindowsLogPAL::addSink(std::shared_ptr<ILogSink> sink) {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.push_back(std::move(sink));
}

void WindowsLogPAL::removeSink(std::shared_ptr<ILogSink> sink) {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.erase(
        std::remove(sinks_.begin(), sinks_.end(), sink),
        sinks_.end()
    );
}

} // namespace windows
} // namespace pal
} // namespace openrtmp

#endif // _WIN32
