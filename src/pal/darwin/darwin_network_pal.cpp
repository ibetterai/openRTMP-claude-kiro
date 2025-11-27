// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS/iOS) Network PAL Implementation

#if defined(__APPLE__)

#include "openrtmp/pal/darwin/darwin_network_pal.hpp"

#include <sys/event.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace openrtmp {
namespace pal {
namespace darwin {

DarwinNetworkPAL::DarwinNetworkPAL() = default;

DarwinNetworkPAL::~DarwinNetworkPAL() {
    stopEventLoop();

    // Close all sockets
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        for (auto& pair : sockets_) {
            if (pair.first >= 0) {
                ::close(pair.first);
            }
        }
        sockets_.clear();
    }

    // Close wake pipe
    if (wakePipe_[0] >= 0) {
        ::close(wakePipe_[0]);
        wakePipe_[0] = -1;
    }
    if (wakePipe_[1] >= 0) {
        ::close(wakePipe_[1]);
        wakePipe_[1] = -1;
    }

    // Close kqueue
    if (kqueueFd_ >= 0) {
        ::close(kqueueFd_);
        kqueueFd_ = -1;
    }
}

core::Result<void, NetworkError> DarwinNetworkPAL::initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::AlreadyInitialized, "NetworkPAL already initialized", 0}
        );
    }

    // Create kqueue
    kqueueFd_ = kqueue();
    if (kqueueFd_ < 0) {
        initialized_ = false;
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::InitializationFailed,
                         "Failed to create kqueue: " + std::string(strerror(errno)),
                         errno}
        );
    }

    // Create wake pipe for stopping the event loop
    if (pipe(wakePipe_) < 0) {
        ::close(kqueueFd_);
        kqueueFd_ = -1;
        initialized_ = false;
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::InitializationFailed,
                         "Failed to create wake pipe: " + std::string(strerror(errno)),
                         errno}
        );
    }

    // Make wake pipe non-blocking
    setNonBlocking(wakePipe_[0]);
    setNonBlocking(wakePipe_[1]);

    // Register wake pipe read end with kqueue
    registerWithKqueue(wakePipe_[0], EVFILT_READ, true);

    return core::Result<void, NetworkError>::success();
}

bool DarwinNetworkPAL::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool DarwinNetworkPAL::registerWithKqueue(int fd, int filter, bool enable) {
    struct kevent event;
    EV_SET(&event, static_cast<uintptr_t>(fd), static_cast<int16_t>(filter),
           enable ? EV_ADD : EV_DELETE, 0, 0, nullptr);

    return kevent(kqueueFd_, &event, 1, nullptr, 0, nullptr) >= 0;
}

void DarwinNetworkPAL::runEventLoop() {
    running_ = true;
    stopRequested_ = false;

    while (!stopRequested_) {
        processEvents();
        processPendingOps();
    }

    running_ = false;
}

void DarwinNetworkPAL::stopEventLoop() {
    stopRequested_ = true;

    // Wake up the event loop
    if (wakePipe_[1] >= 0) {
        char c = 'w';
        ssize_t unused = write(wakePipe_[1], &c, 1);
        (void)unused;
    }
}

bool DarwinNetworkPAL::isRunning() const {
    return running_;
}

void DarwinNetworkPAL::processEvents() {
    struct kevent events[64];
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 100000000;  // 100ms

    int nEvents = kevent(kqueueFd_, nullptr, 0, events, 64, &timeout);

    for (int i = 0; i < nEvents; ++i) {
        int fd = static_cast<int>(events[i].ident);

        // Check for wake pipe
        if (fd == wakePipe_[0]) {
            char buf[256];
            while (read(wakePipe_[0], buf, sizeof(buf)) > 0) {
                // Drain the pipe
            }
            continue;
        }

        // Check for errors
        if (events[i].flags & EV_ERROR) {
            // Handle error
            continue;
        }

        // Handle read/write events
        if (events[i].filter == EVFILT_READ) {
            handleReadable(fd);
        } else if (events[i].filter == EVFILT_WRITE) {
            handleWritable(fd);
        }
    }
}

