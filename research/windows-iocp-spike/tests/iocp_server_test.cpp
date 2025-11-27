// OpenRTMP - Windows IOCP Research Spike
// Unit tests for IOCP Echo Server
//
// Tests validate the IOCP implementation against the benchmark scenarios:
// - Connection acceptance at scale (100, 500, 1000 connections)
// - Echo functionality correctness
// - Resource cleanup and shutdown

#ifdef _WIN32

#include <gtest/gtest.h>
#include "../iocp_echo_server.hpp"

#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <random>

namespace research {
namespace test {

// Test fixture for IOCP tests
class IOCPServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize Winsock
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }

    void TearDown() override {
        WSACleanup();
    }

    // Helper to create and connect a client socket
    SOCKET connectClient(uint16_t port) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return INVALID_SOCKET;

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(sock);
            return INVALID_SOCKET;
        }

        return sock;
    }

    // Helper to send and receive echo data
    bool sendAndVerifyEcho(SOCKET sock, const std::vector<uint8_t>& data) {
        // Send
        int sent = send(sock, reinterpret_cast<const char*>(data.data()),
                        static_cast<int>(data.size()), 0);
        if (sent != static_cast<int>(data.size())) return false;

        // Receive
        std::vector<uint8_t> recvBuf(data.size());
        int totalRecv = 0;
        while (totalRecv < sent) {
            int recv = ::recv(sock, reinterpret_cast<char*>(recvBuf.data() + totalRecv),
                              sent - totalRecv, 0);
            if (recv <= 0) return false;
            totalRecv += recv;
        }

        // Verify
        return recvBuf == data;
    }
};

// =============================================================================
// Basic Functionality Tests
// =============================================================================

TEST_F(IOCPServerTest, InitializeAndListen) {
    IOCPEchoServer server;

    EXPECT_TRUE(server.initialize());
    EXPECT_TRUE(server.listen(9200));
}

TEST_F(IOCPServerTest, SingleConnectionEcho) {
    IOCPEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9201));

    std::thread serverThread([&server]() {
        server.run();
    });

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Connect client
    SOCKET client = connectClient(9201);
    ASSERT_NE(client, INVALID_SOCKET);

    // Send data and verify echo
    std::vector<uint8_t> testData = {0x01, 0x02, 0x03, 0x04, 0x05};
    EXPECT_TRUE(sendAndVerifyEcho(client, testData));

    // Cleanup
    closesocket(client);
    server.stop();
    serverThread.join();

    // Verify stats
    EXPECT_EQ(server.getStats().acceptedConnections.load(), 1);
    EXPECT_GE(server.getStats().bytesReceived.load(), 5);
    EXPECT_GE(server.getStats().bytesSent.load(), 5);
}

TEST_F(IOCPServerTest, LargeMessageEcho) {
    IOCPEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9202));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SOCKET client = connectClient(9202);
    ASSERT_NE(client, INVALID_SOCKET);

    // Generate large random message
    std::vector<uint8_t> largeData(4096);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (auto& byte : largeData) {
        byte = static_cast<uint8_t>(dis(gen));
    }

    EXPECT_TRUE(sendAndVerifyEcho(client, largeData));

    closesocket(client);
    server.stop();
    serverThread.join();
}

// =============================================================================
// Scalability Tests (Benchmark Scenarios)
// =============================================================================

TEST_F(IOCPServerTest, ConcurrentConnections100) {
    IOCPEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9210));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int numConnections = 100;
    std::vector<SOCKET> clients;
    clients.reserve(numConnections);

    // Connect all clients
    for (int i = 0; i < numConnections; ++i) {
        SOCKET client = connectClient(9210);
        if (client != INVALID_SOCKET) {
            clients.push_back(client);
        }
    }

    EXPECT_GE(clients.size(), static_cast<size_t>(numConnections * 0.95));  // 95% success rate

    // Each client sends a message
    std::vector<uint8_t> testData = {0xAA, 0xBB, 0xCC, 0xDD};
    std::atomic<int> successCount{0};

    std::vector<std::thread> clientThreads;
    for (SOCKET sock : clients) {
        clientThreads.emplace_back([this, sock, &testData, &successCount]() {
            if (sendAndVerifyEcho(sock, testData)) {
                successCount.fetch_add(1);
            }
        });
    }

    for (auto& t : clientThreads) {
        t.join();
    }

    EXPECT_GE(successCount.load(), static_cast<int>(clients.size() * 0.95));

    // Cleanup
    for (SOCKET sock : clients) {
        closesocket(sock);
    }

    server.stop();
    serverThread.join();

    EXPECT_EQ(server.getStats().acceptedConnections.load(), clients.size());
}

