// OpenRTMP - Cross-platform RTMP Server
// Windows Network PAL Implementation using IOCP

#if defined(_WIN32)

#include "openrtmp/pal/windows/windows_network_pal.hpp"

#include <cstring>
#include <algorithm>

namespace openrtmp {
namespace pal {
namespace windows {

// =============================================================================
// OperationContext Implementation
// =============================================================================

WindowsNetworkPAL::OperationContext::OperationContext(OperationType t, SOCKET s, size_t bufferSize)
    : type(t)
    , socket(s)
    , buffer(bufferSize)
{
    wsaBuf.buf = reinterpret_cast<char*>(buffer.data());
    wsaBuf.len = static_cast<ULONG>(buffer.size());
}

void WindowsNetworkPAL::OperationContext::resetOverlapped() {
    memset(&overlapped, 0, sizeof(overlapped));
}

// =============================================================================
// WindowsNetworkPAL Implementation
// =============================================================================

WindowsNetworkPAL::WindowsNetworkPAL() = default;

WindowsNetworkPAL::~WindowsNetworkPAL() {
    stopEventLoop();

    // Close all sockets
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        for (auto& pair : sockets_) {
            if (pair.first != INVALID_SOCKET) {
                closesocket(pair.first);
            }
        }
        sockets_.clear();
    }

    // Clear accept contexts
    {
        std::lock_guard<std::mutex> lock(acceptMutex_);
        for (auto& pair : acceptContexts_) {
            for (auto& ctx : pair.second) {
                if (ctx->acceptSocket != INVALID_SOCKET) {
                    closesocket(ctx->acceptSocket);
                }
            }
        }
        acceptContexts_.clear();
    }

    // Close IOCP handle
    if (iocp_ != INVALID_HANDLE_VALUE) {
        CloseHandle(iocp_);
        iocp_ = INVALID_HANDLE_VALUE;
    }

    // Cleanup Winsock
    if (initialized_) {
        WSACleanup();
    }
}

core::Result<void, NetworkError> WindowsNetworkPAL::initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::AlreadyInitialized, "NetworkPAL already initialized", 0}
        );
    }

    if (!initWinsock()) {
        initialized_ = false;
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::InitializationFailed,
                         "Failed to initialize Winsock",
                         WSAGetLastError()}
        );
    }

    if (!createIOCP()) {
        initialized_ = false;
        WSACleanup();
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::InitializationFailed,
                         "Failed to create IOCP",
                         static_cast<int>(GetLastError())}
        );
    }

    return core::Result<void, NetworkError>::success();
}

bool WindowsNetworkPAL::initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    return result == 0;
}

bool WindowsNetworkPAL::createIOCP() {
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    return iocp_ != INVALID_HANDLE_VALUE;
}

bool WindowsNetworkPAL::loadExtensionFunctions(SOCKET socket) {
    if (fnAcceptEx_ != nullptr) {
        return true;  // Already loaded
    }

    // Load AcceptEx
    GUID acceptExGuid = WSAID_ACCEPTEX;
    DWORD bytes = 0;

    int result = WSAIoctl(socket,
                          SIO_GET_EXTENSION_FUNCTION_POINTER,
                          &acceptExGuid, sizeof(acceptExGuid),
                          &fnAcceptEx_, sizeof(fnAcceptEx_),
                          &bytes, nullptr, nullptr);

    if (result == SOCKET_ERROR) {
        return false;
    }

    // Load GetAcceptExSockaddrs
    GUID getSockaddrsGuid = WSAID_GETACCEPTEXSOCKADDRS;
    result = WSAIoctl(socket,
                      SIO_GET_EXTENSION_FUNCTION_POINTER,
                      &getSockaddrsGuid, sizeof(getSockaddrsGuid),
                      &fnGetAcceptExSockaddrs_, sizeof(fnGetAcceptExSockaddrs_),
                      &bytes, nullptr, nullptr);

    return result != SOCKET_ERROR;
}

