// OpenRTMP - Cross-platform RTMP Server
// Log Rotation Component Implementation

#include "openrtmp/core/log_rotation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

#if defined(__APPLE__)
#include <os/log.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#endif

// Optional zlib support for compression
// OPENRTMP_HAS_ZLIB is set by CMake if zlib is found and linked
#if defined(OPENRTMP_HAS_ZLIB)
#define OPENRTMP_USE_ZLIB 1
#include <zlib.h>
#else
#define OPENRTMP_USE_ZLIB 0
#endif

namespace openrtmp {
namespace core {

// =============================================================================
// RotationPolicy Implementation
// =============================================================================

RotationPolicy::RotationPolicy()
    : type_(RotationType::None)
    , maxFileSize_(0)
    , rotationInterval_(0)
    , maxBackupFiles_(5)
    , compressionEnabled_(false)
    , useTimestampedNames_(false) {
}

RotationType RotationPolicy::getRotationType() const {
    return type_;
}

void RotationPolicy::setRotationType(RotationType type) {
    type_ = type;
}

uint64_t RotationPolicy::getMaxFileSize() const {
    return maxFileSize_;
}

void RotationPolicy::setMaxFileSize(uint64_t size) {
    maxFileSize_ = size;
}

std::chrono::milliseconds RotationPolicy::getRotationInterval() const {
    return rotationInterval_;
}

void RotationPolicy::setRotationInterval(std::chrono::milliseconds interval) {
    rotationInterval_ = interval;
}

uint32_t RotationPolicy::getMaxBackupFiles() const {
    return maxBackupFiles_;
}

void RotationPolicy::setMaxBackupFiles(uint32_t count) {
    maxBackupFiles_ = count;
}

bool RotationPolicy::isCompressionEnabled() const {
    return compressionEnabled_;
}

void RotationPolicy::setCompressionEnabled(bool enabled) {
    compressionEnabled_ = enabled;
}

bool RotationPolicy::isTimestampedNames() const {
    return useTimestampedNames_;
}

void RotationPolicy::setUseTimestampedNames(bool enabled) {
    useTimestampedNames_ = enabled;
}

// =============================================================================
// GzipCompressor Implementation
// =============================================================================

class GzipCompressor : public ICompressor {
public:
    GzipCompressor() = default;
    ~GzipCompressor() override = default;

    Result<void, LogRotationError> compress(
        const uint8_t* input,
        size_t inputSize,
        std::vector<uint8_t>& output) override
    {
#if OPENRTMP_USE_ZLIB
        // Calculate maximum compressed size
        uLongf compressedSize = compressBound(static_cast<uLong>(inputSize));
        output.resize(compressedSize);

        int result = compress2(
            output.data(),
            &compressedSize,
            input,
            static_cast<uLong>(inputSize),
            Z_DEFAULT_COMPRESSION
        );

        if (result != Z_OK) {
            return Result<void, LogRotationError>::error(
                LogRotationError(LogRotationErrorCode::CompressionFailed,
                                "zlib compress2 failed: " + std::to_string(result))
            );
        }

        output.resize(compressedSize);
        return Result<void, LogRotationError>::success();
#else
        // No compression available - just copy
        output.assign(input, input + inputSize);
        return Result<void, LogRotationError>::success();
#endif
    }

