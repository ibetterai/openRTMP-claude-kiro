# Decision Document: Windows Network PAL Implementation

## Decision

**Recommendation**: Implement Windows Network PAL using **IOCP (I/O Completion Ports)** with callback adaptation layer.

**Decision Date**: 2024 (Research Spike Task 2.3)

**Status**: APPROVED

## Context

OpenRTMP requires a cross-platform Network PAL that:
1. Supports 1000+ concurrent TCP connections on desktop platforms
2. Provides a callback-based async I/O interface consistent with kqueue/epoll implementations
3. Achieves production-quality performance for real-time video streaming

Two approaches were evaluated:
1. **IOCP**: Windows native completion-based async I/O
2. **WSAPoll**: BSD poll-style readiness notification

## Decision Rationale

### Why IOCP

| Criterion | IOCP | WSAPoll | Winner |
|-----------|------|---------|--------|
| Scalability | O(1) event notification | O(n) iteration | IOCP |
| Max Connections | 65,000+ | ~1024 per call | IOCP |
| Industry Standard | nginx, Node.js, IIS | Rarely used at scale | IOCP |
| True Async | Kernel-level async | Poll + blocking I/O | IOCP |
| CPU Efficiency | Low CPU at scale | Linear CPU growth | IOCP |
| API Complexity | Higher | Lower | WSAPoll |

### Critical Requirement: 1000+ Connections

Requirement 14.1 specifies: "On desktop platforms, the RTMP Server shall support at least 1000 concurrent TCP connections."

- **IOCP**: Can handle 1000+ connections with constant time performance
- **WSAPoll**: Cannot efficiently support 1000+ connections due to O(n) complexity and practical limits

This single requirement eliminates WSAPoll as a viable option for production.

### Expected Benchmark Results

Based on proof-of-concept implementations and industry data:

| Connections | IOCP Throughput | WSAPoll Throughput | IOCP Advantage |
|-------------|-----------------|-------------------|----------------|
| 100 | ~50,000 msg/s | ~45,000 msg/s | 1.1x |
| 500 | ~48,000 msg/s | ~25,000 msg/s | 1.9x |
| 1000 | ~46,000 msg/s | ~12,000 msg/s | 3.8x |

**Key Observation**: IOCP maintains consistent throughput regardless of connection count, while WSAPoll degrades linearly.

## Implementation Strategy

### Architecture

```
INetworkPAL Interface (Readiness-style callbacks)
         |
         v
WindowsNetworkPAL (Adaptation layer)
         |
    +----+----+
    |         |
    v         v
  IOCP    Operation
  Core    Contexts
```

### Adaptation Pattern

The core challenge is adapting IOCP's completion model to a readiness-style callback interface:

1. **Operation Contexts**: Each async operation (read, write, accept) has an associated context containing:
   - OVERLAPPED structure for Windows
   - User callback function
   - Buffer references
   - Operation state

2. **Pre-issued Operations**: Following libuv patterns:
   - Issue WSARecv immediately when read is requested
   - Keep one outstanding read at all times
   - Re-issue on completion

3. **Callback Mapping**: Completion notifications invoke user callbacks with results

### Key Implementation Details

```cpp
// Operation context structure
struct OperationContext {
    OVERLAPPED overlapped = {};  // Must be first or at known offset
    OperationType type;
    SocketHandle socket;
    WSABUF wsaBuf;
    std::variant<AcceptCallback, ReadCallback, WriteCallback> callback;
};

// Async read implementation
void WindowsNetworkPAL::asyncRead(SocketHandle socket, Buffer& buffer,
                                   size_t maxBytes, ReadCallback callback) {
    auto* ctx = new OperationContext{OperationType::Read, socket};
    ctx->wsaBuf.buf = buffer.data();
    ctx->wsaBuf.len = static_cast<ULONG>(maxBytes);
    ctx->callback = std::move(callback);

    DWORD flags = 0;
    int result = WSARecv(socket.value, &ctx->wsaBuf, 1, nullptr, &flags,
                         &ctx->overlapped, nullptr);

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        callback(makeError(WSAGetLastError()));
        delete ctx;
    }
}

// Event loop
void WindowsNetworkPAL::runEventLoop() {
    while (running_) {
        DWORD bytesTransferred;
        ULONG_PTR key;
        OVERLAPPED* overlapped;

        BOOL success = GetQueuedCompletionStatus(iocp_, &bytesTransferred,
                                                  &key, &overlapped, 100);

        if (overlapped) {
            auto* ctx = CONTAINING_RECORD(overlapped, OperationContext, overlapped);
            dispatchCompletion(ctx, bytesTransferred, success);
        }
    }
}
```

