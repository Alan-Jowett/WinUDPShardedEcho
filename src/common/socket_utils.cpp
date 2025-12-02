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
 * @throws socket_exception on failure to create the socket.
 * @return RAII `unique_socket` owning the created SOCKET.
 */
unique_socket create_udp_socket(int family) {
    int af = family;
    if (af != AF_INET && af != AF_INET6) {
        af = AF_INET;
    }
    SOCKET raw = WSASocketW(af, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
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
    // Validate processor_id is within the range of group 0
    DWORD group0_count = GetActiveProcessorCount(0);
    if (processor_id >= group0_count) {
        throw socket_exception(
            std::format("Processor ID {} is out of range for group 0 ({} processors)", processor_id,
                        group0_count));
    }

    GROUP_AFFINITY affinity = {};
    affinity.Group = 0;
    affinity.Mask = 1ULL << processor_id;

    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr)) {
        throw socket_exception(std::format("SetThreadGroupAffinity failed for processor {}: {}",
                                           processor_id, get_last_error_message()));
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
    std::memset(static_cast<OVERLAPPED*>(ctx), 0, sizeof(OVERLAPPED));

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
