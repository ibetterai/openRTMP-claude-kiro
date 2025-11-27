# Implementation Plan

## Tasks

- [ ] 1. Project Foundation and Build System
- [ ] 1.1 (P) Configure CMake cross-platform build system
  - Set up root CMakeLists.txt with C++17 standard requirement
  - Define platform detection logic for macOS, Windows, Linux, iOS, iPadOS, Android
  - Configure conditional compilation flags and platform-specific source directories
  - Set up dependency management for OpenSSL/BoringSSL, spdlog, Google Test
  - Create build presets for each target platform with appropriate toolchains
  - _Requirements: 7.1, 7.2, 7.3, 8.1, 8.2, 8.3_

- [ ] 1.2 (P) Establish project directory structure and common types
  - Create source directories for public API, protocol, streaming, core, PAL layers
  - Define common result types, error codes, and shared data structures
  - Implement basic memory buffer utilities and byte manipulation helpers
  - Set up namespace hierarchy matching the layered architecture
  - _Requirements: 6.1_

- [ ] 2. Platform Abstraction Layer - Core Interfaces
- [ ] 2.1 Define PAL interface contracts
  - Create abstract interface for network operations with async callbacks
  - Create abstract interface for threading primitives and thread pools
  - Create abstract interface for timer scheduling and high-resolution time
  - Create abstract interface for file I/O operations
  - Create abstract interface for platform-native logging
  - Document interface contracts with preconditions and postconditions
  - _Requirements: 6.2, 6.3, 6.4, 6.5, 6.6_

- [ ] 2.2 (P) Implement macOS/iOS PAL using kqueue
  - Implement kqueue-based event loop for async network I/O
  - Implement TCP server socket binding and connection acceptance
  - Implement async read/write operations with scatter-gather support
  - Implement pthread-based threading with Grand Central Dispatch integration
  - Implement timer scheduling using dispatch timers
  - Integrate with os_log for platform-native logging
  - _Requirements: 6.2, 6.3, 6.5, 7.1, 8.1, 8.2, 18.5_

- [ ] 2.3 Conduct Windows IOCP research spike
  - Build proof-of-concept for IOCP vs WSAPoll approaches with simple echo server
  - Benchmark performance at 100, 500, and 1000 concurrent connections
  - Study libuv's Windows implementation patterns for callback adaptation
  - Document IOCP-to-readiness model adaptation strategy with trade-offs
  - Produce decision document with recommended approach for production implementation
  - _Requirements: 6.2, 7.2_
  - _Estimated Duration: 2-3 days_

- [ ] 2.4 (P) Implement Windows PAL using IOCP
  - Implement chosen approach from research spike (IOCP or WSAPoll fallback)
  - Implement Winsock2 TCP server socket with overlapped I/O (if IOCP) or WSAPoll
  - Implement async read/write using completion callbacks
  - Implement Windows thread pool and synchronization primitives
  - Implement timer queue for scheduled callbacks
  - Implement Event Log integration for logging
  - _Requirements: 6.2, 6.3, 6.5, 7.2, 18.5_
  - _Depends on: 2.3 (IOCP research spike)_

- [ ] 2.5 (P) Implement Linux/Android PAL using epoll
  - Implement epoll-based event loop for async network I/O
  - Implement TCP server socket with edge-triggered notifications
  - Implement async read/write with buffer management
  - Implement pthread-based threading with futex support
  - Implement timerfd for timer scheduling
  - Implement Logcat integration for Android logging
  - _Requirements: 6.2, 6.3, 6.5, 7.3, 8.3, 18.5_

- [ ] 3. RTMP Handshake Protocol
- [ ] 3.1 Implement handshake state machine
  - Create state tracking for WaitingC0, WaitingC1, WaitingC2, Complete, Failed
  - Generate cryptographically random 1528-byte S1 data using platform random
  - Validate C0 version byte (must be 3 for RTMP)
  - Process C1 packet and generate S0+S1+S2 response
  - Validate C2 echo matches S1 data with timestamp verification
  - Implement response within 100ms latency target
  - _Requirements: 1.2, 1.3, 1.4_

