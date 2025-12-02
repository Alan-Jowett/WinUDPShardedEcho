/**
 * @file main.cpp
 * @brief Scalable UDP echo client.
 *
 * The client creates one or more worker threads per CPU. Each worker affinitizes
 * sockets and threads to a specific logical processor, posts asynchronous
 * receives, and sends UDP packets containing a sequence number and timestamp.
 * Received echoes are used to compute RTT and detect lost packets.
 * 
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

// Scalable UDP Echo Client
// - Opens a socket per CPU core
// - Uses SIO_CPU_AFFINITY to affinitize each socket
// - Uses an IO Completion Port per socket
// - Services each IOCP using an affinitized thread
// - Sends UDP packets with sequence numbers
// - Tracks received packets to detect dropped packets

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <syncstream>
#include <unordered_set>

#include "common/arg_parser.hpp"
#include "common/socket_utils.hpp"


// Global flag for shutdown; set to true to request orderly termination.
std::atomic<bool> g_shutdown{false};
// Global verbose flag; when true, additional runtime information is logged.
std::atomic<bool> g_verbose{false};

/**
 * @brief Signal handler that requests shutdown.
 *
 * Sets `g_shutdown` to true so worker threads can exit cleanly.
 */
void signal_handler(int) {
    g_shutdown.store(true);
}

/**
 * @brief Per-worker context holding sockets, IOCP and statistics.
 *
 * Each worker owns one IOCP and one or more UDP sockets affinitized to the
 * worker's processor. The struct tracks per-worker counters for sent/recv
 * packets, outstanding sequence numbers, and RTT aggregates.
 */
struct client_worker_context {
    /// Logical processor this worker is affinitized to.
    uint32_t processor_id;
    /// UDP sockets owned by this worker.
    std::vector<unique_socket> sockets;
    /// IO Completion Port used by this worker.
    unique_iocp iocp;
    /// Worker thread instance.
    std::thread worker_thread;
    /// Next sequence number to use for outgoing packets.
    std::atomic<uint64_t> next_sequence{0};
    /// Counters for packets sent/received/dropped.
    std::atomic<uint64_t> packets_sent{0};
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> packets_dropped{0};
    /// Counters for bytes sent/received and RTT aggregations.
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> total_rtt_ns{0};
    std::atomic<uint64_t> min_rtt_ns{UINT64_MAX};
    std::atomic<uint64_t> max_rtt_ns{0};

    /// Outstanding sequence numbers awaiting echo responses.
    std::unordered_set<uint64_t> outstanding_sequences;

    /// Target server address to send packets to.
    sockaddr_storage server_addr;
    /// Length of `server_addr`.
    int server_addr_len;
    /// Per-worker packet rate (packets per second) assigned from global total.
    uint64_t per_worker_rate{0};
    /// Index used to round-robin across multiple sockets.
    std::atomic<size_t> next_socket_index{0};
};

// Packet rate limit total across all workers (packets per second, 0 = unlimited)
// Each worker will be assigned an equal share (plus remainder distribution).
uint64_t g_rate_limit = 10000;  // default total

/**
 * @brief Atomically update a target to the minimum of its current value and `value`.
 */
void update_min(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load();
    while (value < current && !target.compare_exchange_weak(current, value)) {
        // current is updated by compare_exchange_weak
    }
}

/**
 * @brief Atomically update a target to the maximum of its current value and `value`.
 */
void update_max(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load();
    while (value > current && !target.compare_exchange_weak(current, value)) {
        // current is updated by compare_exchange_weak
    }
}

/**
 * @brief Worker thread entrypoint.
 *
 * Each worker affinitizes the thread, posts a pool of receive operations,
 * sends packets according to the per-worker rate quota, and processes IOCP
 * completions for sends and receives.
 *
 * @param ctx Pointer to the worker context owned by the main thread.
 * @param payload_size Size in bytes of the application payload (not including header).
 */
