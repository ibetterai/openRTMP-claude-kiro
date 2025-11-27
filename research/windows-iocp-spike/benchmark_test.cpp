// OpenRTMP - Windows IOCP Research Spike
// Benchmark Test for IOCP vs WSAPoll
//
// This test compares the performance of both approaches at different
// connection counts: 100, 500, and 1000 concurrent connections.
//
// For Windows compilation only. Build with:
// cl /EHsc /O2 benchmark_test.cpp /link ws2_32.lib mswsock.lib
//
// Or with CMake (see CMakeLists.txt in this directory)

#ifdef _WIN32

#include "iocp_echo_server.hpp"
#include "wsapoll_echo_server.hpp"

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <cstring>

namespace research {

// =============================================================================
// Benchmark Configuration
// =============================================================================

struct BenchmarkConfig {
    int numConnections;           // Number of concurrent connections
    int messagesPerConnection;    // Messages each connection sends
    int messageSize;              // Size of each message in bytes
    int warmupSeconds;            // Warmup period before measuring
    int measureSeconds;           // Measurement period
};

struct BenchmarkResult {
    std::string serverType;
    int connections;
    double messagesPerSecond;
    double bytesPerSecond;
    double avgLatencyMs;
    double p99LatencyMs;
    uint64_t totalMessages;
    uint64_t totalBytes;
    double cpuUsagePercent;
    uint64_t pollCalls;          // For WSAPoll
};

// =============================================================================
// Echo Client
// =============================================================================

/**
 * @brief Simple echo client for benchmarking.
 */
class EchoClient {
public:
    EchoClient(const std::string& host, uint16_t port, int messageSize)
        : host_(host)
        , port_(port)
        , messageSize_(messageSize)
        , socket_(INVALID_SOCKET)
        , running_(false)
    {
        // Generate random message data
        message_.resize(messageSize);
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        for (auto& byte : message_) {
            byte = static_cast<uint8_t>(dis(gen));
        }
    }

    ~EchoClient() {
        stop();
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }

    bool connect() {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) {
            return false;
        }

        // Disable Nagle
        int opt = 1;
        setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<char*>(&opt), sizeof(opt));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

        if (::connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }

        return true;
    }

    void start() {
        running_.store(true);
        clientThread_ = std::thread(&EchoClient::runLoop, this);
    }

    void stop() {
        running_.store(false);
        if (clientThread_.joinable()) {
            clientThread_.join();
        }
    }

    uint64_t getMessageCount() const { return messageCount_.load(); }
    uint64_t getByteCount() const { return byteCount_.load(); }
    double getAverageLatency() const {
        uint64_t count = latencyCount_.load();
        return count > 0 ? static_cast<double>(totalLatencyUs_.load()) / count / 1000.0 : 0.0;
    }

private:
    void runLoop() {
        std::vector<uint8_t> recvBuffer(messageSize_);

        while (running_.load()) {
            auto startTime = std::chrono::high_resolution_clock::now();

            // Send message
            int sent = send(socket_, reinterpret_cast<char*>(message_.data()),
                            static_cast<int>(message_.size()), 0);
            if (sent <= 0) {
                break;
            }

            // Receive echo
            int totalReceived = 0;
            while (totalReceived < sent && running_.load()) {
                int received = recv(socket_,
                                    reinterpret_cast<char*>(recvBuffer.data() + totalReceived),
                                    sent - totalReceived, 0);
                if (received <= 0) {
                    running_.store(false);
                    break;
                }
                totalReceived += received;
            }

            if (totalReceived == sent) {
                auto endTime = std::chrono::high_resolution_clock::now();
                auto latencyUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    endTime - startTime).count();

                messageCount_.fetch_add(1);
                byteCount_.fetch_add(sent * 2);  // Sent + received
                totalLatencyUs_.fetch_add(latencyUs);
                latencyCount_.fetch_add(1);
            }
        }
    }

private:
    std::string host_;
    uint16_t port_;
    int messageSize_;
    SOCKET socket_;
    std::vector<uint8_t> message_;

    std::thread clientThread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> messageCount_{0};
    std::atomic<uint64_t> byteCount_{0};
    std::atomic<uint64_t> totalLatencyUs_{0};
    std::atomic<uint64_t> latencyCount_{0};
};

// =============================================================================
// Benchmark Runner
// =============================================================================

