// OpenRTMP - Cross-platform RTMP Server
// Common error codes and Error structure

#ifndef OPENRTMP_CORE_ERROR_CODES_HPP
#define OPENRTMP_CORE_ERROR_CODES_HPP

#include <string>
#include <cstdint>

namespace openrtmp {
namespace core {

/**
 * @brief Common error codes used throughout OpenRTMP.
 *
 * These error codes provide a standardized way to communicate
 * error conditions across all layers of the system.
 */
enum class ErrorCode : uint32_t {
    // General errors (0-99)
    Success = 0,
    Unknown = 1,
    InvalidArgument = 2,
    InvalidState = 3,
    NotInitialized = 4,
    AlreadyInitialized = 5,
    AlreadyExists = 6,
    NotFound = 7,
    NotSupported = 8,
    OutOfMemory = 9,

    // Timeout and cancellation (100-199)
    Timeout = 100,
    Cancelled = 101,
    Interrupted = 102,

    // Network errors (200-299)
    NetworkError = 200,
    ConnectionFailed = 201,
    ConnectionClosed = 202,
    ConnectionReset = 203,
    BindFailed = 204,
    ListenFailed = 205,
    AcceptFailed = 206,
    SendFailed = 207,
    ReceiveFailed = 208,
    AddressInUse = 209,
    AddressNotAvailable = 210,
    HostUnreachable = 211,
    NetworkUnreachable = 212,

    // Protocol errors (300-399)
    ProtocolError = 300,
    HandshakeFailed = 301,
    InvalidHandshakeVersion = 302,
    HandshakeTimeout = 303,
    MalformedPacket = 304,
    InvalidMessageType = 305,
    InvalidChunkSize = 306,
    MessageTooLarge = 307,
    SequenceError = 308,

    // Stream errors (400-499)
    StreamError = 400,
    StreamNotFound = 401,
    StreamKeyInUse = 402,
    StreamKeyInvalid = 403,
    PublishFailed = 404,
    PlayFailed = 405,
    CodecNotSupported = 406,
    StreamEnded = 407,

    // Authentication errors (500-599)
    AuthError = 500,
    AuthFailed = 501,
    AuthTimeout = 502,
    PermissionDenied = 503,
    RateLimitExceeded = 504,
    IPBlocked = 505,

    // Configuration errors (600-699)
    ConfigError = 600,
    ConfigInvalid = 601,
    ConfigNotFound = 602,
    ConfigParseError = 603,

    // TLS errors (700-799)
    TLSError = 700,
    TLSHandshakeFailed = 701,
    CertificateInvalid = 702,
    CertificateExpired = 703,
    CertificateNotTrusted = 704,

    // Resource errors (800-899)
    ResourceExhausted = 800,
    ConnectionLimitReached = 801,
    StreamLimitReached = 802,
    MemoryLimitReached = 803,
    FileLimitReached = 804,

