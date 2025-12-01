# Scalable Echo Server Demo

A high-performance UDP echo server and client implementation for Windows that demonstrates scalable network I/O using:

- **SIO_CPU_AFFINITY** - Socket-level CPU affinity to distribute network I/O across cores
- **IO Completion Ports (IOCP)** - Windows high-performance asynchronous I/O
- **Thread CPU affinity** - Worker threads pinned to specific CPU cores
- **One socket per CPU core** - Maximum parallelism with minimal lock contention

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
echo_client <server_ip> <port> [payload_size] [num_cores] [duration_sec]
```

Arguments:
- `server_ip` - IP address of the echo server
- `port` - UDP port of the echo server (1-65535)
- `payload_size` - (Optional) Size of payload in bytes (default: 64, max: 65479)
- `num_cores` - (Optional) Number of CPU cores to use (default: all available)
- `duration_sec` - (Optional) Test duration in seconds (default: 10)

Example:
```bash
echo_client 127.0.0.1 5000              # Basic test to localhost
echo_client 192.168.1.100 5000 1024 4 30  # 1KB payloads, 4 cores, 30 seconds
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

2. **Per-Socket IOCP**
   - Each socket has its own dedicated IO Completion Port
   - Eliminates contention between cores
   - Scales linearly with core count

3. **Thread Affinity**
   - Worker threads are pinned to the same core as their socket
   - Ensures completion callbacks run on the same core as network I/O
   - Maximizes cache efficiency

4. **Multiple Outstanding Operations**
   - Multiple async receive operations posted per socket
   - Prevents gaps in packet reception
   - Maximizes throughput

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