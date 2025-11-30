// OpenRTMP - Cross-platform RTMP Server
// Log Rotation Component
//
// Provides log file output with configurable rotation policies including
// size-based and time-based rotation triggers, compression of rotated files,
// and integration with platform-native logging systems.
//
// Requirements Covered:
// - 18.4: On desktop platforms, support log output to file with configurable rotation
// - 18.5: On mobile platforms, integrate with platform-native logging systems

#ifndef OPENRTMP_CORE_LOG_ROTATION_HPP
#define OPENRTMP_CORE_LOG_ROTATION_HPP

#include "openrtmp/core/result.hpp"
#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrtmp {
namespace core {

/**
 * @brief Types of log rotation triggers.
 */
enum class RotationType {
    None,       ///< No automatic rotation
    SizeBased,  ///< Rotate when file exceeds size threshold
    TimeBased,  ///< Rotate based on time interval
    Hybrid      ///< Rotate on either size or time trigger
};

/**
 * @brief Error codes for log rotation operations.
 */
enum class LogRotationErrorCode {
    Success = 0,
    FileOpenFailed,
    RotationFailed,
    CompressionFailed,
    PermissionDenied,
    DiskFull,
    InvalidPath,
    Unknown
};

/**
 * @brief Error information for log rotation operations.
 */
struct LogRotationError {
    LogRotationErrorCode code;
    std::string message;

