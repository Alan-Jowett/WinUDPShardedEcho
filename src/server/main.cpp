// Copyright (c) 2025 WinUDPShardedEcho Contributors
// SPDX-License-Identifier: MIT

// Scalable UDP Echo Server
// - Opens a listening socket per CPU core
// - Uses SIO_CPU_AFFINITY to affinitize each socket
// - Uses an IO Completion Port per listening socket
// - Services each IOCP using an affinitized thread

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <numeric>
#include <syncstream>
#include <thread>

#include "common/arg_parser.hpp"
#include "common/socket_utils.hpp"

// Global flag for shutdown
std::atomic<bool> g_shutdown{false};
// Global verbose flag
std::atomic<bool> g_verbose{false};

// Signal handler
void signal_handler(int) {
    g_shutdown.store(true);
}

// Worker context for each CPU/socket pair
struct server_worker_context {
    uint32_t processor_id;
    unique_socket socket;
    unique_iocp iocp;
    std::jthread worker_thread;
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> packets_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> bytes_sent{0};
};

// Worker thread function
void worker_thread_func(server_worker_context* ctx) try {
    // Set thread affinity to match socket affinity
    set_thread_affinity(ctx->processor_id);

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
    std::transform(send_contexts.begin(), send_contexts.end(),
                   std::back_inserter(available_send_contexts),
                   [](const std::unique_ptr<io_context>& ptr) { return ptr.get(); });

    // Post initial receive operations
    for (auto& recv_ctx : recv_contexts) {
        post_recv(ctx->socket, recv_ctx.get());
    }

    if (g_verbose.load())
        std::osyncstream(std::cout)
            << std::format("[CPU {}] Worker started, {} outstanding receives\n", ctx->processor_id,
                           OUTSTANDING_OPS);

    // Short helper lambdas to make the completion-processing loop clearer.
    auto handle_recv_completion = [&](const io_context* io_ctx, DWORD bytes_transferred) {
        // Update basic receive counters
        ctx->packets_received.fetch_add(1);
        ctx->bytes_received.fetch_add(bytes_transferred);

        // If we received data, prepare to echo or process it
        if (bytes_transferred > 0) {
            // NOTE: This is the primary packet-processing area. Real servers would
            // parse or inspect the buffer here, apply protocol logic, and decide
            // whether to reply, forward, or drop the packet. Keep processing
            // extremely quick to avoid blocking the IOCP worker.

            // For this example we echo the packet back to the sender. Get a send context
            // from the pool and post a send.
            return true;  // indicate that further send-handling is required
        }
        return false;
    };

    auto handle_send_completion = [&](const io_context* io_ctx) {
        // Send completed — update counters and make context available again.
        // The caller is responsible for returning the context to the pool.
        return;
    };

    while (!g_shutdown.load()) {
        // Use GetQueuedCompletionStatusEx to batch completions
        const ULONG max_entries = static_cast<ULONG>(OUTSTANDING_OPS * 2);
        std::vector<OVERLAPPED_ENTRY> entries(max_entries);
        ULONG num_removed = 0;

        BOOL ex_result = GetQueuedCompletionStatusEx(ctx->iocp.get(), entries.data(), max_entries,
                                                     &num_removed, IOCP_SHUTDOWN_TIMEOUT_MS, FALSE);

        if (!ex_result) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                continue;
            }
            if (error == ERROR_ABANDONED_WAIT_0) {
                // IOCP was closed, time to exit
                continue;
            }
            std::osyncstream(std::cerr)
                << std::format("[CPU {}] GetQueuedCompletionStatusEx failed with error: {}\n",
                               ctx->processor_id, error);
            continue;
        }

        if (num_removed == 0) continue;

        for (ULONG ei = 0; ei < num_removed; ++ei) {
            const OVERLAPPED_ENTRY& entry = entries[ei];
            DWORD bytes_transferred = entry.dwNumberOfBytesTransferred;
            ULONG_PTR completion_key = entry.lpCompletionKey;
            LPOVERLAPPED overlapped = entry.lpOverlapped;
            if (overlapped == nullptr) continue;

            auto* io_ctx = static_cast<io_context*>(overlapped);

            if (io_ctx->operation == io_operation_type::recv) {
                bool needs_send = handle_recv_completion(io_ctx, bytes_transferred);

                if (needs_send) {
                    // Acquire a send context from the pool
                    io_context* send_ctx = nullptr;
                    if (!available_send_contexts.empty()) {
                        send_ctx = available_send_contexts.back();
                        available_send_contexts.pop_back();
                    } else {
                        std::osyncstream(std::cerr) << std::format(
                            "[CPU {}] No available send context\n", ctx->processor_id);
                        // Re-post receive and continue — do not block here
                        post_recv(ctx->socket, io_ctx);
                        continue;
                    }

                    // Echo the packet back — in a real server you would transform or
                    // generate an appropriate response instead of simply echoing.
                    post_send(ctx->socket, send_ctx, io_ctx->buffer.data(), bytes_transferred,
                              reinterpret_cast<sockaddr*>(&io_ctx->remote_addr),
                              io_ctx->remote_addr_len);
                    ctx->packets_sent.fetch_add(1);
                    ctx->bytes_sent.fetch_add(bytes_transferred);
                }

                // Re-post receive for continuous processing
                post_recv(ctx->socket, io_ctx);
            } else {
                // Send completed — return context to pool
                handle_send_completion(io_ctx);
                available_send_contexts.push_back(io_ctx);
            }
        }
    }

    if (g_verbose.load())
        std::osyncstream(std::cout) << std::format(
            "[CPU {}] Worker shutting down. Stats: recv={}, sent={}, "
            "bytes_recv={}, bytes_sent={}\n",
            ctx->processor_id, ctx->packets_received.load(), ctx->packets_sent.load(),
            ctx->bytes_received.load(), ctx->bytes_sent.load());
} catch (const std::exception& ex) {
    std::osyncstream(std::cerr) << std::format("[CPU {}] Worker thread exception: {}\n",
                                               ctx->processor_id, ex.what());
    // Shutdown on unhandled exception
    g_shutdown.store(true);
} catch (...) {
    std::osyncstream(std::cerr) << std::format("[CPU {}] Worker thread unknown exception\n",
                                               ctx->processor_id);
    // Shutdown on unhandled exception
    g_shutdown.store(true);
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --port, -p <port>         - UDP port to listen on (default: 7)\n"
              << "  --duration, -d <seconds>  - Run for N seconds then exit (0 = unlimited)\n"
              << "  --cores, -c <n>           - Number of cores to use (default: all available)\n"
              << "  --recvbuf, -b <bytes>     - Socket receive buffer size in bytes (default: "
                 "4194304 = 4MB)\n"
              << "  --verbose, -v             - Enable verbose logging (default: minimal)\n"
              << "  --help, -h                - Show this help\n";
}

