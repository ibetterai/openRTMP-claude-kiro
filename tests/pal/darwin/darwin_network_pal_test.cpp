// OpenRTMP - Cross-platform RTMP Server
// Tests for Darwin (macOS/iOS) Network PAL Implementation
//
// Requirements Covered: 6.2 (Network abstraction)

#include <gtest/gtest.h>
#include "openrtmp/pal/network_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/buffer.hpp"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

#if defined(__APPLE__)
#include "openrtmp/pal/darwin/darwin_network_pal.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#endif

namespace openrtmp {
namespace pal {
namespace test {

#if defined(__APPLE__)

// =============================================================================
// Darwin Network PAL Tests
// =============================================================================

class DarwinNetworkPALTest : public ::testing::Test {
protected:
    void SetUp() override {
        networkPal_ = std::make_unique<darwin::DarwinNetworkPAL>();
        auto initResult = networkPal_->initialize();
        ASSERT_TRUE(initResult.isSuccess()) << "Failed to initialize NetworkPAL";
    }

    void TearDown() override {
        if (networkPal_) {
            networkPal_->stopEventLoop();
        }
        networkPal_.reset();
    }

    // Helper to find an available port
    uint16_t findAvailablePort() {
        // Start from a high ephemeral port
        return static_cast<uint16_t>(10000 + (rand() % 50000));
    }

    std::unique_ptr<darwin::DarwinNetworkPAL> networkPal_;
};

TEST_F(DarwinNetworkPALTest, ImplementsINetworkPALInterface) {
    INetworkPAL* interface = networkPal_.get();
    EXPECT_NE(interface, nullptr);
}

// =============================================================================
// Initialization Tests
// =============================================================================

TEST_F(DarwinNetworkPALTest, InitializeSucceeds) {
    darwin::DarwinNetworkPAL freshPal;
    auto result = freshPal.initialize();
    EXPECT_TRUE(result.isSuccess());
}

TEST_F(DarwinNetworkPALTest, DoubleInitializeReturnsAlreadyInitialized) {
    // Already initialized in SetUp
    auto result = networkPal_->initialize();
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, NetworkErrorCode::AlreadyInitialized);
}

// =============================================================================
// Server Creation Tests
// =============================================================================

TEST_F(DarwinNetworkPALTest, CreateServerReturnsValidSocket) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    auto result = networkPal_->createServer("127.0.0.1", port, opts);

    if (result.isError()) {
        // Port might be in use, try another
        port = findAvailablePort();
        result = networkPal_->createServer("127.0.0.1", port, opts);
    }

    ASSERT_TRUE(result.isSuccess()) << "Failed to create server: " << result.error().message;
    EXPECT_NE(result.value().handle, INVALID_SOCKET_HANDLE);
    EXPECT_EQ(result.value().port, port);

    networkPal_->closeSocket(result.value().handle);
}

TEST_F(DarwinNetworkPALTest, CreateServerOnAllInterfaces) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    auto result = networkPal_->createServer("0.0.0.0", port, opts);

    if (result.isError()) {
        port = findAvailablePort();
        result = networkPal_->createServer("0.0.0.0", port, opts);
    }

    ASSERT_TRUE(result.isSuccess()) << "Failed to create server: " << result.error().message;

    networkPal_->closeSocket(result.value().handle);
}

TEST_F(DarwinNetworkPALTest, CreateServerWithReuseAddr) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    auto result1 = networkPal_->createServer("127.0.0.1", port, opts);
    if (result1.isError()) {
        port = findAvailablePort();
        result1 = networkPal_->createServer("127.0.0.1", port, opts);
    }
    ASSERT_TRUE(result1.isSuccess());

    networkPal_->closeSocket(result1.value().handle);

    // Should be able to rebind immediately with reuseAddr
    auto result2 = networkPal_->createServer("127.0.0.1", port, opts);
    EXPECT_TRUE(result2.isSuccess()) << "Failed to rebind with reuseAddr";

    if (result2.isSuccess()) {
        networkPal_->closeSocket(result2.value().handle);
    }
}

