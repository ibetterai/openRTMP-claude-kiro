// OpenRTMP - Windows IOCP Research Spike
// IOCP-based Echo Server Proof of Concept
//
// This implementation demonstrates the IOCP approach for async I/O,
// showing how to adapt the completion-based model to a callback interface.
//
// For Windows compilation only.

#ifndef IOCP_ECHO_SERVER_HPP
#define IOCP_ECHO_SERVER_HPP

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>  // For AcceptEx
#include <windows.h>

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

namespace research {

// Forward declarations
struct OperationContext;
class IOCPEchoServer;

// =============================================================================
// Types and Constants
// =============================================================================

constexpr size_t BUFFER_SIZE = 4096;
constexpr size_t ACCEPT_POOL_SIZE = 10;  // Pre-allocated accept sockets

// Operation types for IOCP
enum class OperationType : uint8_t {
    Accept,
    Read,
    Write,
    Disconnect
};

// Callback types matching INetworkPAL interface
using AcceptCallback = std::function<void(SOCKET clientSocket, int error)>;
using ReadCallback = std::function<void(size_t bytesRead, int error)>;
using WriteCallback = std::function<void(size_t bytesWritten, int error)>;

// =============================================================================
// Operation Context
// =============================================================================

/**
 * @brief Context for an async IOCP operation.
 *
 * IMPORTANT: OVERLAPPED must be the first member (or at a known offset)
 * so we can use CONTAINING_RECORD to recover the context from completion.
 */
struct OperationContext {
    // OVERLAPPED must be first
    OVERLAPPED overlapped = {};

    // Operation metadata
    OperationType type = OperationType::Read;
    SOCKET socket = INVALID_SOCKET;

    // Buffer for this operation
    WSABUF wsaBuf = {};
    std::vector<uint8_t> buffer;

    // For AcceptEx: the accepted socket and address buffer
    SOCKET acceptSocket = INVALID_SOCKET;
    std::vector<uint8_t> acceptBuffer;

    // Callback (type depends on operation type)
    std::function<void(size_t, int)> callback;

    // Constructor
    OperationContext(OperationType t, SOCKET s, size_t bufferSize = BUFFER_SIZE)
        : type(t)
        , socket(s)
        , buffer(bufferSize)
    {
        wsaBuf.buf = reinterpret_cast<char*>(buffer.data());
        wsaBuf.len = static_cast<ULONG>(buffer.size());
    }

    // Reset overlapped for reuse
    void resetOverlapped() {
        memset(&overlapped, 0, sizeof(overlapped));
    }
};

// =============================================================================
// Connection State
// =============================================================================

/**
 * @brief State for a single client connection.
 */
struct ConnectionState {
    SOCKET socket = INVALID_SOCKET;
    std::unique_ptr<OperationContext> readContext;
    std::unique_ptr<OperationContext> writeContext;
    std::vector<uint8_t> pendingWriteData;
    bool isReading = false;
    bool isWriting = false;

    explicit ConnectionState(SOCKET s)
        : socket(s)
        , readContext(std::make_unique<OperationContext>(OperationType::Read, s))
        , writeContext(std::make_unique<OperationContext>(OperationType::Write, s))
    {}
};

// =============================================================================
// IOCP Echo Server
// =============================================================================

/**
 * @brief Echo server using Windows I/O Completion Ports.
 *
 * Demonstrates:
 * - IOCP creation and socket association
 * - Async accept using AcceptEx
 * - Async read/write using WSARecv/WSASend
 * - Callback invocation on completion
 */
class IOCPEchoServer {
public:
    IOCPEchoServer();
    ~IOCPEchoServer();

    // Disable copy
    IOCPEchoServer(const IOCPEchoServer&) = delete;
    IOCPEchoServer& operator=(const IOCPEchoServer&) = delete;

    /**
     * @brief Initialize the server.
     * @return true on success, false on failure
     */
    bool initialize();

    /**
     * @brief Start listening on the specified port.
     * @param port Port number to listen on
     * @return true on success, false on failure
     */
    bool listen(uint16_t port);

    /**
     * @brief Run the event loop (blocks).
     */
    void run();

    /**
     * @brief Stop the event loop.
     */
    void stop();

    /**
     * @brief Check if server is running.
     */
    bool isRunning() const { return running_.load(); }