bool WindowsNetworkPAL::associateWithIOCP(SOCKET socket) {
    HANDLE result = CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(socket),
        iocp_,
        static_cast<ULONG_PTR>(socket),
        0
    );
    return result == iocp_;
}

SOCKET WindowsNetworkPAL::createOverlappedSocket() {
    return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                      nullptr, 0, WSA_FLAG_OVERLAPPED);
}

void WindowsNetworkPAL::runEventLoop() {
    running_ = true;
    stopRequested_ = false;

    while (!stopRequested_) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* overlapped = nullptr;

        // Wait for completion with timeout
        BOOL success = GetQueuedCompletionStatus(
            iocp_,
            &bytesTransferred,
            &completionKey,
            &overlapped,
            100  // 100ms timeout
        );

        // Process pending operations
        processPendingOps();

        if (overlapped == nullptr) {
            // Timeout or shutdown signal
            continue;
        }

        // Recover operation context
        auto* ctx = CONTAINING_RECORD(overlapped, OperationContext, overlapped);

        // Handle the completion
        handleCompletion(ctx, bytesTransferred, success != FALSE);
    }

    running_ = false;
}

void WindowsNetworkPAL::stopEventLoop() {
    stopRequested_ = true;

    // Post a null completion to wake up the event loop
    if (iocp_ != INVALID_HANDLE_VALUE) {
        PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
    }
}

bool WindowsNetworkPAL::isRunning() const {
    return running_;
}

void WindowsNetworkPAL::processPendingOps() {
    std::queue<PendingOp> ops;

    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        std::swap(ops, pendingOps_);
    }

    while (!ops.empty()) {
        PendingOp op = std::move(ops.front());
        ops.pop();

        SOCKET socket = static_cast<SOCKET>(op.socket.value);

        std::lock_guard<std::mutex> lock(socketsMutex_);
        auto it = sockets_.find(socket);
        if (it == sockets_.end()) {
            continue;
        }

        SocketInfo& info = it->second;

        switch (op.type) {
            case PendingOp::Type::Accept:
                info.acceptCallback = std::move(op.acceptCb);
                postAccepts(socket);
                break;

            case PendingOp::Type::Read:
                info.readCallback = std::move(op.readCb);
                info.readBuffer = op.buffer;
                info.maxReadBytes = op.maxBytes;
                postRead(socket, info);
                break;

            case PendingOp::Type::Write:
                info.writeCallback = std::move(op.writeCb);
                info.writeBuffer = std::move(op.writeData);
                info.writeOffset = 0;
                postWrite(socket, info);
                break;
        }
    }
}

