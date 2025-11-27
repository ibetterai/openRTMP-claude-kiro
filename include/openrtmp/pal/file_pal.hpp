// OpenRTMP - Cross-platform RTMP Server
// Platform Abstraction Layer - File I/O Interface
//
// This interface abstracts platform-specific file I/O operations for
// reading/writing configuration files, logs, and stream recordings.
// Implementations handle platform-specific path formats and APIs.
//
// Requirements Covered: 6.4 (file I/O abstraction)

#ifndef OPENRTMP_PAL_FILE_PAL_HPP
#define OPENRTMP_PAL_FILE_PAL_HPP

#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/buffer.hpp"

#include <string>

namespace openrtmp {
namespace pal {

/**
 * @brief Abstract interface for platform-specific file I/O operations.
 *
 * This interface provides a unified API for file operations across all
 * supported platforms. It handles differences in:
 * - Path separators (/ vs \)
 * - Text mode line endings
 * - File system APIs
 * - Error codes
 *
 * ## Thread Safety
 * - Multiple threads may operate on different files concurrently
 * - Operations on the same file handle are not thread-safe
 * - For concurrent access to same file, use external synchronization
 *
 * ## File Lifecycle
 * 1. Open file with open()
 * 2. Perform read/write/seek operations
 * 3. Close file with close()
 *
 * ## Error Handling
 * - All operations return Result with detailed FileError
 * - Files should be closed even if errors occur during operations
 *
 * @invariant File handles are valid from open() until close()
 * @invariant Position is tracked per-handle for read/write operations
 */
class IFilePAL {
public:
    virtual ~IFilePAL() = default;

    // =========================================================================
    // File Open/Close
    // =========================================================================

    /**
     * @brief Open a file.
     *
     * Opens a file with the specified mode. The mode determines whether
     * the file is opened for reading, writing, or both, and controls
     * behavior for non-existent files.
     *
     * @param path Path to the file
     * @param mode File open mode flags
     *
     * @pre path is a valid, non-empty file path
     *
     * @return FileHandle on success, or FileError on failure
     *
     * Error conditions:
     * - NotFound: File doesn't exist and Create flag not set
     * - PermissionDenied: Insufficient permissions
     * - IsADirectory: Path refers to a directory
     * - TooManyOpenFiles: System file descriptor limit reached
     *
     * @code
     * // Read configuration file
     * auto result = filePal->open("/etc/openrtmp/config.json", FileOpenMode::Read);
     * if (result.isError()) {
     *     log("Failed to open config: " + result.error().path);
     *     return;
     * }
     *
     * FileHandle configFile = result.value();
     * @endcode
     *
     * @code
     * // Create new log file
     * auto result = filePal->open(
     *     "/var/log/openrtmp.log",
     *     FileOpenMode::Create | FileOpenMode::Write | FileOpenMode::Append
     * );
     * @endcode
     */
    virtual core::Result<FileHandle, FileError> open(
        const std::string& path,
        FileOpenMode mode
    ) = 0;

    /**
     * @brief Close a file.
     *
     * Closes the file handle and releases associated resources.
     * Any buffered data is flushed before closing.
     *
     * @param handle File to close
     *
     * @pre handle is a valid file handle
     * @post handle is no longer valid
     *
     * @return Success or FileError on failure
     *
     * @note Should be called even after read/write errors
     */
    virtual core::Result<void, FileError> close(FileHandle handle) = 0;

    // =========================================================================
    // Read/Write Operations
    // =========================================================================

    /**
     * @brief Read data from a file.
     *
     * Reads up to maxBytes from the current file position into the buffer.
     * The file position advances by the number of bytes read.
     *
     * @param handle File to read from
     * @param buffer Buffer to store read data
     * @param maxBytes Maximum number of bytes to read
     *
     * @pre handle is a valid file handle opened for reading
     * @pre maxBytes > 0
     *
     * @return Number of bytes read, or FileError on failure
     *
     * @note Returns 0 at end of file
     * @note May read fewer bytes than requested (partial read)
     *
     * @code
     * core::Buffer buffer;
     * buffer.resize(4096);
     *
     * auto result = filePal->read(file, buffer, 4096);
     * if (result.isError()) {
     *     log("Read error: " + result.error().path);
     *     return;
     * }
     *
     * size_t bytesRead = result.value();
     * if (bytesRead == 0) {
     *     // End of file
     * }
     * @endcode
     */
    virtual core::Result<size_t, FileError> read(
        FileHandle handle,
        core::Buffer& buffer,
        size_t maxBytes
    ) = 0;

