// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS/iOS) Network PAL Implementation
//
// Uses kqueue for event notification and BSD sockets for networking
// Requirements Covered: 6.2 (Network abstraction)

#ifndef OPENRTMP_PAL_DARWIN_DARWIN_NETWORK_PAL_HPP
#define OPENRTMP_PAL_DARWIN_DARWIN_NETWORK_PAL_HPP

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

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace openrtmp {
namespace pal {
namespace darwin {

/**
 * @brief Darwin (macOS/iOS) implementation of INetworkPAL using kqueue.
 *
 * This implementation uses:
 * - kqueue() for event notification
 * - kevent() for registering interest and retrieving events
 * - BSD sockets with non-blocking mode
 * - readv/writev for scatter-gather I/O
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Event loop runs in its own thread
 * - Callbacks are invoked from the event loop thread
 */
class DarwinNetworkPAL : public INetworkPAL {
public:
    /**
     * @brief Construct a Darwin network PAL.
     */
    DarwinNetworkPAL();

    /**
     * @brief Destructor.
     *
     * Stops the event loop and closes all sockets.
     */
    ~DarwinNetworkPAL() override;

    // Non-copyable, non-movable
    DarwinNetworkPAL(const DarwinNetworkPAL&) = delete;
    DarwinNetworkPAL& operator=(const DarwinNetworkPAL&) = delete;
    DarwinNetworkPAL(DarwinNetworkPAL&&) = delete;
    DarwinNetworkPAL& operator=(DarwinNetworkPAL&&) = delete;

    // =========================================================================
    // INetworkPAL Implementation
    // =========================================================================

    /**
     * @brief Initialize the network subsystem.
     *
     * Creates the kqueue file descriptor for event notification.
     */
    core::Result<void, NetworkError> initialize() override;

    /**
     * @brief Run the event loop.
     *
     * Blocks and processes events until stopEventLoop() is called.
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
     */
    void asyncAccept(
        ServerSocket& server,
        AcceptCallback callback
    ) override;

    /**
     * @brief Register async read callback.
     */
    void asyncRead(
        SocketHandle socket,
        core::Buffer& buffer,
        size_t maxBytes,
        ReadCallback callback
    ) override;

    /**
     * @brief Register async write callback.
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
        int fd;
        bool isServer;
        bool nonBlocking;
        AcceptCallback acceptCallback;
        ReadCallback readCallback;
        WriteCallback writeCallback;
        core::Buffer* readBuffer;
        size_t maxReadBytes;
        core::Buffer writeBuffer;
        size_t writeOffset;
    };

    /**
     * @brief Pending operation for event loop.
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

    /**
     * @brief Set socket to non-blocking mode.
     */
    bool setNonBlocking(int fd);

    /**
     * @brief Register socket with kqueue.
     */
    bool registerWithKqueue(int fd, int filter, bool enable);

    /**
     * @brief Process kqueue events.
     */
    void processEvents();

    /**
     * @brief Handle read-ready event.
     */
    void handleReadable(int fd);

    /**
     * @brief Handle write-ready event.
     */
    void handleWritable(int fd);

    /**
     * @brief Convert errno to NetworkError.
     */
    NetworkError errnoToNetworkError(int err) const;

    /**
     * @brief Process pending operations.
     */
    void processPendingOps();

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};

    int kqueueFd_{-1};

    mutable std::mutex socketsMutex_;
    std::unordered_map<int, SocketInfo> sockets_;

    mutable std::mutex opsMutex_;
    std::queue<PendingOp> pendingOps_;

    // Pipe for waking up kqueue
    int wakePipe_[2]{-1, -1};
};

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
#endif // OPENRTMP_PAL_DARWIN_DARWIN_NETWORK_PAL_HPP
