// OpenRTMP - Cross-platform RTMP Server
// Platform Abstraction Layer - Common Types
//
// This file defines common types, handles, errors, and callbacks used across
// all PAL interfaces. It provides platform-independent abstractions for
// operating system resources.

#ifndef OPENRTMP_PAL_PAL_TYPES_HPP
#define OPENRTMP_PAL_PAL_TYPES_HPP

#include <cstdint>
#include <string>
#include <chrono>
#include <functional>
#include <memory>

namespace openrtmp {

// Forward declarations
namespace core {
class Buffer;
template<typename T, typename E> class Result;
}

namespace pal {

// =============================================================================
// Handle Types
// =============================================================================

/**
 * @brief Platform-independent socket handle.
 *
 * Wraps the native socket descriptor (int on Unix, SOCKET on Windows).
 * Using a struct provides type safety and prevents implicit conversions.
 */
struct SocketHandle {
    uint64_t value;

    bool operator==(const SocketHandle& other) const { return value == other.value; }
    bool operator!=(const SocketHandle& other) const { return value != other.value; }
};

/**
 * @brief Platform-independent thread handle.
 */
struct ThreadHandle {
    uint64_t value;

    bool operator==(const ThreadHandle& other) const { return value == other.value; }
    bool operator!=(const ThreadHandle& other) const { return value != other.value; }
};

/**
 * @brief Platform-independent thread pool handle.
 */
struct ThreadPoolHandle {
    uint64_t value;

    bool operator==(const ThreadPoolHandle& other) const { return value == other.value; }
    bool operator!=(const ThreadPoolHandle& other) const { return value != other.value; }
};

/**
 * @brief Platform-independent mutex handle.
 */
struct MutexHandle {
    uint64_t value;

    bool operator==(const MutexHandle& other) const { return value == other.value; }
    bool operator!=(const MutexHandle& other) const { return value != other.value; }
};

/**
 * @brief Platform-independent condition variable handle.
 */
struct ConditionVariableHandle {
    uint64_t value;

    bool operator==(const ConditionVariableHandle& other) const { return value == other.value; }
    bool operator!=(const ConditionVariableHandle& other) const { return value != other.value; }
};

/**
 * @brief Platform-independent timer handle.
 */
struct TimerHandle {
    uint64_t value;

    bool operator==(const TimerHandle& other) const { return value == other.value; }
    bool operator!=(const TimerHandle& other) const { return value != other.value; }
};

/**
 * @brief Platform-independent file handle.
 */
struct FileHandle {
    uint64_t value;

    bool operator==(const FileHandle& other) const { return value == other.value; }
    bool operator!=(const FileHandle& other) const { return value != other.value; }
};

/**
 * @brief Thread identifier type.
 */
struct ThreadId {
    uint64_t value;

    bool operator==(const ThreadId& other) const { return value == other.value; }
    bool operator!=(const ThreadId& other) const { return value != other.value; }
};

// =============================================================================
// Invalid Handle Constants
// =============================================================================

constexpr SocketHandle INVALID_SOCKET_HANDLE{0};
constexpr ThreadHandle INVALID_THREAD_HANDLE{0};
constexpr ThreadPoolHandle INVALID_THREAD_POOL_HANDLE{0};
constexpr MutexHandle INVALID_MUTEX_HANDLE{0};
constexpr ConditionVariableHandle INVALID_CONDITION_VARIABLE_HANDLE{0};
constexpr TimerHandle INVALID_TIMER_HANDLE{0};
constexpr FileHandle INVALID_FILE_HANDLE{0};

// =============================================================================
// Error Codes
// =============================================================================

/**
 * @brief Network operation error codes.
 */
enum class NetworkErrorCode : uint32_t {
    Success = 0,
    Unknown = 1,

    // Initialization errors
    InitializationFailed = 100,
    AlreadyInitialized = 101,
    NotInitialized = 102,

    // Socket errors
    SocketCreationFailed = 200,
    BindFailed = 201,
    ListenFailed = 202,
    AcceptFailed = 203,
    ConnectFailed = 204,
    ConnectionFailed = 205,
    ConnectionReset = 206,
    ConnectionRefused = 207,
    ConnectionClosed = 208,

    // I/O errors
    ReadFailed = 300,
    WriteFailed = 301,
    WouldBlock = 302,
    Timeout = 303,
    Interrupted = 304,

    // Address errors
    AddressInUse = 400,
    AddressNotAvailable = 401,
    InvalidAddress = 402,
    HostUnreachable = 403,
    NetworkUnreachable = 404,