void DarwinNetworkPAL::handleReadable(int fd) {
    std::lock_guard<std::mutex> lock(socketsMutex_);

    auto it = sockets_.find(fd);
    if (it == sockets_.end()) {
        return;
    }

    SocketInfo& info = it->second;

    if (info.isServer && info.acceptCallback) {
        // Accept new connection
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        int clientFd = accept(fd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);

        if (clientFd >= 0) {
            setNonBlocking(clientFd);

            // Create socket info for client
            sockets_[clientFd] = SocketInfo{
                clientFd, false, true,
                nullptr, nullptr, nullptr,
                nullptr, 0, core::Buffer{}, 0
            };

            // Register with kqueue
            registerWithKqueue(clientFd, EVFILT_READ, true);

            // Invoke callback
            AcceptCallback cb = std::move(info.acceptCallback);
            info.acceptCallback = nullptr;

            // Must unlock before callback
            lock.~lock_guard();

            cb(core::Result<SocketHandle, NetworkError>::success(SocketHandle{static_cast<uint64_t>(clientFd)}));
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            AcceptCallback cb = std::move(info.acceptCallback);
            info.acceptCallback = nullptr;

            lock.~lock_guard();

            cb(core::Result<SocketHandle, NetworkError>::error(errnoToNetworkError(errno)));
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
            info.maxReadBytes = 0;

            lock.~lock_guard();

            cb(core::Result<size_t, NetworkError>::success(static_cast<size_t>(bytesRead)));
        } else if (bytesRead == 0) {
            // Connection closed
            ReadCallback cb = std::move(info.readCallback);
            info.readCallback = nullptr;
            info.readBuffer = nullptr;
            info.maxReadBytes = 0;

            lock.~lock_guard();

            cb(core::Result<size_t, NetworkError>::error(
                NetworkError{NetworkErrorCode::ConnectionClosed, "Connection closed by peer", 0}
            ));
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ReadCallback cb = std::move(info.readCallback);
            info.readCallback = nullptr;
            info.readBuffer = nullptr;
            info.maxReadBytes = 0;

            lock.~lock_guard();

            cb(core::Result<size_t, NetworkError>::error(errnoToNetworkError(errno)));
        }
    }
}

void DarwinNetworkPAL::handleWritable(int fd) {
    std::lock_guard<std::mutex> lock(socketsMutex_);

    auto it = sockets_.find(fd);
    if (it == sockets_.end()) {
        return;
    }

    SocketInfo& info = it->second;

    if (info.writeCallback && info.writeBuffer.size() > 0) {
        size_t remaining = info.writeBuffer.size() - info.writeOffset;
        ssize_t bytesWritten = write(fd,
            info.writeBuffer.data() + info.writeOffset,
            remaining);

        if (bytesWritten > 0) {
            info.writeOffset += static_cast<size_t>(bytesWritten);

            if (info.writeOffset >= info.writeBuffer.size()) {
                // All data written
                size_t totalWritten = info.writeBuffer.size();

                WriteCallback cb = std::move(info.writeCallback);
                info.writeCallback = nullptr;
                info.writeBuffer.clear();
                info.writeOffset = 0;

                // Disable write notifications
                registerWithKqueue(fd, EVFILT_WRITE, false);

                lock.~lock_guard();

                cb(core::Result<size_t, NetworkError>::success(totalWritten));
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            WriteCallback cb = std::move(info.writeCallback);
            info.writeCallback = nullptr;
            info.writeBuffer.clear();
            info.writeOffset = 0;

            registerWithKqueue(fd, EVFILT_WRITE, false);

            lock.~lock_guard();

            cb(core::Result<size_t, NetworkError>::error(errnoToNetworkError(errno)));
        }
    }
}

void DarwinNetworkPAL::processPendingOps() {
    std::queue<PendingOp> ops;

    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        std::swap(ops, pendingOps_);
    }

    while (!ops.empty()) {
        PendingOp op = std::move(ops.front());
        ops.pop();

        int fd = static_cast<int>(op.socket.value);

        std::lock_guard<std::mutex> lock(socketsMutex_);
        auto it = sockets_.find(fd);
        if (it == sockets_.end()) {
            continue;
        }

        SocketInfo& info = it->second;

        switch (op.type) {
            case PendingOp::Type::Accept:
                info.acceptCallback = std::move(op.acceptCb);
                break;

            case PendingOp::Type::Read:
                info.readCallback = std::move(op.readCb);
                info.readBuffer = op.buffer;
                info.maxReadBytes = op.maxBytes;
                registerWithKqueue(fd, EVFILT_READ, true);
                break;

            case PendingOp::Type::Write:
                info.writeCallback = std::move(op.writeCb);
                info.writeBuffer = std::move(op.writeData);
                info.writeOffset = 0;
                registerWithKqueue(fd, EVFILT_WRITE, true);
                break;
        }
    }
}

