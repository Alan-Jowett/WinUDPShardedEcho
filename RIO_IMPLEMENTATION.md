<!-- SPDX-License-Identifier: MIT
  Copyright (c) 2025 WinUDPShardedEcho contributors -->

# RIO (Registered I/O) Implementation

This document describes the Windows Registered I/O (RIO) implementation added to the echo server for side-by-side performance comparison with the IOCP implementation.

## Overview

Windows Registered I/O (RIO) is a high-performance socket API introduced in Windows 8/Server 2012 that provides:

- Pre-registered memory buffers for reduced overhead
- Lower latency through reduced kernel-user mode transitions
- Better batching of I/O completions
- Optimized integration with RDMA and advanced NICs

## Implementation Details

### Files Modified

1. **`src/common/socket_utils.hpp`**
   - Added `rio_context` structure for RIO-specific I/O contexts
   - Added RIO constants (`RIO_OUTSTANDING_OPS`, `RIO_CQ_SIZE`, etc.)
   - Added RIO helper function declarations

2. **`src/common/socket_utils.cpp`**
   - Implemented `load_rio_function_table()` to retrieve RIO extension functions
   - Implemented `create_rio_completion_queue()` for RIO completion queues
   - Implemented `create_rio_request_queue()` for RIO request queues
   - Implemented `register_rio_buffer()` for buffer registration
   - Implemented `post_rio_recv()` and `post_rio_send()` for RIO operations
   - Modified `create_udp_socket()` to support `WSA_FLAG_REGISTERED_IO`

3. **`src/server/main.cpp`**
   - Added `g_use_rio` global flag
   - Added `server_rio_worker_context` structure for RIO workers
   - Implemented `worker_thread_func_rio()` as alternative worker loop
   - Added `--use-rio` CLI flag
   - Implemented RIO-specific worker creation and management

## Usage

Enable RIO mode with the `--use-rio` or `-r` flag:

```bash
# Run server in RIO mode
echo_server --port 5000 --use-rio

# Run server in RIO mode with specific cores
echo_server --port 5000 --use-rio --cores 4

# Run server in RIO mode with verbose logging
echo_server --port 5000 --use-rio --verbose
```

## Architecture

### RIO Worker Thread Flow

1. **Initialization**
   - Load RIO function table from Winsock
   - Create socket with `WSA_FLAG_REGISTERED_IO`
   - Set CPU affinity
   - Bind socket to port
   - Create RIO completion queue (polling mode)
   - Create RIO request queue
   - Allocate and register buffers for receive/send contexts

2. **Main Loop (Polling Mode)**
   - Poll RIO completion queue using `RIODequeueCompletion()`
   - Process receive completions:
     - Update statistics
     - Echo data back (sync or async send)
     - Re-post receive operation
   - Process send completions:
     - Return context to available pool
   - Sleep briefly if no completions (to avoid CPU spinning)

3. **Shutdown**
   - Deregister all buffers
   - Close RIO completion queue
   - Clean up resources

## Current Implementation Notes

### Event-Based Notification (Current Implementation)

The implementation now uses **event-based IOCP notification**:
- RIO completion queue is created with IOCP notification
- `RIONotify()` arms the notification before waiting
- `GetQueuedCompletionStatusEx()` blocks until completions are available
- Eliminates CPU spinning from busy polling
- Notification is rearmed after processing each batch of completions

### Known Limitations

1. **Packet Loss Under Load**
   - ~25% packet loss observed at 10,000 pps (localhost testing)
   - May need larger completion queue size or more outstanding operations
   - Notification latency can cause processing delays

2. **Buffer Management**
   - Fixed number of pre-allocated buffers (16 send + 16 receive per worker)
   - No dynamic buffer pool expansion
   - May limit throughput in some scenarios

3. **Error Handling**
   - Basic error handling for RIO-specific errors
   - May need enhancement for production use

## Performance Characteristics

### Observed Behavior (Localhost Testing)

**IOCP Mode (Baseline):**
- Consistent 0% packet loss at 10,000 pps
- Average RTT: ~0.21ms
- Efficient CPU usage
- Proven and stable for sustained load

**RIO Mode (Event-Based with IOCP Notification):**
- ~25% packet loss at 10,000 pps (significantly improved from polling mode's 88%)
- Average RTT: ~0.92ms
- Much better CPU efficiency than polling mode
- Event-based notification eliminates busy-wait CPU overhead
- Still requires tuning for optimal performance

### Optimization Opportunities

1. **Increase Outstanding Operations**
   - Current: 16 outstanding ops per worker
   - Consider increasing to 32 or 64 for higher throughput
   - Balance between memory usage and latency

2. **Buffer Pool Tuning**
   - Increase buffer counts for high-throughput scenarios
   - Dynamic buffer allocation
   - Per-core buffer pools

3. **Completion Queue Sizing**
   - Increase `RIO_CQ_SIZE` from 256 to 512 or 1024
   - Increase `RIO_MAX_RESULTS` from 64 to 128 for better batching
   - Balance latency vs throughput

4. **Notification Tuning**
   - Reduce IOCP timeout from 1000ms to 100ms or less
   - Consider adaptive timeout based on load
   - Hybrid approach: try dequeue before waiting

## Testing

### Basic Functionality Test

```bash
# Terminal 1: Start RIO server
echo_server --port 7777 --use-rio --cores 2 --duration 60

# Terminal 2: Run client
echo_client --server localhost --port 7777 --rate 10000 --duration 30 --cores 1
```

### Side-by-Side Comparison

```bash
# Terminal 1: IOCP mode on port 5000
echo_server --port 5000 --cores 4 --duration 60

# Terminal 2: RIO mode on port 5001  
echo_server --port 5001 --use-rio --cores 4 --duration 60

# Terminal 3: Test IOCP
echo_client --server localhost --port 5000 --rate 50000 --duration 30

# Terminal 4: Test RIO
echo_client --server localhost --port 5001 --rate 50000 --duration 30
```

## Future Work

1. **Performance Tuning**
   - Optimize completion queue and buffer sizes
   - Reduce packet loss through better resource allocation
   - Fine-tune IOCP timeout values

2. **Performance Profiling**
   - ETW tracing
   - CPU profiling
   - Network stack analysis

3. **Advanced Features**
   - Multi-buffer scatter/gather operations
   - Zero-copy buffer sharing
   - RDMA integration (when available)

4. **Configuration Options**
   - Tunable buffer counts
   - Configurable polling intervals
   - Adaptive mode selection

## References

- [Winsock Registered I/O (RIO) Extensions](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/hh997032(v=ws.11))
- [RIO API Documentation](https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-registered-i-o-rio-extensions)
- [Windows Networking Performance](https://learn.microsoft.com/en-us/windows-server/networking/technologies/network-subsystem/net-sub-performance-top)
