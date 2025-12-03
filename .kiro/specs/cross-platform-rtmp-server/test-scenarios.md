# OpenRTMP Comprehensive Test Scenarios

## Purpose
This document defines explicit, verifiable test scenarios that must pass before the implementation is considered production-ready. Each scenario should be implemented as an actual test case and verified manually or automatically before marking features complete.

---

## 1. RTMP Handshake Protocol

### 1.1 Happy Path
- [ ] **HS-001**: Client sends valid C0 (version 3) → Server responds with S0+S1 within 100ms
- [ ] **HS-002**: Client sends valid C1 (1536 bytes) → Server responds with S2
- [ ] **HS-003**: Client sends valid C2 matching S1 → Connection transitions to Established
- [ ] **HS-004**: Complete handshake with OBS Studio encoder
- [ ] **HS-005**: Complete handshake with FFmpeg (`ffmpeg -f flv rtmp://...`)
- [ ] **HS-006**: Complete handshake with VLC player connecting as subscriber

### 1.2 Error Cases
- [ ] **HS-010**: C0 with version 0 → Connection rejected, error logged with client IP
- [ ] **HS-011**: C0 with version 4 → Connection rejected, error logged
- [ ] **HS-012**: C1 with only 1000 bytes (truncated) → Connection rejected
- [ ] **HS-013**: C1 with 2000 bytes (oversized) → Handle gracefully or reject
- [ ] **HS-014**: C2 with mismatched timestamp → Connection rejected
- [ ] **HS-015**: C2 with mismatched random data → Connection rejected
- [ ] **HS-016**: No C0 received within 10 seconds → Timeout, connection closed
- [ ] **HS-017**: C0 received, no C1 within 10 seconds → Timeout, connection closed
- [ ] **HS-018**: Handshake packets sent out of order (C1 before C0) → Rejected
- [ ] **HS-019**: Garbage data instead of handshake → Connection rejected
- [ ] **HS-020**: Connection closed mid-handshake → Resources cleaned up

### 1.3 Stress Cases
- [ ] **HS-030**: 100 simultaneous handshakes completing successfully
- [ ] **HS-031**: 1000 simultaneous handshake attempts (some will be rejected at limit)
- [ ] **HS-032**: Rapid connect/disconnect cycles (100 times in 10 seconds)

---

## 2. RTMP Chunk Parsing

### 2.1 Chunk Types
- [ ] **CP-001**: Parse Type 0 chunk (full header, 11 bytes) correctly
- [ ] **CP-002**: Parse Type 1 chunk (7 bytes, no message stream ID) correctly
- [ ] **CP-003**: Parse Type 2 chunk (3 bytes, timestamp delta only) correctly
- [ ] **CP-004**: Parse Type 3 chunk (no header, continuation) correctly
- [ ] **CP-005**: Parse chunk with 1-byte basic header (cs id 2-63)
- [ ] **CP-006**: Parse chunk with 2-byte basic header (cs id 64-319)
- [ ] **CP-007**: Parse chunk with 3-byte basic header (cs id 320-65599)

### 2.2 Chunk Size Handling
- [ ] **CP-010**: Default chunk size 128 bytes works correctly
- [ ] **CP-011**: Set Chunk Size to 4096 bytes, subsequent parsing works
- [ ] **CP-012**: Set Chunk Size to 65536 bytes (maximum), parsing works
- [ ] **CP-013**: Set Chunk Size to 128 bytes (minimum), parsing works
- [ ] **CP-014**: Message spanning 10 chunks reassembled correctly
- [ ] **CP-015**: Message spanning 100 chunks reassembled correctly
- [ ] **CP-016**: Different chunk sizes per direction (client vs server)

### 2.3 Edge Cases
- [ ] **CP-020**: Extended timestamp (timestamp >= 0xFFFFFF) handled correctly
- [ ] **CP-021**: Zero-length message body parsed correctly
- [ ] **CP-022**: Maximum message length (16MB) parsed correctly
- [ ] **CP-023**: Abort Message (type 2) discards partial chunk stream
- [ ] **CP-024**: Unknown message type logged but doesn't crash
- [ ] **CP-025**: Interleaved chunks from multiple streams parsed correctly
- [ ] **CP-026**: Chunk with invalid format type → Error handled gracefully

