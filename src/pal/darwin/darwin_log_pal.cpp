// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS/iOS) Log PAL Implementation

#if defined(__APPLE__)

#include "openrtmp/pal/darwin/darwin_log_pal.hpp"

#include <os/log.h>
#include <algorithm>

namespace openrtmp {
namespace pal {
namespace darwin {

DarwinLogPAL::DarwinLogPAL() {
    // Create os_log handle for OpenRTMP subsystem
    osLogHandle_ = static_cast<void*>(os_log_create("com.openrtmp", "default"));
}

DarwinLogPAL::~DarwinLogPAL() {
    flush();
    // os_log handles are automatically managed by the system
    // No explicit release needed
}

void DarwinLogPAL::log(
    LogLevel level,
    const std::string& message,
    const std::string& category,
    const LogContext& context)
{
    // Check minimum level
    if (static_cast<int>(level) < static_cast<int>(minLevel_.load())) {
        return;
    }

    // Log to os_log
    logToOsLog(level, message, category);

    // Dispatch to sinks
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        for (auto& sink : sinks_) {
            if (sink) {
                sink->write(level, message, category, context);
            }
        }
    }
}

void DarwinLogPAL::logToOsLog(
    LogLevel level,
    const std::string& message,
    const std::string& category)
{
    if (!osLogHandle_) {
        return;
    }

    os_log_t log = static_cast<os_log_t>(osLogHandle_);

    // Map LogLevel to os_log_type_t
    os_log_type_t osLogType;
    switch (level) {
        case LogLevel::Trace:
        case LogLevel::Debug:
            osLogType = OS_LOG_TYPE_DEBUG;
            break;
        case LogLevel::Info:
            osLogType = OS_LOG_TYPE_INFO;
            break;
        case LogLevel::Warning:
            osLogType = OS_LOG_TYPE_DEFAULT;
            break;
        case LogLevel::Error:
            osLogType = OS_LOG_TYPE_ERROR;
            break;
        case LogLevel::Critical:
            osLogType = OS_LOG_TYPE_FAULT;
            break;
        default:
            osLogType = OS_LOG_TYPE_DEFAULT;
            break;
    }

    // Format: [category] message
    // Using %{public}s to make strings visible in Console.app
    if (!category.empty()) {
        os_log_with_type(log, osLogType, "[%{public}s] %{public}s",
                         category.c_str(), message.c_str());
    } else {
        os_log_with_type(log, osLogType, "%{public}s", message.c_str());
    }
}

void DarwinLogPAL::setMinLevel(LogLevel level) {
    minLevel_.store(level);
}

LogLevel DarwinLogPAL::getMinLevel() const {
    return minLevel_.load();
}

void DarwinLogPAL::flush() {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    for (auto& sink : sinks_) {
        if (sink) {
            sink->flush();
        }
    }
}

void DarwinLogPAL::addSink(std::shared_ptr<ILogSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.push_back(std::move(sink));
}

void DarwinLogPAL::removeSink(std::shared_ptr<ILogSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.erase(
        std::remove(sinks_.begin(), sinks_.end(), sink),
        sinks_.end()
    );
}

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
