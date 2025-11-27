// OpenRTMP - Windows IOCP Research Spike
// Unit tests for WSAPoll Echo Server
//
// Tests validate the WSAPoll implementation against the benchmark scenarios:
// - Connection acceptance at scale (100, 500, up to 1024 connections)
// - Echo functionality correctness
// - Resource cleanup and shutdown
// - Demonstrates WSAPoll limitations

#ifdef _WIN32

#include <gtest/gtest.h>
#include "../wsapoll_echo_server.hpp"

#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <random>
#include <numeric>

namespace research {
namespace test {

// Test fixture for WSAPoll tests
class WSAPollServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }

    void TearDown() override {
        WSACleanup();
    }

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

    bool sendAndVerifyEcho(SOCKET sock, const std::vector<uint8_t>& data) {
        int sent = send(sock, reinterpret_cast<const char*>(data.data()),
                        static_cast<int>(data.size()), 0);
        if (sent != static_cast<int>(data.size())) return false;

        std::vector<uint8_t> recvBuf(data.size());
        int totalRecv = 0;
        while (totalRecv < sent) {
            int recv = ::recv(sock, reinterpret_cast<char*>(recvBuf.data() + totalRecv),
                              sent - totalRecv, 0);
            if (recv <= 0) return false;
            totalRecv += recv;
        }

        return recvBuf == data;
    }
};

// =============================================================================
// Basic Functionality Tests
// =============================================================================

TEST_F(WSAPollServerTest, InitializeAndListen) {
    WSAPollEchoServer server;

    EXPECT_TRUE(server.initialize());
    EXPECT_TRUE(server.listen(9300));
}

TEST_F(WSAPollServerTest, SingleConnectionEcho) {
    WSAPollEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9301));

    std::thread serverThread([&server]() {
        server.run();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SOCKET client = connectClient(9301);
    ASSERT_NE(client, INVALID_SOCKET);

    std::vector<uint8_t> testData = {0x01, 0x02, 0x03, 0x04, 0x05};
    EXPECT_TRUE(sendAndVerifyEcho(client, testData));

    closesocket(client);
    server.stop();
    serverThread.join();

    EXPECT_EQ(server.getStats().acceptedConnections.load(), 1);
}

TEST_F(WSAPollServerTest, LargeMessageEcho) {
    WSAPollEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9302));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SOCKET client = connectClient(9302);
    ASSERT_NE(client, INVALID_SOCKET);

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
// Scalability Tests
// =============================================================================

TEST_F(WSAPollServerTest, ConcurrentConnections100) {
    WSAPollEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9310));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int numConnections = 100;
    std::vector<SOCKET> clients;
    clients.reserve(numConnections);

    for (int i = 0; i < numConnections; ++i) {
        SOCKET client = connectClient(9310);
        if (client != INVALID_SOCKET) {
            clients.push_back(client);
        }
    }

    EXPECT_GE(clients.size(), static_cast<size_t>(numConnections * 0.95));

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

    for (SOCKET sock : clients) {
        closesocket(sock);
    }

    server.stop();
    serverThread.join();
}

TEST_F(WSAPollServerTest, ConcurrentConnections500) {
    WSAPollEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9311));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int numConnections = 500;
    std::vector<SOCKET> clients;
    clients.reserve(numConnections);

    for (int i = 0; i < numConnections; ++i) {
        SOCKET client = connectClient(9311);
        if (client != INVALID_SOCKET) {
            clients.push_back(client);
        }
    }

    // WSAPoll may have limitations here
    EXPECT_GE(clients.size(), static_cast<size_t>(numConnections * 0.85));

    std::vector<uint8_t> testData(128);
    std::iota(testData.begin(), testData.end(), 0);

    std::atomic<int> successCount{0};
    const int batchSize = 100;

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

    EXPECT_GE(successCount.load(), static_cast<int>(clients.size() * 0.80));

    for (SOCKET sock : clients) {
        closesocket(sock);
    }

    server.stop();
    serverThread.join();

    // Track poll calls to demonstrate O(n) behavior
    std::cout << "  Poll calls during test: " << server.getStats().pollCalls.load() << std::endl;
}