    // Resource errors
    TooManyOpenFiles = 500,
    OutOfMemory = 501,
    ResourceExhausted = 502,

    // Permission errors
    PermissionDenied = 600,

    // Event loop errors
    EventLoopError = 700,
};

/**
 * @brief Thread operation error codes.
 */
enum class ThreadErrorCode : uint32_t {
    Success = 0,
    Unknown = 1,

    // Creation errors
    CreationFailed = 100,
    InvalidHandle = 101,

    // Resource errors
    ResourceExhausted = 200,
    OutOfMemory = 201,

    // Operation errors
    JoinFailed = 300,
    DetachFailed = 301,
    DeadlockDetected = 302,

    // Synchronization errors
    MutexCreationFailed = 400,
    MutexLockFailed = 401,
    ConditionVariableError = 402,

    // Thread pool errors
    PoolCreationFailed = 500,
    PoolShutdown = 501,
    WorkQueueFull = 502,
};

/**
 * @brief Timer operation error codes.
 */
enum class TimerErrorCode : uint32_t {
    Success = 0,
    Unknown = 1,
    CreationFailed = 100,
    InvalidHandle = 101,
    CancellationFailed = 102,
    AlreadyCancelled = 103,
};

/**
 * @brief File operation error codes.
 */
enum class FileErrorCode : uint32_t {
    Success = 0,
    Unknown = 1,

    // Open/Close errors
    NotFound = 100,
    AlreadyExists = 101,
    OpenFailed = 102,
    CloseFailed = 103,

    // Permission errors
    PermissionDenied = 200,
    AccessDenied = 201,

    // I/O errors
    ReadError = 300,
    WriteError = 301,
    IOError = 302,
    EndOfFile = 303,

    // Seek errors
    SeekError = 400,
    InvalidPosition = 401,

    // Path errors
    InvalidPath = 500,
    PathTooLong = 501,
    NotADirectory = 502,
    IsADirectory = 503,

    // Resource errors
    DiskFull = 600,
    TooManyOpenFiles = 601,
};

/**
 * @brief Log levels for the logging PAL.
 *
 * Levels are ordered from most verbose (Trace) to least verbose (Critical).
 */
enum class LogLevel : uint32_t {
    Trace = 0,      ///< Extremely detailed tracing information
    Debug = 1,      ///< Debug-level messages for development
    Info = 2,       ///< Informational messages about normal operation
    Warning = 3,    ///< Warning conditions that should be addressed
    Error = 4,      ///< Error conditions that affect operation
    Critical = 5,   ///< Critical conditions requiring immediate attention
    Off = 6         ///< Disable all logging
};

// =============================================================================
// Error Structures
// =============================================================================

/**
 * @brief Detailed network error information.
 *
 * Contains the error code, a human-readable message, and the underlying
 * system error code for debugging purposes.
 */
struct NetworkError {
    NetworkErrorCode code;
    std::string message;
    int32_t systemErrorCode;  ///< Platform-specific error code (errno, WSAGetLastError)

    NetworkError(NetworkErrorCode c = NetworkErrorCode::Unknown,
                 std::string msg = "",
                 int32_t sysErr = 0)
        : code(c)
        , message(std::move(msg))
        , systemErrorCode(sysErr) {}
};

/**
 * @brief Detailed thread error information.
 */
struct ThreadError {
    ThreadErrorCode code;
    std::string message;

    ThreadError(ThreadErrorCode c = ThreadErrorCode::Unknown,
                std::string msg = "")
        : code(c)
        , message(std::move(msg)) {}
};

/**
 * @brief Detailed timer error information.
 */
struct TimerError {
    TimerErrorCode code;
    std::string message;

    TimerError(TimerErrorCode c = TimerErrorCode::Unknown,
               std::string msg = "")
        : code(c)
        , message(std::move(msg)) {}
};

/**
 * @brief Detailed file error information.
 */
struct FileError {
    FileErrorCode code;
    std::string path;  ///< The file path that caused the error

