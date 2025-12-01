// Copyright (c) 2025 scalable_echo_server_demo Contributors
// SPDX-License-Identifier: MIT

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
struct packet_header {
  uint64_t sequence_number;
  uint64_t timestamp_ns;
};
#pragma pack(pop)

constexpr size_t MAX_PACKET_SIZE = 65507;  // Max UDP payload size
constexpr size_t HEADER_SIZE = sizeof(packet_header);
constexpr size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - HEADER_SIZE;

// Shared configuration constants
constexpr size_t OUTSTANDING_OPS = 16;            // Number of outstanding I/O operations per socket
constexpr DWORD IOCP_TIMEOUT_MS = 10;             // IOCP polling timeout in milliseconds
constexpr DWORD IOCP_SHUTDOWN_TIMEOUT_MS = 1000;  // IOCP timeout for shutdown check

// Operation types for IOCP
enum class io_operation_type { recv, send };

// Define RAII wrapper for SOCKET
using unique_socket = wil::unique_socket;

using unique_iocp = wil::unique_handle;

// Define a custom exception for socket errors
class socket_exception : public std::runtime_error {
 public:
  explicit socket_exception(const std::string& message) : std::runtime_error(message) {}
};

// Overlapped structure with additional context for IOCP
struct io_context : OVERLAPPED {
  io_operation_type operation;
  WSABUF wsa_buf;
  std::vector<char> buffer;
  sockaddr_storage remote_addr;
  int remote_addr_len;

  io_context()
      : OVERLAPPED{}, operation{io_operation_type::recv}, remote_addr_len{sizeof(remote_addr)} {
    buffer.resize(MAX_PACKET_SIZE);
    wsa_buf.buf = buffer.data();
    wsa_buf.len = static_cast<ULONG>(buffer.size());
    std::memset(&remote_addr, 0, sizeof(remote_addr));
  }
};

// Initialize Winsock
void initialize_winsock();

// Cleanup Winsock
void cleanup_winsock();

// Create a UDP socket (pass AF_INET or AF_INET6). Default is AF_INET.
unique_socket create_udp_socket(int family = AF_INET);

// Set socket CPU affinity (Windows-specific SIO_CPU_AFFINITY)
void set_socket_cpu_affinity(const unique_socket& sock, uint16_t processor_id);

// Create an unassociated IOCP
unique_iocp create_iocp();

// Create IO Completion Port and associate socket
unique_iocp create_iocp_and_associate(const unique_socket& sock);

// Associate socket with existing IOCP
void associate_socket_with_iocp(const unique_socket& sock, unique_iocp& iocp,
                                ULONG_PTR completion_key);

// Set thread CPU affinity
void set_thread_affinity(uint32_t processor_id);

// Get number of processors
uint32_t get_processor_count();

// Bind socket to address and port (specify family: AF_INET or AF_INET6)
void bind_socket(const unique_socket& sock, uint16_t port, int family = AF_INET);

// Helper that calls setsockopt on `sock` and throws `socket_exception` on failure
void set_socket_option(const unique_socket& sock, int level, int optname, const char* optval,
                       int optlen);

// Post an async receive operation
void post_recv(const unique_socket& sock, io_context* ctx);

// Post an async send operation
void post_send(const unique_socket& sock, io_context* ctx, const char* data, size_t len,
               const sockaddr* dest_addr, int dest_addr_len);

// Get current timestamp in nanoseconds
uint64_t get_timestamp_ns();

// Get local socket name (throws on failure)
std::pair<sockaddr_storage, int> get_socket_name(const unique_socket& sock);

// Format error message
std::string get_last_error_message();