void WindowsNetworkPAL::handleCompletion(OperationContext* ctx, DWORD bytesTransferred, bool success) {
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

void WindowsNetworkPAL::postAccepts(SOCKET listenSocket) {
    std::lock_guard<std::mutex> lock(acceptMutex_);

    // Create accept contexts if needed
    auto& contexts = acceptContexts_[listenSocket];
    while (contexts.size() < ACCEPT_POOL_SIZE) {
        auto ctx = std::make_unique<OperationContext>(OperationType::Accept, listenSocket);

        // Create accept socket
        ctx->acceptSocket = createOverlappedSocket();
        if (ctx->acceptSocket == INVALID_SOCKET) {
            continue;
        }

        // Allocate address buffer for AcceptEx
        ctx->acceptBuffer.resize((sizeof(sockaddr_in) + 16) * 2);

        contexts.push_back(std::move(ctx));
    }

    // Post accept operations
    for (auto& ctx : contexts) {
        ctx->resetOverlapped();

        DWORD bytesReceived = 0;
        BOOL result = fnAcceptEx_(
            listenSocket,
            ctx->acceptSocket,
            ctx->acceptBuffer.data(),
            0,  // Don't receive data with accept
            sizeof(sockaddr_in) + 16,
            sizeof(sockaddr_in) + 16,
            &bytesReceived,
            &ctx->overlapped
        );

        if (!result && WSAGetLastError() != ERROR_IO_PENDING) {
            // Accept failed, recycle socket
            closesocket(ctx->acceptSocket);
            ctx->acceptSocket = createOverlappedSocket();
        }
    }
}

void WindowsNetworkPAL::recycleAcceptContext(OperationContext* ctx, SOCKET listenSocket) {
    // Create new accept socket
    if (ctx->acceptSocket != INVALID_SOCKET) {
        closesocket(ctx->acceptSocket);
    }
    ctx->acceptSocket = createOverlappedSocket();

    if (ctx->acceptSocket != INVALID_SOCKET) {
        ctx->resetOverlapped();

        DWORD bytesReceived = 0;
        BOOL result = fnAcceptEx_(
            listenSocket,
            ctx->acceptSocket,
            ctx->acceptBuffer.data(),
            0,
            sizeof(sockaddr_in) + 16,
            sizeof(sockaddr_in) + 16,
            &bytesReceived,
            &ctx->overlapped
        );

        if (!result && WSAGetLastError() != ERROR_IO_PENDING) {
            closesocket(ctx->acceptSocket);
            ctx->acceptSocket = INVALID_SOCKET;
        }
    }
}

void WindowsNetworkPAL::onAcceptComplete(OperationContext* ctx, DWORD bytesTransferred, bool success) {
    SOCKET listenSocket = ctx->socket;
    SOCKET acceptedSocket = ctx->acceptSocket;

    // Get the accept callback
    AcceptCallback callback;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        auto it = sockets_.find(listenSocket);
        if (it != sockets_.end() && it->second.acceptCallback) {
            callback = std::move(it->second.acceptCallback);
            it->second.acceptCallback = nullptr;
        }
    }

    if (!success || acceptedSocket == INVALID_SOCKET) {
        // Failed accept, recycle
        recycleAcceptContext(ctx, listenSocket);

        if (callback) {
            callback(core::Result<SocketHandle, NetworkError>::error(
                wsaErrorToNetworkError(WSAGetLastError())
            ));
        }
        return;
    }

    // Update socket context (required for AcceptEx)
    setsockopt(acceptedSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
               reinterpret_cast<char*>(&listenSocket), sizeof(listenSocket));

    // Associate with IOCP
    if (associateWithIOCP(acceptedSocket)) {
        // Add to connections
        {
            std::lock_guard<std::mutex> lock(socketsMutex_);
            sockets_[acceptedSocket] = SocketInfo{
                acceptedSocket, false,
                nullptr, nullptr, nullptr,
                nullptr, 0, core::Buffer{}, 0
            };
        }

        // Recycle the accept context for more accepts
        ctx->acceptSocket = createOverlappedSocket();
        recycleAcceptContext(ctx, listenSocket);

        // Invoke callback
        if (callback) {
            callback(core::Result<SocketHandle, NetworkError>::success(
                SocketHandle{static_cast<uint64_t>(acceptedSocket)}
            ));
        }
    } else {
        closesocket(acceptedSocket);
        recycleAcceptContext(ctx, listenSocket);

        if (callback) {
            callback(core::Result<SocketHandle, NetworkError>::error(
                wsaErrorToNetworkError(WSAGetLastError())
            ));
        }
    }
}

void WindowsNetworkPAL::postRead(SOCKET socket, SocketInfo& info) {
    auto ctx = new OperationContext(OperationType::Read, socket, info.maxReadBytes);

    DWORD flags = 0;
    int result = WSARecv(socket, &ctx->wsaBuf, 1, nullptr, &flags,
                         &ctx->overlapped, nullptr);

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        // Immediate failure
        if (info.readCallback) {
            auto cb = std::move(info.readCallback);
            info.readCallback = nullptr;
            cb(core::Result<size_t, NetworkError>::error(
                wsaErrorToNetworkError(WSAGetLastError())
            ));
        }
        delete ctx;
    }
}

