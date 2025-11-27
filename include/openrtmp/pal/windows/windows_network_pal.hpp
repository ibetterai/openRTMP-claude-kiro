// OpenRTMP - Cross-platform RTMP Server
// Windows Network PAL Implementation using IOCP
//
// Uses I/O Completion Ports for async network I/O
// Requirements Covered: 6.2 (Network abstraction), 14.1 (1000+ connections)

#ifndef OPENRTMP_PAL_WINDOWS_WINDOWS_NETWORK_PAL_HPP
#define OPENRTMP_PAL_WINDOWS_WINDOWS_NETWORK_PAL_HPP

#include "openrtmp/pal/network_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/buffer.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <functional>
#include <queue>
#include <memory>
#include <vector>

#if defined(_WIN32)

// Windows headers must be included in specific order
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

// Link against required libraries
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

namespace openrtmp {
namespace pal {
namespace windows {

// Forward declarations
struct OperationContext;
struct ConnectionState;

/**
 * @brief Operation types for IOCP context tracking.
 */
enum class OperationType : uint8_t {
    Accept,
    Read,
    Write,
    Disconnect
};

/**
 * @brief Windows implementation of INetworkPAL using I/O Completion Ports.
 *
 * This implementation uses:
 * - CreateIoCompletionPort for IOCP creation and socket association
 * - GetQueuedCompletionStatus for event loop processing
 * - AcceptEx with pre-allocated socket pool for async accepts
 * - WSARecv/WSASend with OVERLAPPED for async I/O
 * - PostQueuedCompletionStatus for waking the event loop
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Event loop runs in its own thread
 * - Callbacks are invoked from the event loop thread
 */
class WindowsNetworkPAL : public INetworkPAL {
public:
    /**
     * @brief Construct a Windows network PAL.
     */
    WindowsNetworkPAL();

    /**
     * @brief Destructor.
     *
     * Stops the event loop, closes all sockets, and releases IOCP handle.
     */
    ~WindowsNetworkPAL() override;

    // Non-copyable, non-movable
    WindowsNetworkPAL(const WindowsNetworkPAL&) = delete;
    WindowsNetworkPAL& operator=(const WindowsNetworkPAL&) = delete;
    WindowsNetworkPAL(WindowsNetworkPAL&&) = delete;
    WindowsNetworkPAL& operator=(WindowsNetworkPAL&&) = delete;

    // =========================================================================
    // INetworkPAL Implementation
    // =========================================================================

    /**
     * @brief Initialize the network subsystem.
     *
     * Initializes Winsock and creates the IOCP handle.
     */
    core::Result<void, NetworkError> initialize() override;

    /**
     * @brief Run the event loop.
     *
     * Blocks and processes IOCP completions until stopEventLoop() is called.
     */
    void runEventLoop() override;

    /**
     * @brief Stop the event loop.
     */
    void stopEventLoop() override;

    /**
     * @brief Check if event loop is running.
     */
    bool isRunning() const override;

    /**
     * @brief Create a server socket.
     */
    core::Result<ServerSocket, NetworkError> createServer(
        const std::string& address,
        uint16_t port,
        const ServerOptions& options
    ) override;

    /**
     * @brief Register async accept callback.
     *
     * Uses AcceptEx for async accept operations.
     */
    void asyncAccept(
        ServerSocket& server,
        AcceptCallback callback
    ) override;

    /**
     * @brief Register async read callback.
     *
     * Uses WSARecv for async read operations.
     */
    void asyncRead(
        SocketHandle socket,
        core::Buffer& buffer,
        size_t maxBytes,
        ReadCallback callback
    ) override;

    /**
     * @brief Register async write callback.
     *
     * Uses WSASend for async write operations.
     */
    void asyncWrite(
        SocketHandle socket,
        const core::Buffer& data,
        WriteCallback callback
    ) override;

    /**
     * @brief Close a socket.
     */
    void closeSocket(SocketHandle socket) override;

    /**
     * @brief Set socket option.
     */
    core::Result<void, NetworkError> setSocketOption(
        SocketHandle socket,
        SocketOption option,
        int value
    ) override;

