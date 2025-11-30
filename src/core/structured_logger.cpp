// OpenRTMP - Cross-platform RTMP Server
// Structured Logging Component Implementation

#include "openrtmp/core/structured_logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace openrtmp {
namespace core {

// =============================================================================
// Helper Functions
// =============================================================================

std::string logLevelToString(LogLevelConfig level) {
    switch (level) {
        case LogLevelConfig::Debug:
            return "debug";
        case LogLevelConfig::Info:
            return "info";
        case LogLevelConfig::Warning:
            return "warning";
        case LogLevelConfig::Error:
            return "error";
        default:
            return "info";
    }
}

LogLevelConfig stringToLogLevel(const std::string& str) {
    // Convert to lowercase for case-insensitive comparison
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "debug") {
        return LogLevelConfig::Debug;
    } else if (lower == "info") {
        return LogLevelConfig::Info;
    } else if (lower == "warning" || lower == "warn") {
        return LogLevelConfig::Warning;
    } else if (lower == "error") {
        return LogLevelConfig::Error;
    }

    // Default to Info for unknown values
    return LogLevelConfig::Info;
}

std::string connectionEventTypeToString(ConnectionEventType eventType) {
    switch (eventType) {
        case ConnectionEventType::Connected:
            return "connected";
        case ConnectionEventType::Disconnected:
            return "disconnected";
        case ConnectionEventType::PublishStart:
            return "publish_start";
        case ConnectionEventType::PublishStop:
            return "publish_stop";
        case ConnectionEventType::PlayStart:
            return "play_start";
        case ConnectionEventType::PlayStop:
            return "play_stop";
        default:
            return "unknown";
    }
}

// =============================================================================
// StructuredLogger Implementation
// =============================================================================

StructuredLogger::StructuredLogger()
    : level_(LogLevelConfig::Info)
    , jsonFormat_(false) {
}

StructuredLogger::~StructuredLogger() {
    flush();
}

void StructuredLogger::setLevel(LogLevelConfig level) {
    level_.store(level);
}

LogLevelConfig StructuredLogger::getLevel() const {
    return level_.load();
}

void StructuredLogger::setJsonFormat(bool enabled) {
    jsonFormat_.store(enabled);
}

bool StructuredLogger::isJsonFormat() const {
    return jsonFormat_.load();
}

void StructuredLogger::debug(const std::string& message, const std::string& category) {
    log(LogLevelConfig::Debug, message, category);
}

void StructuredLogger::info(const std::string& message, const std::string& category) {
    log(LogLevelConfig::Info, message, category);
}

void StructuredLogger::warning(const std::string& message, const std::string& category) {
    log(LogLevelConfig::Warning, message, category);
}

void StructuredLogger::error(const std::string& message, const std::string& category) {
    log(LogLevelConfig::Error, message, category);
}

void StructuredLogger::logConnectionEvent(
    ConnectionEventType eventType,
    const ClientInfo& client,
    const std::string& streamKey)
{
    // Connection events are logged at Info level
    if (static_cast<int>(LogLevelConfig::Info) < static_cast<int>(level_.load())) {
        return;
    }

    std::string formattedMessage;
    std::string category = "Connection";

    if (jsonFormat_.load()) {
        std::ostringstream oss;
        oss << "{";
        oss << "\"timestamp\":\"" << getTimestamp() << "\"";
        oss << ",\"level\":\"info\"";
        oss << ",\"category\":\"" << category << "\"";
        oss << ",\"event\":\"" << connectionEventTypeToString(eventType) << "\"";
        oss << ",\"client_ip\":\"" << escapeJson(client.ip) << "\"";
        oss << ",\"client_port\":" << client.port;
        if (!client.userAgent.empty()) {
            oss << ",\"user_agent\":\"" << escapeJson(client.userAgent) << "\"";
        }
        if (!streamKey.empty()) {
            oss << ",\"stream_key\":\"" << escapeJson(streamKey) << "\"";
        }
        oss << "}";
        formattedMessage = oss.str();
    } else {
        std::ostringstream oss;
        oss << "[" << getTimestamp() << "] ";
        oss << "[INFO] ";
        oss << "[" << category << "] ";
        oss << "Event: " << connectionEventTypeToString(eventType);
        oss << ", Client: " << client.ip << ":" << client.port;
        if (!client.userAgent.empty()) {
            oss << ", UserAgent: " << client.userAgent;
        }
        if (!streamKey.empty()) {
            oss << ", StreamKey: " << streamKey;
        }
        formattedMessage = oss.str();
    }

    // Dispatch to sinks
    pal::LogContext palContext;
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        for (auto& sink : sinks_) {
            if (sink) {
                sink->write(pal::LogLevel::Info, formattedMessage, category, palContext);
            }
        }
    }
}