    Result<void, LogRotationError> compressFile(
        const std::string& inputPath,
        const std::string& outputPath) override
    {
#if OPENRTMP_USE_ZLIB
        // Read input file
        std::ifstream inFile(inputPath, std::ios::binary);
        if (!inFile) {
            return Result<void, LogRotationError>::error(
                LogRotationError(LogRotationErrorCode::FileOpenFailed,
                                "Cannot open input file: " + inputPath)
            );
        }

        // Read entire file
        std::vector<uint8_t> inputData(
            (std::istreambuf_iterator<char>(inFile)),
            std::istreambuf_iterator<char>()
        );
        inFile.close();

        // Open output file using gzopen
        gzFile gzOut = gzopen(outputPath.c_str(), "wb9");
        if (!gzOut) {
            return Result<void, LogRotationError>::error(
                LogRotationError(LogRotationErrorCode::FileOpenFailed,
                                "Cannot create gzip file: " + outputPath)
            );
        }

        // Write compressed data
        int written = gzwrite(gzOut, inputData.data(), static_cast<unsigned>(inputData.size()));
        gzclose(gzOut);

        if (written == 0 && !inputData.empty()) {
            return Result<void, LogRotationError>::error(
                LogRotationError(LogRotationErrorCode::CompressionFailed,
                                "gzwrite failed for: " + outputPath)
            );
        }

        return Result<void, LogRotationError>::success();
#else
        // No zlib - just copy the file
        try {
            std::filesystem::copy_file(inputPath, outputPath,
                                       std::filesystem::copy_options::overwrite_existing);
            return Result<void, LogRotationError>::success();
        } catch (const std::exception& e) {
            return Result<void, LogRotationError>::error(
                LogRotationError(LogRotationErrorCode::CompressionFailed,
                                std::string("File copy failed: ") + e.what())
            );
        }
#endif
    }
};

ICompressor* createGzipCompressor() {
    return new GzipCompressor();
}

// =============================================================================
// FileSink Implementation
// =============================================================================

FileSink::FileSink(const std::string& filePath)
    : filePath_(filePath)
    , policy_()
    , currentSize_(0)
    , lastRotation_(std::chrono::steady_clock::now())
{
    openFile();
}

FileSink::FileSink(const std::string& filePath, const RotationPolicy& policy)
    : filePath_(filePath)
    , policy_(policy)
    , currentSize_(0)
    , lastRotation_(std::chrono::steady_clock::now())
{
    if (policy_.isCompressionEnabled()) {
        compressor_.reset(createGzipCompressor());
    }
    openFile();
}

FileSink::~FileSink() {
    flush();
    if (file_.is_open()) {
        file_.close();
    }
}

void FileSink::write(pal::LogLevel level, const std::string& message,
                     const std::string& category, const pal::LogContext& context)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!file_.is_open()) {
        return;
    }

    // Check if rotation is needed
    if (shouldRotate()) {
        auto result = performRotation();
        if (result.isError()) {
            // Log rotation failed, but continue writing to current file
            // In production, you might want to log this error elsewhere
        }
    }

    // Write the message with newline
    file_ << message << "\n";
    currentSize_ += message.size() + 1;
}

void FileSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

std::string FileSink::getName() const {
    return "FileSink";
}

bool FileSink::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_.is_open();
}

uint64_t FileSink::getCurrentFileSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentSize_;
}

Result<void, LogRotationError> FileSink::forceRotation() {
    std::lock_guard<std::mutex> lock(mutex_);
    return performRotation();
}

bool FileSink::shouldRotate() const {
    switch (policy_.getRotationType()) {
        case RotationType::None:
            return false;

        case RotationType::SizeBased:
            return policy_.getMaxFileSize() > 0 &&
                   currentSize_ >= policy_.getMaxFileSize();

        case RotationType::TimeBased: {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastRotation_);
            return policy_.getRotationInterval().count() > 0 &&
                   elapsed >= policy_.getRotationInterval();
        }

        case RotationType::Hybrid: {
            // Check both conditions
            bool sizeTriggered = policy_.getMaxFileSize() > 0 &&
                                currentSize_ >= policy_.getMaxFileSize();
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastRotation_);
            bool timeTriggered = policy_.getRotationInterval().count() > 0 &&
                                elapsed >= policy_.getRotationInterval();
            return sizeTriggered || timeTriggered;
        }

        default:
            return false;
    }
}

Result<void, LogRotationError> FileSink::performRotation() {
    // Close current file
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }

    // Generate backup filename
    std::string backupName;
    if (policy_.isTimestampedNames()) {
        backupName = generateTimestampedName();
    } else {
        // Rotate numbered backups first
        rotateBackupFiles();
        backupName = filePath_ + ".1";
    }

    // Rename current file to backup
    try {
        if (std::filesystem::exists(filePath_)) {
            std::filesystem::rename(filePath_, backupName);
        }
    } catch (const std::exception& e) {
        // Try to reopen the original file
        openFile();
        return Result<void, LogRotationError>::error(
            LogRotationError(LogRotationErrorCode::RotationFailed,
                            std::string("Failed to rename log file: ") + e.what())
        );
    }

    // Compress the backup if enabled
    if (policy_.isCompressionEnabled() && compressor_) {
        auto result = compressBackup(backupName);
        if (result.isSuccess()) {
            // Remove uncompressed backup
            try {
                std::filesystem::remove(backupName);
            } catch (...) {
                // Ignore cleanup errors
            }
        }
    }

    // Cleanup old backups
    cleanupOldBackups();

    // Open new log file
    if (!openFile()) {
        return Result<void, LogRotationError>::error(
            LogRotationError(LogRotationErrorCode::FileOpenFailed,
                            "Failed to open new log file after rotation")
        );
    }

    lastRotation_ = std::chrono::steady_clock::now();
    return Result<void, LogRotationError>::success();
}