void worker_thread_func(client_worker_context* ctx, size_t payload_size) try {
    // Set thread affinity to match socket affinity
    set_thread_affinity(ctx->processor_id);

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

    // Post initial receive operations across this worker's sockets (round-robin)
    if (!ctx->sockets.empty()) {
        size_t sock_idx = 0;
        while (!available_recv_contexts.empty()) {
            auto* recv_ctx = available_recv_contexts.back();
            available_recv_contexts.pop_back();
            post_recv(ctx->sockets[sock_idx % ctx->sockets.size()], recv_ctx);
            ++sock_idx;
        }
    }

    if (g_verbose.load())
        std::osyncstream(std::cout) << std::format("[CPU {}] Worker started\n", ctx->processor_id);

    // Rate limiting: each worker maintains a quota = elapsed_time * per_worker_rate
    auto start_time = std::chrono::steady_clock::now();

    while (!g_shutdown.load()) {
        // Try to send new packets up to quota (quota = elapsed_seconds * g_rate_limit)
        auto now = std::chrono::steady_clock::now();
        double elapsed_s =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
        uint64_t allowed =
            ctx->per_worker_rate == 0
                ? UINT64_MAX
                : static_cast<uint64_t>(elapsed_s * static_cast<double>(ctx->per_worker_rate));

        uint64_t sent_so_far = ctx->packets_sent.load();
        while (!available_send_contexts.empty() &&
               (ctx->per_worker_rate == 0 || sent_so_far < allowed)) {
            auto* send_ctx = available_send_contexts.back();
            available_send_contexts.pop_back();

            // Build packet
            packet_header* header = reinterpret_cast<packet_header*>(send_ctx->buffer.data());
            header->sequence_number = ctx->next_sequence.fetch_add(1);
            header->timestamp_ns = get_timestamp_ns();

            size_t total_size = HEADER_SIZE + payload_size;

            ctx->outstanding_sequences.insert(header->sequence_number);

            // Round-robin pick a socket from this worker's sockets

            post_send(ctx->sockets[ctx->next_socket_index.fetch_add(1) % ctx->sockets.size()],
                      send_ctx, send_ctx->buffer.data(), total_size,
                      reinterpret_cast<sockaddr*>(&ctx->server_addr), ctx->server_addr_len);

            ctx->packets_sent.fetch_add(1);
            ctx->bytes_sent.fetch_add(total_size);
            sent_so_far++;

            // recompute allowed in case per-worker rate changed dynamically
            if (ctx->per_worker_rate != 0) {
                now = std::chrono::steady_clock::now();
                elapsed_s =
                    std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time)
                        .count();
                allowed =
                    static_cast<uint64_t>(elapsed_s * static_cast<double>(ctx->per_worker_rate));
            }
        }

        // Check for completions (use GetQueuedCompletionStatusEx to batch completions)
        const ULONG max_entries = static_cast<ULONG>(OUTSTANDING_OPS * 2);
        std::vector<OVERLAPPED_ENTRY> entries(max_entries);
        ULONG num_removed = 0;

        BOOL ex_result = GetQueuedCompletionStatusEx(ctx->iocp.get(), entries.data(), max_entries,
                                                     &num_removed, IOCP_TIMEOUT_MS, FALSE);

        if (!ex_result) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                continue;
            }
            if (error == ERROR_INVALID_HANDLE || error == ERROR_ABANDONED_WAIT_0) {
                // IOCP was closed, time to exit
                continue;
            }

            std::osyncstream(std::cerr)
                << std::format("[Worker {}] GetQueuedCompletionStatusEx failed with error: {}\n",
                               ctx->processor_id, error);
            // On other errors just continue the loop
            continue;
        }

        if (num_removed == 0) {
            continue;
        }

        for (ULONG ei = 0; ei < num_removed; ++ei) {
            const OVERLAPPED_ENTRY& entry = entries[ei];
            DWORD bytes_transferred = entry.dwNumberOfBytesTransferred;
            ULONG_PTR completion_key = entry.lpCompletionKey;
            LPOVERLAPPED overlapped = entry.lpOverlapped;
            if (overlapped == nullptr) continue;

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

                // Re-post receive on the socket that completed
                SOCKET s = static_cast<SOCKET>(completion_key);
                // Find the unique_socket corresponding to this SOCKET
                auto it = std::find_if(ctx->sockets.begin(), ctx->sockets.end(),
                                       [s](const unique_socket& us) { return us.get() == s; });
                post_recv(*it, io_ctx);
            } else {
                // Send completed
                available_send_contexts.push_back(io_ctx);
            }
        }
    }

    // Count remaining outstanding as dropped (add to any already tracked as dropped)
    ctx->packets_dropped.fetch_add(ctx->outstanding_sequences.size());

    if (g_verbose.load())
        std::osyncstream(std::cout) << std::format(
            "[CPU {}] Worker shutting down. Stats: sent={}, recv={}, "
            "dropped={}\n",
            ctx->processor_id, ctx->packets_sent.load(), ctx->packets_received.load(),
            ctx->packets_dropped.load());
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