    FileError(FileErrorCode c = FileErrorCode::Unknown,
              std::string p = "")
        : code(c)
        , path(std::move(p)) {}
};

// =============================================================================
// Configuration Structures
// =============================================================================

/**
 * @brief Options for creating a server socket.
 */
struct ServerOptions {
    int backlog = 128;           ///< Listen backlog size
    bool reuseAddr = true;       ///< Enable SO_REUSEADDR
    bool reusePort = false;      ///< Enable SO_REUSEPORT (where available)
    bool ipv6Only = false;       ///< IPv6 only (no dual-stack)
    int receiveBufferSize = 0;   ///< Receive buffer size (0 = system default)
    int sendBufferSize = 0;      ///< Send buffer size (0 = system default)
};

/**
 * @brief Socket options that can be set/get.
 */
enum class SocketOption : uint32_t {
    NoDelay,           ///< TCP_NODELAY - disable Nagle's algorithm
    KeepAlive,         ///< SO_KEEPALIVE - enable keepalive probes
    ReuseAddr,         ///< SO_REUSEADDR - allow address reuse
    ReusePort,         ///< SO_REUSEPORT - allow port reuse
    SendBufferSize,    ///< SO_SNDBUF - send buffer size
    ReceiveBufferSize, ///< SO_RCVBUF - receive buffer size
    Linger,            ///< SO_LINGER - linger on close
    NonBlocking,       ///< Set non-blocking mode
};

/**
 * @brief Server socket information.
 */
struct ServerSocket {
    SocketHandle handle;
    std::string address;
    uint16_t port;
};

/**
 * @brief Options for creating a thread.
 */
struct ThreadOptions {
    size_t stackSize = 0;        ///< Stack size in bytes (0 = system default)
    std::string name;            ///< Optional thread name for debugging
    int priority = 0;            ///< Thread priority (platform-specific interpretation)
    bool detached = false;       ///< Create as detached thread
};

/**
 * @brief Options for creating a thread pool.
 */
struct ThreadPoolOptions {
    std::string name;                          ///< Pool name for debugging
    std::chrono::milliseconds keepAliveTime{60000};  ///< Idle thread timeout
    size_t queueSize = 1024;                   ///< Maximum queued work items
};

/**
 * @brief File open mode flags.
 */
enum class FileOpenMode : uint32_t {
    Read = 0x01,        ///< Open for reading
    Write = 0x02,       ///< Open for writing
    Append = 0x04,      ///< Append to file
    Create = 0x08,      ///< Create if doesn't exist
    Truncate = 0x10,    ///< Truncate if exists
    Binary = 0x20,      ///< Binary mode (no text conversion)

    // Common combinations
    ReadWrite = Read | Write,
    CreateWrite = Create | Write | Truncate,
    CreateAppend = Create | Write | Append,
};

// Allow bitwise operations on FileOpenMode
inline FileOpenMode operator|(FileOpenMode a, FileOpenMode b) {
    return static_cast<FileOpenMode>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline FileOpenMode operator&(FileOpenMode a, FileOpenMode b) {
    return static_cast<FileOpenMode>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool hasFlag(FileOpenMode mode, FileOpenMode flag) {
    return (static_cast<uint32_t>(mode) & static_cast<uint32_t>(flag)) != 0;
}

/**
 * @brief File seek origin.
 */
enum class SeekOrigin {
    Begin,      ///< Seek from beginning of file
    Current,    ///< Seek from current position
    End         ///< Seek from end of file
};

/**
 * @brief Logging context for structured logging.
 */
struct LogContext {
    const char* file = nullptr;   ///< Source file name
    int line = 0;                 ///< Source line number
    const char* function = nullptr; ///< Function name
    std::string threadName;       ///< Current thread name
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback for async accept operations.
 * @param result Result containing the accepted socket handle or an error
 */
using AcceptCallback = std::function<void(core::Result<SocketHandle, NetworkError>)>;

/**
 * @brief Callback for async read operations.
 * @param result Result containing the number of bytes read or an error
 */
using ReadCallback = std::function<void(core::Result<size_t, NetworkError>)>;

/**
 * @brief Callback for async write operations.
 * @param result Result containing the number of bytes written or an error
 */
using WriteCallback = std::function<void(core::Result<size_t, NetworkError>)>;

/**
 * @brief Callback for timer expiration.
 */
using TimerCallback = std::function<void()>;

/**
 * @brief Function signature for thread entry point.
 * @param arg User-provided argument passed to the thread
 */
using ThreadFunction = std::function<void(void*)>;

/**
 * @brief Work item for thread pool execution.
 */
using WorkItem = std::function<void()>;

// =============================================================================
// Forward Declarations
// =============================================================================

class ILogSink;  // Forward declaration for log sink interface

} // namespace pal
} // namespace openrtmp

#endif // OPENRTMP_PAL_PAL_TYPES_HPP