void FileSink::rotateBackupFiles() {
    // Move .n-1 to .n, .n-2 to .n-1, etc.
    for (int i = static_cast<int>(policy_.getMaxBackupFiles()) - 1; i >= 1; --i) {
        std::string oldName = filePath_ + "." + std::to_string(i);
        std::string newName = filePath_ + "." + std::to_string(i + 1);

        // Also check for compressed versions
        std::string oldNameGz = oldName + ".gz";
        std::string newNameGz = newName + ".gz";

        try {
            if (std::filesystem::exists(oldNameGz)) {
                std::filesystem::rename(oldNameGz, newNameGz);
            } else if (std::filesystem::exists(oldName)) {
                std::filesystem::rename(oldName, newName);
            }
        } catch (...) {
            // Ignore rotation errors for individual files
        }
    }
}

void FileSink::cleanupOldBackups() {
    uint32_t maxBackups = policy_.getMaxBackupFiles();

    // Remove backups exceeding the limit
    for (uint32_t i = maxBackups + 1; i <= maxBackups + 10; ++i) {
        std::string backupName = filePath_ + "." + std::to_string(i);
        std::string backupNameGz = backupName + ".gz";

        try {
            if (std::filesystem::exists(backupNameGz)) {
                std::filesystem::remove(backupNameGz);
            }
            if (std::filesystem::exists(backupName)) {
                std::filesystem::remove(backupName);
            }
        } catch (...) {
            // Ignore cleanup errors
        }
    }

    // Also cleanup timestamped backups if there are too many
    if (policy_.isTimestampedNames()) {
        try {
            std::filesystem::path logPath(filePath_);
            std::filesystem::path logDir = logPath.parent_path();
            std::string logBase = logPath.filename().string();

            std::vector<std::filesystem::path> backups;
            for (const auto& entry : std::filesystem::directory_iterator(logDir)) {
                std::string filename = entry.path().filename().string();
                if (filename.find(logBase + ".20") != std::string::npos) {
                    backups.push_back(entry.path());
                }
            }

            // Sort by modification time (oldest first)
            std::sort(backups.begin(), backups.end(),
                     [](const std::filesystem::path& a, const std::filesystem::path& b) {
                         return std::filesystem::last_write_time(a) <
                                std::filesystem::last_write_time(b);
                     });

            // Remove oldest backups if exceeding limit
            while (backups.size() > maxBackups) {
                std::filesystem::remove(backups.front());
                backups.erase(backups.begin());
            }
        } catch (...) {
            // Ignore cleanup errors
        }
    }
}

Result<void, LogRotationError> FileSink::compressBackup(const std::string& filePath) {
    if (!compressor_) {
        return Result<void, LogRotationError>::error(
            LogRotationError(LogRotationErrorCode::CompressionFailed,
                            "No compressor available")
        );
    }

    std::string compressedPath = filePath + ".gz";
    return compressor_->compressFile(filePath, compressedPath);
}

std::string FileSink::generateTimestampedName() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif

    std::ostringstream oss;
    oss << filePath_ << "."
        << std::put_time(&tm_buf, "%Y%m%d-%H%M%S");

    return oss.str();
}

bool FileSink::openFile() {
    try {
        // Create parent directories if needed
        std::filesystem::path path(filePath_);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        // Open file in append mode
        file_.open(filePath_, std::ios::out | std::ios::app);

        if (file_.is_open()) {
            // Get current file size
            try {
                if (std::filesystem::exists(filePath_)) {
                    currentSize_ = std::filesystem::file_size(filePath_);
                } else {
                    currentSize_ = 0;
                }
            } catch (...) {
                currentSize_ = 0;
            }
            return true;
        }
    } catch (...) {
        // File open failed
    }

    return false;
}

// =============================================================================
// Platform Logger Sink Factory
// =============================================================================

std::shared_ptr<pal::ILogSink> createPlatformLoggerSink() {
#if defined(__APPLE__)
    return std::make_shared<OsLogSink>();
#elif defined(__ANDROID__)
    return std::make_shared<LogcatSink>();
#else
    return std::make_shared<ConsoleSink>();
#endif
}

// =============================================================================
// OsLogSink Implementation
// =============================================================================

OsLogSink::OsLogSink(const std::string& subsystem, const std::string& category)
    : subsystem_(subsystem)
    , category_(category)
{
#if defined(__APPLE__)
    osLogHandle_ = static_cast<void*>(
        os_log_create(subsystem_.c_str(), category_.c_str())
    );
#endif
}

OsLogSink::~OsLogSink() {
    // os_log handles are automatically managed by the system
}