int main(int argc, char* argv[]) try {
    ArgParser parser;
    parser.add_option("verbose", 'v', "0", false);
    parser.add_option("port", 'p', "7", true);  // Note: The IANA-assigned port for echo is 7
    parser.add_option("duration", 'd', "0", true);
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
    const std::string duration_str = parser.get("duration");
    const std::string verbose_str = parser.get("verbose");
    if (!verbose_str.empty() && verbose_str != "0") {
        g_verbose.store(true);
    }

    if (port_str.empty()) {
        throw std::invalid_argument("Port number is required");
    }

    char* endptr = nullptr;
    long port_l = std::strtol(port_str.c_str(), &endptr, 10);
    if (endptr == port_str.c_str() || port_l <= 0 || port_l > 65535) {
        throw std::invalid_argument("Invalid port number");
    }
    int port = static_cast<int>(port_l);
    if (port <= 0 || port > 65535) {
        throw std::invalid_argument("Port number out of range");
    }

    uint32_t num_processors = get_processor_count();
    uint32_t num_workers = num_processors;
    if (!cores_str.empty()) {
        int requested = static_cast<int>(std::strtol(cores_str.c_str(), nullptr, 10));
        if (requested > 0 && static_cast<uint32_t>(requested) <= num_processors) {
            num_workers = static_cast<uint32_t>(requested);
        }
    }

    // Parse receive buffer size
    int recvbuf = 4194304;  // default 4MB
    if (!recvbuf_str.empty()) {
        long v = std::strtol(recvbuf_str.c_str(), nullptr, 10);
        if (v > 0) recvbuf = static_cast<int>(v);
    }

    // Parse optional duration (seconds)
    int duration_sec = 0;
    if (!duration_str.empty()) {
        long d = std::strtol(duration_str.c_str(), nullptr, 10);
        if (d > 0) duration_sec = static_cast<int>(d);
    }

    std::cout << std::format("Scalable UDP Echo Server\n");
    std::cout << std::format("Port: {}\n", port);
    std::cout << std::format("Available processors: {}\n", num_processors);
    std::cout << std::format("Using {} worker(s)\n", num_workers);

    // Initialize Winsock
    initialize_winsock();

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create worker contexts
    std::vector<std::unique_ptr<server_worker_context>> workers;

    // Helper to create and initialize a single worker context for a given CPU id.
    auto create_worker = [&](uint32_t cpu_id,
                             int address_family) -> std::unique_ptr<server_worker_context> {
        auto ctx = std::make_unique<server_worker_context>();
        ctx->processor_id = cpu_id;

        ctx->socket = create_udp_socket(address_family);

        set_socket_cpu_affinity(ctx->socket, static_cast<uint16_t>(cpu_id));

        // Increase socket buffers.
        set_socket_option(ctx->socket, SOL_SOCKET, SO_RCVBUF,
                          reinterpret_cast<const char*>(&recvbuf), sizeof(recvbuf));
        set_socket_option(ctx->socket, SOL_SOCKET, SO_SNDBUF,
                          reinterpret_cast<const char*>(&recvbuf), sizeof(recvbuf));

        // Bind socket to the requested port
        bind_socket(ctx->socket, static_cast<uint16_t>(port), address_family);

        // Create IOCP and associate socket
        ctx->iocp = create_iocp_and_associate(ctx->socket);

        if (g_verbose.load())
            std::osyncstream(std::cout)
                << std::format("Created socket and IOCP for CPU {}\n", cpu_id);
        return ctx;
    };

    for (uint32_t i = 0; i < num_workers; ++i) {
        // Start one worker per address family per CPU
        workers.push_back(create_worker(i, AF_INET));
        workers.push_back(create_worker(i, AF_INET6));
    }

    if (workers.empty()) {
        throw std::runtime_error("No worker contexts created");
    }

    // Start worker threads
    auto start_worker_threads = [&](std::vector<std::unique_ptr<server_worker_context>>& wks) {
        for (auto& ctx : wks) {
            ctx->worker_thread = std::jthread(worker_thread_func, ctx.get());
        }
    };

    auto close_iocps = [&](const std::vector<std::unique_ptr<server_worker_context>>& wks) {
        for (const auto& ctx : wks) {
            ctx->iocp.reset();
        }
    };

    auto join_and_cleanup_workers =
        [&](const std::vector<std::unique_ptr<server_worker_context>>& wks) {
            for (const auto& ctx : wks) {
                if (ctx->worker_thread.joinable()) ctx->worker_thread.join();
            }

            for (const auto& ctx : wks) {
                ctx->socket.reset();
            }
        };

    auto print_final_stats = [&](const std::vector<std::unique_ptr<server_worker_context>>& wks) {
        uint64_t total_recv = 0, total_sent = 0, total_bytes_recv = 0, total_bytes_sent = 0;
        for (const auto& ctx : wks) {
            total_recv += ctx->packets_received.load();
            total_sent += ctx->packets_sent.load();
            total_bytes_recv += ctx->bytes_received.load();
            total_bytes_sent += ctx->bytes_sent.load();
        }

        std::osyncstream(std::cout) << std::format("\nFinal Statistics:\n");
        std::osyncstream(std::cout) << std::format("  Total packets received: {}\n", total_recv);
        std::osyncstream(std::cout) << std::format("  Total packets sent: {}\n", total_sent);
        std::osyncstream(std::cout)
            << std::format("  Total bytes received: {}\n", total_bytes_recv);
        std::osyncstream(std::cout) << std::format("  Total bytes sent: {}\n", total_bytes_sent);
    };

    start_worker_threads(workers);

    std::osyncstream(std::cout) << std::format(
        "\nServer running on port {}. Press Ctrl+C to stop.\n\n", port);

    // Optional timed shutdown
    std::thread duration_thread;
    if (duration_sec > 0) {
        duration_thread = std::thread([duration_sec]() {
            std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
            g_shutdown.store(true);
        });
    }

    // RPS printer thread: aggregate per-worker `packets_received` once per second
    std::thread rps_thread([&workers]() {
        uint64_t prev_total = 0;
        while (!g_shutdown.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            uint64_t total_recv = std::accumulate(
                workers.begin(), workers.end(), 0ULL,
                [&](uint64_t sum, const std::unique_ptr<server_worker_context>& ctx) {
                    return sum + ctx->packets_received.load(std::memory_order_relaxed);
                });

            uint64_t rps = (total_recv >= prev_total) ? (total_recv - prev_total) : 0;
            prev_total = total_recv;

            std::osyncstream(std::cout) << std::format("[RPS] {} req/s\n", rps);
        }
    });

    // Wait for shutdown signal (main thread sleeps while RPS thread runs)
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Join the RPS thread so it exits cleanly before we teardown workers
    if (rps_thread.joinable()) rps_thread.join();

    // Join duration thread if running
    if (duration_thread.joinable()) duration_thread.join();

    std::osyncstream(std::cout) << "\nShutting down...\n";

    // Close IOCPs to wake up worker threads, then join and cleanup
    close_iocps(workers);
    join_and_cleanup_workers(workers);

    // Print final stats and cleanup winsock
    print_final_stats(workers);

    cleanup_winsock();
    return 0;
} catch (const socket_exception& ex) {
    std::osyncstream(std::cerr) << std::format("Socket exception in main: {}\n", ex.what());
    return 1;
} catch (const std::exception& ex) {
    std::osyncstream(std::cerr) << std::format("Exception in main: {}\n", ex.what());
    return 1;
} catch (...) {
    std::osyncstream(std::cerr) << "Unknown exception in main\n";
    return 1;
}