### 2.4 Corruption Scenarios
- [ ] **CP-030**: Truncated chunk mid-parse → Buffer until complete or timeout
- [ ] **CP-031**: Invalid message length (negative or > 16MB) → Reject
- [ ] **CP-032**: Chunk stream ID 0 (reserved) → Handle appropriately
- [ ] **CP-033**: Memory exhaustion during reassembly → Reject and recover

---

## 3. AMF Encoding/Decoding

### 3.1 AMF0 Types
- [ ] **AMF-001**: Decode Number (0x00) - integer values
- [ ] **AMF-002**: Decode Number (0x00) - floating point values (3.14159)
- [ ] **AMF-003**: Decode Number (0x00) - NaN, Infinity, -Infinity
- [ ] **AMF-004**: Decode Boolean (0x01) - true and false
- [ ] **AMF-005**: Decode String (0x02) - empty string
- [ ] **AMF-006**: Decode String (0x02) - ASCII string
- [ ] **AMF-007**: Decode String (0x02) - UTF-8 string with emoji 🎬
- [ ] **AMF-008**: Decode String (0x02) - maximum length (65535 bytes)
- [ ] **AMF-009**: Decode Object (0x03) - empty object
- [ ] **AMF-010**: Decode Object (0x03) - nested objects (3 levels deep)
- [ ] **AMF-011**: Decode Null (0x05)
- [ ] **AMF-012**: Decode Undefined (0x06)
- [ ] **AMF-013**: Decode ECMAArray (0x08) - sparse array
- [ ] **AMF-014**: Decode StrictArray (0x0A) - dense array
- [ ] **AMF-015**: Decode Date (0x0B) with timezone
- [ ] **AMF-016**: Decode Long String (0x0C) - > 65535 bytes
- [ ] **AMF-017**: Encode all above types and verify round-trip

### 3.2 AMF0 Edge Cases
- [ ] **AMF-020**: Object with 100 properties
- [ ] **AMF-021**: Object nested 32 levels deep (max allowed)
- [ ] **AMF-022**: Object nested 33 levels deep → Error
- [ ] **AMF-023**: Reference (0x07) resolves correctly
- [ ] **AMF-024**: Circular reference via Reference type
- [ ] **AMF-025**: Malformed UTF-8 in string → Error with offset
- [ ] **AMF-026**: Truncated data mid-object → Error with offset
- [ ] **AMF-027**: Unknown type marker → Error

### 3.3 AMF3 Types (Enhanced RTMP)
- [ ] **AMF-030**: Decode AMF3 Integer
- [ ] **AMF-031**: Decode AMF3 Double
- [ ] **AMF-032**: Decode AMF3 String with reference table
- [ ] **AMF-033**: Decode AMF3 Object with traits
- [ ] **AMF-034**: Decode AMF3 Array
- [ ] **AMF-035**: Decode AMF3 ByteArray
- [ ] **AMF-036**: AMF3 reference table resets per command

---

## 4. RTMP Commands

### 4.1 Connect Command
- [ ] **CMD-001**: `connect` with valid app name → `_result` within 50ms
- [ ] **CMD-002**: `connect` with invalid app name → `_error` response
- [ ] **CMD-003**: `connect` with all optional parameters (tcUrl, swfUrl, pageUrl)
- [ ] **CMD-004**: `connect` from OBS with full handshake info
- [ ] **CMD-005**: `connect` already connected → Error response
- [ ] **CMD-006**: `connect` with empty app name → Error response
- [ ] **CMD-007**: `connect` app name with special characters (`/`, `?`, `#`)

