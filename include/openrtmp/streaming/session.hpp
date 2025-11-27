// OpenRTMP - Cross-platform RTMP Server
// Session State Machine - Connection and stream session management
//
// Responsibilities:
// - Track connection states: Connecting, Handshaking, Connected, Publishing, Subscribing, Disconnected
// - Enforce valid state transitions for RTMP commands
// - Associate sessions with connections and streams
// - Handle unexpected disconnection with state cleanup
// - Maintain session context for authentication and authorization
//
// Requirements coverage:
// - Requirement 3.3: publish command with stream key validation
// - Requirement 3.4: play command for subscription
// - Requirement 3.5: deleteStream for resource release
// - Requirement 3.6: closeStream for stream stop

#ifndef OPENRTMP_STREAMING_SESSION_HPP
#define OPENRTMP_STREAMING_SESSION_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <functional>
#include <chrono>
#include <mutex>
#include <atomic>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Session State Enumeration
// =============================================================================

/**
 * @brief Session state enumeration.
 *
 * Represents the current state of an RTMP session lifecycle.
 * State transitions follow the RTMP protocol specification.
 */
enum class SessionState {
    Connecting,     ///< TCP connection accepted, handshake not started
    Handshaking,    ///< RTMP handshake in progress
    Connected,      ///< Handshake complete, ready for commands
    Publishing,     ///< Actively publishing a stream
    Subscribing,    ///< Actively subscribing to a stream
    Disconnected    ///< Session ended (terminal state)
};

/**
 * @brief Convert session state to string representation.
 *
 * @param state The session state
 * @return String representation of the state
 */
inline const char* sessionStateToString(SessionState state) {
    switch (state) {
        case SessionState::Connecting:    return "Connecting";
        case SessionState::Handshaking:   return "Handshaking";
        case SessionState::Connected:     return "Connected";
        case SessionState::Publishing:    return "Publishing";
        case SessionState::Subscribing:   return "Subscribing";
        case SessionState::Disconnected:  return "Disconnected";
        default:                          return "Unknown";
    }
}

// =============================================================================
// Session Error Types
// =============================================================================

/**
 * @brief Session error information.
 */
struct SessionError {
    /**
     * @brief Error codes for session operations.
     */
    enum class Code {
        InvalidTransition,      ///< Attempted invalid state transition
        AlreadyPublishing,      ///< Session is already publishing
        AlreadySubscribing,     ///< Session is already subscribing
        NotConnected,           ///< Session is not in connected state
        SessionClosed,          ///< Session has been closed
        InternalError           ///< Internal error occurred
    };

    Code code;                  ///< Error code
    std::string message;        ///< Human-readable error message

