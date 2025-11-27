// OpenRTMP - Cross-platform RTMP Server
// Linux/Android Network PAL Implementation
//
// Uses epoll for event notification and BSD sockets for networking
// Requirements Covered: 6.2 (Network abstraction)

#ifndef OPENRTMP_PAL_LINUX_LINUX_NETWORK_PAL_HPP
#define OPENRTMP_PAL_LINUX_LINUX_NETWORK_PAL_HPP

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

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace openrtmp {
namespace pal {
namespace linux {

/**
 * @brief Linux/Android implementation of INetworkPAL using epoll.
 *
 * This implementation uses:
 * - epoll_create1() for creating the event notification instance
 * - epoll_ctl() for registering interest in socket events
 * - epoll_wait() for waiting on events
 * - BSD sockets with non-blocking mode
 * - Edge-triggered mode (EPOLLET) for scalability
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Event loop runs in its own thread
 * - Callbacks are invoked from the event loop thread
 */
class LinuxNetworkPAL : public INetworkPAL {
public:
    /**
     * @brief Construct a Linux network PAL.
     */
    LinuxNetworkPAL();

    /**
     * @brief Destructor.
     *
     * Stops the event loop and closes all sockets.
     */
    ~LinuxNetworkPAL() override;

    // Non-copyable, non-movable
    LinuxNetworkPAL(const LinuxNetworkPAL&) = delete;
    LinuxNetworkPAL& operator=(const LinuxNetworkPAL&) = delete;
    LinuxNetworkPAL(LinuxNetworkPAL&&) = delete;
    LinuxNetworkPAL& operator=(LinuxNetworkPAL&&) = delete;

    // =========================================================================
    // INetworkPAL Implementation
    // =========================================================================

    /**
     * @brief Initialize the network subsystem.
     *
     * Creates the epoll file descriptor for event notification.
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
     * @brief Register socket with epoll.
     */
    bool registerWithEpoll(int fd, uint32_t events, bool modify = false);

    /**
     * @brief Unregister socket from epoll.
     */
    void unregisterFromEpoll(int fd);

    /**
     * @brief Process epoll events.
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

    int epollFd_{-1};

    mutable std::mutex socketsMutex_;
    std::unordered_map<int, SocketInfo> sockets_;

    mutable std::mutex opsMutex_;
    std::queue<PendingOp> pendingOps_;

    // Eventfd for waking up epoll
    int wakeEventFd_{-1};
};

} // namespace linux
} // namespace pal
} // namespace openrtmp

#endif // defined(__linux__) || defined(__ANDROID__)
#endif // OPENRTMP_PAL_LINUX_LINUX_NETWORK_PAL_HPP