### 4.2 Stream Commands
- [ ] **CMD-010**: `createStream` after connect → Returns stream ID
- [ ] **CMD-011**: `createStream` without connect → Error response
- [ ] **CMD-012**: `createStream` multiple times → Different stream IDs
- [ ] **CMD-013**: `deleteStream` releases stream ID
- [ ] **CMD-014**: `deleteStream` non-existent stream → Graceful handling
- [ ] **CMD-015**: `closeStream` stops stream but keeps connection

### 4.3 Publish Command
- [ ] **CMD-020**: `publish` with valid stream key → `NetStream.Publish.Start`
- [ ] **CMD-021**: `publish` with invalid stream key (auth enabled) → `_error`
- [ ] **CMD-022**: `publish` stream key already in use → `_error` with conflict
- [ ] **CMD-023**: `publish` with "live" type
- [ ] **CMD-024**: `publish` with "record" type
- [ ] **CMD-025**: `publish` with "append" type
- [ ] **CMD-026**: `publish` without `createStream` → Error
- [ ] **CMD-027**: `unpublish` stops publishing, keeps connection

### 4.4 Play Command
- [ ] **CMD-030**: `play` existing stream → Receives data from keyframe
- [ ] **CMD-031**: `play` non-existent stream → `NetStream.Play.StreamNotFound`
- [ ] **CMD-032**: `play` with start/duration parameters
- [ ] **CMD-033**: `play` while stream starts mid-connection
- [ ] **CMD-034**: `stop` command stops playback
- [ ] **CMD-035**: `pause` and `resume` commands work correctly

### 4.5 Command Edge Cases
- [ ] **CMD-040**: Command with transaction ID 0
- [ ] **CMD-041**: Command with very large transaction ID
- [ ] **CMD-042**: Unknown command name → Logged, connection continues
- [ ] **CMD-043**: Malformed command object → Error response
- [ ] **CMD-044**: Command flood (1000 commands/second) → Rate limited or handled

---

## 5. Stream Ingestion

### 5.1 Video Codec Support
- [ ] **ING-001**: H.264/AVC video stream ingested correctly
- [ ] **ING-002**: H.264 sequence header (SPS/PPS) stored
- [ ] **ING-003**: H.264 keyframe (IDR) detected correctly
- [ ] **ING-004**: H.264 P-frames and B-frames processed
- [ ] **ING-005**: H.265/HEVC via Enhanced RTMP FourCC "hvc1"
- [ ] **ING-006**: H.265 VPS/SPS/PPS sequence header stored
- [ ] **ING-007**: H.265 keyframe (IRAP) detected
- [ ] **ING-008**: VP9 via Enhanced RTMP (if supported)
- [ ] **ING-009**: AV1 via Enhanced RTMP (if supported)
- [ ] **ING-010**: Unknown codec → Logged, data passed through

### 5.2 Audio Codec Support
- [ ] **ING-020**: AAC audio stream ingested correctly
- [ ] **ING-021**: AAC AudioSpecificConfig stored
- [ ] **ING-022**: AAC-LC profile
- [ ] **ING-023**: AAC-HE profile
- [ ] **ING-024**: MP3 audio (if supported)
- [ ] **ING-025**: Opus via Enhanced RTMP (if supported)
- [ ] **ING-026**: Audio-only stream (no video)
- [ ] **ING-027**: Video-only stream (no audio)

### 5.3 Timestamp Handling
- [ ] **ING-030**: Monotonically increasing timestamps processed
- [ ] **ING-031**: Timestamp gap < 1 second → No warning
- [ ] **ING-032**: Timestamp gap > 1 second → Warning logged
- [ ] **ING-033**: Timestamp rollover (wrap at 32-bit) handled
- [ ] **ING-034**: Timestamp jump backward → Handled gracefully
- [ ] **ING-035**: Composition time offset for B-frames

### 5.4 Publisher Lifecycle
- [ ] **ING-040**: Publisher disconnect → Stream marked unavailable within 5s
- [ ] **ING-041**: Publisher disconnect → Subscribers notified
- [ ] **ING-042**: Publisher reconnect with same key → Old session cleaned up
- [ ] **ING-043**: Graceful unpublish → Resources released immediately
- [ ] **ING-044**: Publisher sends no data for 30s → Timeout handling

