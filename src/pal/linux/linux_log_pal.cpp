// OpenRTMP - Cross-platform RTMP Server
// Linux/Android Log PAL Implementation

#include "openrtmp/pal/linux/linux_log_pal.hpp"

#if defined(__linux__) || defined(__ANDROID__)

#ifdef __ANDROID__
#include <android/log.h>
#else
#include <syslog.h>
#endif

#include <iostream>
#include <cstdio>

namespace openrtmp {
namespace pal {
namespace linux {

// =============================================================================
// Constructor / Destructor
// =============================================================================

LinuxLogPAL::LinuxLogPAL() {
#ifndef __ANDROID__
    // Open syslog on Linux
    openlog("openrtmp", LOG_PID | LOG_NDELAY, LOG_USER);
    syslogOpened_ = true;
#endif
}

LinuxLogPAL::~LinuxLogPAL() {
    flush();

#ifndef __ANDROID__
    if (syslogOpened_) {
        closelog();
        syslogOpened_ = false;
    }
#endif
}

// =============================================================================
// Logging Operations
// =============================================================================

void LinuxLogPAL::log(
    LogLevel level,
    const std::string& message,
    const std::string& category,
    const LogContext& context
) {
    // Check minimum level
    if (static_cast<uint32_t>(level) < static_cast<uint32_t>(minLevel_.load())) {
        return;
    }

    // Log to platform-native logging system
    logToPlatform(level, message, category);

    // Log to registered sinks
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        for (auto& sink : sinks_) {
            sink->write(level, message, category, context);
        }
    }
}

void LinuxLogPAL::logToPlatform(
    LogLevel level,
    const std::string& message,
    const std::string& category
) {
#ifdef __ANDROID__
    // Log to Android Logcat
    int priority = toAndroidLogPriority(level);
    __android_log_print(priority, category.c_str(), "%s", message.c_str());
#else
    // Log to syslog on Linux
    int priority = toSyslogPriority(level);
    syslog(priority, "[%s] %s", category.c_str(), message.c_str());

    // Also output to stderr for development
    const char* levelStr = "";
    switch (level) {
        case LogLevel::Trace:    levelStr = "TRACE"; break;
        case LogLevel::Debug:    levelStr = "DEBUG"; break;
        case LogLevel::Info:     levelStr = "INFO"; break;
        case LogLevel::Warning:  levelStr = "WARNING"; break;
        case LogLevel::Error:    levelStr = "ERROR"; break;
        case LogLevel::Critical: levelStr = "CRITICAL"; break;
        case LogLevel::Off:      levelStr = "OFF"; break;
    }
    fprintf(stderr, "[%s] [%s] %s\n", levelStr, category.c_str(), message.c_str());
#endif
}

#ifdef __ANDROID__
int LinuxLogPAL::toAndroidLogPriority(LogLevel level) const {
    switch (level) {
        case LogLevel::Trace:
        case LogLevel::Debug:
            return ANDROID_LOG_DEBUG;
        case LogLevel::Info:
            return ANDROID_LOG_INFO;
        case LogLevel::Warning:
            return ANDROID_LOG_WARN;
        case LogLevel::Error:
            return ANDROID_LOG_ERROR;
        case LogLevel::Critical:
            return ANDROID_LOG_FATAL;
        case LogLevel::Off:
        default:
            return ANDROID_LOG_SILENT;
    }
}
#else
int LinuxLogPAL::toSyslogPriority(LogLevel level) const {
    switch (level) {
        case LogLevel::Trace:
        case LogLevel::Debug:
            return LOG_DEBUG;
        case LogLevel::Info:
            return LOG_INFO;
        case LogLevel::Warning:
            return LOG_WARNING;
        case LogLevel::Error:
            return LOG_ERR;
        case LogLevel::Critical:
            return LOG_CRIT;
        case LogLevel::Off:
        default:
            return LOG_DEBUG;
    }
}
#endif

// =============================================================================
// Level Management
// =============================================================================

void LinuxLogPAL::setMinLevel(LogLevel level) {
    minLevel_ = level;
}

LogLevel LinuxLogPAL::getMinLevel() const {
    return minLevel_.load();
}

// =============================================================================
// Sink Management
// =============================================================================

void LinuxLogPAL::flush() {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    for (auto& sink : sinks_) {
        sink->flush();
    }
}

void LinuxLogPAL::addSink(std::shared_ptr<ILogSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.push_back(std::move(sink));
}

void LinuxLogPAL::removeSink(std::shared_ptr<ILogSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.erase(
        std::remove(sinks_.begin(), sinks_.end(), sink),
        sinks_.end()
    );
}

} // namespace linux
} // namespace pal
} // namespace openrtmp

#endif // defined(__linux__) || defined(__ANDROID__)