template<typename ServerType>
BenchmarkResult runBenchmark(const std::string& serverType,
                             const BenchmarkConfig& config,
                             uint16_t port) {
    BenchmarkResult result;
    result.serverType = serverType;
    result.connections = config.numConnections;

    // Initialize Winsock for clients
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // Create and start server
    ServerType server;
    if (!server.initialize()) {
        std::cerr << "Failed to initialize " << serverType << " server" << std::endl;
        return result;
    }

    if (!server.listen(port)) {
        std::cerr << "Failed to listen on port " << port << std::endl;
        return result;
    }

    std::thread serverThread([&server]() {
        server.run();
    });

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create clients
    std::vector<std::unique_ptr<EchoClient>> clients;
    for (int i = 0; i < config.numConnections; ++i) {
        auto client = std::make_unique<EchoClient>("127.0.0.1", port, config.messageSize);
        if (client->connect()) {
            clients.push_back(std::move(client));
        } else {
            std::cerr << "Failed to connect client " << i << std::endl;
        }
    }

    std::cout << "  Connected " << clients.size() << " clients" << std::endl;

    // Start clients
    for (auto& client : clients) {
        client->start();
    }

    // Warmup period
    std::cout << "  Warming up for " << config.warmupSeconds << " seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(config.warmupSeconds));

    // Reset counters after warmup
    uint64_t startMessages = 0;
    uint64_t startBytes = 0;
    for (auto& client : clients) {
        startMessages += client->getMessageCount();
        startBytes += client->getByteCount();
    }

    // Measurement period
    std::cout << "  Measuring for " << config.measureSeconds << " seconds..." << std::endl;
    auto measureStart = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(config.measureSeconds));
    auto measureEnd = std::chrono::steady_clock::now();

    // Collect results
    uint64_t endMessages = 0;
    uint64_t endBytes = 0;
    double totalLatency = 0.0;
    for (auto& client : clients) {
        endMessages += client->getMessageCount();
        endBytes += client->getByteCount();
        totalLatency += client->getAverageLatency();
    }

    double measureDuration = std::chrono::duration<double>(measureEnd - measureStart).count();

    result.totalMessages = endMessages - startMessages;
    result.totalBytes = endBytes - startBytes;
    result.messagesPerSecond = result.totalMessages / measureDuration;
    result.bytesPerSecond = result.totalBytes / measureDuration;
    result.avgLatencyMs = totalLatency / clients.size();
    result.p99LatencyMs = result.avgLatencyMs * 2.5;  // Rough estimate

    // Get server stats
    const auto& stats = server.getStats();
    result.pollCalls = 0;  // Only for WSAPoll

    // Stop everything
    for (auto& client : clients) {
        client->stop();
    }
    clients.clear();

    server.stop();
    if (serverThread.joinable()) {
        serverThread.join();
    }

    return result;
}

// Specialization to get poll calls for WSAPoll server
template<>
BenchmarkResult runBenchmark<WSAPollEchoServer>(const std::string& serverType,
                                                  const BenchmarkConfig& config,
                                                  uint16_t port) {
    BenchmarkResult result;
    result.serverType = serverType;
    result.connections = config.numConnections;

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    WSAPollEchoServer server;
    if (!server.initialize() || !server.listen(port)) {
        std::cerr << "Failed to start " << serverType << " server" << std::endl;
        return result;
    }

    std::thread serverThread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<std::unique_ptr<EchoClient>> clients;
    for (int i = 0; i < config.numConnections; ++i) {
        auto client = std::make_unique<EchoClient>("127.0.0.1", port, config.messageSize);
        if (client->connect()) {
            clients.push_back(std::move(client));
        }
    }

    std::cout << "  Connected " << clients.size() << " clients" << std::endl;

    for (auto& client : clients) {
        client->start();
    }

    std::cout << "  Warming up for " << config.warmupSeconds << " seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(config.warmupSeconds));

    uint64_t startMessages = 0, startBytes = 0;
    for (auto& client : clients) {
        startMessages += client->getMessageCount();
        startBytes += client->getByteCount();
    }

    uint64_t startPollCalls = server.getStats().pollCalls.load();

    std::cout << "  Measuring for " << config.measureSeconds << " seconds..." << std::endl;
    auto measureStart = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(config.measureSeconds));
    auto measureEnd = std::chrono::steady_clock::now();

    uint64_t endMessages = 0, endBytes = 0;
    double totalLatency = 0.0;
    for (auto& client : clients) {
        endMessages += client->getMessageCount();
        endBytes += client->getByteCount();
        totalLatency += client->getAverageLatency();
    }

    double measureDuration = std::chrono::duration<double>(measureEnd - measureStart).count();

    result.totalMessages = endMessages - startMessages;
    result.totalBytes = endBytes - startBytes;
    result.messagesPerSecond = result.totalMessages / measureDuration;
    result.bytesPerSecond = result.totalBytes / measureDuration;
    result.avgLatencyMs = totalLatency / clients.size();
    result.p99LatencyMs = result.avgLatencyMs * 2.5;
    result.pollCalls = server.getStats().pollCalls.load() - startPollCalls;

    for (auto& client : clients) {
        client->stop();
    }
    clients.clear();

    server.stop();
    if (serverThread.joinable()) {
        serverThread.join();
    }

    return result;
}

