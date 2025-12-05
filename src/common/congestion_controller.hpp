/**
 * @file congetion_controller.hpp
 * @brief Congestion controller concept definition for client pacers.
 *
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <concepts>
#include <cstdint>

/**
 * @brief A congestion controller concept used by client_send_pacer.
 *
 * @tparam T The congestion controller type to check.
 */
template <typename T>
concept CongestionControllerConcept =
    requires(T cc, double pps, uint64_t now_ns, uint64_t seq, uint64_t rtt_ns) {
        { cc.set_initial_rate(pps) } -> std::same_as<void>;
        { cc.on_send(now_ns, seq) } -> std::same_as<void>;
        { cc.on_ack(now_ns, seq, rtt_ns) } -> std::same_as<void>;
        { cc.on_poll(now_ns) } -> std::same_as<void>;
        { cc.target_rate_pps() } -> std::same_as<double>;
    };
