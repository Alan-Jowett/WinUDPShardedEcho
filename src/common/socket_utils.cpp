/**
 * @file socket_utils.cpp
 * @brief Implementation of Win32 socket utilities and IOCP helpers.
 *
 * This translation unit implements helpers for initializing and cleaning up
 * Winsock, creating UDP sockets, binding sockets, posting overlapped send/recv
 * operations and working with IO Completion Ports (IOCP). It provides a thin
 * platform-specific layer used by the scalable echo server and client
 * examples.
 * 
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

#include "socket_utils.hpp"

#pragma comment(lib, "ws2_32.lib")

// SOCKET_PROCESSOR_AFFINITY may not be defined in older SDKs
#ifndef SIO_CPU_AFFINITY
#define SIO_CPU_AFFINITY _WSAIOW(IOC_VENDOR, 21)
#endif

// Implement RAII wrapper methods
unique_rio_cq::~unique_rio_cq() noexcept {
    reset();
}

void unique_rio_cq::reset() noexcept {
    if (cq_ != RIO_INVALID_CQ && rio_) {
        rio_->RIOCloseCompletionQueue(cq_);
        cq_ = RIO_INVALID_CQ;
        rio_ = nullptr;
    }
}

unique_rio_buffer::~unique_rio_buffer() noexcept {
    reset();
}

void unique_rio_buffer::reset() noexcept {
    if (buffer_id_ != RIO_INVALID_BUFFERID && rio_) {
        rio_->RIODeregisterBuffer(buffer_id_);
        buffer_id_ = RIO_INVALID_BUFFERID;
        rio_ = nullptr;
    }
}

/**
 * @brief Initialize the Winsock library (WSAStartup).
 *
 * Throws a `socket_exception` if WSAStartup fails.
 */
void initialize_winsock() {
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        throw socket_exception(std::format("WSAStartup failed: {}", result));
    }
}

/**
 * @brief Cleanup the Winsock library (WSACleanup).
 */
void cleanup_winsock() {
    WSACleanup();
}

/**
 * @brief Create a UDP socket for the given address family.
 *
 * @param family Address family (AF_INET or AF_INET6). If an unsupported
 *               family is passed, AF_INET will be used.
 * @param use_rio If true, creates a socket suitable for RIO operations.
 * @throws socket_exception on failure to create the socket.
 * @return RAII `unique_socket` owning the created SOCKET.
 */
unique_socket create_udp_socket(int family, bool use_rio) {
    int af = family;
    if (af != AF_INET && af != AF_INET6) {
        af = AF_INET;
    }
    DWORD flags = WSA_FLAG_OVERLAPPED;
    if (use_rio) {
        flags |= WSA_FLAG_REGISTERED_IO;
    }
    SOCKET raw = WSASocketW(af, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, flags);
    if (raw == INVALID_SOCKET) {
        throw socket_exception(std::format("WSASocket failed: {}", get_last_error_message()));
    }
    return unique_socket(raw);
}

/**
 * @brief Set CPU affinity for a socket using SIO_CPU_AFFINITY.
 *
 * @param sock Socket to configure.
 * @param processor_id Logical processor index to bind the socket to.
 * @throws socket_exception on failure.
 */
void set_socket_cpu_affinity(const unique_socket& sock, uint16_t processor_id) {
    // SIO_CPU_AFFINITY sets the processor affinity for the socket

    DWORD bytes_returned = 0;
    SOCKET raw = sock.get();
    int result = WSAIoctl(raw, SIO_CPU_AFFINITY, &processor_id, sizeof(processor_id), nullptr, 0,
                          &bytes_returned, nullptr, nullptr);

    if (result == SOCKET_ERROR) {
        throw socket_exception(
            std::format("WSAIoctl SIO_CPU_AFFINITY failed: {}", get_last_error_message()));
    }
}

/**
 * @brief Create an IO Completion Port and associate the provided socket with it.
 *
 * @param sock Socket to associate with the newly created IOCP.
 * @throws socket_exception on failure.
 * @return A `unique_iocp` handle for the created IOCP.
 */
