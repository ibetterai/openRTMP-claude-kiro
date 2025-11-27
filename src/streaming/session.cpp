// OpenRTMP - Cross-platform RTMP Server
// Session State Machine Implementation
//
// Thread-safe implementation of session state machine with:
// - Atomic state transitions with validation
// - Thread-safe property access using mutex
// - State change notifications via callback
// - Cleanup on disconnection
//
// Requirements coverage:
// - Requirement 3.3: publish command with stream key validation
// - Requirement 3.4: play command for subscription
// - Requirement 3.5: deleteStream for resource release
// - Requirement 3.6: closeStream for stream stop

#include "openrtmp/streaming/session.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Static Members
// =============================================================================

std::atomic<SessionId> Session::nextSessionId_{1};

// =============================================================================
// Constructor / Destructor
// =============================================================================

Session::Session(ConnectionId connectionId)
    : sessionId_(generateSessionId())
    , connectionId_(connectionId)
    , state_(SessionState::Connecting)
    , streamKey_()
    , streamId_(core::INVALID_STREAM_ID)
    , allocatedStreamId_(core::INVALID_STREAM_ID)
    , appName_()
    , authenticated_(false)
    , clientInfo_()
    , createdAt_(std::chrono::steady_clock::now())
    , lastActivity_(createdAt_)
    , stateChangeCallback_()
{
}

Session::~Session() = default;

Session::Session(Session&& other) noexcept
    : sessionId_(other.sessionId_)
    , connectionId_(other.connectionId_)
    , state_(other.state_)
    , streamKey_(std::move(other.streamKey_))
    , streamId_(other.streamId_)
    , allocatedStreamId_(other.allocatedStreamId_)
    , appName_(std::move(other.appName_))
    , authenticated_(other.authenticated_)
    , clientInfo_(std::move(other.clientInfo_))
    , createdAt_(other.createdAt_)
    , lastActivity_(other.lastActivity_)
    , stateChangeCallback_(std::move(other.stateChangeCallback_))
{
}

Session& Session::operator=(Session&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);

        sessionId_ = other.sessionId_;
        connectionId_ = other.connectionId_;
        state_ = other.state_;
        streamKey_ = std::move(other.streamKey_);
        streamId_ = other.streamId_;
        allocatedStreamId_ = other.allocatedStreamId_;
        appName_ = std::move(other.appName_);
        authenticated_ = other.authenticated_;
        clientInfo_ = std::move(other.clientInfo_);
        createdAt_ = other.createdAt_;
        lastActivity_ = other.lastActivity_;
        stateChangeCallback_ = std::move(other.stateChangeCallback_);
    }
    return *this;
}

// =============================================================================
// State Accessors
// =============================================================================

SessionState Session::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

SessionId Session::sessionId() const {
    // Session ID is immutable after construction, no lock needed
    return sessionId_;
}

ConnectionId Session::connectionId() const {
    // Connection ID is immutable after construction, no lock needed
    return connectionId_;
}

StreamId Session::streamId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streamId_;
}

StreamKey Session::streamKey() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streamKey_;
}

const std::string& Session::appName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return appName_;
}

// =============================================================================
// State Transitions
// =============================================================================

core::Result<void, SessionError> Session::startHandshake() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isValidTransition(state_, SessionState::Handshaking)) {
        return core::Result<void, SessionError>::error(
            SessionError{
                SessionError::Code::InvalidTransition,
                "Cannot transition from " + std::string(sessionStateToString(state_)) +
                " to Handshaking"
            }
        );
    }

    SessionState oldState = state_;
    state_ = SessionState::Handshaking;
    updateLastActivity();

    // Emit event outside the lock scope is safer but requires copy
    // For simplicity, emit while holding lock (callback should be fast)
    emitStateChange(oldState, state_);

    return core::Result<void, SessionError>::success();
}

core::Result<void, SessionError> Session::completeHandshake() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isValidTransition(state_, SessionState::Connected)) {
        return core::Result<void, SessionError>::error(
            SessionError{
                SessionError::Code::InvalidTransition,
                "Cannot transition from " + std::string(sessionStateToString(state_)) +
                " to Connected"
            }
        );
    }

    SessionState oldState = state_;
    state_ = SessionState::Connected;
    updateLastActivity();

    emitStateChange(oldState, state_);

    return core::Result<void, SessionError>::success();
}

core::Result<void, SessionError> Session::startPublishing(
    const StreamKey& key,
    StreamId streamId
) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isValidTransition(state_, SessionState::Publishing)) {
        return core::Result<void, SessionError>::error(
            SessionError{
                SessionError::Code::InvalidTransition,
                "Cannot transition from " + std::string(sessionStateToString(state_)) +
                " to Publishing"
            }
        );
    }

    SessionState oldState = state_;
    state_ = SessionState::Publishing;
    streamKey_ = key;
    streamId_ = streamId;
    updateLastActivity();

    emitStateChange(oldState, state_);

    return core::Result<void, SessionError>::success();
}

core::Result<void, SessionError> Session::stopPublishing() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != SessionState::Publishing) {
        return core::Result<void, SessionError>::error(
            SessionError{
                SessionError::Code::InvalidTransition,
                "Cannot stop publishing from " + std::string(sessionStateToString(state_))
            }
        );
    }

    SessionState oldState = state_;
    state_ = SessionState::Connected;
    streamKey_ = StreamKey();  // Clear stream key
    streamId_ = core::INVALID_STREAM_ID;  // Clear stream ID
    updateLastActivity();

    emitStateChange(oldState, state_);

    return core::Result<void, SessionError>::success();
}