core::Result<ServerSocket, NetworkError> DarwinNetworkPAL::createServer(
    const std::string& address,
    uint16_t port,
    const ServerOptions& options)
{
    // Create socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::SocketCreationFailed,
                         "Failed to create socket: " + std::string(strerror(errno)),
                         errno}
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
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                   &options.receiveBufferSize, sizeof(options.receiveBufferSize));
    }

    if (options.sendBufferSize > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                   &options.sendBufferSize, sizeof(options.sendBufferSize));
    }

    // Parse address
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (address == "0.0.0.0" || address.empty()) {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        int parseResult = inet_pton(AF_INET, address.c_str(), &serverAddr.sin_addr);
        if (parseResult <= 0) {
            ::close(fd);
            return core::Result<ServerSocket, NetworkError>::error(
                NetworkError{NetworkErrorCode::InvalidAddress,
                             "Invalid address: " + address,
                             0}
            );
        }
    }

    // Bind
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        int err = errno;
        ::close(fd);
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::BindFailed,
                         "Failed to bind: " + std::string(strerror(err)),
                         err}
        );
    }

    // Listen
    if (listen(fd, options.backlog) < 0) {
        int err = errno;
        ::close(fd);
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::ListenFailed,
                         "Failed to listen: " + std::string(strerror(err)),
                         err}
        );
    }

    // Set non-blocking
    setNonBlocking(fd);

    // Register with kqueue
    registerWithKqueue(fd, EVFILT_READ, true);

    // Store socket info
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        sockets_[fd] = SocketInfo{
            fd, true, true,
            nullptr, nullptr, nullptr,
            nullptr, 0, core::Buffer{}, 0
        };
    }

    ServerSocket server;
    server.handle = SocketHandle{static_cast<uint64_t>(fd)};
    server.address = address;
    server.port = port;

    return core::Result<ServerSocket, NetworkError>::success(server);
}

void DarwinNetworkPAL::asyncAccept(ServerSocket& server, AcceptCallback callback) {
    PendingOp op;
    op.type = PendingOp::Type::Accept;
    op.socket = server.handle;
    op.acceptCb = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        pendingOps_.push(std::move(op));
    }

    // Wake the event loop
    if (wakePipe_[1] >= 0) {
        char c = 'a';
        ssize_t unused = write(wakePipe_[1], &c, 1);
        (void)unused;
    }
}

void DarwinNetworkPAL::asyncRead(
    SocketHandle socket,
    core::Buffer& buffer,
    size_t maxBytes,
    ReadCallback callback)
{
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

    // Wake the event loop
    if (wakePipe_[1] >= 0) {
        char c = 'r';
        ssize_t unused = write(wakePipe_[1], &c, 1);
        (void)unused;
    }
}

void DarwinNetworkPAL::asyncWrite(
    SocketHandle socket,
    const core::Buffer& data,
    WriteCallback callback)
{
    PendingOp op;
    op.type = PendingOp::Type::Write;
    op.socket = socket;
    op.writeCb = std::move(callback);
    op.writeData = data;

    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        pendingOps_.push(std::move(op));
    }

    // Wake the event loop
    if (wakePipe_[1] >= 0) {
        char c = 'w';
        ssize_t unused = write(wakePipe_[1], &c, 1);
        (void)unused;
    }
}

