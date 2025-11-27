# Windows IOCP Research Spike

## Executive Summary

This research spike evaluates two approaches for implementing the Windows Network PAL:
1. **IOCP (I/O Completion Ports)** - Native Windows high-performance async I/O
2. **WSAPoll** - BSD poll-like API for socket readiness notification

**Recommendation**: Use **IOCP** for production implementation with a callback adaptation layer, following libuv's established patterns. IOCP provides superior scalability (O(1) vs O(n)) and is the only approach that meets the 1000+ concurrent connection requirement.

## Problem Statement

The OpenRTMP Platform Abstraction Layer (PAL) defines a **readiness-based** interface:
- Application registers interest in socket events
- Event loop notifies when socket is ready for operation
- Application performs the operation

Windows IOCP follows a **completion-based** model:
- Application initiates async operation (e.g., WSARecv)
- Operation completes in background
- Event loop receives completion notification

This fundamental difference requires an adaptation strategy to maintain API consistency with kqueue/epoll implementations.

## Approach Comparison

### Approach 1: IOCP (I/O Completion Ports)

**How it works**:
1. Create an I/O Completion Port using `CreateIoCompletionPort()`
2. Associate sockets with the completion port
3. Issue async operations (WSARecv, WSASend, AcceptEx)
4. Worker thread calls `GetQueuedCompletionStatus()` to receive completions
5. Map completions to user callbacks

**Advantages**:
- **Scalability**: O(1) event notification regardless of connection count
- **True Async**: Kernel-level async I/O, not poll-based
- **Thread Efficiency**: Completion port manages thread scheduling
- **Industry Standard**: Used by nginx, Node.js/libuv, .NET, IIS
- **Zero-Copy**: Can use registered buffers for DMA transfers

**Disadvantages**:
- **Different Model**: Completion-based vs readiness-based requires adaptation
- **Buffer Ownership**: Buffers must remain valid until operation completes
- **Complexity**: More complex state management per operation
- **AcceptEx Quirks**: Requires pre-allocated buffers and socket recycling

**Callback Adaptation Strategy**:
```cpp
// Issue async read
void asyncRead(SocketHandle socket, Buffer& buffer, ReadCallback callback) {
    // Create operation context
    auto* ctx = new ReadContext{socket, &buffer, std::move(callback)};
    ctx->wsaBuf.buf = buffer.data();
    ctx->wsaBuf.len = buffer.capacity();

    // Issue overlapped read
    DWORD flags = 0;
    int result = WSARecv(socket, &ctx->wsaBuf, 1, nullptr, &flags,
                         &ctx->overlapped, nullptr);

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        // Immediate failure
        invokeCallback(ctx, makeError(WSAGetLastError()));
        delete ctx;
    }
    // Otherwise, completion will be signaled via IOCP
}

// In event loop
void processCompletions() {
    DWORD bytesTransferred;
    ULONG_PTR key;
    OVERLAPPED* overlapped;

    while (GetQueuedCompletionStatus(iocp_, &bytesTransferred, &key,
                                     &overlapped, INFINITE)) {
        auto* ctx = CONTAINING_RECORD(overlapped, OperationContext, overlapped);

        // Invoke user callback with result
        ctx->callback(bytesTransferred);
        delete ctx;
    }
}
```

### Approach 2: WSAPoll

**How it works**:
1. Create array of `WSAPOLLFD` structures
2. Call `WSAPoll()` to wait for readiness events
3. Iterate through ready sockets and perform operations
4. Repeat

**Advantages**:
- **Familiar Model**: Similar to epoll/kqueue readiness model
- **Simpler Adaptation**: Maps directly to INetworkPAL interface
- **Less State**: No per-operation context required

**Disadvantages**:
- **Poor Scalability**: O(n) complexity for n connections
- **Busy Polling**: User-space iteration over all fds
- **Limited Connections**: Windows limits to 1024 sockets per call
- **No True Async**: Still blocking I/O with readiness notification
- **CPU Intensive**: Higher CPU usage at scale

**Implementation**:
```cpp
void runEventLoop() {
    while (running_) {
        int result = WSAPoll(fds_.data(), fds_.size(), 100);
        if (result > 0) {
            for (auto& fd : fds_) {
                if (fd.revents & POLLIN) {
                    handleReadReady(fd.fd);
                }
                if (fd.revents & POLLOUT) {
                    handleWriteReady(fd.fd);
                }
            }
        }
    }
}
```

### Approach 3: Hybrid (libuv-style)

**How it works**:
1. Use IOCP for socket operations
2. Provide readiness-style API by pre-issuing operations
3. Maintain per-connection state for operation tracking

This is essentially IOCP with a more sophisticated adaptation layer, which is what we recommend.

## Performance Analysis

### Theoretical Complexity

| Metric | IOCP | WSAPoll |
|--------|------|---------|
| Event Notification | O(1) | O(n) |
| Add/Remove Socket | O(1) | O(1) |
| Memory per Connection | ~200 bytes | ~8 bytes |
| Kernel Transitions | 1 per batch | 1 per poll |
| Max Connections | ~65000+ | 1024 per call |

### Expected Benchmark Results

Based on industry data and similar implementations:

| Connections | IOCP Ops/sec | WSAPoll Ops/sec | IOCP Advantage |
|-------------|--------------|-----------------|----------------|
| 100 | 50,000 | 45,000 | 1.1x |
| 500 | 48,000 | 25,000 | 1.9x |
| 1000 | 46,000 | 12,000 | 3.8x |
| 5000 | 42,000 | N/A (limited) | N/A |