    /**
     * @brief Get local address of socket.
     */
    core::Result<std::string, NetworkError> getLocalAddress(
        SocketHandle socket
    ) const override;

    /**
     * @brief Get local port of socket.
     */
    core::Result<uint16_t, NetworkError> getLocalPort(
        SocketHandle socket
    ) const override;

private:
    /**
     * @brief Socket state information.
     */
    struct SocketInfo {
        SOCKET socket;
        bool isServer;
        AcceptCallback acceptCallback;
        ReadCallback readCallback;
        WriteCallback writeCallback;
        core::Buffer* readBuffer;
        size_t maxReadBytes;
        core::Buffer writeBuffer;
        size_t writeOffset;
    };

    /**
     * @brief IOCP operation context.
     *
     * OVERLAPPED must be first member for CONTAINING_RECORD to work.
     */
    struct OperationContext {
        OVERLAPPED overlapped = {};
        OperationType type = OperationType::Read;
        SOCKET socket = INVALID_SOCKET;
        WSABUF wsaBuf = {};
        std::vector<uint8_t> buffer;

        // For AcceptEx
        SOCKET acceptSocket = INVALID_SOCKET;
        std::vector<uint8_t> acceptBuffer;

        // Callback stored as variant-like structure
        std::function<void(size_t, int)> callback;

        OperationContext(OperationType t, SOCKET s, size_t bufferSize = 4096);
        void resetOverlapped();
    };

    /**
     * @brief Pending operation for thread-safe operation submission.
     */
    struct PendingOp {
        enum class Type { Accept, Read, Write };
        Type type;
        SocketHandle socket;
        AcceptCallback acceptCb;
        ReadCallback readCb;
        WriteCallback writeCb;
        core::Buffer* buffer;
        size_t maxBytes;
        core::Buffer writeData;
    };

    // Initialization and cleanup
    bool initWinsock();
    bool createIOCP();
    bool loadExtensionFunctions(SOCKET socket);

    // Socket management
    bool associateWithIOCP(SOCKET socket);
    SOCKET createOverlappedSocket();

    // Accept pool management
    void postAccepts(SOCKET listenSocket);
    void recycleAcceptContext(OperationContext* ctx, SOCKET listenSocket);

    // Event handling
    void handleCompletion(OperationContext* ctx, DWORD bytesTransferred, bool success);
    void onAcceptComplete(OperationContext* ctx, DWORD bytesTransferred, bool success);
    void onReadComplete(OperationContext* ctx, DWORD bytesTransferred, bool success);
    void onWriteComplete(OperationContext* ctx, DWORD bytesTransferred, bool success);

    // Async operations
    void postRead(SOCKET socket, SocketInfo& info);
    void postWrite(SOCKET socket, SocketInfo& info);

    // Error handling
    NetworkError wsaErrorToNetworkError(int error) const;

    // Process pending operations
    void processPendingOps();

    // State
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};

    // IOCP handle
    HANDLE iocp_ = INVALID_HANDLE_VALUE;

    // Extension function pointers
    LPFN_ACCEPTEX fnAcceptEx_ = nullptr;
    LPFN_GETACCEPTEXSOCKADDRS fnGetAcceptExSockaddrs_ = nullptr;

    // Socket management
    mutable std::mutex socketsMutex_;
    std::unordered_map<SOCKET, SocketInfo> sockets_;

    // Accept context pool (per listen socket)
    std::mutex acceptMutex_;
    std::unordered_map<SOCKET, std::vector<std::unique_ptr<OperationContext>>> acceptContexts_;

    // Pending operations queue
    std::mutex opsMutex_;
    std::queue<PendingOp> pendingOps_;

    // Constants
    static constexpr size_t ACCEPT_POOL_SIZE = 10;
    static constexpr size_t BUFFER_SIZE = 4096;
};

} // namespace windows
} // namespace pal
} // namespace openrtmp

#endif // _WIN32
#endif // OPENRTMP_PAL_WINDOWS_WINDOWS_NETWORK_PAL_HPP
