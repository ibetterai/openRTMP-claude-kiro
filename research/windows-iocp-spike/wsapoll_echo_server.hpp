// OpenRTMP - Windows IOCP Research Spike
// WSAPoll-based Echo Server Proof of Concept
//
// This implementation demonstrates the WSAPoll approach for async I/O,
// using a readiness-based model similar to BSD poll/epoll.
//
// For Windows compilation only.

#ifndef WSAPOLL_ECHO_SERVER_HPP
#define WSAPOLL_ECHO_SERVER_HPP

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
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

namespace research {

// =============================================================================
// Types and Constants
// =============================================================================

constexpr size_t WSAPOLL_BUFFER_SIZE = 4096;
constexpr int WSAPOLL_TIMEOUT_MS = 100;  // Poll timeout in milliseconds

// =============================================================================
// Connection State for WSAPoll
// =============================================================================

/**
 * @brief State for a single client connection using WSAPoll.
 */
struct WSAPollConnectionState {
    SOCKET socket = INVALID_SOCKET;
    std::vector<uint8_t> readBuffer;
    std::vector<uint8_t> writeBuffer;
    size_t writeOffset = 0;  // Bytes already written from writeBuffer

    explicit WSAPollConnectionState(SOCKET s)
        : socket(s)
        , readBuffer(WSAPOLL_BUFFER_SIZE)
    {}

    bool hasDataToWrite() const {
        return writeOffset < writeBuffer.size();
    }
};

// =============================================================================
// WSAPoll Echo Server
// =============================================================================

/**
 * @brief Echo server using Windows WSAPoll.
 *
 * Demonstrates:
 * - WSAPoll-based readiness notification
 * - Non-blocking socket I/O
 * - Simple event loop pattern
 *
 * Limitations:
 * - O(n) complexity for event polling
 * - Maximum ~1024 sockets per WSAPoll call (can be worked around)
 * - Higher CPU usage at scale
 */
class WSAPollEchoServer {
public:
    WSAPollEchoServer();
    ~WSAPollEchoServer();

    // Disable copy
    WSAPollEchoServer(const WSAPollEchoServer&) = delete;
    WSAPollEchoServer& operator=(const WSAPollEchoServer&) = delete;

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
        std::atomic<uint64_t> pollCalls{0};  // Track poll syscalls
    };

    const Stats& getStats() const { return stats_; }

private:
    // Initialize Winsock
    bool initWinsock();

    // Create and configure the listen socket
    bool createListenSocket(uint16_t port);

    // Set socket to non-blocking mode
    bool setNonBlocking(SOCKET socket);

    // Rebuild the poll FD array
    void rebuildPollFds();

    // Handle events
    void handleAccept();
    void handleRead(SOCKET socket);
    void handleWrite(SOCKET socket);
    void handleError(SOCKET socket);

    // Connection management
    void addConnection(SOCKET socket);
    void removeConnection(SOCKET socket);
    WSAPollConnectionState* getConnection(SOCKET socket);

private:
    // Listen socket
    SOCKET listenSocket_ = INVALID_SOCKET;

    // Poll file descriptors
    std::vector<WSAPOLLFD> pollFds_;
    bool pollFdsNeedRebuild_ = true;

    // Active connections
    std::unordered_map<SOCKET, std::unique_ptr<WSAPollConnectionState>> connections_;
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

inline WSAPollEchoServer::WSAPollEchoServer() {}

inline WSAPollEchoServer::~WSAPollEchoServer() {
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

    // Cleanup Winsock
    WSACleanup();
}

inline bool WSAPollEchoServer::initialize() {
    if (initialized_.load()) {
        return true;
    }

    if (!initWinsock()) {
        return false;
    }

    initialized_.store(true);
    return true;
}

inline bool WSAPollEchoServer::initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        return false;
    }
    return true;
}

inline bool WSAPollEchoServer::listen(uint16_t port) {
    if (!initialized_.load()) {
        return false;
    }

    if (!createListenSocket(port)) {
        return false;
    }

    return true;
}

inline bool WSAPollEchoServer::createListenSocket(uint16_t port) {
    // Create socket
    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        return false;
    }

    // Set non-blocking
    if (!setNonBlocking(listenSocket_)) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
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

    pollFdsNeedRebuild_ = true;
    return true;
}

inline bool WSAPollEchoServer::setNonBlocking(SOCKET socket) {
    u_long mode = 1;  // 1 = non-blocking
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
}

inline void WSAPollEchoServer::rebuildPollFds() {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    pollFds_.clear();

    // Add listen socket
    WSAPOLLFD listenFd = {};
    listenFd.fd = listenSocket_;
    listenFd.events = POLLIN;
    pollFds_.push_back(listenFd);

    // Add all client connections
    for (auto& pair : connections_) {
        WSAPOLLFD clientFd = {};
        clientFd.fd = pair.second->socket;
        clientFd.events = POLLIN;  // Always interested in reading

        // If we have data to write, also check for writability
        if (pair.second->hasDataToWrite()) {
            clientFd.events |= POLLOUT;
        }

        pollFds_.push_back(clientFd);
    }

    pollFdsNeedRebuild_ = false;
}

