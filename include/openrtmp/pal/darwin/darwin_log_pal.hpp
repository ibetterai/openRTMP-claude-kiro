// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS/iOS) Log PAL Implementation
//
// Uses os_log for platform-native logging integration
// Requirements Covered: 18.5 (Platform-native logging integration - os_log on iOS)

#ifndef OPENRTMP_PAL_DARWIN_DARWIN_LOG_PAL_HPP
#define OPENRTMP_PAL_DARWIN_DARWIN_LOG_PAL_HPP

#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <atomic>
#include <mutex>
#include <vector>
#include <memory>

#if defined(__APPLE__)

namespace openrtmp {
namespace pal {
namespace darwin {

/**
 * @brief Darwin (macOS/iOS) implementation of ILogPAL using os_log.
 *
 * This implementation routes log messages to the Apple Unified Logging system
 * via os_log. Log levels are mapped to os_log types as follows:
 * - Trace/Debug -> OS_LOG_TYPE_DEBUG
 * - Info -> OS_LOG_TYPE_INFO
 * - Warning -> OS_LOG_TYPE_DEFAULT
 * - Error -> OS_LOG_TYPE_ERROR
 * - Critical -> OS_LOG_TYPE_FAULT
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Sink management uses mutex for protection
 * - os_log calls are inherently thread-safe
 */
class DarwinLogPAL : public ILogPAL {
public:
    /**
     * @brief Construct a Darwin log PAL.
     *
     * Creates an os_log handle for the "com.openrtmp" subsystem.
     */
    DarwinLogPAL();

    /**
     * @brief Destructor.
     *
     * Flushes all sinks before destruction.
     */
    ~DarwinLogPAL() override;

    // Non-copyable, non-movable
    DarwinLogPAL(const DarwinLogPAL&) = delete;
    DarwinLogPAL& operator=(const DarwinLogPAL&) = delete;
    DarwinLogPAL(DarwinLogPAL&&) = delete;
    DarwinLogPAL& operator=(DarwinLogPAL&&) = delete;

    // =========================================================================
    // ILogPAL Implementation
    // =========================================================================

    /**
     * @brief Log a message via os_log and registered sinks.
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
     * @brief Log to os_log with appropriate type mapping.
     */
    void logToOsLog(
        LogLevel level,
        const std::string& message,
        const std::string& category
    );

    std::atomic<LogLevel> minLevel_{LogLevel::Debug};
    mutable std::mutex sinksMutex_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;

    // os_log handle (stored as void* to avoid including os/log.h in header)
    void* osLogHandle_{nullptr};
};

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
#endif // OPENRTMP_PAL_DARWIN_DARWIN_LOG_PAL_HPP
