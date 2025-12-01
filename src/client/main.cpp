// Copyright (c) 2025 Alan Jowett
// SPDX-License-Identifier: MIT

// Scalable UDP Echo Client
// - Opens a socket per CPU core
// - Uses SIO_CPU_AFFINITY to affinitize each socket
// - Uses an IO Completion Port per socket
// - Services each IOCP using an affinitized thread
// - Sends UDP packets with sequence numbers
// - Tracks received packets to detect dropped packets

#include "common/socket_utils.hpp"
#include "common/arg_parser.hpp"

#include <csignal>
#include <syncstream>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <cstring>

// Global flag for shutdown
std::atomic<bool> g_shutdown{false};

// Signal handler
void signal_handler(int) {
    g_shutdown.store(true);
}

// Worker context for each CPU/socket pair
struct worker_context {
    uint32_t processor_id;
    SOCKET socket;
    HANDLE iocp;
    std::thread worker_thread;
    std::atomic<uint64_t> next_sequence{0};
    std::atomic<uint64_t> packets_sent{0};
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> packets_dropped{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> total_rtt_ns{0};
    std::atomic<uint64_t> min_rtt_ns{UINT64_MAX};
    std::atomic<uint64_t> max_rtt_ns{0};
    
    std::unordered_set<uint64_t> outstanding_sequences;
    
    // Target server
    sockaddr_storage server_addr;
    int server_addr_len;
};

// Packet rate limit per worker (packets per second, 0 = unlimited)
uint64_t g_rate_limit = 10000; // default

// Update min atomically
void update_min(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load();
    while (value < current && !target.compare_exchange_weak(current, value)) {
        // current is updated by compare_exchange_weak
    }
}

// Update max atomically
void update_max(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load();
    while (value > current && !target.compare_exchange_weak(current, value)) {
        // current is updated by compare_exchange_weak
    }
}

// Worker thread function
void worker_thread_func(worker_context* ctx, size_t payload_size) {
    // Set thread affinity to match socket affinity
    if (!set_thread_affinity(ctx->processor_id)) {
        std::osyncstream(std::cerr) << std::format("[CPU {}] Failed to set thread affinity\n", ctx->processor_id);
    } else {
        std::osyncstream(std::cout) << std::format("[CPU {}] Thread affinity set successfully\n", ctx->processor_id);
    }

    // Allocate IO contexts
    std::vector<std::unique_ptr<io_context>> contexts;
    for (size_t i = 0; i < OUTSTANDING_OPS * 2; ++i) {
        contexts.push_back(std::make_unique<io_context>());
    }
    
    // Split into send and receive pools
    std::vector<io_context*> available_recv_contexts;
    std::vector<io_context*> available_send_contexts;
    for (size_t i = 0; i < OUTSTANDING_OPS; ++i) {
        available_recv_contexts.push_back(contexts[i].get());
    }
    for (size_t i = OUTSTANDING_OPS; i < contexts.size(); ++i) {
        available_send_contexts.push_back(contexts[i].get());
    }

    // Post initial receive operations
    while (!available_recv_contexts.empty()) {
        auto* recv_ctx = available_recv_contexts.back();
        available_recv_contexts.pop_back();
        if (!post_recv(ctx->socket, recv_ctx)) {
            available_recv_contexts.push_back(recv_ctx);
            break;
        }
    }

    std::osyncstream(std::cout) << std::format("[CPU {}] Worker started\n", ctx->processor_id);

    // Rate limiting: schedule sends evenly over each second
    auto next_send_time = std::chrono::steady_clock::now();
    const uint64_t ns_per_packet = g_rate_limit > 0 ? 1000000000ULL / g_rate_limit : 0;

    while (!g_shutdown.load()) {
        // Try to send new packets (paced evenly)
        auto now = std::chrono::steady_clock::now();

        if (!available_send_contexts.empty() && (g_rate_limit == 0 || now >= next_send_time)) {
            auto* send_ctx = available_send_contexts.back();
            available_send_contexts.pop_back();

            // Build packet
            packet_header* header = reinterpret_cast<packet_header*>(send_ctx->buffer.data());
            header->sequence_number = ctx->next_sequence.fetch_add(1);
            header->timestamp_ns = get_timestamp_ns();

            size_t total_size = HEADER_SIZE + payload_size;

            ctx->outstanding_sequences.insert(header->sequence_number);

            if (post_send(ctx->socket, send_ctx, send_ctx->buffer.data(), total_size,
                         reinterpret_cast<sockaddr*>(&ctx->server_addr), ctx->server_addr_len)) {
                ctx->packets_sent.fetch_add(1);
                ctx->bytes_sent.fetch_add(total_size);
                if (g_rate_limit > 0) {
                    next_send_time += std::chrono::nanoseconds(ns_per_packet);
                    // avoid falling behind indefinitely
                    if (next_send_time < now) next_send_time = now;
                }
            } else {
                ctx->outstanding_sequences.erase(header->sequence_number);
                available_send_contexts.push_back(send_ctx);
            }
        }

        // Check for completions
        DWORD bytes_transferred = 0;
        ULONG_PTR completion_key = 0;
        LPOVERLAPPED overlapped = nullptr;

        BOOL result = GetQueuedCompletionStatus(
            ctx->iocp,
            &bytes_transferred,
            &completion_key,
            &overlapped,
            IOCP_TIMEOUT_MS
        );

        if (!result) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                continue;
            }
            if (overlapped != nullptr) {
                auto* io_ctx = static_cast<io_context*>(overlapped);
                if (io_ctx->operation == io_operation_type::recv) {
                    // Re-post receive
                    post_recv(ctx->socket, io_ctx);
                } else {
                    available_send_contexts.push_back(io_ctx);
                }
            }
            continue;
        }

