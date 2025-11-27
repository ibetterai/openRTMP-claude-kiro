// OpenRTMP - Cross-platform RTMP Server
// Linux/Android Network PAL Implementation using epoll

#include "openrtmp/pal/linux/linux_network_pal.hpp"

#if defined(__linux__) || defined(__ANDROID__)

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <errno.h>

namespace openrtmp {
namespace pal {
namespace linux {

// =============================================================================
// Constructor / Destructor
// =============================================================================

LinuxNetworkPAL::LinuxNetworkPAL() = default;

LinuxNetworkPAL::~LinuxNetworkPAL() {
    stopEventLoop();

    // Close all sockets
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        for (auto& pair : sockets_) {
            if (pair.first >= 0) {
                close(pair.first);
            }
        }
        sockets_.clear();
    }

    // Close wake event fd
    if (wakeEventFd_ >= 0) {
        close(wakeEventFd_);
        wakeEventFd_ = -1;
    }

    // Close epoll fd
    if (epollFd_ >= 0) {
        close(epollFd_);
        epollFd_ = -1;
    }
}

// =============================================================================
// Initialization
// =============================================================================

core::Result<void, NetworkError> LinuxNetworkPAL::initialize() {
    if (initialized_.load()) {
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::AlreadyInitialized, "Network PAL already initialized", 0}
        );
    }

    // Create epoll instance
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::InitializationFailed, "epoll_create1 failed: " + std::string(strerror(errno)), errno}
        );
    }

    // Create eventfd for waking up epoll
    wakeEventFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeEventFd_ < 0) {
        close(epollFd_);
        epollFd_ = -1;
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::InitializationFailed, "eventfd failed: " + std::string(strerror(errno)), errno}
        );
    }

    // Add wakeEventFd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = wakeEventFd_;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeEventFd_, &ev) < 0) {
        close(wakeEventFd_);
        close(epollFd_);
        wakeEventFd_ = -1;
        epollFd_ = -1;
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::InitializationFailed, "epoll_ctl failed: " + std::string(strerror(errno)), errno}
        );
    }

    initialized_ = true;
    return core::Result<void, NetworkError>::success();
}

// =============================================================================
// Event Loop
// =============================================================================

void LinuxNetworkPAL::runEventLoop() {
    if (!initialized_.load()) {
        return;
    }

    running_ = true;
    stopRequested_ = false;

    while (!stopRequested_.load()) {
        processPendingOps();
        processEvents();
    }

    running_ = false;
}

void LinuxNetworkPAL::stopEventLoop() {
    stopRequested_ = true;

    // Wake up epoll
    if (wakeEventFd_ >= 0) {
        uint64_t val = 1;
        ssize_t result = write(wakeEventFd_, &val, sizeof(val));
        (void)result;
    }
}

bool LinuxNetworkPAL::isRunning() const {
    return running_.load();
}

void LinuxNetworkPAL::processEvents() {
    const int maxEvents = 64;
    struct epoll_event events[maxEvents];

    int nfds = epoll_wait(epollFd_, events, maxEvents, 100);  // 100ms timeout
    if (nfds < 0) {
        if (errno != EINTR) {
            // Log error
        }
        return;
    }

    for (int i = 0; i < nfds; ++i) {
        int fd = events[i].data.fd;

        // Check if it's the wake event
        if (fd == wakeEventFd_) {
            uint64_t val;
            while (read(wakeEventFd_, &val, sizeof(val)) > 0) {
                // Just drain
            }
            continue;
        }

        // Handle socket events
        if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
            handleReadable(fd);
        }

        if (events[i].events & EPOLLOUT) {
            handleWritable(fd);
        }
    }
}