// Note: 1000 connection test may fail or be very slow with WSAPoll due to O(n) complexity
TEST_F(WSAPollServerTest, ConcurrentConnections1000_LimitedTest) {
    WSAPollEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9312));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // WSAPoll traditionally limited to 1024 fds per call
    // We attempt to connect but expect lower success rate
    const int numConnections = 1000;
    std::vector<SOCKET> clients;
    clients.reserve(numConnections);

    const int batchSize = 100;
    for (int batch = 0; batch < numConnections / batchSize; ++batch) {
        for (int i = 0; i < batchSize; ++i) {
            SOCKET client = connectClient(9312);
            if (client != INVALID_SOCKET) {
                clients.push_back(client);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // WSAPoll may struggle with 1000 connections
    // This test documents the limitation rather than expecting success
    std::cout << "  Connected " << clients.size() << " of " << numConnections << " clients" << std::endl;

    // Only test echo if we have a reasonable number of connections
    if (clients.size() >= 500) {
        std::vector<uint8_t> testData(64);
        std::iota(testData.begin(), testData.end(), 0);

        std::atomic<int> successCount{0};

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

        std::cout << "  Successful echoes: " << successCount.load() << std::endl;
    }

    for (SOCKET sock : clients) {
        closesocket(sock);
    }

    server.stop();
    serverThread.join();

    // Report poll call count to show O(n) overhead
    std::cout << "  Total poll calls: " << server.getStats().pollCalls.load() << std::endl;
}

// =============================================================================
// Poll Overhead Measurement
// =============================================================================

TEST_F(WSAPollServerTest, MeasurePollOverhead) {
    WSAPollEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9320));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Connect varying numbers of clients and measure poll calls
    std::vector<std::pair<int, uint64_t>> pollCounts;  // {connections, poll_calls}

    for (int targetConns : {10, 50, 100, 200}) {
        std::vector<SOCKET> clients;
        for (int i = 0; i < targetConns; ++i) {
            SOCKET client = connectClient(9320);
            if (client != INVALID_SOCKET) {
                clients.push_back(client);
            }
        }

        // Wait for connections to stabilize
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        uint64_t startPolls = server.getStats().pollCalls.load();

        // Do some work
        std::vector<uint8_t> testData = {0x01, 0x02};
        for (SOCKET sock : clients) {
            sendAndVerifyEcho(sock, testData);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        uint64_t endPolls = server.getStats().pollCalls.load();
        pollCounts.push_back({static_cast<int>(clients.size()), endPolls - startPolls});

        // Disconnect all
        for (SOCKET sock : clients) {
            closesocket(sock);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.stop();
    serverThread.join();

    // Print results showing O(n) behavior
    std::cout << "\n  Poll overhead by connection count:\n";
    for (const auto& [conns, polls] : pollCounts) {
        std::cout << "    " << conns << " connections: " << polls << " poll calls\n";
    }
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST_F(WSAPollServerTest, RapidConnectDisconnect) {
    WSAPollEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9330));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int iterations = 100;
    for (int i = 0; i < iterations; ++i) {
        SOCKET client = connectClient(9330);
        if (client != INVALID_SOCKET) {
            closesocket(client);
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    server.stop();
    serverThread.join();

    EXPECT_GE(server.getStats().acceptedConnections.load(), static_cast<uint64_t>(iterations * 0.8));
}

TEST_F(WSAPollServerTest, MultipleMessagesPerConnection) {
    WSAPollEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9331));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SOCKET client = connectClient(9331);
    ASSERT_NE(client, INVALID_SOCKET);

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

TEST_F(WSAPollServerTest, GracefulShutdown) {
    WSAPollEchoServer server;
    ASSERT_TRUE(server.initialize());
    ASSERT_TRUE(server.listen(9340));

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<SOCKET> clients;
    for (int i = 0; i < 10; ++i) {
        SOCKET client = connectClient(9340);
        if (client != INVALID_SOCKET) {
            clients.push_back(client);
        }
    }

    server.stop();

    auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (server.isRunning() && std::chrono::steady_clock::now() < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    serverThread.join();
    EXPECT_FALSE(server.isRunning());

    for (SOCKET sock : clients) {
        closesocket(sock);
    }
}

}  // namespace test
}  // namespace research

#else  // !_WIN32

#include <gtest/gtest.h>

TEST(WSAPollServerTest, SkippedOnNonWindows) {
    GTEST_SKIP() << "WSAPoll tests are Windows-only";
}

#endif  // _WIN32
