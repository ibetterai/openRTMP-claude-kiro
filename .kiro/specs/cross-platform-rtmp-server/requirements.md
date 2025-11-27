# Requirements Document

## Introduction

This document specifies the requirements for OpenRTMP, a cross-platform RTMP (Real-Time Messaging Protocol) server designed to run on macOS, Windows 11, Linux, iPadOS, iOS, and Android. The server enables live video stream ingestion from encoders and broadcasters, and supports stream distribution to connected clients. The implementation must address platform-specific constraints while maintaining a consistent API and behavior across all supported operating systems.

## Requirements

### Requirement 1: RTMP Handshake Protocol
**Objective:** As a streaming client developer, I want the server to implement the standard RTMP handshake protocol, so that any RTMP-compliant encoder or player can establish connections.

#### Acceptance Criteria
1. When a client initiates a TCP connection on the configured RTMP port, the RTMP Server shall accept the connection and wait for the C0 handshake packet.
2. When the server receives a valid C0 packet, the RTMP Server shall respond with S0 and S1 packets within 100 milliseconds.
3. When the server receives a valid C1 packet, the RTMP Server shall respond with an S2 packet containing the echoed C1 timestamp and random data.
4. When the server receives a valid C2 packet matching the S1 data, the RTMP Server shall transition the connection to the established state.
5. If the handshake packets are malformed or out of sequence, the RTMP Server shall terminate the connection and log the error with client IP address.
6. If the handshake is not completed within 10 seconds of connection initiation, the RTMP Server shall terminate the connection with a timeout error.

### Requirement 2: RTMP Message Handling
**Objective:** As a streaming client developer, I want the server to correctly parse and process RTMP messages, so that stream data and commands are handled reliably.

#### Acceptance Criteria
1. The RTMP Server shall support chunk sizes from 128 bytes to 65536 bytes as per RTMP specification.
2. When a client sends a Set Chunk Size message, the RTMP Server shall acknowledge and use the new chunk size for subsequent parsing of that client's messages.
3. When the server receives an RTMP message spanning multiple chunks, the RTMP Server shall reassemble the complete message before processing.
4. The RTMP Server shall support all standard RTMP message types including audio (type 8), video (type 9), data (type 18), and command (type 20) messages.
5. When the server receives an Abort Message, the RTMP Server shall discard the partially received message for the specified chunk stream.
6. If an unknown or unsupported message type is received, the RTMP Server shall log the message type and continue processing subsequent messages.

### Requirement 3: RTMP Command Processing
**Objective:** As a broadcaster, I want the server to handle RTMP commands correctly, so that I can publish and manage my streams.

#### Acceptance Criteria
1. When a client sends a connect command, the RTMP Server shall validate the application name and respond with _result or _error within 50 milliseconds.
2. When a client sends a createStream command after successful connection, the RTMP Server shall allocate a stream ID and return it in the _result response.
3. When a client sends a publish command with a valid stream key, the RTMP Server shall transition to receiving mode and begin accepting audio/video data.
4. When a client sends a play command with a valid stream key, the RTMP Server shall begin transmitting the requested stream data if available.
5. When a client sends a deleteStream command, the RTMP Server shall release the stream resources and stop any associated data transmission.
6. When a client sends a closeStream command, the RTMP Server shall stop the stream but maintain the connection for potential reuse.
7. If a publish command specifies a stream key already in use, the RTMP Server shall reject the command with an error response indicating stream key conflict.

### Requirement 4: Stream Ingestion
**Objective:** As a broadcaster, I want to publish live streams to the server, so that my content can be received and potentially distributed to viewers.

#### Acceptance Criteria
1. When a publisher sends video data, the RTMP Server shall buffer and store the stream with the associated stream key for distribution.
2. The RTMP Server shall support H.264/AVC video codec for ingested streams.
3. The RTMP Server shall support AAC audio codec for ingested streams.
4. When a publisher sends enhanced RTMP metadata with HEVC/H.265 codec information, the RTMP Server shall accept and process the video data.
5. When a publisher disconnects unexpectedly, the RTMP Server shall mark the stream as unavailable within 5 seconds and notify connected subscribers.
6. The RTMP Server shall maintain a GOP (Group of Pictures) buffer of at least 2 seconds for each active stream to enable instant playback for new subscribers.
7. While receiving stream data, the RTMP Server shall validate timestamp continuity and log warnings for gaps exceeding 1 second.

