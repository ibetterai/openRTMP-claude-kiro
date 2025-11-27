// OpenRTMP - Cross-platform RTMP Server
// Linux/Android Log PAL Implementation
//
// Uses syslog on Linux and Logcat on Android for platform-native logging
// Requirements Covered: 18.5 (Platform-native logging integration - Logcat on Android)

#ifndef OPENRTMP_PAL_LINUX_LINUX_LOG_PAL_HPP
#define OPENRTMP_PAL_LINUX_LINUX_LOG_PAL_HPP

#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <atomic>
#include <mutex>
#include <vector>
#include <memory>

#if defined(__linux__) || defined(__ANDROID__)

namespace openrtmp {
namespace pal {
namespace linux {

/**
 * @brief Linux/Android implementation of ILogPAL.
 *
 * On Linux:
 * - Routes log messages to syslog
 * - Also outputs to stderr for development
 *
 * On Android:
 * - Routes log messages to Logcat via __android_log_print
 * - Log levels are mapped to Android log priorities:
 *   - Trace/Debug -> ANDROID_LOG_DEBUG
 *   - Info -> ANDROID_LOG_INFO
 *   - Warning -> ANDROID_LOG_WARN
 *   - Error -> ANDROID_LOG_ERROR
 *   - Critical -> ANDROID_LOG_FATAL
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Sink management uses mutex for protection
 * - syslog and __android_log_print are inherently thread-safe
 */
class LinuxLogPAL : public ILogPAL {
public:
    /**
     * @brief Construct a Linux/Android log PAL.
     *
     * Opens syslog on Linux with "openrtmp" ident.
     */
    LinuxLogPAL();

    /**
     * @brief Destructor.
     *
     * Flushes all sinks and closes syslog on Linux.
     */
    ~LinuxLogPAL() override;

    // Non-copyable, non-movable
    LinuxLogPAL(const LinuxLogPAL&) = delete;
    LinuxLogPAL& operator=(const LinuxLogPAL&) = delete;
    LinuxLogPAL(LinuxLogPAL&&) = delete;
    LinuxLogPAL& operator=(LinuxLogPAL&&) = delete;

    // =========================================================================
    // ILogPAL Implementation
    // =========================================================================

    /**
     * @brief Log a message via platform logging and registered sinks.
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
     * @brief Log to platform-native logging system.
     *
     * On Linux: syslog
     * On Android: Logcat via __android_log_print
     */
    void logToPlatform(
        LogLevel level,
        const std::string& message,
        const std::string& category
    );

#ifdef __ANDROID__
    /**
     * @brief Convert OpenRTMP LogLevel to Android log priority.
     */
    int toAndroidLogPriority(LogLevel level) const;
#else
    /**
     * @brief Convert OpenRTMP LogLevel to syslog priority.
     */
    int toSyslogPriority(LogLevel level) const;
#endif

    std::atomic<LogLevel> minLevel_{LogLevel::Debug};
    mutable std::mutex sinksMutex_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;

    bool syslogOpened_{false};
};

} // namespace linux
} // namespace pal
} // namespace openrtmp

#endif // defined(__linux__) || defined(__ANDROID__)
#endif // OPENRTMP_PAL_LINUX_LINUX_LOG_PAL_HPP