void StructuredLogger::errorWithContext(
    const std::string& message,
    const LogContext& context,
    const std::string& category)
{
    // Error with context always logs at Error level
    if (static_cast<int>(LogLevelConfig::Error) < static_cast<int>(level_.load())) {
        return;
    }

    std::string formattedMessage = formatMessage(LogLevelConfig::Error, message, category, &context);

    // Dispatch to sinks
    pal::LogContext palContext;
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        for (auto& sink : sinks_) {
            if (sink) {
                sink->write(pal::LogLevel::Error, formattedMessage, category, palContext);
            }
        }
    }
}

void StructuredLogger::addSink(std::shared_ptr<pal::ILogSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.push_back(std::move(sink));
}

void StructuredLogger::removeSink(std::shared_ptr<pal::ILogSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.erase(
        std::remove(sinks_.begin(), sinks_.end(), sink),
        sinks_.end()
    );
}

void StructuredLogger::flush() {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    for (auto& sink : sinks_) {
        if (sink) {
            sink->flush();
        }
    }
}

void StructuredLogger::log(LogLevelConfig level, const std::string& message, const std::string& category) {
    // Check minimum level
    if (static_cast<int>(level) < static_cast<int>(level_.load())) {
        return;
    }

    std::string formattedMessage = formatMessage(level, message, category);

    // Dispatch to sinks
    pal::LogContext palContext;
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        for (auto& sink : sinks_) {
            if (sink) {
                sink->write(toPalLogLevel(level), formattedMessage, category, palContext);
            }
        }
    }
}

std::string StructuredLogger::formatMessage(
    LogLevelConfig level,
    const std::string& message,
    const std::string& category,
    const LogContext* context)
{
    if (jsonFormat_.load()) {
        return formatJson(level, message, category, context);
    } else {
        return formatPlainText(level, message, category);
    }
}

std::string StructuredLogger::formatJson(
    LogLevelConfig level,
    const std::string& message,
    const std::string& category,
    const LogContext* context)
{
    std::ostringstream oss;
    oss << "{";
    oss << "\"timestamp\":\"" << getTimestamp() << "\"";
    oss << ",\"level\":\"" << logLevelToString(level) << "\"";
    oss << ",\"category\":\"" << escapeJson(category) << "\"";
    oss << ",\"message\":\"" << escapeJson(message) << "\"";

    // Add context fields if present
    if (context) {
        if (!context->streamKey.empty()) {
            oss << ",\"stream_key\":\"" << escapeJson(context->streamKey) << "\"";
        }
        if (!context->clientIP.empty()) {
            oss << ",\"client_ip\":\"" << escapeJson(context->clientIP) << "\"";
        }
        if (context->errorCode != 0) {
            oss << ",\"error_code\":" << context->errorCode;
        }
        if (context->sessionId != 0) {
            oss << ",\"session_id\":" << context->sessionId;
        }
    }

    oss << "}";
    return oss.str();
}

std::string StructuredLogger::formatPlainText(
    LogLevelConfig level,
    const std::string& message,
    const std::string& category)
{
    std::ostringstream oss;
    oss << "[" << getTimestamp() << "] ";
    oss << "[" << logLevelToString(level) << "] ";
    oss << "[" << category << "] ";
    oss << message;
    return oss.str();
}

std::string StructuredLogger::getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf;
#if defined(_WIN32)
    gmtime_s(&tm_buf, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    oss << "." << std::setfill('0') << std::setw(3) << ms.count();
    oss << "Z";
    return oss.str();
}

std::string StructuredLogger::escapeJson(const std::string& str) {
    std::ostringstream oss;
    for (char c : str) {
        switch (c) {
            case '"':
                oss << "\\\"";
                break;
            case '\\':
                oss << "\\\\";
                break;
            case '\n':
                oss << "\\n";
                break;
            case '\r':
                oss << "\\r";
                break;
            case '\t':
                oss << "\\t";
                break;
            case '\b':
                oss << "\\b";
                break;
            case '\f':
                oss << "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Control characters - escape as \uXXXX
                    oss << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                        << static_cast<int>(c);
                } else {
                    oss << c;
                }
                break;
        }
    }
    return oss.str();
}

pal::LogLevel StructuredLogger::toPalLogLevel(LogLevelConfig level) {
    switch (level) {
        case LogLevelConfig::Debug:
            return pal::LogLevel::Debug;
        case LogLevelConfig::Info:
            return pal::LogLevel::Info;
        case LogLevelConfig::Warning:
            return pal::LogLevel::Warning;
        case LogLevelConfig::Error:
            return pal::LogLevel::Error;
        default:
            return pal::LogLevel::Info;
    }
}

} // namespace core
} // namespace openrtmp