void WindowsNetworkPAL::onReadComplete(OperationContext* ctx, DWORD bytesTransferred, bool success) {
    SOCKET socket = ctx->socket;

    ReadCallback callback;
    core::Buffer* buffer = nullptr;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        auto it = sockets_.find(socket);
        if (it != sockets_.end()) {
            callback = std::move(it->second.readCallback);
            buffer = it->second.readBuffer;
            it->second.readCallback = nullptr;
            it->second.readBuffer = nullptr;
            it->second.maxReadBytes = 0;
        }
    }

    if (callback) {
        if (!success || bytesTransferred == 0) {
            callback(core::Result<size_t, NetworkError>::error(
                NetworkError{NetworkErrorCode::ConnectionClosed, "Connection closed", 0}
            ));
        } else {
            // Copy data to user buffer
            if (buffer) {
                buffer->resize(bytesTransferred);
                memcpy(buffer->data(), ctx->buffer.data(), bytesTransferred);
            }
            callback(core::Result<size_t, NetworkError>::success(
                static_cast<size_t>(bytesTransferred)
            ));
        }
    }

    delete ctx;
}

void WindowsNetworkPAL::postWrite(SOCKET socket, SocketInfo& info) {
    auto ctx = new OperationContext(OperationType::Write, socket, info.writeBuffer.size());

    // Copy data to write buffer
    memcpy(ctx->buffer.data(), info.writeBuffer.data(), info.writeBuffer.size());
    ctx->wsaBuf.buf = reinterpret_cast<char*>(ctx->buffer.data());
    ctx->wsaBuf.len = static_cast<ULONG>(info.writeBuffer.size());

    int result = WSASend(socket, &ctx->wsaBuf, 1, nullptr, 0,
                         &ctx->overlapped, nullptr);

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        // Immediate failure
        if (info.writeCallback) {
            auto cb = std::move(info.writeCallback);
            info.writeCallback = nullptr;
            cb(core::Result<size_t, NetworkError>::error(
                wsaErrorToNetworkError(WSAGetLastError())
            ));
        }
        delete ctx;
    }
}

void WindowsNetworkPAL::onWriteComplete(OperationContext* ctx, DWORD bytesTransferred, bool success) {
    SOCKET socket = ctx->socket;

    WriteCallback callback;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        auto it = sockets_.find(socket);
        if (it != sockets_.end()) {
            callback = std::move(it->second.writeCallback);
            it->second.writeCallback = nullptr;
            it->second.writeBuffer.clear();
            it->second.writeOffset = 0;
        }
    }

    if (callback) {
        if (!success) {
            callback(core::Result<size_t, NetworkError>::error(
                wsaErrorToNetworkError(WSAGetLastError())
            ));
        } else {
            callback(core::Result<size_t, NetworkError>::success(
                static_cast<size_t>(bytesTransferred)
            ));
        }
    }

    delete ctx;
}

core::Result<ServerSocket, NetworkError> WindowsNetworkPAL::createServer(
    const std::string& address,
    uint16_t port,
    const ServerOptions& options)
{
    // Create socket
    SOCKET socket = createOverlappedSocket();
    if (socket == INVALID_SOCKET) {
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::SocketCreationFailed,
                         "Failed to create socket",
                         WSAGetLastError()}
        );
    }

    // Load extension functions
    if (!loadExtensionFunctions(socket)) {
        closesocket(socket);
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::InitializationFailed,
                         "Failed to load extension functions",
                         WSAGetLastError()}
        );
    }

    // Set socket options
    if (options.reuseAddr) {
        int opt = 1;
        setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<char*>(&opt), sizeof(opt));
    }

    if (options.receiveBufferSize > 0) {
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&options.receiveBufferSize),
                   sizeof(options.receiveBufferSize));
    }

    if (options.sendBufferSize > 0) {
        setsockopt(socket, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&options.sendBufferSize),
                   sizeof(options.sendBufferSize));
    }

    // Parse address
    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (address == "0.0.0.0" || address.empty()) {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        int parseResult = inet_pton(AF_INET, address.c_str(), &serverAddr.sin_addr);
        if (parseResult <= 0) {
            closesocket(socket);
            return core::Result<ServerSocket, NetworkError>::error(
                NetworkError{NetworkErrorCode::InvalidAddress,
                             "Invalid address: " + address,
                             0}
            );
        }
    }

    // Bind
    if (bind(socket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(socket);
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::BindFailed,
                         "Failed to bind",
                         err}
        );
    }

    // Listen
    if (listen(socket, options.backlog) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(socket);
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::ListenFailed,
                         "Failed to listen",
                         err}
        );
    }

    // Associate with IOCP
    if (!associateWithIOCP(socket)) {
        int err = WSAGetLastError();
        closesocket(socket);
        return core::Result<ServerSocket, NetworkError>::error(
            NetworkError{NetworkErrorCode::InitializationFailed,
                         "Failed to associate with IOCP",
                         err}
        );
    }

    // Store socket info
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        sockets_[socket] = SocketInfo{
            socket, true,
            nullptr, nullptr, nullptr,
            nullptr, 0, core::Buffer{}, 0
        };
    }

    ServerSocket server;
    server.handle = SocketHandle{static_cast<uint64_t>(socket)};
    server.address = address;
    server.port = port;

    return core::Result<ServerSocket, NetworkError>::success(server);
}