### Requirement 5: Stream Distribution
**Objective:** As a viewer, I want to receive live streams from the server, so that I can watch broadcasts in real-time.

#### Acceptance Criteria
1. When a subscriber requests a stream that is currently being published, the RTMP Server shall begin transmitting data starting from the most recent keyframe.
2. The RTMP Server shall support multiple simultaneous subscribers for a single published stream.
3. When a new subscriber connects to an active stream, the RTMP Server shall send the cached metadata and sequence headers before stream data.
4. While distributing a stream, the RTMP Server shall maintain independent send buffers for each subscriber to prevent slow clients from affecting others.
5. If a subscriber's buffer exceeds 5 seconds of data, the RTMP Server shall drop non-keyframe packets to prevent unbounded memory growth.
6. When a stream ends, the RTMP Server shall send a stream EOF message to all connected subscribers within 1 second.

### Requirement 6: Cross-Platform Abstraction Layer
**Objective:** As a developer, I want a unified API across all platforms, so that I can build applications without platform-specific code paths.

#### Acceptance Criteria
1. The RTMP Server shall expose identical public APIs on macOS, Windows 11, Linux, iPadOS, iOS, and Android.
2. The RTMP Server shall abstract platform-specific socket implementations behind a common network interface.
3. The RTMP Server shall abstract platform-specific threading primitives behind a common concurrency interface.
4. The RTMP Server shall abstract platform-specific file I/O behind a common storage interface.
5. The RTMP Server shall abstract platform-specific time and timer functions behind a common timing interface.
6. When platform-specific behavior is unavoidable, the RTMP Server shall document the differences in the API documentation.

### Requirement 7: Desktop Platform Support (macOS, Windows, Linux)
**Objective:** As a desktop user, I want to run the RTMP server on my computer, so that I can host streams from my desktop environment.

#### Acceptance Criteria
1. The RTMP Server shall compile and run natively on macOS 12.0 (Monterey) and later versions.
2. The RTMP Server shall compile and run natively on Windows 11 with both x64 and ARM64 architectures.
3. The RTMP Server shall compile and run natively on Linux distributions with kernel 5.4 or later.
4. On desktop platforms, the RTMP Server shall support binding to any available network interface including localhost.
5. On desktop platforms, the RTMP Server shall support running as a background service or daemon.
6. On Windows, the RTMP Server shall integrate with Windows Firewall to request necessary port exceptions during installation.
7. On macOS, the RTMP Server shall request necessary network permissions through the standard macOS permission dialogs.

### Requirement 8: Mobile Platform Support (iOS, iPadOS, Android)
**Objective:** As a mobile user, I want to run the RTMP server on my mobile device, so that I can host streams directly from my phone or tablet.

#### Acceptance Criteria
1. The RTMP Server shall compile and run on iOS 15.0 and later versions.
2. The RTMP Server shall compile and run on iPadOS 15.0 and later versions.
3. The RTMP Server shall compile and run on Android API level 26 (Android 8.0) and later versions.
4. On iOS and iPadOS, the RTMP Server shall request local network access permission before binding to network interfaces.
5. On Android, the RTMP Server shall declare and request INTERNET and ACCESS_NETWORK_STATE permissions.
6. On mobile platforms, the RTMP Server shall operate within a single process without requiring additional background services.

### Requirement 9: Mobile Background Execution
**Objective:** As a mobile user, I want the server to continue operating when the app is in the background, so that streams are not interrupted when I switch apps.

#### Acceptance Criteria
1. On iOS and iPadOS, when the app enters background state, the RTMP Server shall request background task continuation to maintain active connections.
2. On iOS and iPadOS, while running in background, the RTMP Server shall minimize CPU usage by reducing non-essential processing.
3. On Android, when the app enters background state, the RTMP Server shall start a foreground service with a persistent notification to maintain operation.
4. On Android, the foreground service notification shall display current server status including active stream count and connection count.
5. If iOS background task time is about to expire, the RTMP Server shall gracefully notify connected clients of impending disconnection.
6. Where the device supports Background App Refresh on iOS, the RTMP Server shall register for background refresh to periodically check server state.

### Requirement 10: Mobile Battery Optimization
**Objective:** As a mobile user, I want the server to be battery-efficient, so that I can run it without rapidly draining my device battery.

