// Copyright (c) 2025 Alan Jowett
// SPDX-License-Identifier: MIT

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <iostream>
#include <format>

// Packet header for tracking sequence numbers
#pragma pack(push, 1)
struct packet_header {
    uint64_t sequence_number;
    uint64_t timestamp_ns;
};
#pragma pack(pop)

constexpr size_t MAX_PACKET_SIZE = 65507; // Max UDP payload size
constexpr size_t HEADER_SIZE = sizeof(packet_header);
constexpr size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - HEADER_SIZE;

// Shared configuration constants
constexpr size_t OUTSTANDING_OPS = 16;        // Number of outstanding I/O operations per socket
constexpr DWORD IOCP_TIMEOUT_MS = 10;         // IOCP polling timeout in milliseconds
constexpr DWORD IOCP_SHUTDOWN_TIMEOUT_MS = 1000; // IOCP timeout for shutdown check

// Operation types for IOCP
enum class io_operation_type {
    recv,
    send
};

// Overlapped structure with additional context for IOCP
struct io_context : OVERLAPPED {
    io_operation_type operation;
    WSABUF wsa_buf;
    std::vector<char> buffer;
    sockaddr_storage remote_addr;
    int remote_addr_len;
    
    io_context() : OVERLAPPED{}, operation{io_operation_type::recv}, remote_addr_len{sizeof(remote_addr)} {
        buffer.resize(MAX_PACKET_SIZE);
        wsa_buf.buf = buffer.data();
        wsa_buf.len = static_cast<ULONG>(buffer.size());
        std::memset(&remote_addr, 0, sizeof(remote_addr));
    }
};

// Initialize Winsock
bool initialize_winsock();

// Cleanup Winsock
void cleanup_winsock();

// Create a UDP socket
SOCKET create_udp_socket();

// Set socket CPU affinity (Windows-specific SIO_CPU_AFFINITY)
bool set_socket_cpu_affinity(SOCKET sock, uint16_t processor_id);

// Create IO Completion Port and associate socket
HANDLE create_iocp_and_associate(SOCKET sock);

// Associate socket with existing IOCP
bool associate_socket_with_iocp(SOCKET sock, HANDLE iocp, ULONG_PTR completion_key);

// Set thread CPU affinity
bool set_thread_affinity(uint32_t processor_id);

// Get number of processors
uint32_t get_processor_count();

// Bind socket to address and port
bool bind_socket(SOCKET sock, uint16_t port);

// Post an async receive operation
bool post_recv(SOCKET sock, io_context* ctx);

// Post an async send operation
bool post_send(SOCKET sock, io_context* ctx, const char* data, size_t len, 
               const sockaddr* dest_addr, int dest_addr_len);

// Get current timestamp in nanoseconds
uint64_t get_timestamp_ns();

// Format error message
std::string get_last_error_message();