### 5.5 Metadata
- [ ] **ING-050**: `@setDataFrame` / `onMetaData` parsed and stored
- [ ] **ING-051**: Metadata includes width, height, framerate
- [ ] **ING-052**: Metadata includes audio sample rate, channels
- [ ] **ING-053**: Metadata update mid-stream handled
- [ ] **ING-054**: Missing metadata → Defaults applied

---

## 6. GOP Buffer and Distribution

### 6.1 GOP Buffer
- [ ] **GOP-001**: Buffer maintains minimum 2 seconds of media
- [ ] **GOP-002**: New subscriber receives from last keyframe
- [ ] **GOP-003**: Metadata sent before media data
- [ ] **GOP-004**: Sequence headers (SPS/PPS) sent before video
- [ ] **GOP-005**: Buffer eviction when exceeds configured size
- [ ] **GOP-006**: Keyframe index updated on each new keyframe

### 6.2 Subscriber Management
- [ ] **DIST-001**: Single publisher, single subscriber → Data flows
- [ ] **DIST-002**: Single publisher, 10 subscribers → All receive data
- [ ] **DIST-003**: Single publisher, 100 subscribers → All receive data
- [ ] **DIST-004**: Subscriber joins mid-stream → Receives from keyframe
- [ ] **DIST-005**: Subscriber leaves → Resources cleaned up
- [ ] **DIST-006**: All subscribers leave → Stream continues for future subscribers

### 6.3 Slow Subscriber Handling
- [ ] **DIST-010**: Subscriber buffer > 5 seconds → Non-keyframes dropped
- [ ] **DIST-011**: Frame drops logged with subscriber ID
- [ ] **DIST-012**: Keyframes never dropped (stream remains playable)
- [ ] **DIST-013**: Audio frames preserved during video drops
- [ ] **DIST-014**: Slow subscriber doesn't affect fast subscribers

### 6.4 Stream End
- [ ] **DIST-020**: Publisher ends stream → EOF sent within 1 second
- [ ] **DIST-021**: EOF sent to all connected subscribers
- [ ] **DIST-022**: Subscribers disconnected gracefully after EOF

---

## 7. Authentication & Security

### 7.1 Stream Key Authentication
- [ ] **AUTH-001**: Valid stream key → Publish allowed
- [ ] **AUTH-002**: Invalid stream key → Publish rejected with error
- [ ] **AUTH-003**: Empty stream key → Rejected
- [ ] **AUTH-004**: Stream key with special characters handled
- [ ] **AUTH-005**: Case-sensitive stream key matching (or case-insensitive if configured)

### 7.2 IP-Based ACL
- [ ] **AUTH-010**: IP in allow list → Connection accepted
- [ ] **AUTH-011**: IP in deny list → Connection rejected
- [ ] **AUTH-012**: IP matches CIDR range in allow list → Accepted
- [ ] **AUTH-013**: IP matches CIDR range in deny list → Rejected
- [ ] **AUTH-014**: Deny takes precedence over allow
- [ ] **AUTH-015**: Localhost (127.0.0.1) handling

### 7.3 Rate Limiting
- [ ] **AUTH-020**: 5 failed auth attempts from same IP → Temporarily blocked
- [ ] **AUTH-021**: Block duration expires → IP allowed to try again
- [ ] **AUTH-022**: Successful auth resets failure count
- [ ] **AUTH-023**: Rate limit logged with IP address

### 7.4 External Authentication
- [ ] **AUTH-030**: HTTP callback returns `{allowed: true}` → Publish proceeds
- [ ] **AUTH-031**: HTTP callback returns `{allowed: false}` → Publish rejected
- [ ] **AUTH-032**: HTTP callback timeout → Circuit breaker opens, default deny
- [ ] **AUTH-033**: HTTP callback error → Circuit breaker, default deny
- [ ] **AUTH-034**: Circuit breaker half-open → Test request sent
- [ ] **AUTH-035**: Circuit breaker closes after successful requests