inline void WSAPollEchoServer::run() {
    running_.store(true);

    while (running_.load()) {
        // Rebuild poll fds if needed
        if (pollFdsNeedRebuild_) {
            rebuildPollFds();
        }

        // Check for writable sockets (need to update events)
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            for (size_t i = 1; i < pollFds_.size(); ++i) {
                auto* conn = getConnection(pollFds_[i].fd);
                if (conn && conn->hasDataToWrite()) {
                    pollFds_[i].events |= POLLOUT;
                } else {
                    pollFds_[i].events &= ~POLLOUT;
                }
            }
        }

        // Poll for events
        stats_.pollCalls.fetch_add(1);
        int result = WSAPoll(pollFds_.data(), static_cast<ULONG>(pollFds_.size()),
                             WSAPOLL_TIMEOUT_MS);

        if (result < 0) {
            // Error
            continue;
        }

        if (result == 0) {
            // Timeout
            continue;
        }

        // Process events
        // Start from end to allow safe removal during iteration
        for (int i = static_cast<int>(pollFds_.size()) - 1; i >= 0; --i) {
            auto& pfd = pollFds_[i];

            if (pfd.revents == 0) {
                continue;
            }

            if (pfd.revents & POLLERR || pfd.revents & POLLHUP || pfd.revents & POLLNVAL) {
                if (i > 0) {  // Not listen socket
                    handleError(pfd.fd);
                }
                continue;
            }

            if (i == 0) {
                // Listen socket
                if (pfd.revents & POLLIN) {
                    handleAccept();
                }
            } else {
                // Client socket
                if (pfd.revents & POLLIN) {
                    handleRead(pfd.fd);
                }
                if (pfd.revents & POLLOUT) {
                    handleWrite(pfd.fd);
                }
            }
        }
    }
}

inline void WSAPollEchoServer::stop() {
    running_.store(false);
}

inline void WSAPollEchoServer::handleAccept() {
    while (true) {
        sockaddr_in clientAddr = {};
        int addrLen = sizeof(clientAddr);

        SOCKET clientSocket = accept(listenSocket_,
                                     reinterpret_cast<sockaddr*>(&clientAddr),
                                     &addrLen);

        if (clientSocket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                // No more pending connections
                break;
            }
            // Other error, stop accepting
            break;
        }

        // Set non-blocking
        if (!setNonBlocking(clientSocket)) {
            closesocket(clientSocket);
            continue;
        }

        // Disable Nagle
        int opt = 1;
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<char*>(&opt), sizeof(opt));

        addConnection(clientSocket);
        stats_.acceptedConnections.fetch_add(1);
        stats_.activeConnections.fetch_add(1);
    }
}

inline void WSAPollEchoServer::handleRead(SOCKET socket) {
    auto* conn = getConnection(socket);
    if (!conn) {
        return;
    }

    while (true) {
        int bytesRead = recv(socket,
                             reinterpret_cast<char*>(conn->readBuffer.data()),
                             static_cast<int>(conn->readBuffer.size()),
                             0);

        if (bytesRead < 0) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                // No more data available
                break;
            }
            // Error, close connection
            removeConnection(socket);
            stats_.activeConnections.fetch_sub(1);
            return;
        }

        if (bytesRead == 0) {
            // Connection closed
            removeConnection(socket);
            stats_.activeConnections.fetch_sub(1);
            return;
        }

        stats_.bytesReceived.fetch_add(bytesRead);

        // Echo: append to write buffer
        conn->writeBuffer.insert(conn->writeBuffer.end(),
                                 conn->readBuffer.begin(),
                                 conn->readBuffer.begin() + bytesRead);
    }
}

inline void WSAPollEchoServer::handleWrite(SOCKET socket) {
    auto* conn = getConnection(socket);
    if (!conn || !conn->hasDataToWrite()) {
        return;
    }

    while (conn->hasDataToWrite()) {
        size_t remaining = conn->writeBuffer.size() - conn->writeOffset;
        int toWrite = static_cast<int>(std::min(remaining, static_cast<size_t>(WSAPOLL_BUFFER_SIZE)));

        int bytesSent = send(socket,
                             reinterpret_cast<char*>(conn->writeBuffer.data() + conn->writeOffset),
                             toWrite,
                             0);

        if (bytesSent < 0) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                // Can't write more now
                break;
            }
            // Error, close connection
            removeConnection(socket);
            stats_.activeConnections.fetch_sub(1);
            return;
        }

        conn->writeOffset += bytesSent;
        stats_.bytesSent.fetch_add(bytesSent);

        // Check if write is complete
        if (conn->writeOffset >= conn->writeBuffer.size()) {
            conn->writeBuffer.clear();
            conn->writeOffset = 0;
            stats_.completedEchoes.fetch_add(1);
        }
    }
}

inline void WSAPollEchoServer::handleError(SOCKET socket) {
    removeConnection(socket);
    stats_.activeConnections.fetch_sub(1);
}

inline void WSAPollEchoServer::addConnection(SOCKET socket) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_[socket] = std::make_unique<WSAPollConnectionState>(socket);
    pollFdsNeedRebuild_ = true;
}

inline void WSAPollEchoServer::removeConnection(SOCKET socket) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto it = connections_.find(socket);
    if (it != connections_.end()) {
        closesocket(socket);
        connections_.erase(it);
        pollFdsNeedRebuild_ = true;
    }
}

inline WSAPollConnectionState* WSAPollEchoServer::getConnection(SOCKET socket) {
    // Note: caller must hold lock or be in single-threaded context
    auto it = connections_.find(socket);
    return it != connections_.end() ? it->second.get() : nullptr;
}

}  // namespace research

#endif  // _WIN32
#endif  // WSAPOLL_ECHO_SERVER_HPP
