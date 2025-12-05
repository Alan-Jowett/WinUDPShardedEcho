/**
 * @file null_cc.hpp
 * @brief Null congestion controller (no control).
 *
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

/**
 * @class null_congestion_controller
 * @brief A no-op congestion controller used for testing or disabling pacing.
 *
 * This controller implements the required congestion controller concept but
 * performs no rate estimation or state updates. It is useful as a placeholder
 * when congestion control is intentionally disabled or for unit tests.
 */
class null_congestion_controller {
   public:
    /**
     * @brief Default constructor.
     */
    null_congestion_controller() = default;

    /**
     * @brief No-op: accept an initial rate but ignore it.
     *
     * @param pps Initial packets-per-second (ignored).
     */
    void set_initial_rate(double) {}

    /**
     * @brief No-op hook for packet send events.
     *
     * @param now_ns Timestamp of send in nanoseconds (ignored).
     * @param seq Sequence number of the sent packet (ignored).
     */
    void on_send(uint64_t, uint64_t) {}

    /**
     * @brief No-op hook for ACK events.
     *
     * @param now_ns ACK receive time in nanoseconds (ignored).
     * @param seq Sequence number acknowledged (ignored).
     * @param rtt_ns Measured RTT in nanoseconds (ignored).
     */
    void on_ack(uint64_t, uint64_t, uint64_t) {}

    /**
     * @brief No-op periodic poll hook.
     *
     * @param now_ns Current time in nanoseconds (ignored).
     */
    void on_poll(uint64_t) {}

    /**
     * @brief Returns the controller's target sending rate (pps).
     *
     * For the null controller this is always 0.0 which indicates no pacing
     * guidance to the caller.
     *
     * @returns Target packets-per-second (always 0.0).
     */
    double target_rate_pps() const { return 0.0; }
};

static_assert(CongestionControllerConcept<null_congestion_controller>,
              "null_congestion_controller does not meet "
              "CongestionControllerConcept requirements");