unique_iocp create_iocp_and_associate(const unique_socket& sock) {
    HANDLE iocp = CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock.get()), nullptr, 0, 1);
    if (iocp == nullptr) {
        throw socket_exception(
            std::format("CreateIoCompletionPort failed: {}", get_last_error_message()));
    }
    // Attempt to set completion notification modes on the IOCP handle. If this
    // fails we'll let the wrapper throw a socket_exception.
    set_file_completion_notification_modes(reinterpret_cast<HANDLE>(sock.get()));
    return unique_iocp(iocp);
}

/**
 * @brief Associate an existing socket with an existing IO Completion Port.
 *
 * @param sock Socket to associate.
 * @param iocp IOCP handle to associate the socket with.
 * @param completion_key Completion key passed to IOCP completions.
 * @throws socket_exception on failure.
 */
void associate_socket_with_iocp(const unique_socket& sock, unique_iocp& iocp,
                                ULONG_PTR completion_key) {
    HANDLE result =
        CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock.get()), iocp.get(), completion_key, 0);
    if (result == nullptr) {
        throw socket_exception(
            std::format("CreateIoCompletionPort (associate) failed: {}", get_last_error_message()));
    }
    // Ensure the IOCP handle has file completion notification modes set.
    set_file_completion_notification_modes(reinterpret_cast<HANDLE>(sock.get()));
}

/**
 * @brief Wrapper around SetFileCompletionNotificationModes that throws on failure.
 */
void set_file_completion_notification_modes(HANDLE handle, UCHAR flags) {
    
    if (!SetFileCompletionNotificationModes(handle, flags)) {
        throw socket_exception(std::format("SetFileCompletionNotificationModes failed: {}", get_last_error_message()));
    }
}

// Create an unassociated IOCP
/**
 * @brief Create an unassociated IO Completion Port (IOCP).
 *
 * @throws socket_exception on failure.
 * @return A `unique_iocp` owning the IOCP handle.
 */
unique_iocp create_iocp() {
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (iocp == nullptr) {
        throw socket_exception(
            std::format("CreateIoCompletionPort failed: {}", get_last_error_message()));
    }
    return unique_iocp(iocp);
}

/**
 * @brief Pin the current thread to the given logical processor (group 0).
 *
 * @param processor_id Logical processor index within group 0.
 * @throws socket_exception if the processor id is out of range or setting
 *                          the group affinity fails.
 */
void set_thread_affinity(uint32_t processor_id) {
    // Treat `processor_id` as a global logical processor index. Resolve it
    // into a (group, index) pair and set the thread's GROUP_AFFINITY. This
    // correctly supports systems with >64 logical processors (processor
    // groups).
    WORD groupCount = GetActiveProcessorGroupCount();
    if (groupCount == 0) {
        throw socket_exception("No processor groups found");
    }

    // Map global processor index to (group, index) by walking groups and
    // subtracting their active processor counts. This correctly handles
    // uneven group sizes and systems with arbitrary group layouts.
    uint32_t remaining = processor_id;
    WORD target_group = 0;
    DWORD target_index = 0;
    bool found = false;

    for (WORD g = 0; g < groupCount; ++g) {
        DWORD cnt = GetActiveProcessorCount(g);
        if (remaining < cnt) {
            target_group = g;
            target_index = remaining;
            found = true;
            break;
        }
        remaining -= cnt;
    }

    if (!found) {
        throw socket_exception(std::format("Processor ID {} is out of range ({} logical processors)", processor_id, processor_id + 1u));
    }

    GROUP_AFFINITY affinity = {};
    affinity.Group = target_group;
    affinity.Mask = static_cast<KAFFINITY>(1ULL) << target_index;

    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr)) {
        throw socket_exception(std::format(
            "SetThreadGroupAffinity failed for processor {}: {}", processor_id, get_last_error_message()));
    }
}

/**
 * @brief Return the number of logical processors available to the system.
 *
 * @return Number of logical processors.
 */
uint32_t get_processor_count() {
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    return sys_info.dwNumberOfProcessors;
}

/**
 * @brief Bind a UDP socket to the specified port for the given address family.
 *
 * @param sock Socket to bind.
 * @param port Port number in host byte order.
 * @param family AF_INET or AF_INET6.
 * @throws socket_exception on failure.
 */