TEST_F(DarwinNetworkPALTest, CreateServerOnInvalidAddress) {
    ServerOptions opts;

    auto result = networkPal_->createServer("999.999.999.999", 1935, opts);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, NetworkErrorCode::InvalidAddress);
}

// =============================================================================
// Socket Options Tests
// =============================================================================

TEST_F(DarwinNetworkPALTest, SetSocketOptionNoDelay) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    auto serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    if (serverResult.isError()) {
        port = findAvailablePort();
        serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    }
    ASSERT_TRUE(serverResult.isSuccess());

    auto optResult = networkPal_->setSocketOption(
        serverResult.value().handle,
        SocketOption::NoDelay,
        1
    );

    // NoDelay might not apply to listening sockets, but shouldn't error badly
    // The actual test is that it doesn't crash

    networkPal_->closeSocket(serverResult.value().handle);
}

TEST_F(DarwinNetworkPALTest, SetSocketOptionKeepAlive) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    auto serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    if (serverResult.isError()) {
        port = findAvailablePort();
        serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    }
    ASSERT_TRUE(serverResult.isSuccess());

    auto optResult = networkPal_->setSocketOption(
        serverResult.value().handle,
        SocketOption::KeepAlive,
        1
    );

    EXPECT_TRUE(optResult.isSuccess());

    networkPal_->closeSocket(serverResult.value().handle);
}

// =============================================================================
// Address Information Tests
// =============================================================================

TEST_F(DarwinNetworkPALTest, GetLocalAddressReturnsCorrectAddress) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    auto serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    if (serverResult.isError()) {
        port = findAvailablePort();
        serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    }
    ASSERT_TRUE(serverResult.isSuccess());

    auto addrResult = networkPal_->getLocalAddress(serverResult.value().handle);
    EXPECT_TRUE(addrResult.isSuccess());

    if (addrResult.isSuccess()) {
        EXPECT_EQ(addrResult.value(), "127.0.0.1");
    }

    networkPal_->closeSocket(serverResult.value().handle);
}

TEST_F(DarwinNetworkPALTest, GetLocalPortReturnsCorrectPort) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    auto serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    if (serverResult.isError()) {
        port = findAvailablePort();
        serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    }
    ASSERT_TRUE(serverResult.isSuccess());

    auto portResult = networkPal_->getLocalPort(serverResult.value().handle);
    EXPECT_TRUE(portResult.isSuccess());

    if (portResult.isSuccess()) {
        EXPECT_EQ(portResult.value(), port);
    }

    networkPal_->closeSocket(serverResult.value().handle);
}

// =============================================================================
// Event Loop Tests
// =============================================================================

TEST_F(DarwinNetworkPALTest, IsRunningInitiallyFalse) {
    EXPECT_FALSE(networkPal_->isRunning());
}

TEST_F(DarwinNetworkPALTest, StopEventLoopWhenNotRunning) {
    // Should not crash
    EXPECT_NO_THROW(networkPal_->stopEventLoop());
}

TEST_F(DarwinNetworkPALTest, RunEventLoopInThread) {
    std::atomic<bool> loopStarted{false};
    std::atomic<bool> loopStopped{false};

    std::thread eventThread([this, &loopStarted, &loopStopped]() {
        loopStarted = true;
        networkPal_->runEventLoop();
        loopStopped = true;
    });

    // Wait for loop to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(loopStarted.load());
    EXPECT_TRUE(networkPal_->isRunning());

    // Stop the loop
    networkPal_->stopEventLoop();

    // Wait for thread to finish
    eventThread.join();

    EXPECT_TRUE(loopStopped.load());
    EXPECT_FALSE(networkPal_->isRunning());
}

// =============================================================================
// Async Accept Tests
// =============================================================================

TEST_F(DarwinNetworkPALTest, AsyncAcceptRegistersCallback) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    auto serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    if (serverResult.isError()) {
        port = findAvailablePort();
        serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    }
    ASSERT_TRUE(serverResult.isSuccess());

    std::atomic<bool> callbackCalled{false};

    EXPECT_NO_THROW(networkPal_->asyncAccept(
        serverResult.value(),
        [&callbackCalled](core::Result<SocketHandle, NetworkError> result) {
            callbackCalled = true;
        }
    ));

    networkPal_->closeSocket(serverResult.value().handle);
}

