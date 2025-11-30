// OpenRTMP - Cross-platform RTMP Server
// Structured Logging Component
//
// Provides structured logging with JSON support for log aggregation systems.
// Routes to platform-native logging on mobile platforms.
//
// Requirements Covered:
// - 18.1: Support configurable log levels (debug, info, warning, error)
// - 18.2: Log all connection events with timestamps and client details
// - 18.6: Support JSON structured log format for aggregation systems
// - 18.7: Include context in error logs (stream keys, client IPs, error codes)

#ifndef OPENRTMP_CORE_STRUCTURED_LOGGER_HPP
#define OPENRTMP_CORE_STRUCTURED_LOGGER_HPP

#include "openrtmp/core/types.hpp"
#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrtmp {
namespace core {

/**
 * @brief Configurable log levels per requirement 18.1.
 *
 * Four supported levels: debug, info, warning, error.
 * Messages below the configured level are filtered out.
 */
enum class LogLevelConfig {
    Debug = 0,    ///< Detailed debugging information
    Info = 1,     ///< Informational messages about normal operation
    Warning = 2,  ///< Warning conditions that should be addressed
    Error = 3     ///< Error conditions that affect operation
};

/**
 * @brief Convert log level to string representation.
 */
std::string logLevelToString(LogLevelConfig level);

/**
 * @brief Convert string to log level (case-insensitive).
 * @param str String representation of log level
 * @return Corresponding LogLevelConfig, defaults to Info for unknown
 */
LogLevelConfig stringToLogLevel(const std::string& str);

/**
 * @brief Connection event types for logging per requirement 18.2.
 *
 * Covers all connection lifecycle events: connect, disconnect,
 * publish start/stop, play start/stop.
 */
enum class ConnectionEventType {
    Connected,       ///< Client connected
    Disconnected,    ///< Client disconnected
    PublishStart,    ///< Client started publishing
    PublishStop,     ///< Client stopped publishing
    PlayStart,       ///< Client started playback
    PlayStop         ///< Client stopped playback
};

/**
 * @brief Convert connection event type to string representation.
 */
std::string connectionEventTypeToString(ConnectionEventType eventType);

/**
 * @brief Context information for structured error logs per requirement 18.7.
 *
 * Contains contextual data that helps diagnose issues:
 * - Stream keys
 * - Client IPs
 * - Error codes
 * - Session IDs
 */
struct LogContext {
    std::string streamKey;   ///< Stream key (e.g., "live/mystream")
    std::string clientIP;    ///< Client IP address
    int32_t errorCode = 0;   ///< Error code for the operation
    uint64_t sessionId = 0;  ///< Session identifier

    LogContext() = default;
};

/**
 * @brief Structured logger with JSON format support.
 *
 * This component provides structured logging functionality that supports:
 * - Configurable log levels (debug, info, warning, error)
 * - ISO 8601 timestamps
 * - JSON structured format for log aggregation systems
 * - Contextual error logging with stream keys, client IPs, error codes
 * - Thread-safe operation
 * - Multiple sink support (file, console, platform-native)
 *
 * ## Thread Safety
 * All methods are thread-safe. Log messages from different threads may
 * interleave but will not corrupt data structures.
 *
 * ## Platform Integration
 * When running on mobile platforms, logs can be routed to platform-native
 * logging systems (os_log on iOS, Logcat on Android) through sink adapters.
 *
 * ## Usage Example
 * @code
 * auto logger = std::make_shared<StructuredLogger>();
 * logger->setLevel(LogLevelConfig::Info);
 * logger->setJsonFormat(true);
 *
 * // Basic logging
 * logger->info("Server started", "Server");
 *
 * // Connection event logging
 * ClientInfo client;
 * client.ip = "192.168.1.100";
 * logger->logConnectionEvent(ConnectionEventType::Connected, client);
 *
 * // Error with context
 * LogContext ctx;
 * ctx.streamKey = "live/stream1";
 * ctx.clientIP = "192.168.1.100";
 * ctx.errorCode = 1001;
 * logger->errorWithContext("Stream authentication failed", ctx);
 * @endcode
 */
class StructuredLogger {
public:
    /**
     * @brief Default constructor.
     *
     * Creates a logger with Info level and non-JSON format.
     */
    StructuredLogger();

    /**
     * @brief Destructor.
     *
     * Flushes all sinks before destruction.
     */
    ~StructuredLogger();

    // Non-copyable, non-movable for thread safety
    StructuredLogger(const StructuredLogger&) = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;
    StructuredLogger(StructuredLogger&&) = delete;
    StructuredLogger& operator=(StructuredLogger&&) = delete;

    // =========================================================================
    // Log Level Configuration (Requirement 18.1)
    // =========================================================================

    /**
     * @brief Set the minimum log level.
     *
     * Messages below this level are filtered out.
     *
     * @param level Minimum level to log
     */
    void setLevel(LogLevelConfig level);

    /**
     * @brief Get the current minimum log level.
     *
     * @return Current minimum log level
     */
    LogLevelConfig getLevel() const;