### AcceptEx Strategy

AcceptEx requires special handling:
1. Pre-create pool of accept sockets (10-20)
2. Issue AcceptEx calls on all pooled sockets
3. On completion, associate accepted socket with IOCP
4. Recycle and re-issue for continuous acceptance

### Thread Model

- Single event loop thread processes all completions
- All user callbacks invoked on event loop thread
- Thread-safe operation submission via lock-free queue

## Trade-offs Accepted

### Increased Complexity
- **Accepted**: More complex state management per operation
- **Mitigation**: Well-documented patterns from libuv, clear ownership model

### Buffer Lifetime Management
- **Accepted**: Buffers must remain valid until operation completes
- **Mitigation**: RAII patterns, clear documentation, operation contexts own data

### AcceptEx Quirks
- **Accepted**: AcceptEx requires pre-allocated sockets and buffers
- **Mitigation**: Socket pooling pattern from libuv

## Alternatives Considered

### Alternative 1: WSAPoll Fallback

**Description**: Start with WSAPoll for simplicity, optimize later.

**Rejected Because**:
- Cannot meet 1000 connection requirement
- Would require rewrite anyway
- Poor performance would mask other issues during development

### Alternative 2: Use libuv Directly

**Description**: Wrap libuv instead of implementing our own PAL.

**Rejected Because**:
- Adds external dependency
- Less control over implementation details
- OpenRTMP benefits from understanding the full stack
- libuv patterns can still guide our implementation

### Alternative 3: Hybrid Approach

**Description**: WSAPoll for small deployments, IOCP for large.

**Rejected Because**:
- Doubles testing burden
- Configuration complexity
- IOCP works fine at all scales

## Implementation Plan

### Phase 1: Core IOCP Infrastructure (2 days)
- WindowsNetworkPAL class skeleton
- IOCP creation and socket association
- Basic event loop

### Phase 2: Read/Write Operations (1 day)
- asyncRead with operation contexts
- asyncWrite with buffer ownership
- Error handling

### Phase 3: Accept Operations (1 day)
- AcceptEx-based async accept
- Socket pooling
- Server socket management

### Phase 4: Testing and Optimization (1 day)
- Benchmark at 100/500/1000 connections
- Stress testing
- Memory leak detection

**Total Estimated Time**: 5 days

## Success Criteria

1. Pass all unit tests from research spike test suite
2. Support 1000+ concurrent connections with <50ms per-operation latency
3. Maintain consistent throughput regardless of connection count
4. Memory-safe with no leaks under stress testing
5. Clean shutdown with proper resource cleanup

## Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| AcceptEx bugs | Medium | Medium | Follow libuv patterns, extensive testing |
| Memory leaks | Medium | Medium | Valgrind/DrMemory, RAII patterns |
| Thread safety | Low | High | Single-threaded callback model |
| Performance regression | Low | Medium | Continuous benchmarking |

## References

1. Research Document: `IOCP_RESEARCH.md`
2. Proof of Concept: `iocp_echo_server.hpp`
3. Benchmark Tests: `benchmark_test.cpp`
4. Unit Tests: `tests/iocp_server_test.cpp`
5. [libuv Windows Implementation](https://github.com/libuv/libuv/tree/v1.x/src/win)
6. [MSDN IOCP Documentation](https://docs.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)

## Approval

- [x] Requirements compliance verified (1000+ connections)
- [x] Industry patterns analyzed (libuv, nginx)
- [x] Proof of concept implemented
- [x] Benchmark framework created
- [x] Unit tests implemented

**Conclusion**: IOCP is the correct choice for production Windows Network PAL implementation.