    // Statistics
    struct Stats {
        std::atomic<uint64_t> acceptedConnections{0};
        std::atomic<uint64_t> bytesReceived{0};
        std::atomic<uint64_t> bytesSent{0};
        std::atomic<uint64_t> activeConnections{0};
        std::atomic<uint64_t> completedEchoes{0};
    };

    const Stats& getStats() const { return stats_; }

private:
    // Initialize Winsock
    bool initWinsock();

    // Create the I/O Completion Port
    bool createIOCP();

    // Create and configure the listen socket
    bool createListenSocket(uint16_t port);

    // Associate a socket with the IOCP
    bool associateWithIOCP(SOCKET socket);

    // Post async accept operations
    void postAccepts();

    // Handle completed operations
    void handleCompletion(OperationContext* ctx, DWORD bytesTransferred, bool success);

    // Operation handlers
    void onAcceptComplete(OperationContext* ctx, DWORD bytesTransferred, bool success);
    void onReadComplete(OperationContext* ctx, DWORD bytesTransferred, bool success);
    void onWriteComplete(OperationContext* ctx, DWORD bytesTransferred, bool success);

    // Async operations
    void postRead(ConnectionState* conn);
    void postWrite(ConnectionState* conn, const uint8_t* data, size_t length);

    // Connection management
    void addConnection(SOCKET socket);
    void removeConnection(SOCKET socket);
    ConnectionState* getConnection(SOCKET socket);

    // Load AcceptEx function pointer
    bool loadAcceptEx();

private:
    // IOCP handle
    HANDLE iocp_ = INVALID_HANDLE_VALUE;

    // Listen socket
    SOCKET listenSocket_ = INVALID_SOCKET;

    // AcceptEx function pointer (must be loaded dynamically)
    LPFN_ACCEPTEX fnAcceptEx_ = nullptr;
    LPFN_GETACCEPTEXSOCKADDRS fnGetAcceptExSockaddrs_ = nullptr;

    // Accept socket pool
    std::vector<std::unique_ptr<OperationContext>> acceptContexts_;

    // Active connections
    std::unordered_map<SOCKET, std::unique_ptr<ConnectionState>> connections_;
    std::mutex connectionsMutex_;

    // Running state
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    // Statistics
    Stats stats_;
};

// =============================================================================
// Implementation
// =============================================================================

inline IOCPEchoServer::IOCPEchoServer() {}

inline IOCPEchoServer::~IOCPEchoServer() {
    stop();

    // Close all connections
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& pair : connections_) {
            closesocket(pair.second->socket);
        }
        connections_.clear();
    }

    // Close listen socket
    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    // Close IOCP handle
    if (iocp_ != INVALID_HANDLE_VALUE) {
        CloseHandle(iocp_);
        iocp_ = INVALID_HANDLE_VALUE;
    }

    // Cleanup Winsock
    WSACleanup();
}

inline bool IOCPEchoServer::initialize() {
    if (initialized_.load()) {
        return true;
    }

    if (!initWinsock()) {
        return false;
    }

    if (!createIOCP()) {
        return false;
    }

    initialized_.store(true);
    return true;
}

inline bool IOCPEchoServer::initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        return false;
    }
    return true;
}

inline bool IOCPEchoServer::createIOCP() {
    // Create IOCP with 0 threads (system determines optimal)
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    return iocp_ != INVALID_HANDLE_VALUE;
}

inline bool IOCPEchoServer::listen(uint16_t port) {
    if (!initialized_.load()) {
        return false;
    }

    if (!createListenSocket(port)) {
        return false;
    }

    if (!loadAcceptEx()) {
        return false;
    }

    // Associate listen socket with IOCP
    if (!associateWithIOCP(listenSocket_)) {
        return false;
    }

    return true;
}

inline bool IOCPEchoServer::createListenSocket(uint16_t port) {
    // Create socket
    listenSocket_ = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                               nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (listenSocket_ == INVALID_SOCKET) {
        return false;
    }

    // Set SO_REUSEADDR
    int opt = 1;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&opt), sizeof(opt));

    // Bind
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listenSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    // Listen
    if (::listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    return true;
}