#### Acceptance Criteria
1. While no active streams are present, the RTMP Server shall enter a low-power idle mode reducing CPU wake-ups to less than 1 per second.
2. The RTMP Server shall use platform-native asynchronous I/O to avoid busy-wait loops on all mobile platforms.
3. On mobile platforms, the RTMP Server shall batch network operations where possible to reduce radio wake-ups.
4. The RTMP Server shall provide battery usage statistics through the API including CPU time and network data transferred.
5. On iOS and iPadOS, the RTMP Server shall comply with App Store guidelines for background execution and battery usage.
6. On Android, the RTMP Server shall respect Doze mode and App Standby by reducing activity when the system requests power conservation.

### Requirement 11: Mobile Network Handling
**Objective:** As a mobile user, I want the server to handle network changes gracefully, so that streams can recover from Wi-Fi to cellular transitions.

#### Acceptance Criteria
1. When network connectivity changes on mobile platforms, the RTMP Server shall detect the change within 2 seconds.
2. When Wi-Fi connectivity is lost and cellular is available, the RTMP Server shall attempt to rebind to the cellular interface if configured to allow cellular.
3. When the primary network interface changes, the RTMP Server shall notify connected clients of the new server address if multicast discovery is enabled.
4. If network connectivity is lost entirely, the RTMP Server shall maintain connection state for up to 30 seconds to allow recovery without stream restart.
5. The RTMP Server shall provide configuration options to restrict operation to Wi-Fi only, cellular only, or any available network.
6. When operating on cellular network, the RTMP Server shall warn users about potential data usage through the API.

### Requirement 12: Performance - Latency
**Objective:** As a broadcaster, I want minimal latency in stream processing, so that my viewers receive content as close to real-time as possible.

#### Acceptance Criteria
1. The RTMP Server shall achieve glass-to-glass latency of less than 2 seconds on desktop platforms under normal operating conditions.
2. The RTMP Server shall achieve glass-to-glass latency of less than 3 seconds on mobile platforms under normal operating conditions.
3. The RTMP Server shall process incoming RTMP chunks and forward to subscribers within 50 milliseconds of receipt.
4. The RTMP Server shall provide a low-latency mode option that reduces buffering at the cost of potential playback stuttering.
5. When low-latency mode is enabled, the RTMP Server shall maintain a maximum buffer of 500 milliseconds per subscriber.
6. The RTMP Server shall expose latency metrics through the API including ingestion delay and distribution delay per stream.

### Requirement 13: Performance - Throughput
**Objective:** As a system administrator, I want the server to handle high bitrate streams, so that I can support high-quality video content.

#### Acceptance Criteria
1. On desktop platforms, the RTMP Server shall support ingestion of streams up to 50 Mbps bitrate.
2. On mobile platforms, the RTMP Server shall support ingestion of streams up to 20 Mbps bitrate.
3. On desktop platforms, the RTMP Server shall support aggregate distribution throughput of at least 500 Mbps.
4. On mobile platforms, the RTMP Server shall support aggregate distribution throughput of at least 100 Mbps.
5. The RTMP Server shall provide throughput metrics through the API including current bitrate per stream and total server throughput.
6. If incoming stream bitrate exceeds configured limits, the RTMP Server shall log a warning but continue processing unless explicitly configured to reject.

### Requirement 14: Performance - Concurrent Connections
**Objective:** As a system administrator, I want the server to handle multiple simultaneous connections, so that I can support multiple broadcasters and viewers.

#### Acceptance Criteria
1. On desktop platforms, the RTMP Server shall support at least 1000 concurrent TCP connections.
2. On mobile platforms, the RTMP Server shall support at least 100 concurrent TCP connections.
3. The RTMP Server shall support at least 10 simultaneous publishing streams on desktop platforms.
4. The RTMP Server shall support at least 3 simultaneous publishing streams on mobile platforms.
5. The RTMP Server shall implement connection pooling to efficiently manage connection resources.
6. When maximum connection limits are reached, the RTMP Server shall reject new connections with an appropriate RTMP error code and log the event.

### Requirement 15: Authentication
**Objective:** As a system administrator, I want to control who can publish and subscribe to streams, so that I can prevent unauthorized access.