- [ ] 3.2 Implement handshake timeout and error handling
  - Start 10-second timeout timer on connection acceptance
  - Cancel timer on successful handshake completion
  - Terminate connection with logged error on timeout
  - Detect and reject malformed packets with sequence errors
  - Log handshake failures with client IP address for diagnostics
  - Transition connection to established state on success
  - _Requirements: 1.1, 1.5, 1.6_

- [ ] 4. RTMP Chunk Parsing and Message Assembly
- [ ] 4.1 Implement chunk stream parser
  - Parse Basic Header to extract chunk stream ID and format type
  - Parse Message Header variants (Type 0-3) based on format
  - Support chunk sizes from 128 to 65536 bytes per specification
  - Track per-chunk-stream state for header compression
  - Handle Set Chunk Size protocol control message
  - Process Abort Message to discard partial chunk stream data
  - _Requirements: 2.1, 2.2, 2.5_

- [ ] 4.2 Implement message reassembly from chunks
  - Buffer partial message data across multiple chunks
  - Reassemble complete messages before forwarding for processing
  - Support all standard RTMP message types (audio 8, video 9, data 18, command 20)
  - Log unknown message types and continue processing
  - Validate message integrity and length consistency
  - _Requirements: 2.3, 2.4, 2.6_

- [ ] 5. AMF Encoding and Command Processing
- [ ] 5.1 Implement AMFCodec component
  - Create IAMFCodec interface with decodeAMF0, encodeAMF0, decodeAMF3, encodeAMF3 methods
  - Implement AMFValue variant type supporting all AMF data types
  - Decode AMF0 types: Number (0x00), Boolean (0x01), String (0x02), Object (0x03), Null (0x05), Undefined (0x06), Reference (0x07), ECMAArray (0x08), StrictArray (0x0A), Date (0x0B), Long String (0x0C)
  - Decode AMF3 types with reference table and traits support
  - Encode AMF0/AMF3 response messages for command results
  - Implement AMFError with detailed error codes and byte offset reporting
  - Enforce safety limits: 32 levels max nesting, 64KB max string length
  - Reset reference tables per command message
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7_

- [ ] 5.2 Implement RTMP command handler
  - Process connect command with application name validation
  - Respond with _result or _error within 50ms latency target
  - Process createStream command and allocate stream IDs
  - Process publish command with stream key validation and conflict detection
  - Process play command to initiate subscription
  - Process deleteStream and closeStream for cleanup
  - Reject duplicate stream key publish attempts with error response
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7_

- [ ] 6. Stream Registry and Session Management
- [ ] 6.1 Implement stream registry
  - Maintain map of active streams by stream key
  - Track publisher and subscriber associations per stream
  - Allocate and release stream IDs atomically
  - Support concurrent access with appropriate locking
  - Emit domain events for stream lifecycle changes
  - _Requirements: 3.2, 3.7, 4.1_

- [ ] 6.2 Implement session state machine
  - Track connection states: Connecting, Handshaking, Connected, Publishing, Subscribing, Disconnected
  - Enforce valid state transitions for RTMP commands
  - Associate sessions with connections and streams
  - Handle unexpected disconnection with state cleanup
  - Maintain session context for authentication and authorization
  - _Requirements: 3.3, 3.4, 3.5, 3.6_

- [ ] 7. Stream Ingestion Pipeline
- [ ] 7.1 Implement media data reception
  - Accept audio (type 8) and video (type 9) message types
  - Parse codec sequence headers for H.264/AVC and AAC
  - Detect Enhanced RTMP FourCC for H.265/HEVC codec support
  - Identify keyframes from NAL unit types for buffer management
  - Store stream metadata from data messages (type 18)
  - Forward validated media to GOP buffer and distribution
  - _Requirements: 4.1, 4.2, 4.3, 4.4_

- [ ] 7.2 Implement timestamp validation and publisher lifecycle
  - Monitor timestamp continuity and log gaps exceeding 1 second
  - Mark stream unavailable within 5 seconds of unexpected publisher disconnect
  - Notify connected subscribers of stream unavailability
  - Track stream statistics including bitrate and frame counts
  - Support graceful unpublish with resource cleanup
  - _Requirements: 4.5, 4.7_