### 7.5 TLS/RTMPS
- [ ] **TLS-001**: RTMPS connection on port 443 (or configured)
- [ ] **TLS-002**: TLS 1.2 connection succeeds
- [ ] **TLS-003**: TLS 1.3 connection succeeds
- [ ] **TLS-004**: TLS 1.1 connection rejected (below minimum)
- [ ] **TLS-005**: Self-signed certificate works in development
- [ ] **TLS-006**: Certificate pinning rejects mismatched cert
- [ ] **TLS-007**: Expired certificate rejected
- [ ] **TLS-008**: RTMP (non-TLS) and RTMPS on different ports simultaneously

---

## 8. Platform-Specific Tests

### 8.1 macOS
- [ ] **MAC-001**: Server starts and binds to port on macOS 12+
- [ ] **MAC-002**: Network permission dialog appears on first run
- [ ] **MAC-003**: Runs as launchd daemon
- [ ] **MAC-004**: Graceful shutdown on SIGTERM
- [ ] **MAC-005**: Logs appear in Console.app via os_log
- [ ] **MAC-006**: Works on both Intel and Apple Silicon

### 8.2 Windows
- [ ] **WIN-001**: Server starts on Windows 11 x64
- [ ] **WIN-002**: Server starts on Windows 11 ARM64
- [ ] **WIN-003**: Windows Firewall prompt appears for port access
- [ ] **WIN-004**: Runs as Windows Service
- [ ] **WIN-005**: Graceful shutdown via Service Control Manager
- [ ] **WIN-006**: Logs appear in Event Viewer

### 8.3 Linux
- [ ] **LIN-001**: Server starts on Ubuntu 22.04 (kernel 5.4+)
- [ ] **LIN-002**: Server starts on Debian 12
- [ ] **LIN-003**: Runs as systemd service
- [ ] **LIN-004**: Graceful shutdown on SIGTERM
- [ ] **LIN-005**: Logs via journald or syslog
- [ ] **LIN-006**: Works in Docker container

### 8.4 iOS/iPadOS
- [ ] **IOS-001**: Server starts and binds within app
- [ ] **IOS-002**: Local network permission requested and granted
- [ ] **IOS-003**: Background task requested on app background
- [ ] **IOS-004**: Connections maintained for ~3 minutes in background
- [ ] **IOS-005**: Clients notified before background task expires
- [ ] **IOS-006**: Audio-only stream continues in background (Audio mode)
- [ ] **IOS-007**: Video stream warns user foreground required
- [ ] **IOS-008**: Background App Refresh registers for periodic checks

### 8.5 Android
- [ ] **AND-001**: Server starts with INTERNET permission
- [ ] **AND-002**: Foreground service starts on app background
- [ ] **AND-003**: Notification shows stream/connection count
- [ ] **AND-004**: Notification updates as counts change
- [ ] **AND-005**: Doze mode respected (reduced activity)
- [ ] **AND-006**: Foreground service stops on app foreground return
- [ ] **AND-007**: Works on Android 8.0 (API 26) minimum

---

## 9. Network Handling (Mobile)

### 9.1 Network Changes
- [ ] **NET-001**: WiFi → Cellular transition detected within 2 seconds
- [ ] **NET-002**: Cellular → WiFi transition detected within 2 seconds
- [ ] **NET-003**: Network loss detected within 2 seconds
- [ ] **NET-004**: Server rebinds to new interface (if allowed)
- [ ] **NET-005**: Clients notified of new server address

### 9.2 Network Recovery
- [ ] **NET-010**: Network lost → Connection state held for 30 seconds
- [ ] **NET-011**: Network returns within 30s → Stream resumes
- [ ] **NET-012**: Network lost > 30s → Connections terminated gracefully
- [ ] **NET-013**: Rapid network toggling handled without crash

### 9.3 Network Restrictions
- [ ] **NET-020**: WiFi-only mode → Cellular rejected
- [ ] **NET-021**: Cellular-only mode → WiFi rejected
- [ ] **NET-022**: Any network mode → Both work
- [ ] **NET-023**: Cellular data warning API provides usage info

