// Copyright (c) 2025 Alan Jowett
// SPDX-License-Identifier: MIT

#include "socket_utils.hpp"

#pragma comment(lib, "ws2_32.lib")

// SOCKET_PROCESSOR_AFFINITY may not be defined in older SDKs
#ifndef SIO_CPU_AFFINITY
#define SIO_CPU_AFFINITY _WSAIOW(IOC_VENDOR, 21)
#endif

bool initialize_winsock() {
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        std::cerr << std::format("WSAStartup failed: {}\n", result);
        return false;
    }
    return true;
}

void cleanup_winsock() {
    WSACleanup();
}

SOCKET create_udp_socket(int family) {
    int af = family;
    if (af != AF_INET && af != AF_INET6) {
        af = AF_INET;
    }
    SOCKET sock = WSASocketW(af, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (sock == INVALID_SOCKET) {
        std::cerr << std::format("WSASocket failed: {}\n", get_last_error_message());
        return INVALID_SOCKET;
    }
    return sock;
}

bool set_socket_cpu_affinity(SOCKET sock, uint16_t processor_id) {
    // SIO_CPU_AFFINITY sets the processor affinity for the socket

    DWORD bytes_returned = 0;
    int result = WSAIoctl(
        sock,
        SIO_CPU_AFFINITY,
        &processor_id,
        sizeof(processor_id),
        nullptr,
        0,
        &bytes_returned,
        nullptr,
        nullptr
    );

    if (result == SOCKET_ERROR) {
        std::cerr << std::format("SIO_CPU_AFFINITY failed for processor {}: {}\n", 
                                  processor_id, get_last_error_message());
        return false;
    }
    return true;
}

HANDLE create_iocp_and_associate(SOCKET sock) {
    HANDLE iocp = CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), nullptr, 0, 1);
    if (iocp == nullptr) {
        std::cerr << std::format("CreateIoCompletionPort failed: {}\n", get_last_error_message());
        return nullptr;
    }
    return iocp;
}

bool associate_socket_with_iocp(SOCKET sock, HANDLE iocp, ULONG_PTR completion_key) {
    HANDLE result = CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), iocp, completion_key, 0);
    if (result == nullptr) {
        std::cerr << std::format("CreateIoCompletionPort (associate) failed: {}\n", get_last_error_message());
        return false;
    }
    return true;
}

bool set_thread_affinity(uint32_t processor_id) {
    // Validate processor_id is within the range of group 0
    DWORD group0_count = GetActiveProcessorCount(0);
    if (processor_id >= group0_count) {
        std::cerr << std::format("Processor ID {} is out of range for group 0 ({} processors)\n", processor_id, group0_count);
        return false;
    }

    GROUP_AFFINITY affinity = {};
    affinity.Group = 0;
    affinity.Mask = 1ULL << processor_id;

    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr)) {
        std::cerr << std::format("SetThreadGroupAffinity failed for processor {}: {}\n", 
                                  processor_id, get_last_error_message());
        return false;
    }
    return true;
}

uint32_t get_processor_count() {
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    return sys_info.dwNumberOfProcessors;
}

bool bind_socket(SOCKET sock, uint16_t port) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << std::format("bind failed: {}\n", get_last_error_message());
        return false;
    }
    return true;
}

bool post_recv(SOCKET sock, io_context* ctx) {
    ctx->operation = io_operation_type::recv;
    ctx->wsa_buf.buf = ctx->buffer.data();
    ctx->wsa_buf.len = static_cast<ULONG>(ctx->buffer.size());
    ctx->remote_addr_len = sizeof(ctx->remote_addr);
    
    // Reset OVERLAPPED structure
    std::memset(static_cast<OVERLAPPED*>(ctx), 0, sizeof(OVERLAPPED));

    DWORD flags = 0;
    DWORD bytes_received = 0;
    
    int result = WSARecvFrom(
        sock,
        &ctx->wsa_buf,
        1,
        &bytes_received,
        &flags,
        reinterpret_cast<sockaddr*>(&ctx->remote_addr),
        &ctx->remote_addr_len,
        ctx,
        nullptr
    );

    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            std::cerr << std::format("WSARecvFrom failed: {} ({})\n", get_last_error_message(), error);
            return false;
        }
    }
    return true;
}

bool post_send(SOCKET sock, io_context* ctx, const char* data, size_t len, 
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
    
    int result = WSASendTo(
        sock,
        &ctx->wsa_buf,
        1,
        &bytes_sent,
        0,
        dest_addr,
        dest_addr_len,
        ctx,
        nullptr
    );

    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            std::cerr << std::format("WSASendTo failed: {} ({})\n", get_last_error_message(), error);
            return false;
        }
    }
    return true;
}

uint64_t get_timestamp_ns() {
    static LARGE_INTEGER frequency = {};
    static std::once_flag freq_once;
    std::call_once(freq_once, [&](){
        QueryPerformanceFrequency(&frequency);
    });

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    // Convert to nanoseconds
    return static_cast<uint64_t>(counter.QuadPart * 1000000000ULL / frequency.QuadPart);
}

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
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr
    );

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