void bind_socket(const unique_socket& sock, uint16_t port, int family) {
    if (family == AF_INET) {
        sockaddr_in addr = {};
        addr.sin_family = family;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            throw socket_exception(std::format("bind failed: {}", get_last_error_message()));
        }
    } else if (family == AF_INET6) {
        sockaddr_in6 addr6 = {};
        addr6.sin6_family = family;
        addr6.sin6_addr = in6addr_any;
        addr6.sin6_port = htons(port);
        if (bind(sock.get(), reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6)) == SOCKET_ERROR) {
            throw socket_exception(std::format("bind failed: {}", get_last_error_message()));
        }
    } else {
        throw socket_exception(std::format("Unsupported address family for bind: {}", family));
    }
}

/**
 * @brief Helper wrapper around `setsockopt` that throws on error.
 *
 * @param sock Socket to configure.
 * @param level Protocol level for the option.
 * @param optname Option name.
 * @param optval Pointer to option value buffer.
 * @param optlen Length of option value buffer.
 * @throws socket_exception on failure.
 */
void set_socket_option(const unique_socket& sock, int level, int optname, const char* optval,
                       int optlen) {
    if (setsockopt(sock.get(), level, optname, optval, optlen) != 0) {
        throw socket_exception(std::format("setsockopt failed (level={}, optname={}): {}", level,
                                           optname, get_last_error_message()));
    }
}

/**
 * @brief Post an asynchronous receive (WSARecvFrom) for `sock` using `ctx`.
 *
 * On failure other than `WSA_IO_PENDING` the error is logged to `std::cerr`.
 */
void post_recv(const unique_socket& sock, io_context* ctx) {
    ctx->operation = io_operation_type::recv;
    ctx->wsa_buf.buf = ctx->buffer.data();
    ctx->wsa_buf.len = static_cast<ULONG>(ctx->buffer.size());
    ctx->remote_addr_len = sizeof(ctx->remote_addr);

    // Reset OVERLAPPED structure
    ctx->Internal = 0;
    ctx->InternalHigh = 0;
    ctx->Offset = 0;
    ctx->OffsetHigh = 0;

    DWORD flags = 0;
    DWORD bytes_received = 0;

    int result = WSARecvFrom(sock.get(), &ctx->wsa_buf, 1, &bytes_received, &flags,
                             reinterpret_cast<sockaddr*>(&ctx->remote_addr), &ctx->remote_addr_len,
                             ctx, nullptr);

    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        // Ignore WSAECONNRESET which can happen with UDP when no one is listening
        if (error != WSA_IO_PENDING && error != WSAECONNRESET) {
            std::cerr << std::format("WSARecvFrom failed: {} ({})\n", get_last_error_message(),
                                     error);
        }
    }
}

/**
 * @brief Post an asynchronous send (WSASendTo) copying `data` into `ctx`.
 *
 * @param sock Socket to send on.
 * @param ctx Overlapped I/O context to use for the operation.
 * @param data Pointer to the data to send. Data is copied into `ctx->buffer`.
 * @param len Length of data to send; truncated to the context buffer size if larger.
 * @param dest_addr Destination socket address.
 * @param dest_addr_len Length of the destination socket address.
 * @throws socket_exception on non-pending failures from WSASendTo.
 */
void post_send(const unique_socket& sock, io_context* ctx, const char* data, size_t len,
               const sockaddr* dest_addr, int dest_addr_len) {
    ctx->operation = io_operation_type::send;

    // Copy data to buffer
    if (len > ctx->buffer.size()) {
        len = ctx->buffer.size();
    }
    std::memcpy(ctx->buffer.data(), data, len);
    ctx->wsa_buf.buf = ctx->buffer.data();
    ctx->wsa_buf.len = static_cast<ULONG>(len);

    // Reset OVERLAPPED structure
    std::memset(static_cast<OVERLAPPED*>(ctx), 0, sizeof(OVERLAPPED));

    DWORD bytes_sent = 0;

    int result = WSASendTo(sock.get(), &ctx->wsa_buf, 1, &bytes_sent, 0, dest_addr, dest_addr_len,
                           ctx, nullptr);

    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            throw socket_exception(
                std::format("WSASendTo failed: {} ({})", get_last_error_message(), error));
        }
    }
}

    int send_sync(const unique_socket& sock, const char* data, size_t len, const sockaddr* dest_addr,
                  int dest_addr_len) {
        // For UDP, sendto either sends the full datagram or fails.
        int to_send = static_cast<int>(std::min<size_t>(len, INT_MAX));
        int sent = sendto(sock.get(), data, to_send, 0, dest_addr, dest_addr_len);
        if (sent == SOCKET_ERROR) {
            throw socket_exception(std::format("sendto failed: {}", get_last_error_message()));
        }
        return sent;
    }