void LinuxNetworkPAL::processPendingOps() {
    std::queue<PendingOp> ops;

    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        std::swap(ops, pendingOps_);
    }

    while (!ops.empty()) {
        PendingOp op = std::move(ops.front());
        ops.pop();

        std::lock_guard<std::mutex> lock(socketsMutex_);
        auto it = sockets_.find(static_cast<int>(op.socket.value));
        if (it == sockets_.end()) {
            // Socket not found - report error to callback
            if (op.type == PendingOp::Type::Accept && op.acceptCb) {
                op.acceptCb(core::Result<SocketHandle, NetworkError>::error(
                    NetworkError{NetworkErrorCode::SocketCreationFailed, "Socket not found", 0}
                ));
            } else if (op.type == PendingOp::Type::Read && op.readCb) {
                op.readCb(core::Result<size_t, NetworkError>::error(
                    NetworkError{NetworkErrorCode::ReadFailed, "Socket not found", 0}
                ));
            } else if (op.type == PendingOp::Type::Write && op.writeCb) {
                op.writeCb(core::Result<size_t, NetworkError>::error(
                    NetworkError{NetworkErrorCode::WriteFailed, "Socket not found", 0}
                ));
            }
            continue;
        }

        switch (op.type) {
            case PendingOp::Type::Accept:
                it->second.acceptCallback = std::move(op.acceptCb);
                registerWithEpoll(it->first, EPOLLIN | EPOLLET, true);
                break;

            case PendingOp::Type::Read:
                it->second.readCallback = std::move(op.readCb);
                it->second.readBuffer = op.buffer;
                it->second.maxReadBytes = op.maxBytes;
                registerWithEpoll(it->first, EPOLLIN | EPOLLET, true);
                break;

            case PendingOp::Type::Write:
                it->second.writeCallback = std::move(op.writeCb);
                it->second.writeBuffer = std::move(op.writeData);
                it->second.writeOffset = 0;
                registerWithEpoll(it->first, EPOLLIN | EPOLLOUT | EPOLLET, true);
                break;
        }
    }
}

void LinuxNetworkPAL::handleReadable(int fd) {
    std::lock_guard<std::mutex> lock(socketsMutex_);

    auto it = sockets_.find(fd);
    if (it == sockets_.end()) {
        return;
    }

    SocketInfo& info = it->second;

    if (info.isServer && info.acceptCallback) {
        // Accept connection
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);

        int clientFd = accept(fd, reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen);
        if (clientFd >= 0) {
            setNonBlocking(clientFd);

            SocketHandle clientHandle{static_cast<uint64_t>(clientFd)};

            // Register client socket
            SocketInfo clientInfo;
            clientInfo.fd = clientFd;
            clientInfo.isServer = false;
            clientInfo.nonBlocking = true;
            clientInfo.readBuffer = nullptr;
            clientInfo.maxReadBytes = 0;
            clientInfo.writeOffset = 0;
            sockets_[clientFd] = clientInfo;

            registerWithEpoll(clientFd, EPOLLIN | EPOLLET, false);

            AcceptCallback cb = std::move(info.acceptCallback);
            info.acceptCallback = nullptr;

            // Invoke callback outside the lock would be better, but for simplicity...
            cb(core::Result<SocketHandle, NetworkError>::success(clientHandle));
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            AcceptCallback cb = std::move(info.acceptCallback);
            info.acceptCallback = nullptr;

            cb(core::Result<SocketHandle, NetworkError>::error(
                errnoToNetworkError(errno)
            ));
        }
    } else if (info.readCallback && info.readBuffer) {
        // Read data
        info.readBuffer->resize(info.maxReadBytes);

        ssize_t bytesRead = read(fd, info.readBuffer->data(), info.maxReadBytes);

        if (bytesRead > 0) {
            info.readBuffer->resize(static_cast<size_t>(bytesRead));

            ReadCallback cb = std::move(info.readCallback);
            info.readCallback = nullptr;
            info.readBuffer = nullptr;

            cb(core::Result<size_t, NetworkError>::success(static_cast<size_t>(bytesRead)));
        } else if (bytesRead == 0) {
            // Connection closed
            ReadCallback cb = std::move(info.readCallback);
            info.readCallback = nullptr;
            info.readBuffer = nullptr;

            cb(core::Result<size_t, NetworkError>::error(
                NetworkError{NetworkErrorCode::ConnectionClosed, "Connection closed by peer", 0}
            ));
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ReadCallback cb = std::move(info.readCallback);
            info.readCallback = nullptr;
            info.readBuffer = nullptr;

            cb(core::Result<size_t, NetworkError>::error(
                errnoToNetworkError(errno)
            ));
        }
    }
}

