// OpenRTMP Simple Server Example
// A minimal RTMP server that accepts connections and streams

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

#include "openrtmp/api/rtmp_server.hpp"
#include "openrtmp/api/server_lifecycle.hpp"

std::atomic<bool> g_running{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

void printUsage(const char* programName) {
    std::cout << "OpenRTMP Server v0.1.0\n"
              << "Usage: " << programName << " [options]\n"
              << "\nOptions:\n"
              << "  -p, --port PORT       RTMP port (default: 1935)\n"
              << "  -c, --connections N   Max connections (default: 1000)\n"
              << "  -h, --help            Show this help\n"
              << "\nExample:\n"
              << "  " << programName << " -p 1935\n"
              << "\nPublish with OBS/FFmpeg:\n"
              << "  ffmpeg -re -i input.mp4 -c copy -f flv rtmp://localhost:1935/live/stream\n"
              << "\nPlay with FFmpeg:\n"
              << "  ffplay rtmp://localhost:1935/live/stream\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    uint16_t port = 1935;
    uint32_t maxConnections = 1000;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-c" || arg == "--connections") && i + 1 < argc) {
            maxConnections = static_cast<uint32_t>(std::stoi(argv[++i]));
        }
    }

    // Set up signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "╔══════════════════════════════════════════╗" << std::endl;
    std::cout << "║        OpenRTMP Server v0.1.0            ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    // Configure the server
    openrtmp::api::ServerConfig config;
    config.port = port;
    config.maxConnections = maxConnections;
    config.enableTLS = false;  // TLS disabled for simple example

    // Create and initialize server
    openrtmp::api::RTMPServer server;

    std::cout << "[INFO] Initializing server..." << std::endl;
    auto initResult = server.initialize(config);
    if (initResult.isError()) {
        std::cerr << "[ERROR] Failed to initialize: " << initResult.error().message << std::endl;
        return 1;
    }

    // Set up event callback for connection and stream events
    server.setEventCallback([](const openrtmp::api::ServerEvent& event) {
        switch (event.type) {
            case openrtmp::api::ServerEventType::ServerStarted:
                std::cout << "[SERVER] Server started" << std::endl;
                break;
            case openrtmp::api::ServerEventType::ServerStopped:
                std::cout << "[SERVER] Server stopped" << std::endl;
                break;
            case openrtmp::api::ServerEventType::ClientConnected:
                std::cout << "[CONN] Client connected: " << event.clientIP << ":" << event.clientPort << std::endl;
                break;
            case openrtmp::api::ServerEventType::ClientDisconnected:
                std::cout << "[CONN] Client disconnected: " << event.clientIP << ":" << event.clientPort << std::endl;
                break;
            case openrtmp::api::ServerEventType::StreamStarted:
                std::cout << "[STREAM] Publishing started: " << event.streamKey << std::endl;
                break;
            case openrtmp::api::ServerEventType::StreamEnded:
                std::cout << "[STREAM] Publishing stopped: " << event.streamKey << std::endl;
                break;
            case openrtmp::api::ServerEventType::SubscriberJoined:
                std::cout << "[STREAM] Subscriber joined: " << event.streamKey << std::endl;
                break;
            case openrtmp::api::ServerEventType::SubscriberLeft:
                std::cout << "[STREAM] Subscriber left: " << event.streamKey << std::endl;
                break;
            case openrtmp::api::ServerEventType::AuthenticationFailed:
                std::cout << "[AUTH] Authentication failed: " << event.message << std::endl;
                break;
            case openrtmp::api::ServerEventType::ConnectionLimitReached:
                std::cout << "[WARN] Connection limit reached" << std::endl;
                break;
            case openrtmp::api::ServerEventType::Error:
                std::cerr << "[ERROR] " << event.message << std::endl;
                break;
        }
    });

    // Start the server
    std::cout << "[INFO] Starting server on port " << port << "..." << std::endl;
    auto startResult = server.start();
    if (startResult.isError()) {
        std::cerr << "[ERROR] Failed to start: " << startResult.error().message << std::endl;
        return 1;
    }

    std::cout << "[INFO] Server running. Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;
    std::cout << "  RTMP URL: rtmp://localhost:" << port << "/live/<stream_key>" << std::endl;
    std::cout << std::endl;

    // Main loop - wait for shutdown signal
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Optionally print stats periodically
        static auto lastStats = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastStats).count() >= 30) {
            auto metrics = server.getServerMetrics();
            std::cout << "[STATS] Connections: " << metrics.activeConnections
                      << " | Streams: " << metrics.activeStreams
                      << " | Bytes In: " << metrics.bytesReceived
                      << " | Bytes Out: " << metrics.bytesSent << std::endl;
            lastStats = now;
        }
    }

    // Graceful shutdown
    std::cout << "[INFO] Stopping server..." << std::endl;
    server.stop(true);  // graceful shutdown
    std::cout << "[INFO] Server stopped. Goodbye!" << std::endl;

    return 0;
}
