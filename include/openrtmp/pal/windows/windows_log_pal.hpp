// OpenRTMP - Cross-platform RTMP Server
// Windows Log PAL Implementation
//
// Uses OutputDebugStringW for debug output and Windows Event Log for critical events
// Requirements Covered: 18.5 (Platform-native logging integration)

#ifndef OPENRTMP_PAL_WINDOWS_WINDOWS_LOG_PAL_HPP
#define OPENRTMP_PAL_WINDOWS_WINDOWS_LOG_PAL_HPP

#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <atomic>
#include <mutex>
#include <vector>
#include <memory>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace openrtmp {
namespace pal {
namespace windows {

/**
 * @brief Windows implementation of ILogPAL using OutputDebugStringW.
 *
 * This implementation routes log messages to:
 * - OutputDebugStringW for all messages (viewable in debugger and DebugView)
 * - Optionally Windows Event Log for Error/Critical messages
 *
 * Log level mapping:
 * - Trace/Debug -> OutputDebugStringW only
 * - Info -> OutputDebugStringW only
 * - Warning -> OutputDebugStringW only
 * - Error -> OutputDebugStringW + Event Log (EVENTLOG_ERROR_TYPE)
 * - Critical -> OutputDebugStringW + Event Log (EVENTLOG_ERROR_TYPE)
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Sink management uses mutex for protection
 * - OutputDebugStringW is inherently thread-safe
 */
class WindowsLogPAL : public ILogPAL {
public:
    /**
     * @brief Construct a Windows log PAL.
     */
    WindowsLogPAL();

    /**
     * @brief Destructor.
     *
     * Flushes all sinks before destruction.
     */
    ~WindowsLogPAL() override;

    // Non-copyable, non-movable
    WindowsLogPAL(const WindowsLogPAL&) = delete;
    WindowsLogPAL& operator=(const WindowsLogPAL&) = delete;
    WindowsLogPAL(WindowsLogPAL&&) = delete;
    WindowsLogPAL& operator=(WindowsLogPAL&&) = delete;

    // =========================================================================
    // ILogPAL Implementation
    // =========================================================================

    /**
     * @brief Log a message via OutputDebugStringW and registered sinks.
     */
    void log(
        LogLevel level,
        const std::string& message,
        const std::string& category,
        const LogContext& context
    ) override;

    /**
     * @brief Set minimum log level.
     */
    void setMinLevel(LogLevel level) override;

    /**
     * @brief Get current minimum log level.
     */
    LogLevel getMinLevel() const override;

    /**
     * @brief Flush all registered sinks.
     */
    void flush() override;

    /**
     * @brief Add a log sink.
     */
    void addSink(std::shared_ptr<ILogSink> sink) override;

    /**
     * @brief Remove a log sink.
     */
    void removeSink(std::shared_ptr<ILogSink> sink) override;

private:
    /**
     * @brief Log to OutputDebugStringW.
     */
    void logToDebugOutput(
        LogLevel level,
        const std::string& message,
        const std::string& category,
        const LogContext& context
    );

    /**
     * @brief Convert log level to string.
     */
    const char* levelToString(LogLevel level) const;

    /**
     * @brief Convert UTF-8 string to wide string.
     */
    std::wstring utf8ToWide(const std::string& utf8) const;

    std::atomic<LogLevel> minLevel_{LogLevel::Debug};
    mutable std::mutex sinksMutex_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;
};

} // namespace windows
} // namespace pal
} // namespace openrtmp

#endif // _WIN32
#endif // OPENRTMP_PAL_WINDOWS_WINDOWS_LOG_PAL_HPP