void LinuxNetworkPAL::handleWritable(int fd) {
    std::lock_guard<std::mutex> lock(socketsMutex_);

    auto it = sockets_.find(fd);
    if (it == sockets_.end()) {
        return;
    }

    SocketInfo& info = it->second;

    if (info.writeCallback && !info.writeBuffer.empty()) {
        const uint8_t* data = info.writeBuffer.data() + info.writeOffset;
        size_t remaining = info.writeBuffer.size() - info.writeOffset;

        ssize_t bytesWritten = write(fd, data, remaining);

        if (bytesWritten > 0) {
            info.writeOffset += static_cast<size_t>(bytesWritten);

            if (info.writeOffset >= info.writeBuffer.size()) {
                // Write complete
                WriteCallback cb = std::move(info.writeCallback);
                info.writeCallback = nullptr;
                info.writeBuffer.clear();
                info.writeOffset = 0;

                // Remove EPOLLOUT interest
                registerWithEpoll(fd, EPOLLIN | EPOLLET, true);

                cb(core::Result<size_t, NetworkError>::success(info.writeBuffer.size()));
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            WriteCallback cb = std::move(info.writeCallback);
            info.writeCallback = nullptr;
            info.writeBuffer.clear();
            info.writeOffset = 0;

            cb(core::Result<size_t, NetworkError>::error(
                errnoToNetworkError(errno)
            ));
        }
    }
}

// =============================================================================
// Server Socket Operations
// =============================================================================

core::Result<ServerSocket, NetworkError> LinuxNetworkPAL::createServer(
    const std::string& address,
    uint16_t port,
    const ServerOptions& options
) {
    // Validate address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::InvalidAddress, "Invalid IPv4 address: " + address, 0}
        );
    }

    // Create socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::SocketCreationFailed, "socket failed: " + std::string(strerror(errno)), errno}
        );
    }

    // Set socket options
    if (options.reuseAddr) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    if (options.reusePort) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    if (options.receiveBufferSize > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &options.receiveBufferSize, sizeof(options.receiveBufferSize));
    }

    if (options.sendBufferSize > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &options.sendBufferSize, sizeof(options.sendBufferSize));
    }

    // Bind socket
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = errno;
        close(fd);
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::BindFailed, "bind failed: " + std::string(strerror(err)), err}
        );
    }

    // Listen
    if (listen(fd, options.backlog) < 0) {
        int err = errno;
        close(fd);
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::ListenFailed, "listen failed: " + std::string(strerror(err)), err}
        );
    }

    // Set non-blocking
    setNonBlocking(fd);

    // Register with epoll
    registerWithEpoll(fd, EPOLLIN | EPOLLET, false);

    // Create socket info
    SocketInfo info;
    info.fd = fd;
    info.isServer = true;
    info.nonBlocking = true;
    info.readBuffer = nullptr;
    info.maxReadBytes = 0;
    info.writeOffset = 0;

    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        sockets_[fd] = info;
    }

    ServerSocket server;
    server.handle = SocketHandle{static_cast<uint64_t>(fd)};
    server.address = address;
    server.port = port;

    return core::Result<ServerSocket, NetworkError>::success(server);
}

// =============================================================================
// Async I/O Operations
// =============================================================================

void LinuxNetworkPAL::asyncAccept(
    ServerSocket& server,
    AcceptCallback callback
) {
    PendingOp op;
    op.type = PendingOp::Type::Accept;
    op.socket = server.handle;
    op.acceptCb = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        pendingOps_.push(std::move(op));
    }

    // Wake up event loop
    if (wakeEventFd_ >= 0) {
        uint64_t val = 1;
        ssize_t result = write(wakeEventFd_, &val, sizeof(val));
        (void)result;
    }
}

void LinuxNetworkPAL::asyncRead(
    SocketHandle socket,
    core::Buffer& buffer,
    size_t maxBytes,
    ReadCallback callback
) {
    PendingOp op;
    op.type = PendingOp::Type::Read;
    op.socket = socket;
    op.readCb = std::move(callback);
    op.buffer = &buffer;
    op.maxBytes = maxBytes;

    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        pendingOps_.push(std::move(op));
    }

    // Wake up event loop
    if (wakeEventFd_ >= 0) {
        uint64_t val = 1;
        ssize_t result = write(wakeEventFd_, &val, sizeof(val));
        (void)result;
    }
}

void LinuxNetworkPAL::asyncWrite(
    SocketHandle socket,
    const core::Buffer& data,
    WriteCallback callback
) {
    PendingOp op;
    op.type = PendingOp::Type::Write;
    op.socket = socket;
    op.writeCb = std::move(callback);
    op.writeData = data;  // Copy the data

    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        pendingOps_.push(std::move(op));
    }

    // Wake up event loop
    if (wakeEventFd_ >= 0) {
        uint64_t val = 1;
        ssize_t result = write(wakeEventFd_, &val, sizeof(val));
        (void)result;
    }
}

// =============================================================================
// Socket Management
// =============================================================================

void LinuxNetworkPAL::closeSocket(SocketHandle socket) {
    if (socket == INVALID_SOCKET_HANDLE) {
        return;
    }

    int fd = static_cast<int>(socket.value);

    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        auto it = sockets_.find(fd);
        if (it == sockets_.end()) {
            return;
        }
        sockets_.erase(it);
    }

    unregisterFromEpoll(fd);
    close(fd);
}