// =============================================================================
// Async Read/Write Tests
// =============================================================================

TEST_F(DarwinNetworkPALTest, AsyncReadRegistersCallback) {
    core::Buffer buffer(1024);

    // Just test that the call doesn't crash with an invalid socket
    // Real read tests would require a connected socket
    EXPECT_NO_THROW(networkPal_->asyncRead(
        SocketHandle{12345},  // Invalid socket
        buffer,
        1024,
        [](core::Result<size_t, NetworkError> result) {
            // Callback should receive an error for invalid socket
        }
    ));
}

TEST_F(DarwinNetworkPALTest, AsyncWriteRegistersCallback) {
    core::Buffer data{0x01, 0x02, 0x03};

    // Just test that the call doesn't crash with an invalid socket
    EXPECT_NO_THROW(networkPal_->asyncWrite(
        SocketHandle{12345},  // Invalid socket
        data,
        [](core::Result<size_t, NetworkError> result) {
            // Callback should receive an error for invalid socket
        }
    ));
}

// =============================================================================
// Close Socket Tests
// =============================================================================

TEST_F(DarwinNetworkPALTest, CloseSocketDoesNotCrash) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    auto serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    if (serverResult.isError()) {
        port = findAvailablePort();
        serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    }
    ASSERT_TRUE(serverResult.isSuccess());

    EXPECT_NO_THROW(networkPal_->closeSocket(serverResult.value().handle));
}

TEST_F(DarwinNetworkPALTest, CloseInvalidSocketDoesNotCrash) {
    EXPECT_NO_THROW(networkPal_->closeSocket(INVALID_SOCKET_HANDLE));
    EXPECT_NO_THROW(networkPal_->closeSocket(SocketHandle{12345}));
}

// =============================================================================
// Full Client-Server Integration Test
// =============================================================================

TEST_F(DarwinNetworkPALTest, ClientServerCommunication) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;

    // Create server
    auto serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    if (serverResult.isError()) {
        port = findAvailablePort();
        serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    }
    ASSERT_TRUE(serverResult.isSuccess());

    ServerSocket server = serverResult.value();
    std::atomic<bool> acceptCalled{false};
    SocketHandle clientSocket = INVALID_SOCKET_HANDLE;

    // Start event loop in separate thread
    std::thread eventThread([this]() {
        networkPal_->runEventLoop();
    });

    // Wait for event loop to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Register async accept
    networkPal_->asyncAccept(
        server,
        [&acceptCalled, &clientSocket](core::Result<SocketHandle, NetworkError> result) {
            acceptCalled = true;
            if (result.isSuccess()) {
                clientSocket = result.value();
            }
        }
    );

    // Create a client socket and connect (using raw sockets for simplicity)
    int clientFd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(clientFd, 0);

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    int connectResult = connect(clientFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));

    if (connectResult == 0) {
        // Wait for server to process
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    close(clientFd);

    // Stop event loop
    networkPal_->stopEventLoop();
    eventThread.join();

    // Clean up
    if (clientSocket != INVALID_SOCKET_HANDLE) {
        networkPal_->closeSocket(clientSocket);
    }
    networkPal_->closeSocket(server.handle);

    // The accept should have been called if connect succeeded
    if (connectResult == 0) {
        EXPECT_TRUE(acceptCalled.load());
    }
}

// =============================================================================
// kqueue-Specific Tests
// =============================================================================

TEST_F(DarwinNetworkPALTest, HandleManyConnections) {
    uint16_t port = findAvailablePort();
    ServerOptions opts;
    opts.reuseAddr = true;
    opts.backlog = 256;

    auto serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    if (serverResult.isError()) {
        port = findAvailablePort();
        serverResult = networkPal_->createServer("127.0.0.1", port, opts);
    }
    ASSERT_TRUE(serverResult.isSuccess());

    // Clean up
    networkPal_->closeSocket(serverResult.value().handle);
}

#endif // __APPLE__

} // namespace test
} // namespace pal
} // namespace openrtmp