        if (overlapped == nullptr) {
            continue;
        }

        auto* io_ctx = static_cast<io_context*>(overlapped);

        if (io_ctx->operation == io_operation_type::recv) {
            // Received echo response
            ctx->packets_received.fetch_add(1);
            ctx->bytes_received.fetch_add(bytes_transferred);

            if (bytes_transferred >= HEADER_SIZE) {
                packet_header* header = reinterpret_cast<packet_header*>(io_ctx->buffer.data());
                uint64_t recv_time = get_timestamp_ns();
                uint64_t rtt = recv_time - header->timestamp_ns;

                ctx->total_rtt_ns.fetch_add(rtt);
                update_min(ctx->min_rtt_ns, rtt);
                update_max(ctx->max_rtt_ns, rtt);
                ctx->outstanding_sequences.erase(header->sequence_number);
            }

            // Re-post receive
            post_recv(ctx->socket, io_ctx);
        } else {
            // Send completed
            available_send_contexts.push_back(io_ctx);
        }
    }

    // Count remaining outstanding as dropped (add to any already tracked as dropped)
    ctx->packets_dropped.fetch_add(ctx->outstanding_sequences.size());

    std::osyncstream(std::cout) << std::format("[CPU {}] Worker shutting down. Stats: sent={}, recv={}, "
                                               "dropped={}\n",
                                               ctx->processor_id, 
                                               ctx->packets_sent.load(),
                                               ctx->packets_received.load(),
                                               ctx->packets_dropped.load());
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --server, -s <host>       - Server hostname or IP (required)\n"
              << "  --port, -p <port>         - Server UDP port (required)\n"
              << "  --payload, -l <bytes>     - Payload size in bytes (default: 64)\n"
              << "  --cores, -c <n>           - Number of cores/workers to use (default: all)\n"
              << "  --duration, -d <seconds>  - Test duration in seconds (default: 10)\n"
              << "  --rate, -r <pps>          - Packets per second per worker (0 = unlimited)\n"
              << "  --recvbuf, -b <bytes>     - Socket receive buffer size in bytes (default: 4194304 = 4MB)\n"
              << "  --help, -h                - Show this help\n";
}