TEST_F(IOCPServerTest, ConcurrentConnections500) {
    IOCPEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9211));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int numConnections = 500;
    std::vector<SOCKET> clients;
    clients.reserve(numConnections);

    for (int i = 0; i < numConnections; ++i) {
        SOCKET client = connectClient(9211);
        if (client != INVALID_SOCKET) {
            clients.push_back(client);
        }
    }

    EXPECT_GE(clients.size(), static_cast<size_t>(numConnections * 0.90));  // 90% for larger scale

    std::vector<uint8_t> testData(256);
    std::iota(testData.begin(), testData.end(), 0);

    std::atomic<int> successCount{0};
    std::vector<std::thread> clientThreads;

    for (SOCKET sock : clients) {
        clientThreads.emplace_back([this, sock, &testData, &successCount]() {
            if (sendAndVerifyEcho(sock, testData)) {
                successCount.fetch_add(1);
            }
        });
    }

    for (auto& t : clientThreads) {
        t.join();
    }

    EXPECT_GE(successCount.load(), static_cast<int>(clients.size() * 0.90));

    for (SOCKET sock : clients) {
        closesocket(sock);
    }

    server.stop();
    serverThread.join();
}

TEST_F(IOCPServerTest, ConcurrentConnections1000) {
    IOCPEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9212));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // More time for larger test

    const int numConnections = 1000;
    std::vector<SOCKET> clients;
    clients.reserve(numConnections);

    // Connect in batches to avoid overwhelming the accept queue
    const int batchSize = 100;
    for (int batch = 0; batch < numConnections / batchSize; ++batch) {
        for (int i = 0; i < batchSize; ++i) {
            SOCKET client = connectClient(9212);
            if (client != INVALID_SOCKET) {
                clients.push_back(client);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_GE(clients.size(), static_cast<size_t>(numConnections * 0.85));  // 85% for 1000

    std::vector<uint8_t> testData(128);
    std::iota(testData.begin(), testData.end(), 0);

    std::atomic<int> successCount{0};

    // Process in batches to manage thread creation
    for (size_t start = 0; start < clients.size(); start += batchSize) {
        std::vector<std::thread> batchThreads;
        size_t end = std::min(start + batchSize, clients.size());

        for (size_t i = start; i < end; ++i) {
            batchThreads.emplace_back([this, sock = clients[i], &testData, &successCount]() {
                if (sendAndVerifyEcho(sock, testData)) {
                    successCount.fetch_add(1);
                }
            });
        }

        for (auto& t : batchThreads) {
            t.join();
        }
    }

    EXPECT_GE(successCount.load(), static_cast<int>(clients.size() * 0.85));

    for (SOCKET sock : clients) {
        closesocket(sock);
    }

    server.stop();
    serverThread.join();

    // Verify IOCP handled the scale
    EXPECT_GE(server.getStats().acceptedConnections.load(), clients.size());
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST_F(IOCPServerTest, RapidConnectDisconnect) {
    IOCPEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9220));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Rapidly connect and disconnect
    const int iterations = 100;
    for (int i = 0; i < iterations; ++i) {
        SOCKET client = connectClient(9220);
        if (client != INVALID_SOCKET) {
            closesocket(client);
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    server.stop();
    serverThread.join();

    // Server should have handled all connections without crash
    EXPECT_GE(server.getStats().acceptedConnections.load(), static_cast<uint64_t>(iterations * 0.8));
}

TEST_F(IOCPServerTest, MultipleMessagesPerConnection) {
    IOCPEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9221));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SOCKET client = connectClient(9221);
    ASSERT_NE(client, INVALID_SOCKET);

    // Send multiple messages
    const int numMessages = 50;
    std::vector<uint8_t> testData = {0x11, 0x22, 0x33, 0x44};

    for (int i = 0; i < numMessages; ++i) {
        EXPECT_TRUE(sendAndVerifyEcho(client, testData));
    }

    closesocket(client);
    server.stop();
    serverThread.join();

    EXPECT_GE(server.getStats().completedEchoes.load(), static_cast<uint64_t>(numMessages));
}

// =============================================================================
// Resource Management Tests
// =============================================================================

TEST_F(IOCPServerTest, GracefulShutdown) {
    IOCPEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9230));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Connect some clients
    std::vector<SOCKET> clients;
    for (int i = 0; i < 10; ++i) {
        SOCKET client = connectClient(9230);
        if (client != INVALID_SOCKET) {
            clients.push_back(client);
        }
    }

    // Stop server while connections exist
    server.stop();

    // Server thread should exit cleanly
    auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (server.isRunning() && std::chrono::steady_clock::now() < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    serverThread.join();
    EXPECT_FALSE(server.isRunning());

    // Cleanup
    for (SOCKET sock : clients) {
        closesocket(sock);
    }
}

}  // namespace test
}  // namespace research

#else  // !_WIN32

#include <gtest/gtest.h>

TEST(IOCPServerTest, SkippedOnNonWindows) {
    GTEST_SKIP() << "IOCP tests are Windows-only";
}

#endif  // _WIN32