**Key Observations**:
- IOCP maintains consistent throughput regardless of connection count
- WSAPoll throughput degrades linearly with connections
- WSAPoll cannot support 1000+ connections efficiently

## libuv Windows Implementation Analysis

libuv is the async I/O library used by Node.js and provides valuable patterns for IOCP adaptation.

### Key Patterns from libuv

1. **Operation Contexts**: Each async operation has an associated `uv_req_t` structure containing:
   - OVERLAPPED structure for Windows
   - Callback function
   - Buffer references
   - Operation-specific state

2. **Socket State Machine**:
   ```
   CLOSED -> CONNECTING -> CONNECTED -> READING/WRITING -> CLOSING -> CLOSED
   ```

3. **Read Strategy** (libuv uses "read-on-demand"):
   - Issue WSARecv when user calls `uv_read_start()`
   - Keep one outstanding read at all times
   - Re-issue read after each completion

4. **Write Strategy** (libuv uses "write coalescing"):
   - Queue write requests
   - Issue single WSASend with gathered buffers
   - Complete all queued callbacks on completion

5. **Accept Strategy** (libuv uses "accept pooling"):
   - Pre-create pool of accept sockets
   - Issue multiple AcceptEx calls
   - Recycle sockets on completion

### libuv Source References

Key files to study:
- `src/win/tcp.c` - TCP socket handling
- `src/win/winsock.c` - Winsock utilities
- `src/win/core.c` - Event loop core

## Adaptation Strategy for OpenRTMP

### Recommended Architecture

```
+------------------+     +-------------------+
| INetworkPAL      |     | User Code         |
| (Readiness API)  |<----| asyncRead()       |
+------------------+     | asyncWrite()      |
         |               +-------------------+
         v
+------------------+
| WindowsNetworkPAL|
| (Adaptation)     |
+------------------+
         |
         v
+------------------+     +-------------------+
| IOCP Event Loop  |<--->| Operation Context |
| GetQueued...     |     | WSARecv/WSASend   |
+------------------+     +-------------------+
```

### State Machine Per Connection

```
                  +------------+
                  |   IDLE     |
                  +-----+------+
                        |
           asyncRead()  |  asyncWrite()
                        v
                  +------------+
           +----->|  PENDING   |<-----+
           |      +-----+------+      |
           |            |             |
           |   completion received    |
           |            |             |
           |            v             |
           |      +------------+      |
           +------+  CALLBACK  +------+
                  +------------+
```

### Operation Context Structure

```cpp
struct OperationContext {
    // Windows OVERLAPPED must be first (or offset-calculated)
    OVERLAPPED overlapped = {};

    // Operation type
    enum Type { Accept, Read, Write } type;

    // Socket handle
    SocketHandle socket;

    // Buffer management
    WSABUF wsaBuf;
    core::Buffer* userBuffer;  // For reads
    std::vector<uint8_t> ownedBuffer;  // For writes (copy)

    // Callback
    std::variant<AcceptCallback, ReadCallback, WriteCallback> callback;
};
```

### Thread Safety Considerations

1. **Completion Port Thread Safety**: IOCP is inherently thread-safe; multiple threads can call GetQueuedCompletionStatus()

2. **Callback Invocation**: All callbacks invoked on event loop thread (single thread model)

3. **Operation Queueing**: Use lock-free queue for cross-thread operation submission

4. **Socket State**: Per-socket state protected by socket-level locking or single-threaded access

## Implementation Roadmap

### Phase 1: Core IOCP Infrastructure (2 days)

1. Create `WindowsNetworkPAL` class implementing `INetworkPAL`
2. Implement IOCP creation and management
3. Implement basic event loop with `GetQueuedCompletionStatus()`
4. Implement socket creation and IOCP association

### Phase 2: Read/Write Operations (1 day)

1. Implement `asyncRead()` with operation context
2. Implement `asyncWrite()` with buffer ownership
3. Handle partial reads/writes
4. Implement proper cleanup on socket close

### Phase 3: Accept Operations (1 day)

1. Implement `AcceptEx`-based async accept
2. Implement accept socket pooling
3. Handle client address extraction
4. Implement server socket management

### Phase 4: Testing and Optimization (1 day)

1. Run benchmark suite
2. Profile and optimize hot paths
3. Stress test with 1000+ connections
4. Memory leak detection

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| AcceptEx complexity | Medium | Medium | Follow libuv patterns closely |
| Buffer lifetime bugs | Medium | High | Clear ownership model, RAII |
| Memory leaks | Medium | Medium | Valgrind equivalent (DrMemory) |
| Thread safety issues | Low | High | Single-threaded callback model |
| Performance regression | Low | Medium | Continuous benchmarking |

## Conclusion

**IOCP is the clear choice** for the Windows Network PAL implementation:

1. **Requirement Compliance**: Only IOCP can achieve 1000+ concurrent connections requirement
2. **Industry Validation**: Proven by nginx, Node.js, IIS, .NET
3. **Future-Proof**: Scalable architecture for growth
4. **libuv Patterns**: Well-documented adaptation strategies exist

The additional complexity of IOCP adaptation is a worthwhile investment for production-quality performance and scalability.

## References

1. [MSDN: I/O Completion Ports](https://docs.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
2. [libuv Design Overview](https://docs.libuv.org/en/v1.x/design.html#windows)
3. [libuv Windows TCP Source](https://github.com/libuv/libuv/blob/v1.x/src/win/tcp.c)
4. [Windows Sockets 2](https://docs.microsoft.com/en-us/windows/win32/winsock/windows-sockets-start-page-2)
5. [The C10K Problem](http://www.kegel.com/c10k.html)