- [ ] 8. GOP Buffer and Media Caching
- [ ] 8.1 Implement circular GOP buffer
  - Maintain minimum 2 seconds of buffered media per stream
  - Index keyframe positions for instant playback start
  - Store metadata and codec sequence headers separately
  - Implement reference-counted buffer sharing for distribution
  - Support configurable buffer duration for low-latency mode
  - _Requirements: 4.6, 5.1, 5.3_

- [ ] 8.2 Implement buffer overflow protection
  - Track per-subscriber buffer levels independently
  - Drop non-keyframe packets when buffer exceeds 5 seconds
  - Preserve keyframes and audio to maintain stream continuity
  - Log dropped frame statistics per subscriber
  - Support configurable maximum buffer thresholds
  - _Requirements: 5.4, 5.5_

- [ ] 9. Stream Distribution Engine
- [ ] 9.1 Implement subscriber management
  - Add subscribers to active streams with configuration options
  - Maintain independent send buffers per subscriber
  - Support low-latency mode with 500ms maximum buffer
  - Remove subscribers cleanly on disconnect or stop
  - Track subscriber statistics including bytes delivered and dropped frames
  - _Requirements: 5.2, 5.4, 12.4, 12.5_

- [ ] 9.2 Implement media distribution
  - Send cached metadata and sequence headers on subscription start
  - Transmit from most recent keyframe for instant playback
  - Forward live media from ingestion pipeline to all subscribers
  - Send stream EOF message within 1 second of stream end
  - Implement slow subscriber detection and frame dropping
  - _Requirements: 5.1, 5.3, 5.6_

- [ ] 10. Authentication Service
- [ ] 10.1 Implement stream key authentication
  - Validate stream keys against configured allow list
  - Support dynamic stream key generation and revocation
  - Integrate with command processor for publish/play validation
  - Log authentication attempts with client IP and outcome
  - Return appropriate error responses for authentication failures
  - _Requirements: 15.1, 15.2, 15.3, 15.5_

- [ ] 10.2 Implement external authentication callback
  - Support HTTP POST callback to external auth service
  - Send app name, stream key, client IP, and action in request
  - Handle callback timeout with configurable fallback behavior
  - Implement circuit breaker for failed auth service calls
  - Cache positive authentication results for performance
  - _Requirements: 15.4, 20.4_

- [ ] 10.3 Implement access control and rate limiting
  - Support IP-based allow and deny ACL rules with CIDR notation
  - Enforce rate limiting of 5 failed auth attempts per IP per minute
  - Block IPs exceeding rate limit for configurable duration
  - Log rate limit violations with client details
  - Support application-scoped ACL rules
  - _Requirements: 15.6, 15.7_

- [ ] 11. TLS and Transport Security
- [ ] 11.1 Implement TLS service for RTMPS
  - Load TLS certificates and private keys from file paths
  - Require minimum TLS 1.2 version with configurable cipher suites
  - Wrap TCP connections with TLS encryption layer
  - Support self-signed certificates for development environments
  - Operate RTMP and RTMPS on separate configurable ports
  - _Requirements: 16.1, 16.2, 16.3, 16.5_

- [ ] 11.2 (P) Implement advanced TLS features
  - Support ACME protocol for automatic certificate provisioning
  - Implement certificate pinning for controlled deployments
  - Handle certificate expiration and renewal gracefully
  - Validate client certificates when mutual TLS is enabled
  - _Requirements: 16.4, 16.6_

- [ ] 12. Configuration Management
- [ ] 12.1 Implement configuration loading
  - Parse JSON and YAML configuration file formats
  - Support environment variable overrides for containerized deployments
  - Validate configuration schema on startup with detailed error messages
  - Apply sensible defaults when configuration file is absent
  - Log effective configuration values during initialization
  - _Requirements: 17.1, 17.2, 17.4, 17.5_

- [ ] 12.2 Implement runtime configuration
  - Support hot-reload of non-critical parameters without restart
  - Provide configuration dump command for inspection
  - Validate configuration changes before applying
  - Emit events on configuration changes for component notification
  - _Requirements: 17.3, 17.6_

- [ ] 13. Logging and Metrics
- [ ] 13.1 Implement structured logging
  - Support configurable log levels: debug, info, warning, error
  - Log all connection events with timestamps and client details
  - Support JSON structured log format for aggregation systems
  - Include context in error logs: stream keys, client IPs, error codes
  - Route to platform-native logging on mobile
  - _Requirements: 18.1, 18.2, 18.6, 18.7_

