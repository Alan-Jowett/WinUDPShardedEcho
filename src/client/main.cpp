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

#include <csignal>
#include <syncstream>
#include <set>
#include <mutex>
#include <chrono>

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
    
    // Tracking sent packets (protected by mutex)
    std::mutex sent_mutex;
    std::set<uint64_t> outstanding_sequences;
    
    // Target server
    sockaddr_storage server_addr;
    int server_addr_len;
};

// Packet rate limit per worker (packets per second, 0 = unlimited)
constexpr uint64_t PACKETS_PER_SECOND = 10000;

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

    // Rate limiting
    auto last_send_time = std::chrono::steady_clock::now();
    uint64_t packets_this_second = 0;
    const uint64_t ns_per_packet = PACKETS_PER_SECOND > 0 ? 1000000000ULL / PACKETS_PER_SECOND : 0;

    while (!g_shutdown.load()) {
        // Try to send new packets (rate limited)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_send_time);
        if (elapsed.count() >= 1) {
            last_send_time = now;
            packets_this_second = 0;
        }

        if (!available_send_contexts.empty() && 
            (PACKETS_PER_SECOND == 0 || packets_this_second < PACKETS_PER_SECOND)) {
            
            auto* send_ctx = available_send_contexts.back();
            available_send_contexts.pop_back();

            // Build packet
            packet_header* header = reinterpret_cast<packet_header*>(send_ctx->buffer.data());
            header->sequence_number = ctx->next_sequence.fetch_add(1);
            header->timestamp_ns = get_timestamp_ns();

            size_t total_size = HEADER_SIZE + payload_size;

            // Track outstanding sequence
            {
                std::lock_guard<std::mutex> lock(ctx->sent_mutex);
                ctx->outstanding_sequences.insert(header->sequence_number);
            }

            if (post_send(ctx->socket, send_ctx, send_ctx->buffer.data(), total_size,
                         reinterpret_cast<sockaddr*>(&ctx->server_addr), ctx->server_addr_len)) {
                ctx->packets_sent.fetch_add(1);
                ctx->bytes_sent.fetch_add(total_size);
                packets_this_second++;
            } else {
                // Remove from tracking on failure
                {
                    std::lock_guard<std::mutex> lock(ctx->sent_mutex);
                    ctx->outstanding_sequences.erase(header->sequence_number);
                }
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

                // Remove from outstanding
                {
                    std::lock_guard<std::mutex> lock(ctx->sent_mutex);
                    ctx->outstanding_sequences.erase(header->sequence_number);
                }
            }

            // Re-post receive
            post_recv(ctx->socket, io_ctx);
        } else {
            // Send completed
            available_send_contexts.push_back(io_ctx);
        }
    }

    // Count remaining outstanding as dropped (add to any already tracked as dropped)
    {
        std::lock_guard<std::mutex> lock(ctx->sent_mutex);
        ctx->packets_dropped.fetch_add(ctx->outstanding_sequences.size());
    }

    std::osyncstream(std::cout) << std::format("[CPU {}] Worker shutting down. Stats: sent={}, recv={}, "
                                               "dropped={}\n",
                                               ctx->processor_id, 
                                               ctx->packets_sent.load(),
                                               ctx->packets_received.load(),
                                               ctx->packets_dropped.load());
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " <server_ip> <port> [payload_size] [num_cores] [duration_sec]\n"
              << "  server_ip    - IP address of the echo server\n"
              << "  port         - UDP port of the echo server (1-65535)\n"
              << "  payload_size - Size of payload in bytes (default: 64, max: " << MAX_PAYLOAD_SIZE << ")\n"
              << "  num_cores    - Number of cores to use (default: all available)\n"
              << "  duration_sec - Test duration in seconds (default: 10)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* server_ip = argv[1];
    
    int port = std::atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port number\n";
        print_usage(argv[0]);
        return 1;
    }

    size_t payload_size = 64;
    if (argc >= 4) {
        int requested = std::atoi(argv[3]);
        if (requested > 0 && static_cast<size_t>(requested) <= MAX_PAYLOAD_SIZE) {
            payload_size = static_cast<size_t>(requested);
        }
    }

    uint32_t num_processors = get_processor_count();
    uint32_t num_workers = num_processors;

    if (argc >= 5) {
        int requested = std::atoi(argv[4]);
        if (requested > 0 && static_cast<uint32_t>(requested) <= num_processors) {
            num_workers = static_cast<uint32_t>(requested);
        }
    }

    int duration_sec = 10;
    if (argc >= 6) {
        int requested = std::atoi(argv[5]);
        if (requested > 0) {
            duration_sec = requested;
        }
    }

    std::cout << std::format("Scalable UDP Echo Client\n");
    std::cout << std::format("Server: {}:{}\n", server_ip, port);
    std::cout << std::format("Payload size: {} bytes\n", payload_size);
    std::cout << std::format("Available processors: {}\n", num_processors);
    std::cout << std::format("Using {} worker(s)\n", num_workers);
    std::cout << std::format("Duration: {} seconds\n", duration_sec);
    std::cout << std::format("Rate limit: {} packets/sec per worker\n", PACKETS_PER_SECOND);

    // Initialize Winsock
    if (!initialize_winsock()) {
        return 1;
    }

    // Parse server address
    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        std::cerr << "Invalid server IP address\n";
        cleanup_winsock();
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

        // Copy server address
        std::memcpy(&ctx->server_addr, &server_addr, sizeof(server_addr));
        ctx->server_addr_len = sizeof(server_addr);

        // Create UDP socket
        ctx->socket = create_udp_socket();
        if (ctx->socket == INVALID_SOCKET) {
            std::cerr << std::format("Failed to create socket for CPU {}\n", i);
            continue;
        }

        // Set socket CPU affinity
        if (!set_socket_cpu_affinity(ctx->socket, static_cast<uint16_t>(i))) {
            std::cerr << std::format("Warning: Could not set CPU affinity for socket on CPU {}\n", i);
        }

        // Bind socket to any available port
        sockaddr_in bind_addr = {};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0;  // Any port
        if (bind(ctx->socket, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
            std::cerr << std::format("Failed to bind socket for CPU {}: {}\n", i, get_last_error_message());
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