/**
 * @brief Retrieve the local socket name (getsockname) for `sock`.
 *
 * @param sock Socket to query.
 * @return Pair of `sockaddr_storage` and actual length in bytes.
 * @throws socket_exception on failure.
 */
std::pair<sockaddr_storage, int> get_socket_name(const unique_socket& sock) {
    sockaddr_storage storage = {};
    int len = static_cast<int>(sizeof(storage));
    if (getsockname(sock.get(), reinterpret_cast<sockaddr*>(&storage), &len) != 0) {
        throw socket_exception(std::format("getsockname failed: {}", get_last_error_message()));
    }
    return {storage, len};
}

/**
 * @brief Return a monotonic timestamp in nanoseconds using QueryPerformanceCounter.
 */
uint64_t get_timestamp_ns() {
    static LARGE_INTEGER frequency = {};
    static std::once_flag freq_once;
    std::call_once(freq_once, [&]() { QueryPerformanceFrequency(&frequency); });

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    // Convert to nanoseconds
    return static_cast<uint64_t>(counter.QuadPart * 1000000000ULL / frequency.QuadPart);
}

/**
 * @brief Format the last Win32 or Winsock error code into a human-readable string.
 *
 * The function checks `GetLastError()` and then `WSAGetLastError()` and
 * uses `FormatMessageA` to produce a textual description.
 *
 * @return Human-readable error message string.
 */
std::string get_last_error_message() {
    DWORD error = GetLastError();
    if (error == 0) {
        error = static_cast<DWORD>(WSAGetLastError());
    }

    if (error == 0) {
        return "No error";
    }

    LPSTR buffer = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buffer),
        0, nullptr);

    std::string message;
    if (size > 0 && buffer != nullptr) {
        message = std::string(buffer, size);
        // Remove trailing newline
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.pop_back();
        }
        LocalFree(buffer);
    } else {
        message = std::format("Error code {}", error);
    }

    return message;
}

/**
 * @brief Load the RIO extension function table from Winsock.
 *
 * @param sock A valid socket used to retrieve extension functions via WSAIoctl.
 * @throws socket_exception if loading the function table fails.
 * @return RIO_EXTENSION_FUNCTION_TABLE with all RIO function pointers populated.
 */
RIO_EXTENSION_FUNCTION_TABLE load_rio_function_table(const unique_socket& sock) {
    RIO_EXTENSION_FUNCTION_TABLE rio = {};
    GUID rio_guid = WSAID_MULTIPLE_RIO;
    DWORD bytes_returned = 0;

    int result = WSAIoctl(sock.get(), SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &rio_guid,
                          sizeof(rio_guid), &rio, sizeof(rio), &bytes_returned, nullptr, nullptr);

    if (result == SOCKET_ERROR) {
        throw socket_exception(
            std::format("Failed to load RIO function table: {}", get_last_error_message()));
    }

    return rio;
}

/**
 * @brief Create a RIO completion queue.
 *
 * @param rio RIO function table.
 * @param queue_size Size of the completion queue.
 * @throws socket_exception on failure.
 * @return RIO_CQ handle for the completion queue.
 */
unique_rio_cq create_rio_completion_queue(const RIO_EXTENSION_FUNCTION_TABLE& rio, DWORD queue_size) {
    // Create a completion queue using polling mode (IOCP notification doesn't work with UDP)
    RIO_CQ cq = rio.RIOCreateCompletionQueue(queue_size, nullptr);
    if (cq == RIO_INVALID_CQ) {
        throw socket_exception(
            std::format("RIOCreateCompletionQueue failed: {}", get_last_error_message()));
    }

    return unique_rio_cq(cq, &rio);
}

