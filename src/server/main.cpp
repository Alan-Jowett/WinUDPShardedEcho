// Copyright (c) 2025 Alan Jowett
// SPDX-License-Identifier: MIT

// Scalable UDP Echo Server
// - Opens a listening socket per CPU core
// - Uses SIO_CPU_AFFINITY to affinitize each socket
// - Uses an IO Completion Port per listening socket
// - Services each IOCP using an affinitized thread

#include "common/socket_utils.hpp"

#include <csignal>
#include <syncstream>
#include "common/arg_parser.hpp"

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
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> packets_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> bytes_sent{0};
};

// Worker thread function
void worker_thread_func(worker_context* ctx) {
    // Set thread affinity to match socket affinity
    if (!set_thread_affinity(ctx->processor_id)) {
        std::osyncstream(std::cerr) << std::format("[CPU {}] Failed to set thread affinity\n", ctx->processor_id);
    } else {
        std::osyncstream(std::cout) << std::format("[CPU {}] Thread affinity set successfully\n", ctx->processor_id);
    }

    // Allocate receive contexts
    std::vector<std::unique_ptr<io_context>> recv_contexts;
    for (size_t i = 0; i < OUTSTANDING_OPS; ++i) {
        recv_contexts.push_back(std::make_unique<io_context>());
    }

    // Pool of send contexts
    std::vector<std::unique_ptr<io_context>> send_contexts;
    for (size_t i = 0; i < OUTSTANDING_OPS; ++i) {
        send_contexts.push_back(std::make_unique<io_context>());
    }
    std::vector<io_context*> available_send_contexts;
    for (auto& ctx_ptr : send_contexts) {
        available_send_contexts.push_back(ctx_ptr.get());
    }

    // Post initial receive operations
    for (auto& recv_ctx : recv_contexts) {
        if (!post_recv(ctx->socket, recv_ctx.get())) {
            std::osyncstream(std::cerr) << std::format("[CPU {}] Failed to post initial recv\n", ctx->processor_id);
        }
    }

    std::osyncstream(std::cout) << std::format("[CPU {}] Worker started, {} outstanding receives\n", 
                                               ctx->processor_id, OUTSTANDING_OPS);

    while (!g_shutdown.load()) {
        // Use GetQueuedCompletionStatusEx to batch completions
        const ULONG max_entries = static_cast<ULONG>(OUTSTANDING_OPS * 2);
        std::vector<OVERLAPPED_ENTRY> entries(max_entries);
        ULONG num_removed = 0;

        BOOL ex_result = GetQueuedCompletionStatusEx(
            ctx->iocp,
            entries.data(),
            max_entries,
            &num_removed,
            IOCP_SHUTDOWN_TIMEOUT_MS,
            FALSE
        );

        if (!ex_result) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                continue;
            }
            continue;
        }

        if (num_removed == 0) continue;

        for (ULONG ei = 0; ei < num_removed; ++ei) {
            OVERLAPPED_ENTRY &entry = entries[ei];
            DWORD bytes_transferred = entry.dwNumberOfBytesTransferred;
            ULONG_PTR completion_key = entry.lpCompletionKey;
            LPOVERLAPPED overlapped = entry.lpOverlapped;
            if (overlapped == nullptr) continue;

            auto* io_ctx = static_cast<io_context*>(overlapped);

            if (io_ctx->operation == io_operation_type::recv) {
                // Received a packet
                ctx->packets_received.fetch_add(1);
                ctx->bytes_received.fetch_add(bytes_transferred);

                if (bytes_transferred > 0) {
                    // Get a send context
                    io_context* send_ctx = nullptr;
                    if (!available_send_contexts.empty()) {
                        send_ctx = available_send_contexts.back();
                        available_send_contexts.pop_back();
                    } else {
                        std::osyncstream(std::cerr) << std::format("[CPU {}] No available send context\n", ctx->processor_id);
                        post_recv(ctx->socket, io_ctx);
                        continue;
                    }

                    // Echo the packet back using the originating socket
                    if (post_send(ctx->socket, send_ctx, io_ctx->buffer.data(), bytes_transferred,
                                 reinterpret_cast<sockaddr*>(&io_ctx->remote_addr), io_ctx->remote_addr_len)) {
                        ctx->packets_sent.fetch_add(1);
                        ctx->bytes_sent.fetch_add(bytes_transferred);
                    } else {
                        available_send_contexts.push_back(send_ctx);
                    }
                }

                // Re-post receive on worker's socket
                post_recv(ctx->socket, io_ctx);
            } else {
                // Send completed, return context to pool
                available_send_contexts.push_back(io_ctx);
            }
        }
    }

    std::osyncstream(std::cout) << std::format("[CPU {}] Worker shutting down. Stats: recv={}, sent={}, "
                                               "bytes_recv={}, bytes_sent={}\n",
                                               ctx->processor_id, 
                                               ctx->packets_received.load(),
                                               ctx->packets_sent.load(),
                                               ctx->bytes_received.load(),
                                               ctx->bytes_sent.load());
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --port, -p <port>         - UDP port to listen on (required)\n"
              << "  --cores, -c <n>           - Number of cores to use (default: all available)\n"
              << "  --recvbuf, -b <bytes>     - Socket receive buffer size in bytes (default: 4194304 = 4MB)\n"
              << "  --help, -h                - Show this help\n";
}

