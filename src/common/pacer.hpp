/**
 * pacer.hpp
 *
 * Lightweight packet pacer for rate-limiting outgoing packets.
 *
 * @file pacer.hpp
 * @brief Packet pacer for rate-limiting outgoing packets.
 *
 * @copyright Copyright (c) 2025 LinuxUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 *
 */
#pragma once

#include <cstdint>
#include <algorithm>
#include <chrono>

/**
 * @brief Client send pacer using token bucket algorithm.
 * @note: Not thread-safe; caller must ensure synchronization if used from multiple threads.
 */
class client_send_pacer {
public:
    /**
     * @brief Construct a new client send pacer object.
     * 
     * @param[in] pps The target packet rate in packets per second (0 = unlimited).
     */
    explicit client_send_pacer(double pps)
        : rate_pps_(pps),
          unlimited_(pps == 0.0),
          // Allow a small burst window (fraction of a second) to tolerate
          // scheduling jitter. Default burst window = 0.005s (5 ms).
          capacity_((std::max)(1.0, pps * 0.005)),
                    tokens_(0.0),
          last_refill_ns_(now_ns()) {}

    ~client_send_pacer() = default;

    /**
     * @brief Query whether a packet can be sent now.
     *
     * This method obtains the current steady-clock timestamp internally and
     * checks the token bucket state.
     *
     * @return true A packet may be sent immediately.
     * @return false A packet must be delayed to satisfy the rate.
     */
    bool can_send() {
        if (unlimited_) return true;
        uint64_t now = now_ns();
        refill(now);
        return tokens_ >= 1.0 - 1e-12;
    }

    /**
     * @brief Record that a packet has been sent (uses current time internally).
     *
     * Decrements the token count; if called when tokens are unavailable, a
     * bounded negative debt is permitted to represent transient overshoot.
     */
    void record_send() {
        if (unlimited_) return;
        uint64_t now = now_ns();
        refill(now);
        // consume one token; do not allow negative debt
        tokens_ = (std::max)(0.0, tokens_ - 1.0);
    }

    /**
     * @brief Perform periodic polling to update internal state.
     */
    void poll() {
        if (unlimited_) return;
        uint64_t now = now_ns();
        refill(now);
    }

    /**
     * @brief Get the relative wait time (ns) from now until a packet can be sent.
     *
     * Returns 0 when a packet can be sent immediately. For limited rates this
     * returns the required delay in nanoseconds.
     *
     * @return uint64_t wait time in nanoseconds (0 == send now)
     */
    uint64_t get_next_send_time_ns() const {
        if (unlimited_) return 0;
        uint64_t now = now_ns();
        // compute tokens as if we refilled now (without mutating state)
        double tokens_at_now = tokens_;
        if (now > last_refill_ns_) {
            double delta_s = static_cast<double>(now - last_refill_ns_) / 1e9;
            tokens_at_now = (std::min)(capacity_, tokens_at_now + rate_pps_ * delta_s);
        }
        if (tokens_at_now >= 1.0) return 0;
        double deficit = 1.0 - tokens_at_now; // tokens needed
        double wait_s = deficit / rate_pps_;
        uint64_t wait_ns = static_cast<uint64_t>(wait_s * 1e9);
        return wait_ns;
    }

    /**
     * @brief Reset pacer timing to now, clearing any accumulated tokens.
     *
     * Use this to align pacer state to a synchronized start time so that
     * no tokens accumulate before measurement begins.
     */
    void reset_to_now() {
        if (unlimited_) return;
        uint64_t now = now_ns();
        tokens_ = 0.0;
        last_refill_ns_ = now;
    }

private:
    static uint64_t now_ns() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    void refill(uint64_t now_ns) {
        if (now_ns <= last_refill_ns_) return;
        double delta_s = static_cast<double>(now_ns - last_refill_ns_) / 1e9;
        double add = rate_pps_ * delta_s;
        tokens_ = (std::min)(capacity_, tokens_ + add);
        last_refill_ns_ = now_ns;
    }

    double rate_pps_{0.0};
    bool unlimited_{false};
    double capacity_{1.0};
    double tokens_{0.0};
    uint64_t last_refill_ns_{0};
};