/**
 * @brief Create a RIO request queue for a socket.
 *
 * @param rio RIO function table.
 * @param sock Socket to create the request queue for.
 * @param completion_queue Completion queue to associate with the request queue.
 * @param max_outstanding_recv Maximum outstanding receive operations.
 * @param max_outstanding_send Maximum outstanding send operations.
 * @throws socket_exception on failure.
 * @return RIO_RQ handle for the request queue.
 */
unique_rio_rq create_rio_request_queue(const RIO_EXTENSION_FUNCTION_TABLE& rio, const unique_socket& sock,
                                const unique_rio_cq& completion_queue, DWORD max_outstanding_recv,
                                DWORD max_outstanding_send) {
    RIO_RQ rq = rio.RIOCreateRequestQueue(sock.get(), max_outstanding_recv, 1,
                                          max_outstanding_send, 1, completion_queue.get(),
                                          completion_queue.get(), nullptr);

    if (rq == RIO_INVALID_RQ) {
        throw socket_exception(
            std::format("RIOCreateRequestQueue failed: {}", get_last_error_message()));
    }

    return unique_rio_rq(rq);
}

/**
 * @brief Register a buffer for use with RIO.
 *
 * @param rio RIO function table.
 * @param buffer Pointer to the buffer to register.
 * @param size Size of the buffer in bytes.
 * @throws socket_exception on failure.
 * @return RIO_BUFFERID for the registered buffer.
 */
unique_rio_buffer register_rio_buffer(const RIO_EXTENSION_FUNCTION_TABLE& rio, void* buffer, DWORD size) {
    RIO_BUFFERID buffer_id = rio.RIORegisterBuffer(reinterpret_cast<PCHAR>(buffer), size);

    if (buffer_id == RIO_INVALID_BUFFERID) {
        throw socket_exception(
            std::format("RIORegisterBuffer failed: {}", get_last_error_message()));
    }

    return unique_rio_buffer(buffer_id, &rio);
}

/**
 * @brief Post a RIO receive operation.
 *
 * @param rio RIO function table.
 * @param rq Request queue.
 * @param ctx RIO context with registered buffers.
 */
void post_rio_recv(const RIO_EXTENSION_FUNCTION_TABLE& rio, RIO_RQ rq, rio_context* ctx) {
    ctx->operation = io_operation_type::recv;
    ctx->remote_addr_len = sizeof(ctx->remote_addr);

    RIO_BUF data_buf = {};
    data_buf.BufferId = ctx->buffer_id.get();
    data_buf.Offset = 0;
    data_buf.Length = static_cast<ULONG>(ctx->buffer.size());

    RIO_BUF addr_buf = {};
    addr_buf.BufferId = ctx->addr_buffer_id.get();
    addr_buf.Offset = 0;
    addr_buf.Length = sizeof(ctx->remote_addr);

    if (!rio.RIOReceiveEx(rq, &data_buf, 1, nullptr, &addr_buf, nullptr, nullptr, 0, ctx)) {
        throw socket_exception(
            std::format("RIOReceiveEx failed: {}", get_last_error_message()));
    }
}

/**
 * @brief Post a RIO send operation.
 *
 * @param rio RIO function table.
 * @param rq Request queue.
 * @param ctx RIO context with data to send.
 * @param len Length of data to send.
 */
void post_rio_send(const RIO_EXTENSION_FUNCTION_TABLE& rio, RIO_RQ rq, rio_context* ctx, DWORD length) {
    ctx->operation = io_operation_type::send;

    RIO_BUF data_buf = {};
    data_buf.BufferId = ctx->buffer_id.get();
    data_buf.Offset = 0;
    data_buf.Length = length;

    RIO_BUF addr_buf = {};
    addr_buf.BufferId = ctx->addr_buffer_id.get();
    addr_buf.Offset = 0;
    addr_buf.Length = ctx->remote_addr_len;

    if (!rio.RIOSendEx(rq, &data_buf, 1, nullptr, &addr_buf, nullptr, nullptr, 0, ctx)) {
        throw socket_exception(
            std::format("RIOSendEx failed: {}", get_last_error_message()));
    }
}