---

## 10. Performance Tests

### 10.1 Latency
- [ ] **PERF-001**: Glass-to-glass latency < 2 seconds (desktop)
- [ ] **PERF-002**: Glass-to-glass latency < 3 seconds (mobile)
- [ ] **PERF-003**: Chunk processing < 50ms from receipt to forward
- [ ] **PERF-004**: Low-latency mode: buffer < 500ms
- [ ] **PERF-005**: Latency stable over 1-hour stream

### 10.2 Throughput
- [ ] **PERF-010**: Ingest 50 Mbps stream (desktop)
- [ ] **PERF-011**: Ingest 20 Mbps stream (mobile)
- [ ] **PERF-012**: Distribute 500 Mbps aggregate (desktop)
- [ ] **PERF-013**: Distribute 100 Mbps aggregate (mobile)
- [ ] **PERF-014**: No frame drops at sustained throughput
- [ ] **PERF-015**: Bitrate warning logged when limits exceeded

### 10.3 Scalability
- [ ] **PERF-020**: 1000 concurrent connections (desktop)
- [ ] **PERF-021**: 100 concurrent connections (mobile)
- [ ] **PERF-022**: 10 simultaneous publishers (desktop)
- [ ] **PERF-023**: 3 simultaneous publishers (mobile)
- [ ] **PERF-024**: Connection rejection at limit with error code
- [ ] **PERF-025**: No memory leak over 24-hour run

### 10.4 Battery (Mobile)
- [ ] **PERF-030**: Idle mode: < 1 CPU wake-up per second
- [ ] **PERF-031**: Active streaming: Battery drain reasonable
- [ ] **PERF-032**: Battery stats API returns accurate data
- [ ] **PERF-033**: No excessive network radio wake-ups

---

## 11. Configuration

### 11.1 Config Loading
- [ ] **CFG-001**: JSON config file loads successfully
- [ ] **CFG-002**: YAML config file loads successfully
- [ ] **CFG-003**: Environment variables override config file
- [ ] **CFG-004**: Invalid config → Startup fails with specific error
- [ ] **CFG-005**: Missing config → Sensible defaults used
- [ ] **CFG-006**: Config dump command shows effective config

### 11.2 Runtime Config
- [ ] **CFG-010**: Log level changed at runtime
- [ ] **CFG-011**: Max connections changed at runtime
- [ ] **CFG-012**: Port change requires restart (documented)
- [ ] **CFG-013**: Config change event emitted to listeners

---

## 12. Logging & Metrics

### 12.1 Logging
- [ ] **LOG-001**: Debug level shows all messages
- [ ] **LOG-002**: Info level shows connections and streams
- [ ] **LOG-003**: Warning level shows anomalies
- [ ] **LOG-004**: Error level shows failures only
- [ ] **LOG-005**: JSON structured format parseable
- [ ] **LOG-006**: Client IP included in connection logs
- [ ] **LOG-007**: Stream key included in stream logs (sanitized if sensitive)

### 12.2 Log Rotation
- [ ] **LOG-010**: Size-based rotation at configured size
- [ ] **LOG-011**: Time-based rotation at configured interval
- [ ] **LOG-012**: Old logs compressed
- [ ] **LOG-013**: Maximum log file count enforced

### 12.3 Metrics
- [ ] **MET-001**: Active connection count accurate
- [ ] **MET-002**: Active stream count accurate
- [ ] **MET-003**: Bandwidth ingestion rate accurate
- [ ] **MET-004**: Bandwidth distribution rate accurate
- [ ] **MET-005**: Error counts by category
- [ ] **MET-006**: Per-stream latency metrics

---

## 13. Graceful Shutdown

### 13.1 Graceful Mode
- [ ] **SHUT-001**: New connections rejected immediately
- [ ] **SHUT-002**: Publishers notified of shutdown
- [ ] **SHUT-003**: Active streams continue for grace period (30s)
- [ ] **SHUT-004**: Streams ending before grace period complete normally
- [ ] **SHUT-005**: After grace period, remaining connections terminated
- [ ] **SHUT-006**: Shutdown summary logged

