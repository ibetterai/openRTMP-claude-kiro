// OpenRTMP - Cross-platform RTMP Server
// Platform Abstraction Layer - Logging Interface
//
// This interface abstracts platform-specific logging facilities:
// - os_log on macOS/iOS
// - Event Log on Windows
// - Logcat on Android
// - syslog or file-based on Linux
//
// Requirements Covered: 6.6 (logging abstraction)

#ifndef OPENRTMP_PAL_LOG_PAL_HPP
#define OPENRTMP_PAL_LOG_PAL_HPP

#include "openrtmp/pal/pal_types.hpp"

#include <string>
#include <memory>

namespace openrtmp {
namespace pal {

/**
 * @brief Interface for log output sinks.
 *
 * Log sinks receive formatted log messages and handle output to
 * their respective destinations (console, file, system log, etc.).
 */
class ILogSink {
public:
    virtual ~ILogSink() = default;

    /**
     * @brief Write a log message.
     *
     * @param level Log level of the message
     * @param message Formatted log message
     * @param category Log category (e.g., "Network", "Protocol")
     * @param context Source context (file, line, function)
     */
    virtual void write(
        LogLevel level,
        const std::string& message,
        const std::string& category,
        const LogContext& context
    ) = 0;

    /**
     * @brief Flush any buffered output.
     */
    virtual void flush() = 0;

    /**
     * @brief Get the sink name for debugging.
     */
    virtual std::string getName() const = 0;
};

/**
 * @brief Abstract interface for platform-specific logging.
 *
 * This interface provides a unified API for logging across all supported
 * platforms. It supports:
 * - Multiple log levels (Trace, Debug, Info, Warning, Error, Critical)
 * - Category-based filtering
 * - Structured logging with source context
 * - Multiple output sinks
 *
 * ## Thread Safety
 * - All methods are thread-safe
 * - Log messages from different threads may interleave
 * - Sinks must handle concurrent write calls
 *
 * ## Performance
 * - Logging calls below minLevel are fast no-ops
 * - Message formatting is deferred until needed
 * - Asynchronous sinks recommended for high-throughput scenarios
 *
 * ## Platform Integration
 * The default implementation routes to platform-native logging:
 * - macOS/iOS: os_log (unified logging system)
 * - Windows: OutputDebugString / Event Log
 * - Android: __android_log_print (Logcat)
 * - Linux: syslog or stderr
 *
 * @invariant Messages below minLevel are not processed
 * @invariant All registered sinks receive qualifying messages
 */
class ILogPAL {
public:
    virtual ~ILogPAL() = default;

    // =========================================================================
    // Logging Operations
    // =========================================================================

    /**
     * @brief Log a message.
     *
     * Logs a message at the specified level. If the level is below
     * the minimum level, the call returns immediately without processing.
     *
     * @param level Severity level of the message
     * @param message The log message
     * @param category Category for filtering/routing (e.g., "RTMP", "Network")
     * @param context Source location context
     *
     * @note Thread-safe, may be called from any thread
     * @note Messages are delivered to all registered sinks
     *
     * @code
     * LogContext ctx{__FILE__, __LINE__, __FUNCTION__};
     * logPal->log(LogLevel::Info, "Server started on port 1935", "Server", ctx);
     * @endcode
     */
    virtual void log(
        LogLevel level,
        const std::string& message,
        const std::string& category,
        const LogContext& context
    ) = 0;

    /**
     * @brief Set the minimum log level.
     *
     * Messages below this level are ignored. Default is typically Debug
     * in development builds and Info in release builds.
     *
     * @param level Minimum level to log
     *
     * @note Takes effect immediately for subsequent log calls
     *
     * @code
     * // Reduce logging noise in production
     * logPal->setMinLevel(LogLevel::Warning);
     *
     * // Enable verbose logging for debugging
     * logPal->setMinLevel(LogLevel::Trace);
     * @endcode
     */
    virtual void setMinLevel(LogLevel level) = 0;

    /**
     * @brief Get the current minimum log level.
     *
     * @return Current minimum log level
     */
    virtual LogLevel getMinLevel() const = 0;

    /**
     * @brief Flush all log sinks.
     *
     * Forces all buffered log data to be written to their destinations.
     * Useful before shutdown or when immediate log visibility is needed.
     *
     * @note May block until all sinks have flushed
     */
    virtual void flush() = 0;

    // =========================================================================
    // Sink Management
    // =========================================================================

    /**
     * @brief Add a log sink.
     *
     * Registers a new sink to receive log messages. Multiple sinks may
     * be registered simultaneously.
     *
     * @param sink Sink to add (ownership is shared)
     *
     * @note Thread-safe
     * @note Sink may start receiving messages immediately
     *
     * @code
     * auto fileSink = std::make_shared<FileSink>("/var/log/openrtmp.log");
     * logPal->addSink(fileSink);
     * @endcode
     */
    virtual void addSink(std::shared_ptr<ILogSink> sink) = 0;

    /**
     * @brief Remove a log sink.
     *
     * Unregisters a previously added sink. If the sink is not registered,
     * this call has no effect.
     *
     * @param sink Sink to remove
     *
     * @note Thread-safe
     * @note Sink may still receive in-flight messages briefly
     */
    virtual void removeSink(std::shared_ptr<ILogSink> sink) = 0;
};

// =============================================================================
// Convenience Macros for Logging
// =============================================================================

/**
 * These macros provide convenient shortcuts for logging with automatic
 * source location capture. They require a logger instance to be available.
 *
 * Usage:
 *   OPENRTMP_LOG_INFO(logger, "Server", "Listening on port " << port);
 */

#define OPENRTMP_LOG_CONTEXT() \
    ::openrtmp::pal::LogContext{__FILE__, __LINE__, __FUNCTION__}

#define OPENRTMP_LOG(logger, level, category, message) \
    do { \
        if ((logger) != nullptr) { \
            (logger)->log((level), (message), (category), OPENRTMP_LOG_CONTEXT()); \
        } \
    } while (0)

#define OPENRTMP_LOG_TRACE(logger, category, message) \
    OPENRTMP_LOG(logger, ::openrtmp::pal::LogLevel::Trace, category, message)

#define OPENRTMP_LOG_DEBUG(logger, category, message) \
    OPENRTMP_LOG(logger, ::openrtmp::pal::LogLevel::Debug, category, message)

#define OPENRTMP_LOG_INFO(logger, category, message) \
    OPENRTMP_LOG(logger, ::openrtmp::pal::LogLevel::Info, category, message)

#define OPENRTMP_LOG_WARNING(logger, category, message) \
    OPENRTMP_LOG(logger, ::openrtmp::pal::LogLevel::Warning, category, message)

#define OPENRTMP_LOG_ERROR(logger, category, message) \
    OPENRTMP_LOG(logger, ::openrtmp::pal::LogLevel::Error, category, message)

#define OPENRTMP_LOG_CRITICAL(logger, category, message) \
    OPENRTMP_LOG(logger, ::openrtmp::pal::LogLevel::Critical, category, message)

} // namespace pal
} // namespace openrtmp

#endif // OPENRTMP_PAL_LOG_PAL_HPP
