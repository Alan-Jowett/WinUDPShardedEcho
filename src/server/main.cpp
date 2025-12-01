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

// Number of outstanding receive operations per socket
constexpr size_t OUTSTANDING_RECVS = 16;

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
    for (size_t i = 0; i < OUTSTANDING_RECVS; ++i) {
        recv_contexts.push_back(std::make_unique<io_context>());
    }

    // Pool of send contexts
    std::vector<std::unique_ptr<io_context>> send_contexts;
    for (size_t i = 0; i < OUTSTANDING_RECVS; ++i) {
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
                                               ctx->processor_id, OUTSTANDING_RECVS);

    while (!g_shutdown.load()) {
        DWORD bytes_transferred = 0;
        ULONG_PTR completion_key = 0;
        LPOVERLAPPED overlapped = nullptr;

        BOOL result = GetQueuedCompletionStatus(
            ctx->iocp,
            &bytes_transferred,
            &completion_key,
            &overlapped,
            1000  // 1 second timeout to check shutdown flag
        );

        if (!result) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                continue;
            }
            if (overlapped != nullptr) {
                // I/O operation failed
                std::osyncstream(std::cerr) << std::format("[CPU {}] I/O operation failed: {}\n", 
                                                           ctx->processor_id, get_last_error_message());
                // Re-post the receive (post_recv resets the OVERLAPPED structure)
                auto* io_ctx = static_cast<io_context*>(overlapped);
                if (io_ctx->operation == io_operation_type::recv) {
                    // Reset and re-post receive operation
                    post_recv(ctx->socket, io_ctx);
                } else {
                    // Return send context to pool
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
                    // No available send context, skip echo (shouldn't happen with balanced pools)
                    std::osyncstream(std::cerr) << std::format("[CPU {}] No available send context\n", ctx->processor_id);
                    post_recv(ctx->socket, io_ctx);
                    continue;
                }

                // Echo the packet back
                if (post_send(ctx->socket, send_ctx, io_ctx->buffer.data(), bytes_transferred,
                             reinterpret_cast<sockaddr*>(&io_ctx->remote_addr), io_ctx->remote_addr_len)) {
                    ctx->packets_sent.fetch_add(1);
                    ctx->bytes_sent.fetch_add(bytes_transferred);
                } else {
                    // Return context to pool on failure
                    available_send_contexts.push_back(send_ctx);
                }
            }

            // Re-post receive
            post_recv(ctx->socket, io_ctx);
        } else {
            // Send completed, return context to pool
            available_send_contexts.push_back(io_ctx);
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
    std::cout << "Usage: " << program_name << " <port> [num_cores]\n"
              << "  port      - UDP port to listen on (1-65535)\n"
              << "  num_cores - Number of cores to use (default: all available)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port number\n";
        print_usage(argv[0]);
        return 1;
    }

    uint32_t num_processors = get_processor_count();
    uint32_t num_workers = num_processors;

    if (argc >= 3) {
        int requested = std::atoi(argv[2]);
        if (requested > 0 && static_cast<uint32_t>(requested) <= num_processors) {
            num_workers = static_cast<uint32_t>(requested);
        }
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

        // Create UDP socket
        ctx->socket = create_udp_socket();
        if (ctx->socket == INVALID_SOCKET) {
            std::cerr << std::format("Failed to create socket for CPU {}\n", i);
            continue;
        }

        // Set socket CPU affinity
        if (!set_socket_cpu_affinity(ctx->socket, static_cast<uint16_t>(i))) {
            std::cerr << std::format("Warning: Could not set CPU affinity for socket on CPU {}\n", i);
            // Continue anyway - affinity is an optimization
        }

        // Bind socket to port
        if (!bind_socket(ctx->socket, static_cast<uint16_t>(port))) {
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
