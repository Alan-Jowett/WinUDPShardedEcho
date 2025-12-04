/**
 * @file socket_utils.hpp
 * @brief Cross-cutting Win32 socket utilities and IOCP helpers used by the demo.
 *
 * This header provides small helpers and types for creating UDP sockets,
 * posting overlapped operations and interacting with IO Completion Ports
 * (IOCP) on Windows. It also defines packet framing constants used by the
 * echo server/client.
 *
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off -- to keep the include order
#include <winsock2.h>
// clang-format on

#include <mswsock.h>
#include <wil/resource.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Packet header for tracking sequence numbers
#pragma pack(push, 1)
/**
 * @brief Packet framing header prepended to each UDP payload.
 *
 * The header includes a 64-bit sequence number and a 64-bit timestamp in
 * nanoseconds. It is packed to avoid padding between fields.
 */
struct packet_header {
    /// Monotonic sequence number assigned by sender.
    uint64_t sequence_number;
    /// Sender timestamp in nanoseconds when the packet was created.
    uint64_t timestamp_ns;
};
#pragma pack(pop)

/// Maximum UDP payload size (practical limit for IPv4/IPv6 datagrams).
constexpr size_t MAX_PACKET_SIZE = 65507;  // Max UDP payload size
/// Size of the packet header defined above.
constexpr size_t HEADER_SIZE = sizeof(packet_header);
/// Maximum application payload size after subtracting the header.
constexpr size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - HEADER_SIZE;

// Shared configuration constants
/// Number of simultaneous outstanding asynchronous I/O operations per socket.
constexpr size_t OUTSTANDING_OPS = 16;  // Number of outstanding I/O operations per socket
/// Timeout in milliseconds used when polling an IOCP for events.
constexpr DWORD IOCP_TIMEOUT_MS = 10;  // IOCP polling timeout in milliseconds
/// Timeout used specifically during shutdown checks on the IOCP.
constexpr DWORD IOCP_SHUTDOWN_TIMEOUT_MS = 1000;  // IOCP timeout for shutdown check

// RIO configuration constants
/// Number of outstanding RIO operations per socket.
constexpr size_t RIO_OUTSTANDING_OPS = 256;
/// RIO completion queue depth.
constexpr size_t RIO_CQ_SIZE = 2048;
/// RIO request queue depth.
constexpr size_t RIO_RQ_SIZE = 256;
/// Maximum number of results to dequeue from RIO CQ at once.
constexpr ULONG RIO_MAX_RESULTS = 64;
/// RIO polling timeout in milliseconds.
constexpr DWORD RIO_TIMEOUT_MS = 1000;

/**
 * @brief Operation type stored in the per-I/O context to indicate whether
 * the overlapped operation was a receive or a send.
 */
enum class io_operation_type { recv, send };

// Define RAII wrapper for SOCKET
using unique_socket = wil::unique_socket;

using unique_iocp = wil::unique_handle;

using unique_event = wil::unique_handle;

// RAII wrapper for RIO completion queue
class unique_rio_cq {
    RIO_CQ cq_;
    const RIO_EXTENSION_FUNCTION_TABLE* rio_;

   public:
    unique_rio_cq() noexcept : cq_(RIO_INVALID_CQ), rio_(nullptr) {}

    unique_rio_cq(RIO_CQ cq, const RIO_EXTENSION_FUNCTION_TABLE* rio) noexcept
        : cq_(cq), rio_(rio) {}

    ~unique_rio_cq() noexcept;

    unique_rio_cq(const unique_rio_cq&) = delete;
    unique_rio_cq& operator=(const unique_rio_cq&) = delete;

    unique_rio_cq(unique_rio_cq&& other) noexcept : cq_(other.cq_), rio_(other.rio_) {
        other.cq_ = RIO_INVALID_CQ;
        other.rio_ = nullptr;
    }