inline bool IOCPEchoServer::loadAcceptEx() {
    // Load AcceptEx function pointer
    GUID acceptExGuid = WSAID_ACCEPTEX;
    DWORD bytes = 0;

    int result = WSAIoctl(listenSocket_,
                          SIO_GET_EXTENSION_FUNCTION_POINTER,
                          &acceptExGuid, sizeof(acceptExGuid),
                          &fnAcceptEx_, sizeof(fnAcceptEx_),
                          &bytes, nullptr, nullptr);

    if (result == SOCKET_ERROR) {
        return false;
    }

    // Load GetAcceptExSockaddrs
    GUID getSockaddrsGuid = WSAID_GETACCEPTEXSOCKADDRS;
    result = WSAIoctl(listenSocket_,
                      SIO_GET_EXTENSION_FUNCTION_POINTER,
                      &getSockaddrsGuid, sizeof(getSockaddrsGuid),
                      &fnGetAcceptExSockaddrs_, sizeof(fnGetAcceptExSockaddrs_),
                      &bytes, nullptr, nullptr);

    return result != SOCKET_ERROR;
}

inline bool IOCPEchoServer::associateWithIOCP(SOCKET socket) {
    HANDLE result = CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(socket),
        iocp_,
        static_cast<ULONG_PTR>(socket),  // Completion key
        0
    );
    return result == iocp_;
}

inline void IOCPEchoServer::run() {
    running_.store(true);

    // Post initial accept operations
    postAccepts();

    while (running_.load()) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* overlapped = nullptr;

        // Wait for completion (with timeout for shutdown check)
        BOOL success = GetQueuedCompletionStatus(
            iocp_,
            &bytesTransferred,
            &completionKey,
            &overlapped,
            100  // 100ms timeout
        );

        if (overlapped == nullptr) {
            // Timeout or shutdown signal
            continue;
        }

        // Recover operation context
        auto* ctx = CONTAINING_RECORD(overlapped, OperationContext, overlapped);

        // Handle the completion
        handleCompletion(ctx, bytesTransferred, success != FALSE);
    }
}

inline void IOCPEchoServer::stop() {
    running_.store(false);

    // Post a null completion to wake up the event loop
    if (iocp_ != INVALID_HANDLE_VALUE) {
        PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
    }
}

inline void IOCPEchoServer::postAccepts() {
    // Create accept contexts if needed
    while (acceptContexts_.size() < ACCEPT_POOL_SIZE) {
        auto ctx = std::make_unique<OperationContext>(OperationType::Accept, listenSocket_);

        // Create accept socket
        ctx->acceptSocket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                       nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (ctx->acceptSocket == INVALID_SOCKET) {
            continue;
        }

        // Allocate address buffer for AcceptEx
        // Size: (sizeof(sockaddr_in) + 16) * 2 for local and remote addresses
        ctx->acceptBuffer.resize((sizeof(sockaddr_in) + 16) * 2);

        acceptContexts_.push_back(std::move(ctx));
    }

    // Post accept operations
    for (auto& ctx : acceptContexts_) {
        ctx->resetOverlapped();

        DWORD bytesReceived = 0;
        BOOL result = fnAcceptEx_(
            listenSocket_,
            ctx->acceptSocket,
            ctx->acceptBuffer.data(),
            0,  // Don't receive data with accept
            sizeof(sockaddr_in) + 16,
            sizeof(sockaddr_in) + 16,
            &bytesReceived,
            &ctx->overlapped
        );

        if (!result && WSAGetLastError() != ERROR_IO_PENDING) {
            // Accept failed, try to recycle socket
            closesocket(ctx->acceptSocket);
            ctx->acceptSocket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                           nullptr, 0, WSA_FLAG_OVERLAPPED);
        }
    }
}

inline void IOCPEchoServer::handleCompletion(OperationContext* ctx, DWORD bytesTransferred, bool success) {
    switch (ctx->type) {
        case OperationType::Accept:
            onAcceptComplete(ctx, bytesTransferred, success);
            break;
        case OperationType::Read:
            onReadComplete(ctx, bytesTransferred, success);
            break;
        case OperationType::Write:
            onWriteComplete(ctx, bytesTransferred, success);
            break;
        default:
            break;
    }
}