### 13.2 Force Mode
- [ ] **SHUT-010**: All connections terminated immediately
- [ ] **SHUT-011**: No grace period
- [ ] **SHUT-012**: Resources released cleanly

---

## 14. Error Recovery

### 14.1 Connection Isolation
- [ ] **ERR-001**: One connection crash doesn't affect others
- [ ] **ERR-002**: Invalid data from client isolated to that connection
- [ ] **ERR-003**: Connection cleanup on unexpected disconnect

### 14.2 Resource Exhaustion
- [ ] **ERR-010**: Memory allocation fails → New connections rejected
- [ ] **ERR-011**: File descriptor limit → New connections rejected
- [ ] **ERR-012**: Recovery when resources become available

### 14.3 Component Recovery
- [ ] **ERR-020**: Worker thread crash → Thread restarted
- [ ] **ERR-021**: Task crash → Task restarted with logging
- [ ] **ERR-022**: Circuit breaker for external services

### 14.4 Health Check
- [ ] **ERR-030**: Health check API returns Healthy when all good
- [ ] **ERR-031**: Health check returns Degraded when partial failure
- [ ] **ERR-032**: Health check returns Unhealthy when critical failure

---

## 15. Real-World Encoder Compatibility

### 15.1 OBS Studio
- [ ] **ENC-001**: OBS connects and publishes H.264+AAC
- [ ] **ENC-002**: OBS with HEVC (if OBS supports Enhanced RTMP)
- [ ] **ENC-003**: OBS reconnect after network blip
- [ ] **ENC-004**: OBS with various bitrate settings (1-50 Mbps)
- [ ] **ENC-005**: OBS with various resolutions (720p, 1080p, 4K)
- [ ] **ENC-006**: OBS with different keyframe intervals (1s, 2s, 4s)

### 15.2 FFmpeg
- [ ] **ENC-010**: `ffmpeg -f flv -i ... rtmp://server/app/key` works
- [ ] **ENC-011**: FFmpeg with H.264 baseline, main, high profiles
- [ ] **ENC-012**: FFmpeg with various audio sample rates
- [ ] **ENC-013**: FFmpeg publish + FFplay subscribe roundtrip

### 15.3 Mobile Encoders
- [ ] **ENC-020**: Larix Broadcaster (iOS/Android)
- [ ] **ENC-021**: Streamlabs Mobile
- [ ] **ENC-022**: nanoStream (if applicable)

### 15.4 Hardware Encoders
- [ ] **ENC-030**: Blackmagic ATEM Mini (if applicable)
- [ ] **ENC-031**: Elgato Stream Deck (if applicable)

---

## Test Verification Checklist

Before marking a feature as complete:

1. [ ] All related test scenarios above pass
2. [ ] Tests run on actual target platform (not just mocks)
3. [ ] Manual verification with real encoder (OBS/FFmpeg)
4. [ ] Edge cases tested, not just happy path
5. [ ] Error messages are clear and actionable
6. [ ] Logs include sufficient context for debugging
7. [ ] Memory checked for leaks (Instruments on macOS, Valgrind on Linux)
8. [ ] Performance targets met under load

---

## Running Tests

### Unit Tests
```bash
cd build && ctest --output-on-failure -R "unit_"
```

### Integration Tests
```bash
cd build && ctest --output-on-failure -R "integration_"
```

### E2E Tests (requires running server)
```bash
# Terminal 1: Start server
./openrtmp-server --config config.json

# Terminal 2: Run E2E tests
cd build && ctest --output-on-failure -R "e2e_"
```

### Manual Verification
```bash
# Start server
./openrtmp-server --port 1935

# Publish with FFmpeg
ffmpeg -re -i test.mp4 -c copy -f flv rtmp://localhost:1935/live/test

# Subscribe with FFplay
ffplay rtmp://localhost:1935/live/test
```

---

## Version History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-11-30 | Claude | Initial comprehensive test scenarios |