    SessionError(Code c = Code::InternalError, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

// =============================================================================
// State Change Callback
// =============================================================================

/**
 * @brief Callback type for session state changes.
 *
 * @param oldState The previous state
 * @param newState The new state
 */
using SessionStateChangeCallback = std::function<void(SessionState oldState, SessionState newState)>;

// =============================================================================
// Session Interface
// =============================================================================

/**
 * @brief Interface for session operations.
 *
 * Defines the contract for managing RTMP session state.
 */
class ISession {
public:
    virtual ~ISession() = default;

    // -------------------------------------------------------------------------
    // State Accessors
    // -------------------------------------------------------------------------

    /**
     * @brief Get the current session state.
     *
     * @return Current session state
     */
    virtual SessionState state() const = 0;

    /**
     * @brief Get the unique session identifier.
     *
     * @return Session ID
     */
    virtual SessionId sessionId() const = 0;

    /**
     * @brief Get the associated connection identifier.
     *
     * @return Connection ID
     */
    virtual ConnectionId connectionId() const = 0;

    /**
     * @brief Get the currently active stream ID.
     *
     * @return Stream ID or INVALID_STREAM_ID if none
     */
    virtual StreamId streamId() const = 0;

    /**
     * @brief Get the currently active stream key.
     *
     * @return Stream key (empty if none)
     */
    virtual StreamKey streamKey() const = 0;

    /**
     * @brief Get the connected application name.
     *
     * @return Application name
     */
    virtual const std::string& appName() const = 0;

    // -------------------------------------------------------------------------
    // State Transitions
    // -------------------------------------------------------------------------

    /**
     * @brief Start the RTMP handshake process.
     *
     * Transition: Connecting -> Handshaking
     *
     * @return Result indicating success or error
     */
    virtual core::Result<void, SessionError> startHandshake() = 0;

    /**
     * @brief Complete the RTMP handshake.
     *
     * Transition: Handshaking -> Connected
     *
     * @return Result indicating success or error
     */
    virtual core::Result<void, SessionError> completeHandshake() = 0;

    /**
     * @brief Start publishing a stream.
     *
     * Transition: Connected -> Publishing
     *
     * @param key The stream key to publish
     * @param streamId The allocated stream ID
     * @return Result indicating success or error
     */
    virtual core::Result<void, SessionError> startPublishing(
        const StreamKey& key,
        StreamId streamId
    ) = 0;

    /**
     * @brief Stop publishing and return to connected state.
     *
     * Transition: Publishing -> Connected
     *
     * @return Result indicating success or error
     */
    virtual core::Result<void, SessionError> stopPublishing() = 0;

    /**
     * @brief Start subscribing to a stream.
     *
     * Transition: Connected -> Subscribing
     *
     * @param key The stream key to subscribe to
     * @param streamId The allocated stream ID
     * @return Result indicating success or error
     */
    virtual core::Result<void, SessionError> startSubscribing(
        const StreamKey& key,
        StreamId streamId
    ) = 0;

    /**
     * @brief Stop subscribing and return to connected state.
     *
     * Transition: Subscribing -> Connected
     *
     * @return Result indicating success or error
     */
    virtual core::Result<void, SessionError> stopSubscribing() = 0;

    /**
     * @brief Disconnect the session.
     *
     * Transition: Any -> Disconnected
     *
     * @return Result indicating success or error
     */
    virtual core::Result<void, SessionError> disconnect() = 0;

    // -------------------------------------------------------------------------
    // State Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Check if session is in a connected state.
     *
     * Returns true for Connected, Publishing, and Subscribing states.
     *
     * @return true if connected
     */
    virtual bool isConnected() const = 0;

    /**
     * @brief Check if session is publishing.
     *
     * @return true if in Publishing state
     */
    virtual bool isPublishing() const = 0;

    /**
     * @brief Check if session is subscribing.
     *
     * @return true if in Subscribing state
     */
    virtual bool isSubscribing() const = 0;

    /**
     * @brief Check if session has an active stream.
     *
     * @return true if publishing or subscribing
     */
    virtual bool hasActiveStream() const = 0;

    // -------------------------------------------------------------------------
    // Session Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Set the application name.
     *
     * @param appName The application name
     */
    virtual void setAppName(const std::string& appName) = 0;

    /**
     * @brief Set an allocated stream ID for future use.
     *
     * This is used when createStream allocates an ID before publish/play.
     *
     * @param streamId The allocated stream ID
     */
    virtual void setAllocatedStreamId(StreamId streamId) = 0;

    /**
     * @brief Get the pre-allocated stream ID.
     *
     * @return Allocated stream ID or INVALID_STREAM_ID
     */
    virtual StreamId allocatedStreamId() const = 0;

    /**
     * @brief Release the pre-allocated stream ID.
     */
    virtual void releaseAllocatedStreamId() = 0;

    /**
     * @brief Set authentication status.
     *
     * @param authenticated True if authenticated
     */
    virtual void setAuthenticated(bool authenticated) = 0;

    /**
     * @brief Check if session is authenticated.
     *
     * @return true if authenticated
     */
    virtual bool isAuthenticated() const = 0;

    /**
     * @brief Set client information.
     *
     * @param info Client info (IP, port, user agent)
     */
    virtual void setClientInfo(const ClientInfo& info) = 0;

    /**
     * @brief Get client information.
     *
     * @return Client info reference
     */
    virtual const ClientInfo& clientInfo() const = 0;

    // -------------------------------------------------------------------------
    // Event Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for state changes.
     *
     * @param callback Callback to invoke on state change
     */
    virtual void setStateChangeCallback(SessionStateChangeCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Timestamps
    // -------------------------------------------------------------------------

    /**
     * @brief Get session creation timestamp.
     *
     * @return Creation time point
     */
    virtual std::chrono::steady_clock::time_point createdAt() const = 0;

    /**
     * @brief Get last activity timestamp.
     *
     * @return Last activity time point
     */
    virtual std::chrono::steady_clock::time_point lastActivity() const = 0;
};

// =============================================================================
// Session Implementation
// =============================================================================

/**
 * @brief Thread-safe session state machine implementation.
 *
 * Implements the ISession interface with:
 * - Atomic state transitions
 * - Thread-safe property access
 * - State change notifications
 * - Cleanup on disconnection
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::mutex for state transitions
 * - Uses std::atomic for session ID generation
 */
class Session : public ISession {
public:
    /**
     * @brief Construct a new Session.
     *
     * @param connectionId The associated connection ID
     */
    explicit Session(ConnectionId connectionId);

    /**
     * @brief Destructor.
     */
    ~Session() override;

    // Non-copyable
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Movable (with lock)
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;

    // ISession interface implementation
    SessionState state() const override;
    SessionId sessionId() const override;
    ConnectionId connectionId() const override;
    StreamId streamId() const override;
    StreamKey streamKey() const override;
    const std::string& appName() const override;

    core::Result<void, SessionError> startHandshake() override;
    core::Result<void, SessionError> completeHandshake() override;
    core::Result<void, SessionError> startPublishing(
        const StreamKey& key,
        StreamId streamId
    ) override;
    core::Result<void, SessionError> stopPublishing() override;
    core::Result<void, SessionError> startSubscribing(
        const StreamKey& key,
        StreamId streamId
    ) override;
    core::Result<void, SessionError> stopSubscribing() override;
    core::Result<void, SessionError> disconnect() override;

    bool isConnected() const override;
    bool isPublishing() const override;
    bool isSubscribing() const override;
    bool hasActiveStream() const override;

    void setAppName(const std::string& appName) override;
    void setAllocatedStreamId(StreamId streamId) override;
    StreamId allocatedStreamId() const override;
    void releaseAllocatedStreamId() override;

    void setAuthenticated(bool authenticated) override;
    bool isAuthenticated() const override;

    void setClientInfo(const ClientInfo& info) override;
    const ClientInfo& clientInfo() const override;

    void setStateChangeCallback(SessionStateChangeCallback callback) override;

    std::chrono::steady_clock::time_point createdAt() const override;
    std::chrono::steady_clock::time_point lastActivity() const override;

private:
    /**
     * @brief Generate a unique session ID.
     *
     * @return Unique session ID
     */
    static SessionId generateSessionId();

    /**
     * @brief Check if a state transition is valid.
     *
     * @param from Current state
     * @param to Desired state
     * @return true if transition is valid
     */
    static bool isValidTransition(SessionState from, SessionState to);

    /**
     * @brief Perform cleanup when transitioning to disconnected.
     */
    void performDisconnectCleanup();

    /**
     * @brief Update last activity timestamp.
     */
    void updateLastActivity();

    /**
     * @brief Emit state change event.
     *
     * @param oldState Previous state
     * @param newState New state
     */
    void emitStateChange(SessionState oldState, SessionState newState);

    // Session identifiers
    SessionId sessionId_;
    ConnectionId connectionId_;

    // State
    SessionState state_;
    StreamKey streamKey_;
    StreamId streamId_;
    StreamId allocatedStreamId_;
    std::string appName_;

    // Authentication
    bool authenticated_;
    ClientInfo clientInfo_;

    // Timestamps
    std::chrono::steady_clock::time_point createdAt_;
    std::chrono::steady_clock::time_point lastActivity_;

    // Thread safety
    mutable std::mutex mutex_;

    // Event callback
    SessionStateChangeCallback stateChangeCallback_;

    // Session ID generator
    static std::atomic<SessionId> nextSessionId_;
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_SESSION_HPP