void OsLogSink::write(pal::LogLevel level, const std::string& message,
                      const std::string& category, const pal::LogContext& context)
{
#if defined(__APPLE__)
    if (!osLogHandle_) {
        return;
    }

    os_log_t log = static_cast<os_log_t>(osLogHandle_);

    // Map LogLevel to os_log_type_t
    os_log_type_t osLogType;
    switch (level) {
        case pal::LogLevel::Trace:
        case pal::LogLevel::Debug:
            osLogType = OS_LOG_TYPE_DEBUG;
            break;
        case pal::LogLevel::Info:
            osLogType = OS_LOG_TYPE_INFO;
            break;
        case pal::LogLevel::Warning:
            osLogType = OS_LOG_TYPE_DEFAULT;
            break;
        case pal::LogLevel::Error:
            osLogType = OS_LOG_TYPE_ERROR;
            break;
        case pal::LogLevel::Critical:
            osLogType = OS_LOG_TYPE_FAULT;
            break;
        default:
            osLogType = OS_LOG_TYPE_DEFAULT;
            break;
    }

    // Log with category prefix
    if (!category.empty()) {
        os_log_with_type(log, osLogType, "[%{public}s] %{public}s",
                         category.c_str(), message.c_str());
    } else {
        os_log_with_type(log, osLogType, "%{public}s", message.c_str());
    }
#else
    (void)level;
    (void)message;
    (void)category;
    (void)context;
#endif
}

void OsLogSink::flush() {
    // os_log is unbuffered, nothing to flush
}

std::string OsLogSink::getName() const {
    return "OsLogSink";
}

// =============================================================================
// LogcatSink Implementation
// =============================================================================

LogcatSink::LogcatSink(const std::string& tag)
    : tag_(tag)
{
}

LogcatSink::~LogcatSink() {
}

void LogcatSink::write(pal::LogLevel level, const std::string& message,
                       const std::string& category, const pal::LogContext& context)
{
#if defined(__ANDROID__)
    // Map LogLevel to Android log priority
    int priority;
    switch (level) {
        case pal::LogLevel::Trace:
            priority = ANDROID_LOG_VERBOSE;
            break;
        case pal::LogLevel::Debug:
            priority = ANDROID_LOG_DEBUG;
            break;
        case pal::LogLevel::Info:
            priority = ANDROID_LOG_INFO;
            break;
        case pal::LogLevel::Warning:
            priority = ANDROID_LOG_WARN;
            break;
        case pal::LogLevel::Error:
            priority = ANDROID_LOG_ERROR;
            break;
        case pal::LogLevel::Critical:
            priority = ANDROID_LOG_FATAL;
            break;
        default:
            priority = ANDROID_LOG_DEFAULT;
            break;
    }

    // Format with category if provided
    std::string formattedMessage;
    if (!category.empty()) {
        formattedMessage = "[" + category + "] " + message;
    } else {
        formattedMessage = message;
    }

    __android_log_print(priority, tag_.c_str(), "%s", formattedMessage.c_str());
#else
    (void)level;
    (void)message;
    (void)category;
    (void)context;
#endif
}

void LogcatSink::flush() {
    // Logcat is unbuffered, nothing to flush
}

std::string LogcatSink::getName() const {
    return "LogcatSink";
}

// =============================================================================
// ConsoleSink Implementation
// =============================================================================

ConsoleSink::ConsoleSink(bool useColors)
    : useColors_(useColors)
{
}

ConsoleSink::~ConsoleSink() {
}

void ConsoleSink::write(pal::LogLevel level, const std::string& message,
                        const std::string& category, const pal::LogContext& context)
{
    std::lock_guard<std::mutex> lock(mutex_);

    (void)context;  // Not used for basic console output

    // ANSI color codes
    const char* colorCode = "";
    const char* resetCode = useColors_ ? "\033[0m" : "";

    if (useColors_) {
        switch (level) {
            case pal::LogLevel::Trace:
            case pal::LogLevel::Debug:
                colorCode = "\033[36m";  // Cyan
                break;
            case pal::LogLevel::Info:
                colorCode = "\033[32m";  // Green
                break;
            case pal::LogLevel::Warning:
                colorCode = "\033[33m";  // Yellow
                break;
            case pal::LogLevel::Error:
                colorCode = "\033[31m";  // Red
                break;
            case pal::LogLevel::Critical:
                colorCode = "\033[35m";  // Magenta
                break;
            default:
                colorCode = "";
                break;
        }
    }

    // Format: [LEVEL] [category] message
    std::cerr << colorCode;
    if (!category.empty()) {
        std::cerr << "[" << category << "] ";
    }
    std::cerr << message << resetCode << std::endl;
}

void ConsoleSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cerr.flush();
}

std::string ConsoleSink::getName() const {
    return "ConsoleSink";
}

} // namespace core
} // namespace openrtmp