    LogRotationError(LogRotationErrorCode c = LogRotationErrorCode::Unknown,
                     std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

/**
 * @brief Configuration for log rotation policy.
 *
 * Supports:
 * - Size-based rotation (rotate at specified file size)
 * - Time-based rotation (hourly, daily, weekly)
 * - Hybrid rotation (either trigger)
 * - Configurable backup file count
 * - Optional compression of rotated files
 */
class RotationPolicy {
public:
    /**
     * @brief Default constructor with no automatic rotation.
     */
    RotationPolicy();

    /**
     * @brief Get the rotation type.
     */
    RotationType getRotationType() const;

    /**
     * @brief Set the rotation type.
     */
    void setRotationType(RotationType type);

    /**
     * @brief Get the maximum file size for size-based rotation.
     * @return Maximum file size in bytes (0 = no limit)
     */
    uint64_t getMaxFileSize() const;

    /**
     * @brief Set the maximum file size for size-based rotation.
     * @param size Maximum file size in bytes
     */
    void setMaxFileSize(uint64_t size);

    /**
     * @brief Get the rotation interval for time-based rotation.
     * @return Rotation interval
     */
    std::chrono::milliseconds getRotationInterval() const;

    /**
     * @brief Set the rotation interval for time-based rotation.
     * @param interval Rotation interval (hourly, daily, weekly, etc.)
     */
    void setRotationInterval(std::chrono::milliseconds interval);

    /**
     * @brief Get the maximum number of backup files to keep.
     * @return Maximum backup files
     */
    uint32_t getMaxBackupFiles() const;

    /**
     * @brief Set the maximum number of backup files to keep.
     * @param count Maximum backup files (older files are deleted)
     */
    void setMaxBackupFiles(uint32_t count);

    /**
     * @brief Check if compression is enabled for rotated files.
     * @return true if compression is enabled
     */
    bool isCompressionEnabled() const;

    /**
     * @brief Enable or disable compression for rotated files.
     * @param enabled true to enable gzip compression
     */
    void setCompressionEnabled(bool enabled);

    /**
     * @brief Check if timestamped naming is enabled.
     * @return true if using timestamp in rotated file names
     */
    bool isTimestampedNames() const;

    /**
     * @brief Enable or disable timestamped naming for rotated files.
     * @param enabled true to include timestamp in file name
     */
    void setUseTimestampedNames(bool enabled);

private:
    RotationType type_ = RotationType::None;
    uint64_t maxFileSize_ = 0;
    std::chrono::milliseconds rotationInterval_{0};
    uint32_t maxBackupFiles_ = 5;
    bool compressionEnabled_ = false;
    bool useTimestampedNames_ = false;
};

/**
 * @brief Interface for data compression.
 *
 * Used to compress rotated log files for storage efficiency.
 */
class ICompressor {
public:
    virtual ~ICompressor() = default;

    /**
     * @brief Compress data.
     * @param input Input data
     * @param inputSize Size of input data
     * @param output Output buffer for compressed data
     * @return Result indicating success or error
     */
    virtual Result<void, LogRotationError> compress(
        const uint8_t* input,
        size_t inputSize,
        std::vector<uint8_t>& output
    ) = 0;

    /**
     * @brief Compress a file and save to a new location.
     * @param inputPath Path to input file
     * @param outputPath Path for compressed output file
     * @return Result indicating success or error
     */
    virtual Result<void, LogRotationError> compressFile(
        const std::string& inputPath,
        const std::string& outputPath
    ) = 0;
};

/**
 * @brief Create a gzip compressor instance.
 * @return Pointer to ICompressor implementation (caller owns)
 */
ICompressor* createGzipCompressor();

/**
 * @brief File sink for log output with rotation support.
 *
 * Implements ILogSink to provide file-based logging with:
 * - Configurable rotation policies (size, time, hybrid)
 * - Automatic old file cleanup
 * - Optional compression of rotated files
 * - Thread-safe operation
 *
 * ## Usage Example
 * @code
 * RotationPolicy policy;
 * policy.setRotationType(RotationType::SizeBased);
 * policy.setMaxFileSize(10 * 1024 * 1024);  // 10 MB
 * policy.setMaxBackupFiles(5);
 * policy.setCompressionEnabled(true);
 *
 * auto sink = std::make_shared<FileSink>("/var/log/openrtmp.log", policy);
 * logger.addSink(sink);
 * @endcode
 */
class FileSink : public pal::ILogSink {
public:
    /**
     * @brief Construct a file sink without rotation.
     * @param filePath Path to the log file
     */
    explicit FileSink(const std::string& filePath);

    /**
     * @brief Construct a file sink with rotation policy.
     * @param filePath Path to the log file
     * @param policy Rotation policy configuration
     */
    FileSink(const std::string& filePath, const RotationPolicy& policy);

    /**
     * @brief Destructor - flushes and closes the file.
     */
    ~FileSink() override;

    // Non-copyable, non-movable
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;
    FileSink(FileSink&&) = delete;
    FileSink& operator=(FileSink&&) = delete;

    // ILogSink interface
    void write(pal::LogLevel level, const std::string& message,
               const std::string& category, const pal::LogContext& context) override;
    void flush() override;
    std::string getName() const override;

    /**
     * @brief Check if the file is open and writable.
     * @return true if file is open
     */
    bool isOpen() const;

    /**
     * @brief Get the current file size.
     * @return Current file size in bytes
     */
    uint64_t getCurrentFileSize() const;

    /**
     * @brief Force a rotation regardless of policy.
     * @return Result indicating success or error
     */
    Result<void, LogRotationError> forceRotation();

private:
    /**
     * @brief Check if rotation is needed based on policy.
     * @return true if rotation should occur
     */
    bool shouldRotate() const;

    /**
     * @brief Perform the rotation.
     * @return Result indicating success or error
     */
    Result<void, LogRotationError> performRotation();

    /**
     * @brief Rotate numbered backup files (1 -> 2, 2 -> 3, etc).
     */
    void rotateBackupFiles();

    /**
     * @brief Remove old backup files exceeding max count.
     */
    void cleanupOldBackups();

    /**
     * @brief Compress the specified file.
     * @param filePath Path to file to compress
     * @return Result indicating success or error
     */
    Result<void, LogRotationError> compressBackup(const std::string& filePath);

    /**
     * @brief Generate timestamped backup filename.
     * @return Timestamped filename
     */
    std::string generateTimestampedName() const;

    /**
     * @brief Open or reopen the log file.
     * @return true if successful
     */
    bool openFile();

    std::string filePath_;
    RotationPolicy policy_;
    mutable std::mutex mutex_;
    std::ofstream file_;
    uint64_t currentSize_ = 0;
    std::chrono::steady_clock::time_point lastRotation_;
    std::unique_ptr<ICompressor> compressor_;
};

/**
 * @brief Create a platform-specific logger sink.
 *
 * On mobile platforms:
 * - iOS/macOS: Returns os_log sink
 * - Android: Returns Logcat sink
 *
 * On desktop platforms:
 * - Returns a console/stderr sink
 *
 * @return Shared pointer to platform-appropriate log sink
 */
std::shared_ptr<pal::ILogSink> createPlatformLoggerSink();

/**
 * @brief Platform logger sink for os_log integration (Apple platforms).
 *
 * Integrates with Apple's unified logging system for iOS/macOS.
 * Logs are visible in Console.app and can be filtered by subsystem.
 */
class OsLogSink : public pal::ILogSink {
public:
    /**
     * @brief Construct an os_log sink.
     * @param subsystem Subsystem identifier (e.g., "com.openrtmp")
     * @param category Category for filtering (e.g., "streaming")
     */
    OsLogSink(const std::string& subsystem = "com.openrtmp",
              const std::string& category = "default");

    ~OsLogSink() override;

    // ILogSink interface
    void write(pal::LogLevel level, const std::string& message,
               const std::string& category, const pal::LogContext& context) override;
    void flush() override;
    std::string getName() const override;

private:
    void* osLogHandle_ = nullptr;  // os_log_t on Apple platforms
    std::string subsystem_;
    std::string category_;
};

/**
 * @brief Platform logger sink for Logcat integration (Android).
 *
 * Integrates with Android's logging system.
 * Logs are visible in logcat output.
 */
class LogcatSink : public pal::ILogSink {
public:
    /**
     * @brief Construct a Logcat sink.
     * @param tag Log tag for filtering (e.g., "OpenRTMP")
     */
    explicit LogcatSink(const std::string& tag = "OpenRTMP");

    ~LogcatSink() override;

    // ILogSink interface
    void write(pal::LogLevel level, const std::string& message,
               const std::string& category, const pal::LogContext& context) override;
    void flush() override;
    std::string getName() const override;

private:
    std::string tag_;
};

/**
 * @brief Console sink for desktop platforms.
 *
 * Writes logs to stderr with optional coloring.
 */
class ConsoleSink : public pal::ILogSink {
public:
    /**
     * @brief Construct a console sink.
     * @param useColors Enable ANSI color codes for log levels
     */
    explicit ConsoleSink(bool useColors = true);

    ~ConsoleSink() override;

    // ILogSink interface
    void write(pal::LogLevel level, const std::string& message,
               const std::string& category, const pal::LogContext& context) override;
    void flush() override;
    std::string getName() const override;

private:
    bool useColors_;
    mutable std::mutex mutex_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_LOG_ROTATION_HPP