void DarwinNetworkPAL::closeSocket(SocketHandle socket) {
    int fd = static_cast<int>(socket.value);
    if (fd < 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        auto it = sockets_.find(fd);
        if (it != sockets_.end()) {
            sockets_.erase(it);
        }
    }

    // Unregister from kqueue
    registerWithKqueue(fd, EVFILT_READ, false);
    registerWithKqueue(fd, EVFILT_WRITE, false);

    ::close(fd);
}

core::Result<void, NetworkError> DarwinNetworkPAL::setSocketOption(
    SocketHandle socket,
    SocketOption option,
    int value)
{
    int fd = static_cast<int>(socket.value);
    int result = 0;

    switch (option) {
        case SocketOption::NoDelay: {
            result = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
            break;
        }
        case SocketOption::KeepAlive: {
            result = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));
            break;
        }
        case SocketOption::ReuseAddr: {
            result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
            break;
        }
        case SocketOption::ReusePort: {
            result = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value));
            break;
        }
        case SocketOption::SendBufferSize: {
            result = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value));
            break;
        }
        case SocketOption::ReceiveBufferSize: {
            result = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
            break;
        }
        case SocketOption::Linger: {
            struct linger l;
            l.l_onoff = value ? 1 : 0;
            l.l_linger = value;
            result = setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
            break;
        }
        case SocketOption::NonBlocking: {
            result = setNonBlocking(fd) ? 0 : -1;
            break;
        }
        default:
            return core::Result<void, NetworkError>::error(
                NetworkError{NetworkErrorCode::Unknown, "Unknown socket option", 0}
            );
    }

    if (result < 0) {
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::Unknown,
                         "Failed to set socket option: " + std::string(strerror(errno)),
                         errno}
        );
    }

    return core::Result<void, NetworkError>::success();
}

core::Result<std::string, NetworkError> DarwinNetworkPAL::getLocalAddress(SocketHandle socket) const {
    int fd = static_cast<int>(socket.value);

    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);

    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) < 0) {
        return core::Result<std::string, NetworkError>::error(
            NetworkError{NetworkErrorCode::Unknown,
                         "Failed to get local address: " + std::string(strerror(errno)),
                         errno}
        );
    }

    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ipStr, INET_ADDRSTRLEN);

    return core::Result<std::string, NetworkError>::success(std::string(ipStr));
}

core::Result<uint16_t, NetworkError> DarwinNetworkPAL::getLocalPort(SocketHandle socket) const {
    int fd = static_cast<int>(socket.value);

    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);

    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) < 0) {
        return core::Result<uint16_t, NetworkError>::error(
            NetworkError{NetworkErrorCode::Unknown,
                         "Failed to get local port: " + std::string(strerror(errno)),
                         errno}
        );
    }

    return core::Result<uint16_t, NetworkError>::success(ntohs(addr.sin_port));
}

NetworkError DarwinNetworkPAL::errnoToNetworkError(int err) const {
    NetworkErrorCode code;
    std::string message;

    switch (err) {
        case ECONNRESET:
            code = NetworkErrorCode::ConnectionReset;
            message = "Connection reset by peer";
            break;
        case ECONNREFUSED:
            code = NetworkErrorCode::ConnectionRefused;
            message = "Connection refused";
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
        case EADDRINUSE:
            code = NetworkErrorCode::AddressInUse;
            message = "Address already in use";
            break;
        case EADDRNOTAVAIL:
            code = NetworkErrorCode::AddressNotAvailable;
            message = "Address not available";
            break;
        case EHOSTUNREACH:
            code = NetworkErrorCode::HostUnreachable;
            message = "Host unreachable";
            break;
        case ENETUNREACH:
            code = NetworkErrorCode::NetworkUnreachable;
            message = "Network unreachable";
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
        case EACCES:
        case EPERM:
            code = NetworkErrorCode::PermissionDenied;
            message = "Permission denied";
            break;
        default:
            code = NetworkErrorCode::Unknown;
            message = strerror(err);
            break;
    }

    return NetworkError{code, message, err};
}

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