int main(int argc, char* argv[]) {
    ArgParser parser;
    parser.add_option("port", 'p', "", true);
    parser.add_option("cores", 'c', "0", true);
    parser.add_option("recvbuf", 'b', "4194304", true);
    parser.add_option("help", 'h', "0", false);
    parser.parse(argc, argv);

    if (parser.is_set("help")) {
        print_usage(argv[0]);
        return 0;
    }

    const std::string port_str = parser.get("port");
    const std::string cores_str = parser.get("cores");
    const std::string recvbuf_str = parser.get("recvbuf");

    if (port_str.empty()) {
        std::cerr << "Port is required\n";
        parser.print_help(argv[0]);
        return 1;
    }

    int port = std::atoi(port_str.c_str());
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port\n";
        parser.print_help(argv[0]);
        return 1;
    }

    uint32_t num_processors = get_processor_count();
    uint32_t num_workers = num_processors;
    if (!cores_str.empty()) {
        int requested = std::atoi(cores_str.c_str());
        if (requested > 0 && static_cast<uint32_t>(requested) <= num_processors) {
            num_workers = static_cast<uint32_t>(requested);
        }
    }

    // Parse receive buffer size
    int recvbuf = 4194304; // default 4MB
    if (!recvbuf_str.empty()) {
        long v = std::strtol(recvbuf_str.c_str(), nullptr, 10);
        if (v > 0) recvbuf = static_cast<int>(v);
    }

    std::cout << std::format("Scalable UDP Echo Server\n");
    std::cout << std::format("Port: {}\n", port);
    std::cout << std::format("Available processors: {}\n", num_processors);
    std::cout << std::format("Using {} worker(s)\n", num_workers);

    // Initialize Winsock
    if (!initialize_winsock()) {
        return 1;
    }

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create worker contexts
    std::vector<std::unique_ptr<worker_context>> workers;

    for (uint32_t i = 0; i < num_workers; ++i) {
        auto ctx = std::make_unique<worker_context>();
        ctx->processor_id = i;

        // Create UDP socket: try IPv6 dual-stack first, fall back to IPv4
        SOCKET sock = create_udp_socket(AF_INET6);
        bool using_ipv6 = false;
        if (sock == INVALID_SOCKET) {
            // Fall back to IPv4
            sock = create_udp_socket(AF_INET);
            if (sock == INVALID_SOCKET) {
                std::cerr << std::format("Failed to create socket for CPU {}\n", i);
                continue;
            }
        } else {
            // Try to make the IPv6 socket dual-stack (allow IPv4 mapped addresses)
            int v6only = 0;
            if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) == 0) {
                using_ipv6 = true;
            } else {
                // Could not set dual-stack; continue using IPv6 anyway
                using_ipv6 = true;
            }
        }

        ctx->socket = sock;

        // Set socket CPU affinity
        if (!set_socket_cpu_affinity(ctx->socket, static_cast<uint16_t>(i))) {
            std::cerr << std::format("Warning: Could not set CPU affinity for socket on CPU {}\n", i);
            // Continue anyway - affinity is an optimization
        }

        // Increase socket buffer sizes to reduce drops
        if (setsockopt(ctx->socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvbuf), sizeof(recvbuf)) != 0) {
            std::cerr << std::format("Warning: Could not set SO_RCVBUF to {} on CPU {}: {}\n", recvbuf, i, get_last_error_message());
        }
        int sndbuf = recvbuf;
        if (setsockopt(ctx->socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf)) != 0) {
            std::cerr << std::format("Warning: Could not set SO_SNDBUF to {} on CPU {}: {}\n", sndbuf, i, get_last_error_message());
        }

        // Bind socket to port (match family)
        bool bind_ok = false;
        if (using_ipv6) {
            sockaddr_in6 addr6 = {};
            addr6.sin6_family = AF_INET6;
            addr6.sin6_port = htons(static_cast<uint16_t>(port));
            addr6.sin6_addr = in6addr_any;
            if (bind(ctx->socket, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6)) != SOCKET_ERROR) {
                bind_ok = true;
            }
        } else {
            sockaddr_in addr4 = {};
            addr4.sin_family = AF_INET;
            addr4.sin_port = htons(static_cast<uint16_t>(port));
            addr4.sin_addr.s_addr = INADDR_ANY;
            if (bind(ctx->socket, reinterpret_cast<sockaddr*>(&addr4), sizeof(addr4)) != SOCKET_ERROR) {
                bind_ok = true;
            }
        }

        if (!bind_ok) {
            std::cerr << std::format("Failed to bind socket for CPU {}\n", i);
            closesocket(ctx->socket);
            continue;
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
        ctx->worker_thread = std::thread(worker_thread_func, ctx.get());
    }

    std::cout << std::format("\nServer running on port {}. Press Ctrl+C to stop.\n\n", port);

    // Wait for shutdown
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "\nShutting down...\n";

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

    // Print final stats
    uint64_t total_recv = 0, total_sent = 0, total_bytes_recv = 0, total_bytes_sent = 0;
    for (const auto& ctx : workers) {
        total_recv += ctx->packets_received.load();
        total_sent += ctx->packets_sent.load();
        total_bytes_recv += ctx->bytes_received.load();
        total_bytes_sent += ctx->bytes_sent.load();
    }

    std::cout << std::format("\nFinal Statistics:\n");
    std::cout << std::format("  Total packets received: {}\n", total_recv);
    std::cout << std::format("  Total packets sent: {}\n", total_sent);
    std::cout << std::format("  Total bytes received: {}\n", total_bytes_recv);
    std::cout << std::format("  Total bytes sent: {}\n", total_bytes_sent);

    cleanup_winsock();
    return 0;
}