    /**
     * @brief Write data to a file.
     *
     * Writes data from the buffer to the file at the current position
     * (or appends if opened with Append flag). The file position advances
     * by the number of bytes written.
     *
     * @param handle File to write to
     * @param data Buffer containing data to write
     *
     * @pre handle is a valid file handle opened for writing
     * @pre data.size() > 0
     *
     * @return Number of bytes written, or FileError on failure
     *
     * Error conditions:
     * - IOError: Write operation failed
     * - DiskFull: No space left on device
     *
     * @note May write fewer bytes than requested (partial write)
     * @note For full writes, check return value and continue if needed
     *
     * @code
     * core::Buffer data;
     * data.append(logMessage.data(), logMessage.size());
     *
     * auto result = filePal->write(logFile, data);
     * if (result.isError()) {
     *     // Handle write error
     * }
     * @endcode
     */
    virtual core::Result<size_t, FileError> write(
        FileHandle handle,
        const core::Buffer& data
    ) = 0;

    // =========================================================================
    // Position Operations
    // =========================================================================

    /**
     * @brief Seek to a position in the file.
     *
     * Sets the file position for the next read or write operation.
     *
     * @param handle File to seek in
     * @param offset Offset from the origin
     * @param origin Reference point for the offset
     *
     * @pre handle is a valid file handle
     *
     * @return New absolute position, or FileError on failure
     *
     * @code
     * // Seek to beginning
     * filePal->seek(file, 0, SeekOrigin::Begin);
     *
     * // Seek to end
     * filePal->seek(file, 0, SeekOrigin::End);
     *
     * // Skip forward 100 bytes
     * filePal->seek(file, 100, SeekOrigin::Current);
     * @endcode
     */
    virtual core::Result<size_t, FileError> seek(
        FileHandle handle,
        int64_t offset,
        SeekOrigin origin
    ) = 0;

    /**
     * @brief Get the current file position.
     *
     * @param handle File to query
     *
     * @pre handle is a valid file handle
     *
     * @return Current position in bytes from start, or FileError on failure
     */
    virtual core::Result<size_t, FileError> tell(FileHandle handle) const = 0;

    /**
     * @brief Get the size of a file.
     *
     * @param handle File to query
     *
     * @pre handle is a valid file handle
     *
     * @return File size in bytes, or FileError on failure
     */
    virtual core::Result<size_t, FileError> size(FileHandle handle) const = 0;

    // =========================================================================
    // Synchronization
    // =========================================================================

    /**
     * @brief Flush buffered data to disk.
     *
     * Ensures all data written to the file is persisted to storage.
     * This may involve synchronizing the file system cache.
     *
     * @param handle File to flush
     *
     * @pre handle is a valid file handle opened for writing
     *
     * @return Success or FileError on failure
     *
     * @note May block until data is written to disk
     * @note Use sparingly as it can impact performance
     */
    virtual core::Result<void, FileError> flush(FileHandle handle) = 0;

    // =========================================================================
    // File System Operations
    // =========================================================================

    /**
     * @brief Check if a file exists.
     *
     * @param path Path to check
     *
     * @return true if file exists, false otherwise, or FileError on failure
     */
    virtual core::Result<bool, FileError> exists(const std::string& path) const = 0;

    /**
     * @brief Remove a file.
     *
     * Deletes the specified file from the file system.
     *
     * @param path Path to the file to remove
     *
     * @return Success or FileError on failure
     *
     * Error conditions:
     * - NotFound: File doesn't exist
     * - PermissionDenied: Insufficient permissions
     * - IsADirectory: Path refers to a directory
     */
    virtual core::Result<void, FileError> remove(const std::string& path) = 0;

    /**
     * @brief Rename or move a file.
     *
     * Renames a file, optionally moving it to a different directory.
     *
     * @param oldPath Current path of the file
     * @param newPath New path for the file
     *
     * @return Success or FileError on failure
     *
     * @note Behavior when newPath exists is platform-dependent
     */
    virtual core::Result<void, FileError> rename(
        const std::string& oldPath,
        const std::string& newPath
    ) = 0;

    /**
     * @brief Create a directory.
     *
     * Creates the specified directory. Parent directories must exist.
     *
     * @param path Path of the directory to create
     *
     * @return Success or FileError on failure
     *
     * Error conditions:
     * - AlreadyExists: Directory already exists
     * - NotFound: Parent directory doesn't exist
     * - PermissionDenied: Insufficient permissions
     */
    virtual core::Result<void, FileError> createDirectory(const std::string& path) = 0;
};

} // namespace pal
} // namespace openrtmp

#endif // OPENRTMP_PAL_FILE_PAL_HPP
