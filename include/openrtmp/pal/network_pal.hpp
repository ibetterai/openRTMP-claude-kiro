// OpenRTMP - Cross-platform RTMP Server
// Platform Abstraction Layer - Network Interface
//
// This interface abstracts platform-specific async network I/O operations.
// Implementations use:
// - kqueue on macOS/iOS
// - epoll on Linux/Android
// - IOCP on Windows
//
// Requirements Covered: 6.2 (network abstraction)

#ifndef OPENRTMP_PAL_NETWORK_PAL_HPP
#define OPENRTMP_PAL_NETWORK_PAL_HPP

#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/buffer.hpp"

namespace openrtmp {
namespace pal {

/**
 * @brief Abstract interface for platform-specific network operations.
 *
 * This interface provides a unified API for asynchronous network I/O
 * across all supported platforms. It follows the reactor pattern where
 * an event loop waits for I/O events and dispatches callbacks.
 *
 * ## Thread Safety
 * - All methods must be thread-safe unless otherwise noted
 * - Callbacks are invoked on the event loop thread
 * - Callbacks must not block or perform long-running operations
 *
 * ## Lifecycle
 * 1. Create implementation instance
 * 2. Call initialize() to set up the event loop
 * 3. Create server sockets and register callbacks
 * 4. Call runEventLoop() to start processing (blocks)
 * 5. Call stopEventLoop() from another thread to stop
 *
 * ## Error Handling
 * All operations that can fail return Result<T, NetworkError>.
 * Async operations report errors through their callbacks.
 *
 * @invariant After initialize() succeeds, all other methods may be called
 * @invariant After stopEventLoop(), runEventLoop() must be called again to resume
 */
class INetworkPAL {
public:
    virtual ~INetworkPAL() = default;

    // =========================================================================
    // Event Loop Control
    // =========================================================================

    /**
     * @brief Initialize the network subsystem.
     *
     * This must be called before any other network operations. It sets up
     * the platform-specific event notification mechanism (kqueue/epoll/IOCP).
     *
     * @pre Not already initialized
     * @post Event loop is ready to run
     * @return Success or NetworkError with initialization failure details
     *
     * @code
     * auto result = networkPal->initialize();
     * if (result.isError()) {
     *     // Handle initialization failure
     *     log("Failed to initialize network: " + result.error().message);
     *     return;
     * }
     * @endcode
     */
    virtual core::Result<void, NetworkError> initialize() = 0;

    /**
     * @brief Run the event loop.
     *
     * This method blocks and processes network events until stopEventLoop()
     * is called. All registered callbacks will be invoked from this thread.
     *
     * @pre initialize() has been called successfully
     * @note This method blocks until stopEventLoop() is called
     * @note All callbacks are invoked on the calling thread
     */
    virtual void runEventLoop() = 0;

    /**
     * @brief Stop the event loop.
     *
     * This method signals the event loop to stop processing and return from
     * runEventLoop(). It is safe to call from any thread, including from
     * within a callback.
     *
     * @note Thread-safe, can be called from any thread
     * @note May be called from within a callback
     * @post runEventLoop() will return
     */
    virtual void stopEventLoop() = 0;

    /**
     * @brief Check if the event loop is currently running.
     *
     * @return true if runEventLoop() is currently executing
     */
    virtual bool isRunning() const = 0;

    // =========================================================================
    // Server Socket Operations
    // =========================================================================

    /**
     * @brief Create and bind a server socket.
     *
     * Creates a TCP server socket, binds it to the specified address and port,
     * and starts listening for connections.
     *
     * @param address IP address to bind to ("0.0.0.0" for all interfaces, or specific IP)
     * @param port Port number to listen on
     * @param options Server socket configuration options
     *
     * @pre initialize() has been called
     * @pre address is a valid IP address string
     * @pre port > 0
     *
     * @return ServerSocket containing the socket handle and bound address/port,
     *         or NetworkError on failure
     *
     * Error conditions:
     * - AddressInUse: Another process is using the address/port
     * - BindFailed: Failed to bind to the specified address
     * - SocketCreationFailed: Failed to create the socket
     * - PermissionDenied: Insufficient privileges (e.g., port < 1024 on Unix)
     *
     * @code
     * ServerOptions opts;
     * opts.reuseAddr = true;
     * opts.backlog = 256;
     *
     * auto result = networkPal->createServer("0.0.0.0", 1935, opts);
     * if (result.isError()) {
     *     log("Failed to create server: " + result.error().message);
     *     return;
     * }
     *
     * ServerSocket& server = result.value();
     * log("Server listening on port " + std::to_string(server.port));
     * @endcode
     */
    virtual core::Result<ServerSocket, NetworkError> createServer(
        const std::string& address,
        uint16_t port,
        const ServerOptions& options
    ) = 0;

    // =========================================================================
    // Asynchronous I/O Operations
    // =========================================================================