core::Result<void, SessionError> Session::startSubscribing(
    const StreamKey& key,
    StreamId streamId
) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isValidTransition(state_, SessionState::Subscribing)) {
        return core::Result<void, SessionError>::error(
            SessionError{
                SessionError::Code::InvalidTransition,
                "Cannot transition from " + std::string(sessionStateToString(state_)) +
                " to Subscribing"
            }
        );
    }

    SessionState oldState = state_;
    state_ = SessionState::Subscribing;
    streamKey_ = key;
    streamId_ = streamId;
    updateLastActivity();

    emitStateChange(oldState, state_);

    return core::Result<void, SessionError>::success();
}

core::Result<void, SessionError> Session::stopSubscribing() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != SessionState::Subscribing) {
        return core::Result<void, SessionError>::error(
            SessionError{
                SessionError::Code::InvalidTransition,
                "Cannot stop subscribing from " + std::string(sessionStateToString(state_))
            }
        );
    }

    SessionState oldState = state_;
    state_ = SessionState::Connected;
    streamKey_ = StreamKey();  // Clear stream key
    streamId_ = core::INVALID_STREAM_ID;  // Clear stream ID
    updateLastActivity();

    emitStateChange(oldState, state_);

    return core::Result<void, SessionError>::success();
}

core::Result<void, SessionError> Session::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Disconnect is always valid (transition to terminal state)
    if (state_ == SessionState::Disconnected) {
        // Already disconnected, idempotent operation
        return core::Result<void, SessionError>::success();
    }

    SessionState oldState = state_;
    state_ = SessionState::Disconnected;
    performDisconnectCleanup();
    updateLastActivity();

    emitStateChange(oldState, state_);

    return core::Result<void, SessionError>::success();
}

// =============================================================================
// State Queries
// =============================================================================

bool Session::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == SessionState::Connected ||
           state_ == SessionState::Publishing ||
           state_ == SessionState::Subscribing;
}

bool Session::isPublishing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == SessionState::Publishing;
}

bool Session::isSubscribing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == SessionState::Subscribing;
}

bool Session::hasActiveStream() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == SessionState::Publishing || state_ == SessionState::Subscribing;
}

// =============================================================================
// Session Configuration
// =============================================================================

void Session::setAppName(const std::string& appName) {
    std::lock_guard<std::mutex> lock(mutex_);
    appName_ = appName;
    updateLastActivity();
}

void Session::setAllocatedStreamId(StreamId streamId) {
    std::lock_guard<std::mutex> lock(mutex_);
    allocatedStreamId_ = streamId;
}

StreamId Session::allocatedStreamId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocatedStreamId_;
}

void Session::releaseAllocatedStreamId() {
    std::lock_guard<std::mutex> lock(mutex_);
    allocatedStreamId_ = core::INVALID_STREAM_ID;
}

void Session::setAuthenticated(bool authenticated) {
    std::lock_guard<std::mutex> lock(mutex_);
    authenticated_ = authenticated;
}

bool Session::isAuthenticated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return authenticated_;
}

void Session::setClientInfo(const ClientInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    clientInfo_ = info;
}

const ClientInfo& Session::clientInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clientInfo_;
}

// =============================================================================
// Event Callbacks
// =============================================================================

void Session::setStateChangeCallback(SessionStateChangeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    stateChangeCallback_ = std::move(callback);
}

// =============================================================================
// Timestamps
// =============================================================================

std::chrono::steady_clock::time_point Session::createdAt() const {
    // Created timestamp is immutable, no lock needed
    return createdAt_;
}

std::chrono::steady_clock::time_point Session::lastActivity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastActivity_;
}

// =============================================================================
// Private Methods
// =============================================================================

SessionId Session::generateSessionId() {
    return nextSessionId_.fetch_add(1, std::memory_order_relaxed);
}

bool Session::isValidTransition(SessionState from, SessionState to) {
    // State transition table:
    // Connecting    -> Handshaking, Disconnected
    // Handshaking   -> Connected, Disconnected
    // Connected     -> Publishing, Subscribing, Disconnected
    // Publishing    -> Connected, Disconnected
    // Subscribing   -> Connected, Disconnected
    // Disconnected  -> (none - terminal state)

    switch (from) {
        case SessionState::Connecting:
            return to == SessionState::Handshaking || to == SessionState::Disconnected;

        case SessionState::Handshaking:
            return to == SessionState::Connected || to == SessionState::Disconnected;

        case SessionState::Connected:
            return to == SessionState::Publishing ||
                   to == SessionState::Subscribing ||
                   to == SessionState::Disconnected;

        case SessionState::Publishing:
            return to == SessionState::Connected || to == SessionState::Disconnected;

        case SessionState::Subscribing:
            return to == SessionState::Connected || to == SessionState::Disconnected;

        case SessionState::Disconnected:
            // Terminal state - no valid transitions
            return false;

        default:
            return false;
    }
}

void Session::performDisconnectCleanup() {
    // Clear stream-related state but preserve connection info for logging
    streamKey_ = StreamKey();
    streamId_ = core::INVALID_STREAM_ID;
    // Note: appName_ and connectionId_ are preserved for logging purposes
}

void Session::updateLastActivity() {
    lastActivity_ = std::chrono::steady_clock::now();
}

void Session::emitStateChange(SessionState oldState, SessionState newState) {
    if (stateChangeCallback_) {
        stateChangeCallback_(oldState, newState);
    }
}

} // namespace streaming
} // namespace openrtmp