    unique_rio_cq& operator=(unique_rio_cq&& other) noexcept {
        if (this != &other) {
            reset();
            cq_ = other.cq_;
            rio_ = other.rio_;
            other.cq_ = RIO_INVALID_CQ;
            other.rio_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept;

    RIO_CQ get() const noexcept { return cq_; }
    RIO_CQ release() noexcept {
        RIO_CQ tmp = cq_;
        cq_ = RIO_INVALID_CQ;
        rio_ = nullptr;
        return tmp;
    }
    operator bool() const noexcept { return cq_ != RIO_INVALID_CQ; }
};

// RAII wrapper for RIO request queue (cleaned up automatically with completion queue)
class unique_rio_rq {
    RIO_RQ rq_;

   public:
    unique_rio_rq() noexcept : rq_(RIO_INVALID_RQ) {}
    explicit unique_rio_rq(RIO_RQ rq) noexcept : rq_(rq) {}

    ~unique_rio_rq() noexcept = default;

    unique_rio_rq(const unique_rio_rq&) = delete;
    unique_rio_rq& operator=(const unique_rio_rq&) = delete;

    unique_rio_rq(unique_rio_rq&& other) noexcept : rq_(other.rq_) { other.rq_ = RIO_INVALID_RQ; }

    unique_rio_rq& operator=(unique_rio_rq&& other) noexcept {
        if (this != &other) {
            rq_ = other.rq_;
            other.rq_ = RIO_INVALID_RQ;
        }
        return *this;
    }

    RIO_RQ get() const noexcept { return rq_; }
    RIO_RQ release() noexcept {
        RIO_RQ tmp = rq_;
        rq_ = RIO_INVALID_RQ;
        return tmp;
    }
    operator bool() const noexcept { return rq_ != RIO_INVALID_RQ; }
};

// RAII wrapper for RIO buffer IDs
class unique_rio_buffer {
    RIO_BUFFERID buffer_id_;
    const RIO_EXTENSION_FUNCTION_TABLE* rio_;

   public:
    unique_rio_buffer() noexcept : buffer_id_(RIO_INVALID_BUFFERID), rio_(nullptr) {}

    unique_rio_buffer(RIO_BUFFERID buffer_id, const RIO_EXTENSION_FUNCTION_TABLE* rio) noexcept
        : buffer_id_(buffer_id), rio_(rio) {}

    ~unique_rio_buffer() noexcept;

    unique_rio_buffer(const unique_rio_buffer&) = delete;
    unique_rio_buffer& operator=(const unique_rio_buffer&) = delete;

    unique_rio_buffer(unique_rio_buffer&& other) noexcept
        : buffer_id_(other.buffer_id_), rio_(other.rio_) {
        other.buffer_id_ = RIO_INVALID_BUFFERID;
        other.rio_ = nullptr;
    }

    unique_rio_buffer& operator=(unique_rio_buffer&& other) noexcept {
        if (this != &other) {
            reset();
            buffer_id_ = other.buffer_id_;
            rio_ = other.rio_;
            other.buffer_id_ = RIO_INVALID_BUFFERID;
            other.rio_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept;

    RIO_BUFFERID get() const noexcept { return buffer_id_; }
    RIO_BUFFERID release() noexcept {
        RIO_BUFFERID tmp = buffer_id_;
        buffer_id_ = RIO_INVALID_BUFFERID;
        rio_ = nullptr;
        return tmp;
    }
    operator bool() const noexcept { return buffer_id_ != RIO_INVALID_BUFFERID; }
};

/**
 * @brief Exception type thrown on socket-related failures.
 */
class socket_exception : public std::runtime_error {
   public:
    explicit socket_exception(const std::string& message) : std::runtime_error(message) {}
};

/**
 * @brief Overlapped context extended with additional metadata for IOCP.
 *
 * Instances of `io_context` are allocated per outstanding operation and
 * posted to Winsock APIs. They embed a `WSABUF` and backing buffer, the
 * remote peer address storage and an `io_operation_type` to disambiguate
 * completion handling.
 */
struct io_context : OVERLAPPED {
    /// Operation type (recv or send).
    io_operation_type operation;
    /// WSABUF pointing at the `buffer` storage.
    WSABUF wsa_buf;
    /// Backing storage for the packet (includes header + payload).
    std::vector<char> buffer;
    /// Storage for the remote peer address.
    sockaddr_storage remote_addr;
    /// Length of the remote address in bytes.
    int remote_addr_len;

    io_context()
        : OVERLAPPED{}, operation{io_operation_type::recv}, remote_addr_len{sizeof(remote_addr)} {
        buffer.resize(MAX_PACKET_SIZE);
        wsa_buf.buf = buffer.data();
        wsa_buf.len = static_cast<ULONG>(buffer.size());
        std::memset(&remote_addr, 0, sizeof(remote_addr));
    }
};

/**
 * @brief RIO-specific context for registered I/O operations.
 *
 * Instances track RIO buffer IDs, remote addresses, and operation metadata.
 */
struct rio_context {
    /// Operation type (recv or send).
    io_operation_type operation;
    /// Backing storage for the packet.
    std::vector<char> buffer;
    /// RIO buffer ID for the registered buffer (RAII wrapper).
    unique_rio_buffer buffer_id;
    /// Storage for the remote peer address.
    sockaddr_storage remote_addr;
    /// Length of the remote address in bytes.
    int remote_addr_len;
    /// RIO buffer ID for the remote address (RAII wrapper).
    unique_rio_buffer addr_buffer_id;

    rio_context()
        : operation{io_operation_type::recv},
          buffer_id{},
          remote_addr_len{sizeof(remote_addr)},
          addr_buffer_id{} {
        buffer.resize(MAX_PACKET_SIZE);
        std::memset(&remote_addr, 0, sizeof(remote_addr));
    }
};

/**
 * @name Winsock lifecycle
 * Helper functions to initialize and cleanup the Winsock stack.
 */
//@{
/// Initialize Winsock. Must be called before socket operations.
void initialize_winsock();

/// Cleanup Winsock. Call during shutdown to release resources.
void cleanup_winsock();
//@}

/**
 * @brief Create a UDP socket for the specified address family.
 *
 * @param family Address family (AF_INET or AF_INET6). Defaults to AF_INET.
 * @param use_rio If true, creates a socket suitable for RIO (Registered I/O) operations.
 * @return A RAII `unique_socket` owning the created SOCKET.
 */
unique_socket create_udp_socket(int family = AF_INET, bool use_rio = false);

/**
 * @brief Set the CPU affinity for a socket (Windows SIO_CPU_AFFINITY).
 *
 * @param sock The socket to configure.
 * @param processor_id Logical processor index to bind the socket to.
 */
void set_socket_cpu_affinity(const unique_socket& sock, uint16_t processor_id);

/**
 * @brief Create an unassociated IO Completion Port (IOCP).
 * @return A `unique_iocp` handle for the created IOCP.
 */
unique_iocp create_iocp();

/**
 * @brief Create an IOCP and associate a socket with it.
 *
 * Convenience helper that creates an IOCP and associates `sock` so the
 * application can begin posting asynchronous operations and receive
 * completions via the IOCP.
 */
unique_iocp create_iocp_and_associate(const unique_socket& sock);

/**
 * @brief Associate an existing socket with an existing IOCP.
 *
 * @param sock Socket to associate.
 * @param iocp IOCP handle to associate with.
 * @param completion_key Completion key (typically used to identify socket/thread).
 */
void associate_socket_with_iocp(const unique_socket& sock, unique_iocp& iocp,
                                ULONG_PTR completion_key);

/**
 * @brief Wrapper around SetFileCompletionNotificationModes that throws on failure.
 *
 * @param handle File handle to set completion notification modes on.
 * @param flags Flags passed to SetFileCompletionNotificationModes (default: skip
 *              completion port on success).
 * @throws socket_exception on failure.
 */
void set_file_completion_notification_modes(HANDLE handle,
                                            UCHAR flags = FILE_SKIP_SET_EVENT_ON_HANDLE);

/**
 * @brief Set the current thread's processor affinity.
 *
 * This is used by worker threads that should be pinned to a specific CPU.
 */
void set_thread_affinity(uint32_t processor_id);

/**
 * @brief Query the number of logical processors available on the system.
 */
uint32_t get_processor_count();

/**
 * @brief Bind a UDP socket to the given port and address family.
 *
 * Throws `socket_exception` on failure.
 */
void bind_socket(const unique_socket& sock, uint16_t port, int family = AF_INET);

/**
 * @brief Helper around `setsockopt` that throws `socket_exception` on error.
 */
void set_socket_option(const unique_socket& sock, int level, int optname, const char* optval,
                       int optlen);

/**
 * @brief Post an asynchronous receive (WSARecvFrom) using the provided context.
 */
void post_recv(const unique_socket& sock, io_context* ctx);

/**
 * @brief Post an asynchronous send (WSASendTo) using the provided context.
 *
 * The `data` pointer is copied into the `ctx->buffer` prior to posting.
 */
void post_send(const unique_socket& sock, io_context* ctx, const char* data, size_t len,
               const sockaddr* dest_addr, int dest_addr_len);

/**
 * @brief Synchronously send a UDP datagram using `sendto`.
 *
 * Returns number of bytes sent on success, or throws socket_exception on error.
 */
int send_sync(const unique_socket& sock, const char* data, size_t len, const sockaddr* dest_addr,
              int dest_addr_len);

/**
 * @brief Return a monotonic timestamp in nanoseconds.
 */
uint64_t get_timestamp_ns();

/**
 * @brief Return the local socket address (sockname) for a socket.
 *
 * @return Pair of `sockaddr_storage` and length; throws on error.
 */
std::pair<sockaddr_storage, int> get_socket_name(const unique_socket& sock);

/**
 * @brief Format the last Win32 socket error into a human-readable string.
 */
std::string get_last_error_message();

/**
 * @name Registered I/O (RIO) Functions
 * Helper functions for working with Windows Registered I/O.
 */
//@{

/**
 * @brief Load RIO function table from Winsock.
 *
 * @param sock A valid socket used to retrieve the extension functions.
 * @return RIO_EXTENSION_FUNCTION_TABLE containing RIO function pointers.
 * @throws socket_exception on failure.
 */
RIO_EXTENSION_FUNCTION_TABLE load_rio_function_table(const unique_socket& sock);

/**
 * @brief Create a RIO completion queue.
 *
 * @param rio RIO function table.
 * @param queue_size Size of the completion queue.
 * @param notification_event Event handle for notification (can be nullptr).
 * @return unique_rio_cq RAII wrapper.
 * @throws socket_exception on failure.
 */
unique_rio_cq create_rio_completion_queue(const RIO_EXTENSION_FUNCTION_TABLE& rio, DWORD queue_size,
                                          unique_event& notification_event);

/**
 * @brief Create a RIO request queue for a socket.
 *
 * @param rio RIO function table.
 * @param sock Socket to create the request queue for.
 * @param completion_queue Completion queue to associate with the request queue.
 * @param max_outstanding_recv Maximum outstanding receive operations.
 * @param max_outstanding_send Maximum outstanding send operations.
 * @return unique_rio_rq RAII wrapper.
 * @throws socket_exception on failure.
 */
unique_rio_rq create_rio_request_queue(const RIO_EXTENSION_FUNCTION_TABLE& rio,
                                       const unique_socket& sock,
                                       const unique_rio_cq& completion_queue,
                                       DWORD max_outstanding_recv, DWORD max_outstanding_send);

/**
 * @brief Register a buffer for use with RIO.
 *
 * @param rio RIO function table.
 * @param buffer Pointer to the buffer to register.
 * @param size Size of the buffer in bytes.
 * @return unique_rio_buffer RAII wrapper.
 * @throws socket_exception on failure.
 */
unique_rio_buffer register_rio_buffer(const RIO_EXTENSION_FUNCTION_TABLE& rio, void* buffer,
                                      DWORD size);

/**
 * @brief Post a RIO receive operation.
 *
 * @param rio RIO function table.
 * @param rq Request queue.
 * @param ctx RIO context with registered buffers.
 */
void post_rio_recv(const RIO_EXTENSION_FUNCTION_TABLE& rio, RIO_RQ rq, rio_context* ctx);

/**
 * @brief Post multiple RIO receive operations in batch.
 *
 * Submits multiple receive operations to the RIO request queue in a single call
 * by building RIO_BUF arrays and passing them to RIOReceiveEx. This is more
 * efficient than posting operations individually.
 *
 * Example usage:
 * @code
 * std::vector<rio_context*> recv_contexts = {...};
 * post_rio_recv(rio_table, request_queue, recv_contexts);
 * @endcode
 *
 * @param rio RIO function table.
 * @param rq Request queue.
 * @param contexts Vector of RIO contexts to post receives for.
 * @throws socket_exception if the batch operation fails.
 */
void post_rio_recv(const RIO_EXTENSION_FUNCTION_TABLE& rio, RIO_RQ rq,
                   const std::vector<rio_context*>& contexts);

/**
 * @brief Post a RIO send operation.
 *
 * @param rio RIO function table.
 * @param rq Request queue.
 * @param ctx RIO context with data to send.
 * @param len Length of data to send.
 */
void post_rio_send(const RIO_EXTENSION_FUNCTION_TABLE& rio, RIO_RQ rq, rio_context* ctx,
                   DWORD length);

/**
 * @brief Post multiple RIO send operations in batch.
 *
 * Submits multiple send operations to the RIO request queue in a single call
 * by building RIO_BUF arrays and passing them to RIOSendEx. This is more
 * efficient than posting operations individually.
 *
 * Example usage:
 * @code
 * std::vector<std::pair<rio_context*, DWORD>> send_ops = {
 *     {ctx1, 100},  // Send 100 bytes from ctx1
 *     {ctx2, 256},  // Send 256 bytes from ctx2
 * };
 * post_rio_send(rio_table, request_queue, send_ops);
 * @endcode
 *
 * @param rio RIO function table.
 * @param rq Request queue.
 * @param send_data Vector of pairs containing rio_context pointer and data length.
 * @throws socket_exception if the batch operation fails.
 */
void post_rio_send(const RIO_EXTENSION_FUNCTION_TABLE& rio, RIO_RQ rq,
                   const std::vector<std::pair<rio_context*, DWORD>>& send_data);

//@}