    /**
     * @brief Asynchronously accept an incoming connection.
     *
     * Registers interest in accepting a new connection on the server socket.
     * When a connection is ready, the callback is invoked with the new
     * client socket handle.
     *
     * @param server Server socket to accept connections on
     * @param callback Function called when a connection is accepted or an error occurs
     *
     * @pre server.handle is a valid server socket
     * @pre Event loop is initialized
     *
     * @note The callback is invoked on the event loop thread
     * @note To accept multiple connections, call asyncAccept again from the callback
     *
     * @code
     * void onAccept(core::Result<SocketHandle, NetworkError> result) {
     *     if (result.isError()) {
     *         log("Accept failed: " + result.error().message);
     *         return;
     *     }
     *
     *     SocketHandle client = result.value();
     *     handleNewConnection(client);
     *
     *     // Continue accepting more connections
     *     networkPal->asyncAccept(server, onAccept);
     * }
     *
     * networkPal->asyncAccept(server, onAccept);
     * @endcode
     */
    virtual void asyncAccept(
        ServerSocket& server,
        AcceptCallback callback
    ) = 0;

    /**
     * @brief Asynchronously read data from a socket.
     *
     * Reads up to maxBytes of data from the socket into the buffer.
     * The callback is invoked when data is available, an error occurs,
     * or the connection is closed.
     *
     * @param socket Socket to read from
     * @param buffer Buffer to store read data (resized to received bytes)
     * @param maxBytes Maximum number of bytes to read
     * @param callback Function called with the number of bytes read or an error
     *
     * @pre socket is a valid connected socket
     * @pre maxBytes > 0
     *
     * @note Callback reports 0 bytes on graceful connection close
     * @note Buffer is modified only on successful read
     *
     * @code
     * core::Buffer readBuffer;
     *
     * void onRead(core::Result<size_t, NetworkError> result) {
     *     if (result.isError()) {
     *         if (result.error().code == NetworkErrorCode::ConnectionClosed) {
     *             handleDisconnect();
     *             return;
     *         }
     *         log("Read error: " + result.error().message);
     *         return;
     *     }
     *
     *     size_t bytesRead = result.value();
     *     if (bytesRead == 0) {
     *         handleDisconnect();
     *         return;
     *     }
     *
     *     processData(readBuffer, bytesRead);
     *
     *     // Continue reading
     *     networkPal->asyncRead(socket, readBuffer, 4096, onRead);
     * }
     *
     * networkPal->asyncRead(socket, readBuffer, 4096, onRead);
     * @endcode
     */
    virtual void asyncRead(
        SocketHandle socket,
        core::Buffer& buffer,
        size_t maxBytes,
        ReadCallback callback
    ) = 0;

    /**
     * @brief Asynchronously write data to a socket.
     *
     * Writes data from the buffer to the socket. The callback is invoked
     * when the data has been written or an error occurs.
     *
     * @param socket Socket to write to
     * @param data Buffer containing data to write
     * @param callback Function called with the number of bytes written or an error
     *
     * @pre socket is a valid connected socket
     * @pre data.size() > 0
     *
     * @note May write fewer bytes than requested; check callback result
     * @note For complete writes, caller must track progress and retry
     *
     * @code
     * core::Buffer response{0x03, ...};  // RTMP response data
     *
     * void onWrite(core::Result<size_t, NetworkError> result) {
     *     if (result.isError()) {
     *         log("Write error: " + result.error().message);
     *         handleWriteError();
     *         return;
     *     }
     *
     *     size_t bytesWritten = result.value();
     *     if (bytesWritten < response.size()) {
     *         // Partial write, continue with remaining data
     *         continueWrite(bytesWritten);
     *     } else {
     *         onWriteComplete();
     *     }
     * }
     *
     * networkPal->asyncWrite(socket, response, onWrite);
     * @endcode
     */
    virtual void asyncWrite(
        SocketHandle socket,
        const core::Buffer& data,
        WriteCallback callback
    ) = 0;

    // =========================================================================
    // Socket Management
    // =========================================================================

    /**
     * @brief Close a socket.
     *
     * Closes the socket and releases associated resources. Any pending
     * async operations on this socket will complete with an error.
     *
     * @param socket Socket to close
     *
     * @pre socket is a valid socket handle
     * @post Socket handle is no longer valid
     * @post Pending async operations complete with ConnectionClosed error
     *
     * @note Safe to call on already-closed sockets (no-op)
     */
    virtual void closeSocket(SocketHandle socket) = 0;

    /**
     * @brief Set a socket option.
     *
     * @param socket Socket to configure
     * @param option Option to set
     * @param value Option value
     *
     * @pre socket is a valid socket
     *
     * @return Success or NetworkError on failure
     *
     * @code
     * // Disable Nagle's algorithm for low-latency
     * auto result = networkPal->setSocketOption(socket, SocketOption::NoDelay, 1);
     * if (result.isError()) {
     *     log("Failed to set TCP_NODELAY");
     * }
     * @endcode
     */
    virtual core::Result<void, NetworkError> setSocketOption(
        SocketHandle socket,
        SocketOption option,
        int value
    ) = 0;

    /**
     * @brief Get the local address of a socket.
     *
     * @param socket Socket to query
     * @return Local IP address string or NetworkError
     */
    virtual core::Result<std::string, NetworkError> getLocalAddress(
        SocketHandle socket
    ) const = 0;

    /**
     * @brief Get the local port of a socket.
     *
     * @param socket Socket to query
     * @return Local port number or NetworkError
     */
    virtual core::Result<uint16_t, NetworkError> getLocalPort(
        SocketHandle socket
    ) const = 0;
};

} // namespace pal
} // namespace openrtmp

#endif // OPENRTMP_PAL_NETWORK_PAL_HPP