#### Acceptance Criteria
1. The RTMP Server shall support stream key-based authentication for publish operations.
2. When authentication is enabled, the RTMP Server shall validate stream keys against a configured list or callback before accepting publish commands.
3. The RTMP Server shall support optional authentication for play/subscribe operations.
4. The RTMP Server shall support integration with external authentication services via HTTP callbacks.
5. If authentication fails, the RTMP Server shall reject the connection with an appropriate error message and log the attempt with client IP.
6. The RTMP Server shall support IP-based access control lists for both allow and deny rules.
7. The RTMP Server shall rate-limit authentication attempts to prevent brute-force attacks, allowing no more than 5 failed attempts per IP per minute.

### Requirement 16: Transport Security
**Objective:** As a security-conscious user, I want encrypted connections, so that stream data cannot be intercepted.

#### Acceptance Criteria
1. The RTMP Server shall support RTMPS (RTMP over TLS) for encrypted connections.
2. When RTMPS is enabled, the RTMP Server shall require TLS 1.2 or higher.
3. The RTMP Server shall support configurable TLS certificates including self-signed certificates for development.
4. The RTMP Server shall support automatic certificate management via ACME protocol where platform APIs permit.
5. When both RTMP and RTMPS are enabled, the RTMP Server shall operate on separate configurable ports for each protocol.
6. The RTMP Server shall support certificate pinning for enhanced security in controlled deployments.

### Requirement 17: Configuration Management
**Objective:** As a system administrator, I want flexible configuration options, so that I can customize server behavior for my use case.

#### Acceptance Criteria
1. The RTMP Server shall support configuration through a JSON or YAML configuration file.
2. The RTMP Server shall support configuration through environment variables for containerized deployments.
3. The RTMP Server shall support runtime configuration changes for non-critical parameters without restart.
4. The RTMP Server shall validate configuration on startup and report specific errors for invalid settings.
5. When configuration file is not present, the RTMP Server shall use sensible defaults and log the default values being used.
6. The RTMP Server shall provide a configuration dump command to display current effective configuration.

### Requirement 18: Logging and Monitoring
**Objective:** As a system administrator, I want comprehensive logging and metrics, so that I can monitor server health and troubleshoot issues.

#### Acceptance Criteria
1. The RTMP Server shall support configurable log levels including debug, info, warning, and error.
2. The RTMP Server shall log all connection events including connect, disconnect, publish start, publish stop, play start, and play stop.
3. The RTMP Server shall expose metrics through a programmatic API including active connections, active streams, bandwidth usage, and error counts.
4. On desktop platforms, the RTMP Server shall support log output to file with configurable rotation.
5. On mobile platforms, the RTMP Server shall integrate with platform-native logging systems (os_log on iOS, Logcat on Android).
6. The RTMP Server shall support structured logging in JSON format for integration with log aggregation systems.
7. When an error occurs, the RTMP Server shall log sufficient context to diagnose the issue including relevant stream keys, client IPs, and error codes.

### Requirement 19: Graceful Shutdown
**Objective:** As a system administrator, I want clean server shutdown, so that active streams are not abruptly terminated.

#### Acceptance Criteria
1. When a shutdown signal is received, the RTMP Server shall stop accepting new connections immediately.
2. When a shutdown signal is received, the RTMP Server shall notify all connected publishers of impending shutdown.
3. When a shutdown signal is received, the RTMP Server shall allow active streams up to 30 seconds to complete gracefully.
4. If active streams do not complete within the grace period, the RTMP Server shall forcefully terminate remaining connections.
5. The RTMP Server shall provide a force-shutdown option that immediately terminates all connections without grace period.
6. Upon shutdown completion, the RTMP Server shall log a summary of connections terminated and streams interrupted.

### Requirement 20: Error Handling and Recovery
**Objective:** As a user, I want the server to handle errors gracefully, so that temporary issues do not cause permanent failures.

#### Acceptance Criteria
1. If a single client connection fails, the RTMP Server shall isolate the failure and continue serving other clients.
2. If memory allocation fails, the RTMP Server shall log the error and reject new connections until memory is available.
3. If a thread or task crashes, the RTMP Server shall log the error and attempt to restart the affected component.
4. The RTMP Server shall implement circuit breaker patterns for external service calls (authentication callbacks).
5. If the server encounters an unrecoverable error, the RTMP Server shall log comprehensive diagnostic information before terminating.
6. The RTMP Server shall provide health check endpoints or APIs to enable external monitoring systems to detect degraded states.