void printResult(const BenchmarkResult& result) {
    std::cout << "\n  Results for " << result.serverType
              << " with " << result.connections << " connections:\n";
    std::cout << "    Messages/sec:    " << static_cast<int>(result.messagesPerSecond) << "\n";
    std::cout << "    Bytes/sec:       " << static_cast<int>(result.bytesPerSecond / 1024.0)
              << " KB/s\n";
    std::cout << "    Avg latency:     " << result.avgLatencyMs << " ms\n";
    std::cout << "    Total messages:  " << result.totalMessages << "\n";
    if (result.pollCalls > 0) {
        std::cout << "    Poll calls:      " << result.pollCalls << "\n";
    }
}

void runAllBenchmarks() {
    std::cout << "=================================================\n";
    std::cout << "Windows IOCP vs WSAPoll Benchmark\n";
    std::cout << "=================================================\n\n";

    std::vector<int> connectionCounts = {100, 500, 1000};
    std::vector<BenchmarkResult> iocpResults;
    std::vector<BenchmarkResult> wsapollResults;

    BenchmarkConfig config;
    config.messagesPerConnection = 0;  // Continuous
    config.messageSize = 1024;         // 1KB messages
    config.warmupSeconds = 2;
    config.measureSeconds = 10;

    uint16_t basePort = 9100;

    for (int connCount : connectionCounts) {
        config.numConnections = connCount;

        std::cout << "\n--- Testing with " << connCount << " connections ---\n";

        // Test IOCP
        std::cout << "\nTesting IOCP server...\n";
        auto iocpResult = runBenchmark<IOCPEchoServer>("IOCP", config, basePort);
        iocpResults.push_back(iocpResult);
        printResult(iocpResult);
        basePort += 2;

        // Give system time to release resources
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Test WSAPoll
        std::cout << "\nTesting WSAPoll server...\n";
        auto wsapollResult = runBenchmark<WSAPollEchoServer>("WSAPoll", config, basePort);
        wsapollResults.push_back(wsapollResult);
        printResult(wsapollResult);
        basePort += 2;

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // Print comparison summary
    std::cout << "\n=================================================\n";
    std::cout << "Comparison Summary\n";
    std::cout << "=================================================\n\n";

    std::cout << "Connections | IOCP msg/s  | WSAPoll msg/s | IOCP Advantage\n";
    std::cout << "------------|-------------|---------------|----------------\n";

    for (size_t i = 0; i < connectionCounts.size(); ++i) {
        double iocpMps = iocpResults[i].messagesPerSecond;
        double wsapollMps = wsapollResults[i].messagesPerSecond;
        double advantage = wsapollMps > 0 ? iocpMps / wsapollMps : 0;

        std::cout << std::setw(11) << connectionCounts[i] << " | "
                  << std::setw(11) << static_cast<int>(iocpMps) << " | "
                  << std::setw(13) << static_cast<int>(wsapollMps) << " | "
                  << std::fixed << std::setprecision(2) << advantage << "x\n";
    }

    std::cout << "\nLatency Comparison:\n";
    std::cout << "Connections | IOCP avg ms | WSAPoll avg ms\n";
    std::cout << "------------|-------------|---------------\n";

    for (size_t i = 0; i < connectionCounts.size(); ++i) {
        std::cout << std::setw(11) << connectionCounts[i] << " | "
                  << std::setw(11) << std::fixed << std::setprecision(2)
                  << iocpResults[i].avgLatencyMs << " | "
                  << std::setw(14) << wsapollResults[i].avgLatencyMs << "\n";
    }
}

}  // namespace research

int main() {
    research::runAllBenchmarks();
    return 0;
}

#else  // !_WIN32

#include <iostream>

int main() {
    std::cout << "This benchmark is for Windows only.\n";
    std::cout << "Please compile and run on a Windows system.\n";
    return 1;
}

#endif  // _WIN32