void WindowsNetworkPAL::asyncAccept(ServerSocket& server, AcceptCallback callback) {
    PendingOp op;
    op.type = PendingOp::Type::Accept;
    op.socket = server.handle;
    op.acceptCb = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        pendingOps_.push(std::move(op));
    }

    // Wake the event loop
    if (iocp_ != INVALID_HANDLE_VALUE) {
        PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
    }
}

void WindowsNetworkPAL::asyncRead(
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
    if (iocp_ != INVALID_HANDLE_VALUE) {
        PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
    }
}

void WindowsNetworkPAL::asyncWrite(
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
    if (iocp_ != INVALID_HANDLE_VALUE) {
        PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
    }
}

void WindowsNetworkPAL::closeSocket(SocketHandle socket) {
    SOCKET s = static_cast<SOCKET>(socket.value);
    if (s == INVALID_SOCKET) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        auto it = sockets_.find(s);
        if (it != sockets_.end()) {
            sockets_.erase(it);
        }
    }

    // Clear any accept contexts for this socket
    {
        std::lock_guard<std::mutex> lock(acceptMutex_);
        auto it = acceptContexts_.find(s);
        if (it != acceptContexts_.end()) {
            for (auto& ctx : it->second) {
                if (ctx->acceptSocket != INVALID_SOCKET) {
                    closesocket(ctx->acceptSocket);
                }
            }
            acceptContexts_.erase(it);
        }
    }

    closesocket(s);
}

core::Result<void, NetworkError> WindowsNetworkPAL::setSocketOption(
    SocketHandle socket,
    SocketOption option,
    int value)
{
    SOCKET s = static_cast<SOCKET>(socket.value);
    int result = 0;

    switch (option) {
        case SocketOption::NoDelay: {
            result = setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                               reinterpret_cast<char*>(&value), sizeof(value));
            break;
        }
        case SocketOption::KeepAlive: {
            result = setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
                               reinterpret_cast<char*>(&value), sizeof(value));
            break;
        }
        case SocketOption::ReuseAddr: {
            result = setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                               reinterpret_cast<char*>(&value), sizeof(value));
            break;
        }
        case SocketOption::ReusePort: {
            // Windows doesn't have SO_REUSEPORT, use SO_REUSEADDR
            result = setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                               reinterpret_cast<char*>(&value), sizeof(value));
            break;
        }
        case SocketOption::SendBufferSize: {
            result = setsockopt(s, SOL_SOCKET, SO_SNDBUF,
                               reinterpret_cast<char*>(&value), sizeof(value));
            break;
        }
        case SocketOption::ReceiveBufferSize: {
            result = setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                               reinterpret_cast<char*>(&value), sizeof(value));
            break;
        }
        case SocketOption::Linger: {
            struct linger l;
            l.l_onoff = value ? 1 : 0;
            l.l_linger = static_cast<u_short>(value);
            result = setsockopt(s, SOL_SOCKET, SO_LINGER,
                               reinterpret_cast<char*>(&l), sizeof(l));
            break;
        }
        case SocketOption::NonBlocking: {
            u_long mode = value ? 1 : 0;
            result = ioctlsocket(s, FIONBIO, &mode);
            break;
        }
        default:
            return core::Result<void, NetworkError>::error(
                NetworkError{NetworkErrorCode::Unknown, "Unknown socket option", 0}
            );
    }

    if (result == SOCKET_ERROR) {
        return core::Result<void, NetworkError>::error(
            NetworkError{NetworkErrorCode::Unknown,
                         "Failed to set socket option",
                         WSAGetLastError()}
        );
    }

    return core::Result<void, NetworkError>::success();
}