int main(int argc, char* argv[]) {
    // Use ArgParser for command-line parsing
    ArgParser parser;
    parser.add_option("server", 's', "", true);
    parser.add_option("port", 'p', "", true);
    parser.add_option("payload", 'l', "64", true);
    parser.add_option("cores", 'c', "0", true);
    parser.add_option("duration", 'd', "10", true);
    parser.add_option("rate", 'r', "10000", true);
    parser.add_option("recvbuf", 'b', "4194304", true);
    parser.add_option("help", 'h', "0", false);

    parser.parse(argc, argv);

    if (parser.is_set("help")) {
        print_usage(argv[0]);
        return 0;
    }

    const std::string server_str = parser.get("server");
    const std::string port_arg = parser.get("port");
    const std::string payload_str = parser.get("payload");
    const std::string cores_str = parser.get("cores");
    const std::string duration_str = parser.get("duration");
    const std::string rate_str = parser.get("rate");
    const std::string recvbuf_str = parser.get("recvbuf");

    if (server_str.empty() || port_arg.empty()) {
        std::cerr << "Server and port are required\n";
        parser.print_help(argv[0]);
        return 1;
    }

    const char* server_ip = server_str.c_str();
    int port = std::atoi(port_arg.c_str());
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port number\n";
        return 1;
    }

    size_t payload_size = static_cast<size_t>(std::atoi(payload_str.c_str()));
    if (payload_size == 0 || payload_size > MAX_PAYLOAD_SIZE) {
        std::cerr << "Invalid payload size\n";
        parser.print_help(argv[0]);
        return 1;
    }

    uint32_t num_processors = get_processor_count();
    uint32_t num_workers = num_processors;
    int duration_sec = std::atoi(duration_str.c_str());
    if (!cores_str.empty()) {
        int requested = std::atoi(cores_str.c_str());
        if (requested > 0 && static_cast<uint32_t>(requested) <= num_processors) {
            num_workers = static_cast<uint32_t>(requested);
        }
    }

    g_rate_limit = static_cast<uint64_t>(std::atoi(rate_str.c_str()));

    // Parse receive buffer size for sockets (default 4MB)
    int recvbuf = 4194304;
    if (!recvbuf_str.empty()) {
        long v = std::strtol(recvbuf_str.c_str(), nullptr, 10);
        if (v > 0) recvbuf = static_cast<int>(v);
    }

    std::cout << std::format("Scalable UDP Echo Client\n");
    std::cout << std::format("Server: {}:{}\n", server_ip, port);
    std::cout << std::format("Payload size: {} bytes\n", payload_size);
    std::cout << std::format("Available processors: {}\n", num_processors);
    std::cout << std::format("Using {} worker(s)\n", num_workers);
    std::cout << std::format("Duration: {} seconds\n", duration_sec);
    std::cout << std::format("Rate limit: {} packets/sec per worker\n", g_rate_limit);

    // Initialize Winsock
    if (!initialize_winsock()) {
        return 1;
    }

    // Resolve server name (supports hostnames and IP literals)
    sockaddr_storage server_addr_storage = {};
    int server_addr_len = 0;

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* res = nullptr;
    std::string service_str = std::to_string(port);
    int gai_err = getaddrinfo(server_ip, service_str.c_str(), &hints, &res);
    if (gai_err != 0 || res == nullptr) {
        std::cerr << "Failed to resolve server name '" << server_ip << "': " << gai_strerrorA(gai_err) << "\n";
        cleanup_winsock();
        return 1;
    }

    // Prefer IPv6 result when available, otherwise prefer IPv4, otherwise take first result
    addrinfo* chosen = nullptr;
    // First pass: look for IPv6
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET6) {
            chosen = ai;
            break;
        }
        if (chosen == nullptr) chosen = ai;
    }
    // If no IPv6, try to find IPv4 explicitly (chosen may already be set to first result)
    if (chosen == nullptr || chosen->ai_family != AF_INET6) {
        for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET) {
                chosen = ai;
                break;
            }
        }
    }

    if (chosen == nullptr) {
        std::cerr << "No suitable address found for " << server_ip << "\n";
        freeaddrinfo(res);
        cleanup_winsock();
        return 1;
    }

    // Copy resolved sockaddr into storage and set length
    std::memcpy(&server_addr_storage, chosen->ai_addr, chosen->ai_addrlen);
    server_addr_len = static_cast<int>(chosen->ai_addrlen);
    int server_family = chosen->ai_family;

    freeaddrinfo(res);

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create worker contexts
    std::vector<std::unique_ptr<worker_context>> workers;

    for (uint32_t i = 0; i < num_workers; ++i) {
        auto ctx = std::make_unique<worker_context>();
        ctx->processor_id = i;

        // Copy resolved server address into worker context
        std::memcpy(&ctx->server_addr, &server_addr_storage, static_cast<size_t>(server_addr_len));
        ctx->server_addr_len = server_addr_len;

        // Create UDP socket with the resolved address family
        ctx->socket = create_udp_socket(server_family);
        if (ctx->socket == INVALID_SOCKET) {
            std::cerr << std::format("Failed to create socket for CPU {}\n", i);
            continue;
        }

        // Set socket CPU affinity
        if (!set_socket_cpu_affinity(ctx->socket, static_cast<uint16_t>(i))) {
            std::cerr << std::format("Warning: Could not set CPU affinity for socket on CPU {}\n", i);
        }

        // Increase socket buffer sizes to reduce drops
        if (setsockopt(ctx->socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvbuf), sizeof(recvbuf)) != 0) {
            std::cerr << std::format("Warning: Could not set SO_RCVBUF to {} on CPU {}: {}\n", recvbuf, i, get_last_error_message());
        }
        int sndbuf = recvbuf;
        if (setsockopt(ctx->socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf)) != 0) {
            std::cerr << std::format("Warning: Could not set SO_SNDBUF to {} on CPU {}: {}\n", sndbuf, i, get_last_error_message());
        }

        // Bind socket to any available port (match address family)
        if (server_family == AF_INET6) {
            sockaddr_in6 bind_addr6 = {};
            bind_addr6.sin6_family = AF_INET6;
            bind_addr6.sin6_port = 0; // Any port
            bind_addr6.sin6_addr = in6addr_any;
            bind_addr6.sin6_scope_id = 0;
            if (bind(ctx->socket, reinterpret_cast<sockaddr*>(&bind_addr6), sizeof(bind_addr6)) == SOCKET_ERROR) {
                std::cerr << std::format("Failed to bind IPv6 socket for CPU {}: {}\n", i, get_last_error_message());
                closesocket(ctx->socket);
                continue;
            }
        } else {
            sockaddr_in bind_addr = {};
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_addr.s_addr = INADDR_ANY;
            bind_addr.sin_port = 0;  // Any port
            if (bind(ctx->socket, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
                std::cerr << std::format("Failed to bind socket for CPU {}: {}\n", i, get_last_error_message());
                closesocket(ctx->socket);
                continue;
            }
        }

        // Determine local port assigned by the OS and log it
        sockaddr_storage local_addr = {};
        socklen_t local_len = static_cast<socklen_t>(sizeof(local_addr));
        if (getsockname(ctx->socket, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0) {
            uint16_t local_port = 0;
            if (local_addr.ss_family == AF_INET) {
                local_port = ntohs(reinterpret_cast<sockaddr_in*>(&local_addr)->sin_port);
            } else if (local_addr.ss_family == AF_INET6) {
                local_port = ntohs(reinterpret_cast<sockaddr_in6*>(&local_addr)->sin6_port);
            }
            std::cout << "Socket for CPU " << i << " bound to local port " << local_port << '\n';
        } else {
            std::cerr << "Could not get local port for CPU " << i << ": " << get_last_error_message() << '\n';
        }

        // Create IOCP and associate socket
        ctx->iocp = create_iocp_and_associate(ctx->socket);
        if (ctx->iocp == nullptr) {
            std::cerr << std::format("Failed to create IOCP for CPU {}\n", i);
            closesocket(ctx->socket);
            continue;
        }

        std::cout << std::format("Created socket and IOCP for CPU {}\n", i);
        workers.push_back(std::move(ctx));
    }

    if (workers.empty()) {
        std::cerr << "Failed to create any workers\n";
        cleanup_winsock();
        return 1;
    }

    // Start worker threads
    for (auto& ctx : workers) {
        ctx->worker_thread = std::thread(worker_thread_func, ctx.get(), payload_size);
    }

    std::cout << std::format("\nClient running for {} seconds. Press Ctrl+C to stop early.\n\n", duration_sec);

    // Run for specified duration
    auto start_time = std::chrono::steady_clock::now();
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= duration_sec) {
            break;
        }

        // Print interim stats
        uint64_t total_sent = 0, total_recv = 0;
        for (const auto& ctx : workers) {
            total_sent += ctx->packets_sent.load();
            total_recv += ctx->packets_received.load();
        }
        std::cout << std::format("Progress: sent={}, recv={}, in-flight={}\n", 
                                  total_sent, total_recv, total_sent - total_recv);
    }

    g_shutdown.store(true);
    std::cout << "\nStopping workers...\n";

    // Close IOCPs to wake up worker threads
    for (auto& ctx : workers) {
        if (ctx->iocp != nullptr) {
            CloseHandle(ctx->iocp);
        }
    }

    // Wait for worker threads
    for (auto& ctx : workers) {
        if (ctx->worker_thread.joinable()) {
            ctx->worker_thread.join();
        }
    }

    // Close sockets
    for (auto& ctx : workers) {
        if (ctx->socket != INVALID_SOCKET) {
            closesocket(ctx->socket);
        }
    }

    // Calculate and print final stats
    uint64_t total_sent = 0, total_recv = 0, total_dropped = 0;
    uint64_t total_bytes_sent = 0, total_bytes_recv = 0;
    uint64_t total_rtt = 0;
    uint64_t min_rtt = UINT64_MAX, max_rtt = 0;

    for (const auto& ctx : workers) {
        total_sent += ctx->packets_sent.load();
        total_recv += ctx->packets_received.load();
        total_dropped += ctx->packets_dropped.load();
        total_bytes_sent += ctx->bytes_sent.load();
        total_bytes_recv += ctx->bytes_received.load();
        total_rtt += ctx->total_rtt_ns.load();
        
        uint64_t worker_min = ctx->min_rtt_ns.load();
        uint64_t worker_max = ctx->max_rtt_ns.load();
        if (worker_min < min_rtt) min_rtt = worker_min;
        if (worker_max > max_rtt) max_rtt = worker_max;
    }

    double avg_rtt_us = total_recv > 0 ? (static_cast<double>(total_rtt) / total_recv / 1000.0) : 0.0;
    double min_rtt_us = min_rtt != UINT64_MAX ? static_cast<double>(min_rtt) / 1000.0 : 0.0;
    double max_rtt_us = static_cast<double>(max_rtt) / 1000.0;

    auto actual_duration = std::chrono::steady_clock::now() - start_time;
    double duration_s = std::chrono::duration_cast<std::chrono::milliseconds>(actual_duration).count() / 1000.0;
    double pps_sent = total_sent / duration_s;
    double pps_recv = total_recv / duration_s;
    double mbps_sent = (total_bytes_sent * 8.0) / (duration_s * 1000000.0);
    double mbps_recv = (total_bytes_recv * 8.0) / (duration_s * 1000000.0);

    std::cout << std::format("\n===== Final Statistics =====\n");
    std::cout << std::format("Duration: {:.2f} seconds\n", duration_s);
    std::cout << std::format("Packets sent: {} ({:.0f} pps)\n", total_sent, pps_sent);
    std::cout << std::format("Packets received: {} ({:.0f} pps)\n", total_recv, pps_recv);
    std::cout << std::format("Packets dropped: {} ({:.2f}%)\n", 
                              total_dropped, total_sent > 0 ? (100.0 * total_dropped / total_sent) : 0.0);
    std::cout << std::format("Bytes sent: {} ({:.2f} Mbps)\n", total_bytes_sent, mbps_sent);
    std::cout << std::format("Bytes received: {} ({:.2f} Mbps)\n", total_bytes_recv, mbps_recv);
    std::cout << std::format("RTT (min/avg/max): {:.2f}/{:.2f}/{:.2f} us\n", min_rtt_us, avg_rtt_us, max_rtt_us);

    cleanup_winsock();
    return 0;
}