- [ ] 13.2 (P) Implement log rotation and output
  - Support log file output with configurable rotation policies
  - Implement size-based and time-based rotation triggers
  - Compress rotated log files for storage efficiency
  - Integrate with os_log on iOS and Logcat on Android
  - _Requirements: 18.4, 18.5_

- [ ] 13.3 Implement metrics collection
  - Track active connections, streams, and bandwidth usage
  - Expose metrics through programmatic API
  - Calculate per-stream ingestion and distribution delays
  - Provide latency metrics for monitoring
  - Track error counts by category
  - _Requirements: 12.6, 13.5, 18.3_

- [ ] 14. Connection Management and Resource Limits
- [ ] 14.1 Implement connection pool and limits
  - Pre-allocate connection objects to avoid allocation during accept
  - Support 1000 concurrent connections on desktop platforms
  - Support 100 concurrent connections on mobile platforms
  - Reject new connections with appropriate error when limits reached
  - Log connection limit events for capacity planning
  - _Requirements: 14.1, 14.2, 14.5, 14.6_

- [ ] 14.2 Implement publishing stream limits
  - Enforce 10 simultaneous publishers on desktop platforms
  - Enforce 3 simultaneous publishers on mobile platforms
  - Return error response when publishing limits reached
  - Track and report current publisher count in metrics
  - _Requirements: 14.3, 14.4_

- [ ] 15. Desktop Platform Integration
- [ ] 15.1 (P) Implement macOS service wrapper
  - Create launchd service configuration for daemon operation
  - Request local network access permission through system dialogs
  - Support binding to any available network interface
  - Handle macOS-specific signal handling for graceful shutdown
  - _Requirements: 7.4, 7.5, 7.7_

- [ ] 15.2 (P) Implement Windows service integration
  - Create Windows Service Control Manager integration
  - Register with Windows Firewall for port exception during installation
  - Support both x64 and ARM64 architectures
  - Implement SCM event handling for service control
  - _Requirements: 7.4, 7.5, 7.6_

- [ ] 15.3 (P) Implement Linux daemon support
  - Create systemd service unit configuration
  - Support binding to specific network interfaces
  - Implement proper signal handling for daemon operation
  - Support multiple instances via configuration
  - _Requirements: 7.4, 7.5_

- [ ] 16. Mobile Platform Integration
- [ ] 16.1 (P) Implement iOS/iPadOS specific features
  - Request local network access permission via Info.plist configuration
  - Request background task continuation on app background transition (3-minute limit)
  - Monitor remaining background time and notify clients of impending expiry
  - Register for Background App Refresh for periodic state checks
  - Implement Audio Background Mode for audio-only streams:
    - Configure Info.plist with UIBackgroundModes containing "audio"
    - Manage AVAudioSession with .playback or .playAndRecord category
    - Detect stream type (audio-only vs video) to determine background capability
  - Display user warning when entering background with video-only streams (foreground required)
  - Minimize CPU usage in background by reducing non-essential processing
  - Operate within single process without additional background services
  - _Requirements: 8.4, 8.6, 9.1, 9.2, 9.5, 9.6_

- [ ] 16.2 (P) Implement Android specific features
  - Declare INTERNET and ACCESS_NETWORK_STATE permissions in manifest
  - Start foreground service with persistent notification on background
  - Display active stream and connection counts in notification
  - Respect Doze mode and App Standby power conservation requests
  - Stop foreground service when returning to foreground
  - _Requirements: 8.5, 8.6, 9.3, 9.4, 10.6_

- [ ] 17. Mobile Battery and Network Optimization
- [ ] 17.1 Implement battery optimization
  - Enter low-power idle mode with <1 wake-up per second when no streams active
  - Use platform-native async I/O to avoid busy-wait loops
  - Batch network operations to reduce radio wake-ups
  - Provide battery usage statistics through API
  - Comply with App Store guidelines for background execution
  - _Requirements: 10.1, 10.2, 10.3, 10.4, 10.5_