    // =========================================================================
    // JSON Format Configuration (Requirement 18.6)
    // =========================================================================

    /**
     * @brief Enable or disable JSON structured format.
     *
     * When enabled, log messages are formatted as JSON objects with
     * timestamp, level, message, and optional context fields.
     *
     * @param enabled true to enable JSON format
     */
    void setJsonFormat(bool enabled);

    /**
     * @brief Check if JSON format is enabled.
     *
     * @return true if JSON format is enabled
     */
    bool isJsonFormat() const;

    // =========================================================================
    // Basic Logging Methods
    // =========================================================================

    /**
     * @brief Log a debug-level message.
     *
     * @param message Log message
     * @param category Log category (default: "OpenRTMP")
     */
    void debug(const std::string& message, const std::string& category = "OpenRTMP");

    /**
     * @brief Log an info-level message.
     *
     * @param message Log message
     * @param category Log category (default: "OpenRTMP")
     */
    void info(const std::string& message, const std::string& category = "OpenRTMP");

    /**
     * @brief Log a warning-level message.
     *
     * @param message Log message
     * @param category Log category (default: "OpenRTMP")
     */
    void warning(const std::string& message, const std::string& category = "OpenRTMP");

    /**
     * @brief Log an error-level message.
     *
     * @param message Log message
     * @param category Log category (default: "OpenRTMP")
     */
    void error(const std::string& message, const std::string& category = "OpenRTMP");

    // =========================================================================
    // Connection Event Logging (Requirement 18.2)
    // =========================================================================

    /**
     * @brief Log a connection event with client details.
     *
     * Logs connection lifecycle events with timestamps and client information
     * including IP address, port, and user agent.
     *
     * @param eventType Type of connection event
     * @param client Client information
     * @param streamKey Optional stream key (for publish/play events)
     */
    void logConnectionEvent(
        ConnectionEventType eventType,
        const ClientInfo& client,
        const std::string& streamKey = ""
    );

    // =========================================================================
    // Contextual Error Logging (Requirement 18.7)
    // =========================================================================

    /**
     * @brief Log an error with full context information.
     *
     * Includes stream key, client IP, error code, and session ID
     * in the log output for diagnostic purposes.
     *
     * @param message Error message
     * @param context Context information
     * @param category Log category (default: "OpenRTMP")
     */
    void errorWithContext(
        const std::string& message,
        const LogContext& context,
        const std::string& category = "OpenRTMP"
    );

    // =========================================================================
    // Sink Management
    // =========================================================================

    /**
     * @brief Add a log sink.
     *
     * Sinks receive formatted log messages. Multiple sinks can be registered.
     *
     * @param sink Sink to add
     */
    void addSink(std::shared_ptr<pal::ILogSink> sink);

    /**
     * @brief Remove a log sink.
     *
     * @param sink Sink to remove
     */
    void removeSink(std::shared_ptr<pal::ILogSink> sink);

    /**
     * @brief Flush all sinks.
     *
     * Ensures buffered log data is written to destinations.
     */
    void flush();

private:
    /**
     * @brief Internal logging method.
     *
     * @param level Log level
     * @param message Message to log
     * @param category Log category
     */
    void log(LogLevelConfig level, const std::string& message, const std::string& category);

    /**
     * @brief Format a log message.
     *
     * @param level Log level
     * @param message Message content
     * @param category Log category
     * @param context Optional context (for contextual logs)
     * @return Formatted message string
     */
    std::string formatMessage(
        LogLevelConfig level,
        const std::string& message,
        const std::string& category,
        const LogContext* context = nullptr
    );

    /**
     * @brief Format as JSON.
     *
     * @param level Log level
     * @param message Message content
     * @param category Log category
     * @param context Optional context
     * @return JSON-formatted string
     */
    std::string formatJson(
        LogLevelConfig level,
        const std::string& message,
        const std::string& category,
        const LogContext* context = nullptr
    );

    /**
     * @brief Format as plain text.
     *
     * @param level Log level
     * @param message Message content
     * @param category Log category
     * @return Plain text formatted string
     */
    std::string formatPlainText(
        LogLevelConfig level,
        const std::string& message,
        const std::string& category
    );

    /**
     * @brief Get current timestamp in ISO 8601 format.
     *
     * @return ISO 8601 timestamp string
     */
    std::string getTimestamp();

    /**
     * @brief Escape a string for JSON output.
     *
     * @param str String to escape
     * @return JSON-escaped string
     */
    static std::string escapeJson(const std::string& str);

    /**
     * @brief Convert LogLevelConfig to pal::LogLevel.
     *
     * @param level Configuration level
     * @return PAL log level
     */
    static pal::LogLevel toPalLogLevel(LogLevelConfig level);

    std::atomic<LogLevelConfig> level_{LogLevelConfig::Info};
    std::atomic<bool> jsonFormat_{false};
    mutable std::mutex sinksMutex_;
    std::vector<std::shared_ptr<pal::ILogSink>> sinks_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_STRUCTURED_LOGGER_HPP