/**
 * @brief Print usage/help text to stdout.
 */
void print_usage(const char* program_name) {
    std::cout
        << "Usage: " << program_name << " [options]\n"
        << "Options:\n"
        << "  --server, -s <host>       - Server hostname or IP (required)\n"
        << "  --port, -p <port>         - Server UDP port (required)\n"
        << "  --payload, -l <bytes>     - Payload size in bytes (default: 64)\n"
        << "  --cores, -c <n>           - Number of cores/workers to use (default: all)\n"
        << "  --duration, -d <seconds>  - Test duration in seconds (default: 10)\n"
        << "  --rate, -r <pps>          - Packets per second total across all workers (0 = "
           "unlimited)\n"
        << "  --recvbuf, -b <bytes>     - Socket receive buffer size in bytes (default: "
           "4194304 = 4MB)\n"
        << "  --sockets, -k <n>         - Number of sockets to create per worker (default: 1)\n"
        << "  --verbose, -v             - Enable verbose logging (default: minimal)\n"
        << "  --help, -h                - Show this help\n";
}

/**
 * @brief Program entry point.
 *
 * Parses command-line arguments, initializes Winsock, creates worker
 * contexts and threads, runs for the requested duration and prints
 * final statistics.
 */
int main(int argc, char* argv[]) try {
    // Use ArgParser for command-line parsing
    ArgParser parser;
    parser.add_option("verbose", 'v', "0", false);
    parser.add_option("server", 's', "", true);
    parser.add_option("port", 'p', "7", true); // Note: The IANA-assigned port for echo is 7
    parser.add_option("payload", 'l', "64", true);
    parser.add_option("cores", 'c', "0", true);
    parser.add_option("duration", 'd', "10", true);
    parser.add_option("rate", 'r', "10000", true);
    parser.add_option("recvbuf", 'b', "4194304", true);
    parser.add_option("sockets", 'k', "1", true);
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
    const std::string sockets_str = parser.get("sockets");
    const std::string verbose_str = parser.get("verbose");
    size_t payload_size = 0;
    int duration_sec = 0;
    if (!verbose_str.empty() && verbose_str != "0") {
        g_verbose.store(true);
    }

    if (server_str.empty() || port_arg.empty()) {
        std::cerr << "Server and port are required\n";
        parser.print_help(argv[0]);
        return 1;
    }

    const char* server_ip = server_str.c_str();
    char* endptr = nullptr;
    long port_l = std::strtol(port_arg.c_str(), &endptr, 10);
    if (endptr == port_arg.c_str() || port_l <= 0 || port_l > 65535) {
        throw std::invalid_argument("Invalid port number");
    }
    int port = static_cast<int>(port_l);
    if (port <= 0 || port > 65535) {
        throw std::invalid_argument("Port number out of range");
    }

    payload_size = static_cast<size_t>(std::strtoul(payload_str.c_str(), nullptr, 10));
    if (payload_size == 0 || payload_size > MAX_PAYLOAD_SIZE) {
        throw std::invalid_argument("Invalid payload size");
    }

    uint32_t num_processors = get_processor_count();
    uint32_t num_workers = num_processors;
    duration_sec = static_cast<int>(std::strtol(duration_str.c_str(), nullptr, 10));
    if (!cores_str.empty()) {
        int requested = static_cast<int>(std::strtol(cores_str.c_str(), nullptr, 10));
        if (requested > 0 && static_cast<uint32_t>(requested) <= num_processors) {
            num_workers = static_cast<uint32_t>(requested);
        }
    }

    g_rate_limit = static_cast<uint64_t>(std::strtoull(rate_str.c_str(), nullptr, 10));
    int sockets_per_worker = static_cast<int>(std::strtol(sockets_str.c_str(), nullptr, 10));
    if (sockets_per_worker <= 0) sockets_per_worker = 1;

    // Parse receive buffer size for sockets (default 4MB)
    int recvbuf = 4194304;
    if (!recvbuf_str.empty()) {
        long v = std::strtol(recvbuf_str.c_str(), nullptr, 10);
        if (v > 0) recvbuf = static_cast<int>(v);
    }
    uint64_t per_worker_display = g_rate_limit == 0 ? 0 : (g_rate_limit / num_workers);

    std::cout << std::format("Scalable UDP Echo Client\n");
    std::cout << std::format("Server: {}:{}\n", server_ip, port);
    std::cout << std::format("Payload size: {} bytes\n", payload_size);
    std::cout << std::format("Available processors: {}\n", num_processors);
    std::cout << std::format("Using {} worker(s)\n", num_workers);
    std::cout << std::format("Duration: {} seconds\n", duration_sec);
    std::cout << std::format("Rate limit: {} packets/sec total ({} per worker)\n", g_rate_limit,
                             per_worker_display);

    // Initialize Winsock
    initialize_winsock();

    // Resolve server name (supports hostnames and IP literals)
    sockaddr_storage server_addr_storage = {};
    int server_addr_len = 0;

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* res = nullptr;
    std::string service_str = std::to_string(port);
    using unique_addrinfo = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;
    int gai_err = getaddrinfo(server_ip, service_str.c_str(), &hints, &res);
    if (gai_err != 0 || res == nullptr) {
        throw std::runtime_error(std::format("getaddrinfo failed for {}:{} with error: {}",
                                             server_ip, port, gai_strerror(gai_err)));
    }
    unique_addrinfo res_guard(res, freeaddrinfo);

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
        throw std::runtime_error(
            std::format("No suitable address found for {}:{}", server_ip, port));
    }

    // Copy resolved sockaddr into storage and set length
    std::memcpy(&server_addr_storage, chosen->ai_addr, chosen->ai_addrlen);
    server_addr_len = static_cast<int>(chosen->ai_addrlen);
    int server_family = chosen->ai_family;

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create worker contexts
    std::vector<std::unique_ptr<client_worker_context>> workers;

    for (uint32_t i = 0; i < num_workers; ++i) {
        auto ctx = std::make_unique<client_worker_context>();
        ctx->processor_id = i;

        // Copy resolved server address into worker context
        std::memcpy(&ctx->server_addr, &server_addr_storage, static_cast<size_t>(server_addr_len));
        ctx->server_addr_len = server_addr_len;

        // Create multiple UDP sockets for this worker, each bound to its own ephemeral port
        for (int sidx = 0; sidx < sockets_per_worker; ++sidx) {
            auto sock = create_udp_socket(server_family);

            // Set socket CPU affinity
            set_socket_cpu_affinity(sock, static_cast<uint16_t>(i));

            ctx->sockets.emplace_back(std::move(sock));
        }

        // Increase socket buffer sizes and bind each socket to an ephemeral port
        for (auto& sock : ctx->sockets) {
            set_socket_option(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvbuf),
                              sizeof(recvbuf));
            int sndbuf = recvbuf;
            set_socket_option(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf),
                              sizeof(sndbuf));

            bind_socket(sock, 0, server_family);
        }

        if (g_verbose.load()) {
            for (const auto& sock : ctx->sockets) {
                auto [addr, len] = get_socket_name(sock);
                if (addr.ss_family == AF_INET) {
                    sockaddr_in* in_addr = reinterpret_cast<sockaddr_in*>(&addr);
                    char ip_str[INET_ADDRSTRLEN] = {};
                    InetNtopA(AF_INET, &in_addr->sin_addr, ip_str,
                              static_cast<ULONG>(sizeof(ip_str)));
                    std::cout << std::format("Socket on CPU {} bound to {}:{}\n", i, ip_str,
                                             ntohs(in_addr->sin_port));
                } else if (addr.ss_family == AF_INET6) {
                    sockaddr_in6* in6_addr = reinterpret_cast<sockaddr_in6*>(&addr);
                    char ip_str[INET6_ADDRSTRLEN] = {};
                    InetNtopA(AF_INET6, &in6_addr->sin6_addr, ip_str,
                              static_cast<ULONG>(sizeof(ip_str)));
                    std::cout << std::format("Socket on CPU {} bound to [{}]:{}\n", i, ip_str,
                                             ntohs(in6_addr->sin6_port));
                }
            }
        }

        // Create IOCP for the worker and associate each socket with it
        ctx->iocp = create_iocp();

        for (auto& s : ctx->sockets) {
            associate_socket_with_iocp(s, ctx->iocp, static_cast<ULONG_PTR>(s.get()));
        }

        if (g_verbose.load()) std::cout << std::format("Created socket and IOCP for CPU {}\n", i);
        workers.push_back(std::move(ctx));
    }

    if (workers.empty()) {
        std::cerr << "Failed to create any workers\n";
        cleanup_winsock();
        return 1;
    }

    // Compute per-worker rate: divide global total equally among workers
    uint64_t per_worker_rate = 0;
    if (g_rate_limit == 0) {
        per_worker_rate = 0;  // 0 == unlimited
    } else {
        per_worker_rate = g_rate_limit / static_cast<uint64_t>(workers.size());
    }
    for (const auto& ctx : workers) {
        ctx->per_worker_rate = per_worker_rate;
    }

    // Start worker threads
    for (auto& ctx : workers) {
        ctx->worker_thread = std::thread(worker_thread_func, ctx.get(), payload_size);
    }

    if (g_verbose.load())
        std::cout << std::format("\nClient running for {} seconds. Press Ctrl+C to stop early.\n\n",
                                 duration_sec);

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
        std::cout << std::format("Progress: sent={}, recv={}, in-flight={}\n", total_sent,
                                 total_recv, total_sent - total_recv);
    }

    g_shutdown.store(true);
    std::cout << "\nStopping workers...\n";

    // Close IOCPs to wake up worker threads
    for (const auto& ctx : workers) {
        if (ctx->iocp != nullptr) {
            ctx->iocp.reset();
        }
    }

    // Wait for worker threads
    for (const auto& ctx : workers) {
        if (ctx->worker_thread.joinable()) {
            ctx->worker_thread.join();
        }
    }

    // Close sockets
    for (const auto& ctx : workers) {
        ctx->sockets.clear();
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

    double avg_rtt_us =
        total_recv > 0 ? (static_cast<double>(total_rtt) / total_recv / 1000.0) : 0.0;
    double min_rtt_us = min_rtt != UINT64_MAX ? static_cast<double>(min_rtt) / 1000.0 : 0.0;
    double max_rtt_us = static_cast<double>(max_rtt) / 1000.0;

    auto actual_duration = std::chrono::steady_clock::now() - start_time;
    double duration_s =
        std::chrono::duration_cast<std::chrono::milliseconds>(actual_duration).count() / 1000.0;
    double pps_sent = total_sent / duration_s;
    double pps_recv = total_recv / duration_s;
    double mbps_sent = (total_bytes_sent * 8.0) / (duration_s * 1000000.0);
    double mbps_recv = (total_bytes_recv * 8.0) / (duration_s * 1000000.0);

    std::cout << std::format("\n===== Final Statistics =====\n");
    std::cout << std::format("Duration: {:.2f} seconds\n", duration_s);
    std::cout << std::format("Packets sent: {} ({:.0f} pps)\n", total_sent, pps_sent);
    std::cout << std::format("Packets received: {} ({:.0f} pps)\n", total_recv, pps_recv);
    std::cout << std::format("Packets dropped: {} ({:.2f}%)\n", total_dropped,
                             total_sent > 0 ? (100.0 * total_dropped / total_sent) : 0.0);
    std::cout << std::format("Bytes sent: {} ({:.2f} Mbps)\n", total_bytes_sent, mbps_sent);
    std::cout << std::format("Bytes received: {} ({:.2f} Mbps)\n", total_bytes_recv, mbps_recv);
    std::cout << std::format("RTT (min/avg/max): {:.2f}/{:.2f}/{:.2f} us\n", min_rtt_us, avg_rtt_us,
                             max_rtt_us);

    cleanup_winsock();
    return 0;
} catch (const socket_exception& ex) {
    std::cerr << "Socket error: " << ex.what() << "\n";
    return 1;
} catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    return 1;
} catch (...) {
    std::cerr << "Unknown error occurred\n";
    return 1;
}