core::Result<void, NetworkError> LinuxNetworkPAL::setSocketOption(
    SocketHandle socket,
    SocketOption option,
    int value
) {
    int fd = static_cast<int>(socket.value);

    int result = 0;
    switch (option) {
        case SocketOption::NoDelay:
            result = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
            break;

        case SocketOption::KeepAlive:
            result = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));
            break;

        case SocketOption::ReuseAddr:
            result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
            break;

        case SocketOption::ReusePort:
            result = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value));
            break;

        case SocketOption::SendBufferSize:
            result = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value));
            break;

        case SocketOption::ReceiveBufferSize:
            result = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
            break;

        case SocketOption::Linger: {
            struct linger l;
            l.l_onoff = value ? 1 : 0;
            l.l_linger = value;
            result = setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
            break;
        }

        case SocketOption::NonBlocking:
            if (value) {
                if (!setNonBlocking(fd)) {
                    result = -1;
                }
            } else {
                int flags = fcntl(fd, F_GETFL, 0);
                if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
                    result = -1;
                }
            }
            break;
    }

    if (result < 0) {
        return core::Result<void, NetworkError>::error(
            errnoToNetworkError(errno)
        );
    }

    return core::Result<void, NetworkError>::success();
}

core::Result<std::string, NetworkError> LinuxNetworkPAL::getLocalAddress(
    SocketHandle socket
) const {
    int fd = static_cast<int>(socket.value);

    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);

    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) < 0) {
        return core::Result<std::string, NetworkError>::error(
            errnoToNetworkError(errno)
        );
    }

    char addrStr[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr.sin_addr, addrStr, sizeof(addrStr)) == nullptr) {
        return core::Result<std::string, NetworkError>::error(
            errnoToNetworkError(errno)
        );
    }

    return core::Result<std::string, NetworkError>::success(std::string(addrStr));
}

core::Result<uint16_t, NetworkError> LinuxNetworkPAL::getLocalPort(
    SocketHandle socket
) const {
    int fd = static_cast<int>(socket.value);

    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);

    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) < 0) {
        return core::Result<uint16_t, NetworkError>::error(
            errnoToNetworkError(errno)
        );
    }

    return core::Result<uint16_t, NetworkError>::success(ntohs(addr.sin_port));
}

// =============================================================================
// Helper Functions
// =============================================================================

bool LinuxNetworkPAL::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool LinuxNetworkPAL::registerWithEpoll(int fd, uint32_t events, bool modify) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;

    int op = modify ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    return epoll_ctl(epollFd_, op, fd, &ev) >= 0;
}

void LinuxNetworkPAL::unregisterFromEpoll(int fd) {
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
}

NetworkError LinuxNetworkPAL::errnoToNetworkError(int err) const {
    NetworkErrorCode code;
    std::string message;

    switch (err) {
        case ECONNREFUSED:
            code = NetworkErrorCode::ConnectionRefused;
            message = "Connection refused";
            break;

        case ECONNRESET:
            code = NetworkErrorCode::ConnectionReset;
            message = "Connection reset by peer";
            break;

        case EADDRINUSE:
            code = NetworkErrorCode::AddressInUse;
            message = "Address already in use";
            break;

        case EADDRNOTAVAIL:
            code = NetworkErrorCode::AddressNotAvailable;
            message = "Address not available";
            break;

        case ENETUNREACH:
            code = NetworkErrorCode::NetworkUnreachable;
            message = "Network unreachable";
            break;

        case EHOSTUNREACH:
            code = NetworkErrorCode::HostUnreachable;
            message = "Host unreachable";
            break;

        case ETIMEDOUT:
            code = NetworkErrorCode::Timeout;
            message = "Connection timed out";
            break;

        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
            code = NetworkErrorCode::WouldBlock;
            message = "Operation would block";
            break;

        case EINTR:
            code = NetworkErrorCode::Interrupted;
            message = "Operation interrupted";
            break;

        case EACCES:
        case EPERM:
            code = NetworkErrorCode::PermissionDenied;
            message = "Permission denied";
            break;

        case EMFILE:
        case ENFILE:
            code = NetworkErrorCode::TooManyOpenFiles;
            message = "Too many open files";
            break;

        case ENOMEM:
            code = NetworkErrorCode::OutOfMemory;
            message = "Out of memory";
            break;

        default:
            code = NetworkErrorCode::Unknown;
            message = "Unknown error: " + std::string(strerror(err));
            break;
    }

    return NetworkError{code, message, err};
}

} // namespace linux
} // namespace pal
} // namespace openrtmp

#endif // defined(__linux__) || defined(__ANDROID__)