inline void IOCPEchoServer::onAcceptComplete(OperationContext* ctx, DWORD bytesTransferred, bool success) {
    if (!success || ctx->acceptSocket == INVALID_SOCKET) {
        // Failed accept, re-post
        ctx->acceptSocket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                       nullptr, 0, WSA_FLAG_OVERLAPPED);
        postAccepts();
        return;
    }

    // Update socket context (required for AcceptEx)
    setsockopt(ctx->acceptSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
               reinterpret_cast<char*>(&listenSocket_), sizeof(listenSocket_));

    // Associate with IOCP
    if (associateWithIOCP(ctx->acceptSocket)) {
        // Add to connections and start reading
        addConnection(ctx->acceptSocket);
        stats_.acceptedConnections.fetch_add(1);
        stats_.activeConnections.fetch_add(1);
    } else {
        closesocket(ctx->acceptSocket);
    }

    // Create new accept socket and re-post
    ctx->acceptSocket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                   nullptr, 0, WSA_FLAG_OVERLAPPED);
    postAccepts();
}

inline void IOCPEchoServer::onReadComplete(OperationContext* ctx, DWORD bytesTransferred, bool success) {
    auto* conn = getConnection(ctx->socket);
    if (!conn) {
        return;
    }

    conn->isReading = false;

    if (!success || bytesTransferred == 0) {
        // Connection closed or error
        removeConnection(ctx->socket);
        stats_.activeConnections.fetch_sub(1);
        return;
    }

    stats_.bytesReceived.fetch_add(bytesTransferred);

    // Echo: write back what we received
    postWrite(conn, ctx->buffer.data(), bytesTransferred);
}

inline void IOCPEchoServer::onWriteComplete(OperationContext* ctx, DWORD bytesTransferred, bool success) {
    auto* conn = getConnection(ctx->socket);
    if (!conn) {
        return;
    }

    conn->isWriting = false;

    if (!success) {
        // Connection error
        removeConnection(ctx->socket);
        stats_.activeConnections.fetch_sub(1);
        return;
    }

    stats_.bytesSent.fetch_add(bytesTransferred);
    stats_.completedEchoes.fetch_add(1);

    // Continue reading
    postRead(conn);
}

inline void IOCPEchoServer::postRead(ConnectionState* conn) {
    if (conn->isReading) {
        return;  // Already reading
    }

    auto& ctx = conn->readContext;
    ctx->resetOverlapped();
    ctx->wsaBuf.buf = reinterpret_cast<char*>(ctx->buffer.data());
    ctx->wsaBuf.len = static_cast<ULONG>(ctx->buffer.size());

    DWORD flags = 0;
    int result = WSARecv(conn->socket, &ctx->wsaBuf, 1, nullptr, &flags,
                         &ctx->overlapped, nullptr);

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        // Immediate failure
        removeConnection(conn->socket);
        stats_.activeConnections.fetch_sub(1);
        return;
    }

    conn->isReading = true;
}

inline void IOCPEchoServer::postWrite(ConnectionState* conn, const uint8_t* data, size_t length) {
    if (conn->isWriting) {
        // Queue the write
        conn->pendingWriteData.insert(conn->pendingWriteData.end(), data, data + length);
        return;
    }

    auto& ctx = conn->writeContext;
    ctx->resetOverlapped();

    // Copy data to write buffer
    ctx->buffer.assign(data, data + length);
    ctx->wsaBuf.buf = reinterpret_cast<char*>(ctx->buffer.data());
    ctx->wsaBuf.len = static_cast<ULONG>(ctx->buffer.size());

    int result = WSASend(conn->socket, &ctx->wsaBuf, 1, nullptr, 0,
                         &ctx->overlapped, nullptr);

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        // Immediate failure
        removeConnection(conn->socket);
        stats_.activeConnections.fetch_sub(1);
        return;
    }

    conn->isWriting = true;
}

inline void IOCPEchoServer::addConnection(SOCKET socket) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto conn = std::make_unique<ConnectionState>(socket);
    auto* connPtr = conn.get();
    connections_[socket] = std::move(conn);

    // Start reading
    postRead(connPtr);
}

inline void IOCPEchoServer::removeConnection(SOCKET socket) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto it = connections_.find(socket);
    if (it != connections_.end()) {
        closesocket(socket);
        connections_.erase(it);
    }
}

inline ConnectionState* IOCPEchoServer::getConnection(SOCKET socket) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto it = connections_.find(socket);
    return it != connections_.end() ? it->second.get() : nullptr;
}

}  // namespace research

#endif  // _WIN32
#endif  // IOCP_ECHO_SERVER_HPP