core::Result<std::string, NetworkError> WindowsNetworkPAL::getLocalAddress(SocketHandle socket) const {
    SOCKET s = static_cast<SOCKET>(socket.value);

    sockaddr_in addr;
    int addrLen = sizeof(addr);

    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR) {
        return core::Result<std::string, NetworkError>::error(
            NetworkError{NetworkErrorCode::Unknown,
                         "Failed to get local address",
                         WSAGetLastError()}
        );
    }

    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ipStr, INET_ADDRSTRLEN);

    return core::Result<std::string, NetworkError>::success(std::string(ipStr));
}

core::Result<uint16_t, NetworkError> WindowsNetworkPAL::getLocalPort(SocketHandle socket) const {
    SOCKET s = static_cast<SOCKET>(socket.value);

    sockaddr_in addr;
    int addrLen = sizeof(addr);

    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR) {
        return core::Result<uint16_t, NetworkError>::error(
            NetworkError{NetworkErrorCode::Unknown,
                         "Failed to get local port",
                         WSAGetLastError()}
        );
    }

    return core::Result<uint16_t, NetworkError>::success(ntohs(addr.sin_port));
}

NetworkError WindowsNetworkPAL::wsaErrorToNetworkError(int error) const {
    NetworkErrorCode code;
    std::string message;

    switch (error) {
        case WSAECONNRESET:
            code = NetworkErrorCode::ConnectionReset;
            message = "Connection reset by peer";
            break;
        case WSAECONNREFUSED:
            code = NetworkErrorCode::ConnectionRefused;
            message = "Connection refused";
            break;
        case WSAETIMEDOUT:
            code = NetworkErrorCode::Timeout;
            message = "Connection timed out";
            break;
        case WSAEWOULDBLOCK:
            code = NetworkErrorCode::WouldBlock;
            message = "Operation would block";
            break;
        case WSAEINTR:
            code = NetworkErrorCode::Interrupted;
            message = "Operation interrupted";
            break;
        case WSAEADDRINUSE:
            code = NetworkErrorCode::AddressInUse;
            message = "Address already in use";
            break;
        case WSAEADDRNOTAVAIL:
            code = NetworkErrorCode::AddressNotAvailable;
            message = "Address not available";
            break;
        case WSAEHOSTUNREACH:
            code = NetworkErrorCode::HostUnreachable;
            message = "Host unreachable";
            break;
        case WSAENETUNREACH:
            code = NetworkErrorCode::NetworkUnreachable;
            message = "Network unreachable";
            break;
        case WSAEMFILE:
            code = NetworkErrorCode::TooManyOpenFiles;
            message = "Too many open files";
            break;
        case WSAENOBUFS:
            code = NetworkErrorCode::OutOfMemory;
            message = "Out of buffer space";
            break;
        case WSAEACCES:
            code = NetworkErrorCode::PermissionDenied;
            message = "Permission denied";
            break;
        default:
            code = NetworkErrorCode::Unknown;
            message = "Unknown error";
            break;
    }

    return NetworkError{code, message, error};
}

} // namespace windows
} // namespace pal
} // namespace openrtmp

#endif // _WIN32