- [ ] 17.2 Implement mobile network monitoring
  - Detect network connectivity changes within 2 seconds
  - Attempt interface rebind on WiFi to cellular transition when allowed
  - Notify clients of new server address when interface changes
  - Maintain connection state for 30 seconds during network loss
  - Support configuration to restrict to WiFi-only or cellular-only
  - Warn users about cellular data usage through API
  - _Requirements: 11.1, 11.2, 11.3, 11.4, 11.5, 11.6_

- [ ] 18. Performance Optimization
- [ ] 18.1 Implement latency optimization
  - Target glass-to-glass latency under 2 seconds on desktop
  - Target glass-to-glass latency under 3 seconds on mobile
  - Process and forward chunks within 50ms of receipt
  - Support low-latency mode with 500ms maximum subscriber buffer
  - _Requirements: 12.1, 12.2, 12.3, 12.4, 12.5_

- [ ] 18.2 Implement throughput optimization
  - Support 50 Mbps ingestion bitrate on desktop
  - Support 20 Mbps ingestion bitrate on mobile
  - Support 500 Mbps aggregate distribution on desktop
  - Support 100 Mbps aggregate distribution on mobile
  - Log warnings for streams exceeding configured bitrate limits
  - _Requirements: 13.1, 13.2, 13.3, 13.4, 13.6_

- [ ] 19. Graceful Shutdown and Error Handling
- [ ] 19.1 Implement graceful shutdown
  - Stop accepting new connections immediately on shutdown signal
  - Notify connected publishers of impending shutdown
  - Allow active streams up to 30 seconds grace period to complete
  - Force terminate remaining connections after grace period
  - Support force-shutdown option for immediate termination
  - Log shutdown summary with connection and stream counts
  - _Requirements: 19.1, 19.2, 19.3, 19.4, 19.5, 19.6_

- [ ] 19.2 Implement error isolation and recovery
  - Isolate single connection failures from affecting other clients
  - Reject new connections when memory allocation fails
  - Attempt component restart on thread/task crash
  - Implement circuit breaker for external service failures
  - Log diagnostic information before unrecoverable error termination
  - Provide health check API for external monitoring
  - _Requirements: 20.1, 20.2, 20.3, 20.4, 20.5, 20.6_

- [ ] 20. Public API and Server Lifecycle
- [ ] 20.1 Implement RTMPServer public API
  - Expose initialize, start, and stop lifecycle methods
  - Support graceful and force stop modes
  - Provide runtime configuration update capability
  - Expose server and per-stream metrics through API
  - Support authentication and event callbacks
  - Ensure thread-safe method invocation from any thread
  - _Requirements: 6.1, 17.3_

- [ ] 20.2 Integrate all components into server lifecycle
  - Wire configuration loading during initialization
  - Start event loop and connection acceptance on server start
  - Coordinate component shutdown in correct order
  - Validate configuration and report errors on startup
  - Emit server lifecycle events to registered callbacks
  - _Requirements: 17.4, 17.5_

- [ ] 21. Integration Testing
- [ ] 21.1 Implement unit test suite
  - Test handshake state machine with valid and invalid sequences
  - Test chunk parser with all chunk types and variable sizes
  - Test AMF codec with complex nested objects
  - Test command processor state transitions and error cases
  - Test GOP buffer overflow and keyframe indexing
  - Test authentication service with ACL and rate limiting
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 15.1, 15.2, 15.6, 15.7_

- [ ] 21.2 Implement integration test suite
  - Test complete publish flow from connect through media transmission
  - Test complete subscribe flow with instant playback from keyframe
  - Test multi-subscriber scenario with slow subscriber handling
  - Test authentication integration with mock HTTP callback
  - Test TLS/RTMPS connection establishment
  - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 15.4, 16.1, 16.2_

- [ ] 21.3 Implement E2E and performance tests
  - Test with OBS encoder publishing real streams
  - Test FFmpeg publish-to-play roundtrip
  - Test mobile background/foreground transitions
  - Test network failover scenarios on mobile
  - Validate connection scalability targets (1000 desktop, 100 mobile)
  - Validate throughput targets (50/500 Mbps desktop, 20/100 Mbps mobile)
  - Validate latency targets (2s desktop, 3s mobile)
  - _Requirements: 9.1, 9.2, 9.3, 9.4, 11.1, 11.2, 11.4, 12.1, 12.2, 13.1, 13.2, 13.3, 13.4, 14.1, 14.2_
