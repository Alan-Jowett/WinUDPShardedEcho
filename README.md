# Scalable Echo Server Demo

A high-performance UDP echo server and client implementation for Windows that demonstrates scalable network I/O using:

- **SIO_CPU_AFFINITY** - Socket-level CPU affinity to distribute network I/O across cores
- **IO Completion Ports (IOCP)** - Windows high-performance asynchronous I/O
- **Thread CPU affinity** - Worker threads pinned to specific CPU cores
- **One socket per CPU core** - Maximum parallelism with minimal lock contention
 - **One socket per CPU core (server)** - Server still uses one socket per core
 - **Multiple client sockets per worker (client)** - Client can open multiple sockets per worker, each bound to a unique ephemeral port to increase 5-tuple entropy

## Requirements

- Windows 10/11 or Windows Server 2016+
- Visual Studio 2022 or later with C++20 support
- CMake 3.20 or later

## Building

```bash
# Create build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Build
cmake --build . --config Release
```

## Usage

### Server

```bash
echo_server <port> [num_cores]
```

Arguments:
- `port` - UDP port to listen on (1-65535)
- `num_cores` - (Optional) Number of CPU cores to use (default: all available)

Example:
```bash
echo_server 5000        # Listen on port 5000 using all cores
echo_server 5000 4      # Listen on port 5000 using 4 cores
```

### Client

```bash
echo_client <server_ip> <port> [options]
```

**All Server Options**

- `--port, -p <port>`: UDP port to listen on (required)
- `--cores, -c <n>`: Number of cores/workers to use (default: all available)
- `--recvbuf, -b <bytes>`: Socket receive buffer size in bytes (default: `4194304` = 4MB)
- `--help, -h`: Show help/usage

**All Client Options**

- `--server, -s <host>`: Server hostname or IP (required)
- `--port, -p <port>`: Server UDP port (required)
- `--payload, -l <bytes>`: Payload size in bytes (default: `64`, max: `MAX_PAYLOAD_SIZE`)
- `--cores, -c <n>`: Number of cores/workers to use (default: all available)
- `--duration, -d <seconds>`: Test duration in seconds (default: `10`)
- `--rate, -r <pps>`: Packets per second total across all workers (default: `10000`, `0` = unlimited). The client divides this total evenly across workers.
- `--recvbuf, -b <bytes>`: Socket receive buffer size in bytes (default: `4194304` = 4MB)
- `--sockets, -k <n>`: Number of sockets to create per worker (default: `1`). Each socket is bound to its own ephemeral port (unique source port).
- `--help, -h`: Show help/usage


Example:
```bash
echo_client --server 127.0.0.1 --port 5000 --sockets 4 --rate 20000 --cores 2 --duration 5
echo_client --server 192.168.1.100 --port 5000 --sockets 1 --rate 10000 --payload 1024 --cores 4 --duration 30
```

## Architecture

### Server Architecture

```
+------------------+    +------------------+    +------------------+
|   CPU Core 0     |    |   CPU Core 1     |    |   CPU Core N     |
+------------------+    +------------------+    +------------------+
|  Worker Thread   |    |  Worker Thread   |    |  Worker Thread   |
|  (affinitized)   |    |  (affinitized)   |    |  (affinitized)   |
+--------+---------+    +--------+---------+    +--------+---------+
         |                       |                       |
+--------v---------+    +--------v---------+    +--------v---------+
|      IOCP        |    |      IOCP        |    |      IOCP        |
+--------+---------+    +--------+---------+    +--------+---------+
         |                       |                       |
+--------v---------+    +--------v---------+    +--------v---------+
|   UDP Socket     |    |   UDP Socket     |    |   UDP Socket     |
| (CPU affinitized)|    | (CPU affinitized)|    | (CPU affinitized)|
+------------------+    +------------------+    +------------------+
         |                       |                       |
         +-----------+-----------+-----------+-----------+
                     |
              +------v------+
              |  Port 5000  |
              +-------------+
```

### Packet Format

```
+------------------------+------------------------+
|  Sequence Number (8B)  |  Timestamp NS (8B)     |
+------------------------+------------------------+
|                    Payload                      |
+------------------------------------------------+
```

### Key Features

1. **Socket CPU Affinity (SIO_CPU_AFFINITY)**
   - Each socket is bound to a specific CPU core
   - Ensures network stack processing stays on the designated core
   - Reduces cache misses and improves locality

2. **Per-Worker IOCP (server)**
   - The server uses one socket per worker and a dedicated IOCP serviced by that worker thread
   - Eliminates contention between cores and keeps callbacks affinitized to the same core
   - Scales linearly with core count

3. **Client: multiple sockets per worker + per-worker IOCP**
   - The client can create multiple sockets per worker and associate them with the worker's IOCP
   - Each client socket is bound to a unique ephemeral source port (no SO_REUSEADDR), increasing entropy in the 5-tuple used by the OS hash
   - This helps the server's packet distribution across cores when only a single destination tuple is used

3. **Thread Affinity**
   - Worker threads are pinned to the same core as their socket
   - Ensures completion callbacks run on the same core as network I/O
   - Maximizes cache efficiency

4. **Multiple Outstanding Operations**
   - Multiple async receive operations posted per socket
   - Prevents gaps in packet reception
   - Maximizes throughput

   5. **Batched completion retrieval**
    - Both client and server use `GetQueuedCompletionStatusEx` to retrieve multiple completions per syscall
    - Reduces syscall overhead and improves batching of I/O completions

## Performance Tuning

For best performance:

1. Use RSS (Receive Side Scaling) capable NICs
2. Configure NIC RSS to match the number of cores being used
3. Ensure the server and client use the same number of cores
4. Consider disabling interrupt moderation for lowest latency
5. Increase socket buffer sizes if experiencing drops

## Statistics

The client tracks and reports:
- Packets sent/received per second
- Bytes sent/received (throughput in Mbps)
- Dropped packet count and percentage
- Round-trip time (min/avg/max in microseconds)

## License

MIT License - See [LICENSE](LICENSE) for details