    // I/O errors (900-999)
    IOError = 900,
    FileNotFound = 901,
    FileAccessDenied = 902,
    FileReadError = 903,
    FileWriteError = 904,
    EndOfFile = 905,
};

/**
 * @brief Convert error code to human-readable string.
 * @param code The error code
 * @return String representation of the error code
 */
inline const char* errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::Success: return "Success";
        case ErrorCode::Unknown: return "Unknown error";
        case ErrorCode::InvalidArgument: return "Invalid argument";
        case ErrorCode::InvalidState: return "Invalid state";
        case ErrorCode::NotInitialized: return "Not initialized";
        case ErrorCode::AlreadyInitialized: return "Already initialized";
        case ErrorCode::AlreadyExists: return "Already exists";
        case ErrorCode::NotFound: return "Not found";
        case ErrorCode::NotSupported: return "Not supported";
        case ErrorCode::OutOfMemory: return "Out of memory";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::Interrupted: return "Interrupted";
        case ErrorCode::NetworkError: return "Network error";
        case ErrorCode::ConnectionFailed: return "Connection failed";
        case ErrorCode::ConnectionClosed: return "Connection closed";
        case ErrorCode::ConnectionReset: return "Connection reset";
        case ErrorCode::BindFailed: return "Bind failed";
        case ErrorCode::ListenFailed: return "Listen failed";
        case ErrorCode::AcceptFailed: return "Accept failed";
        case ErrorCode::SendFailed: return "Send failed";
        case ErrorCode::ReceiveFailed: return "Receive failed";
        case ErrorCode::AddressInUse: return "Address in use";
        case ErrorCode::AddressNotAvailable: return "Address not available";
        case ErrorCode::HostUnreachable: return "Host unreachable";
        case ErrorCode::NetworkUnreachable: return "Network unreachable";
        case ErrorCode::ProtocolError: return "Protocol error";
        case ErrorCode::HandshakeFailed: return "Handshake failed";
        case ErrorCode::InvalidHandshakeVersion: return "Invalid handshake version";
        case ErrorCode::HandshakeTimeout: return "Handshake timeout";
        case ErrorCode::MalformedPacket: return "Malformed packet";
        case ErrorCode::InvalidMessageType: return "Invalid message type";
        case ErrorCode::InvalidChunkSize: return "Invalid chunk size";
        case ErrorCode::MessageTooLarge: return "Message too large";
        case ErrorCode::SequenceError: return "Sequence error";
        case ErrorCode::StreamError: return "Stream error";
        case ErrorCode::StreamNotFound: return "Stream not found";
        case ErrorCode::StreamKeyInUse: return "Stream key in use";
        case ErrorCode::StreamKeyInvalid: return "Stream key invalid";
        case ErrorCode::PublishFailed: return "Publish failed";
        case ErrorCode::PlayFailed: return "Play failed";
        case ErrorCode::CodecNotSupported: return "Codec not supported";
        case ErrorCode::StreamEnded: return "Stream ended";
        case ErrorCode::AuthError: return "Authentication error";
        case ErrorCode::AuthFailed: return "Authentication failed";
        case ErrorCode::AuthTimeout: return "Authentication timeout";
        case ErrorCode::PermissionDenied: return "Permission denied";
        case ErrorCode::RateLimitExceeded: return "Rate limit exceeded";
        case ErrorCode::IPBlocked: return "IP blocked";
        case ErrorCode::ConfigError: return "Configuration error";
        case ErrorCode::ConfigInvalid: return "Configuration invalid";
        case ErrorCode::ConfigNotFound: return "Configuration not found";
        case ErrorCode::ConfigParseError: return "Configuration parse error";
        case ErrorCode::TLSError: return "TLS error";
        case ErrorCode::TLSHandshakeFailed: return "TLS handshake failed";
        case ErrorCode::CertificateInvalid: return "Certificate invalid";
        case ErrorCode::CertificateExpired: return "Certificate expired";
        case ErrorCode::CertificateNotTrusted: return "Certificate not trusted";
        case ErrorCode::ResourceExhausted: return "Resource exhausted";
        case ErrorCode::ConnectionLimitReached: return "Connection limit reached";
        case ErrorCode::StreamLimitReached: return "Stream limit reached";
        case ErrorCode::MemoryLimitReached: return "Memory limit reached";
        case ErrorCode::FileLimitReached: return "File limit reached";
        case ErrorCode::IOError: return "I/O error";
        case ErrorCode::FileNotFound: return "File not found";
        case ErrorCode::FileAccessDenied: return "File access denied";
        case ErrorCode::FileReadError: return "File read error";
        case ErrorCode::FileWriteError: return "File write error";
        case ErrorCode::EndOfFile: return "End of file";
        default: return "Unknown error code";
    }
}

/**
 * @brief Error structure containing error code, message, and optional context.
 *
 * This structure provides detailed error information for debugging
 * and logging purposes.
 */
struct Error {
    ErrorCode code;
    std::string message;
    std::string context;

    /**
     * @brief Construct an error with code and message.
     * @param c The error code
     * @param msg The error message
     * @param ctx Optional context information
     */
    Error(ErrorCode c = ErrorCode::Unknown,
          std::string msg = "",
          std::string ctx = "")
        : code(c)
        , message(std::move(msg))
        , context(std::move(ctx)) {}

    /**
     * @brief Check if this represents a successful operation.
     * @return true if error code is Success
     */
    [[nodiscard]] bool isSuccess() const noexcept {
        return code == ErrorCode::Success;
    }

    /**
     * @brief Get a formatted error string.
     * @return Formatted error string
     */
    [[nodiscard]] std::string toString() const {
        std::string result = errorCodeToString(code);
        if (!message.empty()) {
            result += ": " + message;
        }
        if (!context.empty()) {
            result += " [" + context + "]";
        }
        return result;
    }
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_ERROR_CODES_HPP
