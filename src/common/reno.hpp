/**
 * reno.hpp
 *
 * Simple Reno-like congestion controller (window-based) for experiments.
 *
 * @file reno.hpp
 * @brief Reno-like congestion controller
 *
 * @copyright Copyright (c) 2025 LinuxUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>

#include "congestion_controller.hpp"

#undef max
#undef min

/**
 * @class reno_congestion_controller
 * @brief Simple Reno-like window-based congestion controller for experiments.
 *
 * This implementation provides a minimal Reno-style controller that tracks
 * a congestion window (`cwnd_packets_`) and adapts it on ACKs and RTT
 * inflation. It exposes a pacing rate in packets-per-second derived from the
 * congestion window and the observed minimum RTT. The controller is
 * intentionally lightweight and aimed at experimentation rather than
 * production use.
 */
class reno_congestion_controller {
   public:
    /**
     * @brief Default constructor.
     */
    reno_congestion_controller() = default;

    /**
     * @brief Configure an initial target rate (packets-per-second).
     *
     * If `pps` is <= 0 the controller is placed into an `unlimited_` mode and
     * will not perform window-based control. Otherwise the controller seeds
     * its pacing rate and initial window from the provided value.
     *
     * @param pps Initial requested packets-per-second (<=0 disables control).
     */
    void set_initial_rate(double pps) {
        if (pps <= 0.0) {
            unlimited_ = true;
            return;
        }
        unlimited_ = false;
        // initialize min RTT unknown; cwnd start small
        cwnd_packets_ = 10.0;  // start with modest cwnd
        ssthresh_ = 1000.0;
        // store initial request rate and seed pacing rate
        initial_rate_pps_ = pps;
        pacing_rate_pps_ = pps;
        // approximate initial window -> target pps when min RTT known
        // leave ewma and min_rtt unset until acks arrive
    }

    /**
     * @brief Record a send event for sequence tracking.
     *
     * The send timestamp is recorded so that late ACKs and RTTs can be
     * correlated with send events. Ignored when `unlimited_` is true.
     *
     * @param now_ns Send time in nanoseconds.
     * @param seq Sequence number of the sent packet.
     */
    void on_send(uint64_t now_ns, uint64_t seq) {
        if (unlimited_) return;
        send_times_[seq] = now_ns;
    }

    /**
     * @brief Process an ACK: update RTT, cwnd and pacing rate.
     *
     * Updates the observed minimum RTT, removes send-tracking state for the
     * acknowledged packet, and performs Reno slow-start or congestion
     * avoidance updates to the congestion window. The pacing rate is then
     * recomputed from the window and minimum RTT.
     *
     * @param now_ns ACK receive time in nanoseconds.
     * @param seq Sequence number acknowledged.
     * @param rtt_ns RTT sample in nanoseconds for this ACK (0 if unknown).
     */
    void on_ack(uint64_t now_ns, uint64_t seq, uint64_t rtt_ns) {
        if (unlimited_) return;

        // update min RTT observed and remember last RTT
        if (rtt_ns > 0) {
            if (min_rtt_ns_ == 0 || rtt_ns < min_rtt_ns_) min_rtt_ns_ = rtt_ns;
            last_rtt_ns_ = rtt_ns;
        }

        // Remove send tracking
        auto it = send_times_.find(seq);
        if (it != send_times_.end()) send_times_.erase(it);

        // Count ack and adjust cwnd according to Reno rules
        if (cwnd_packets_ < ssthresh_) {
            // slow start: increase by 1 packet per ACK (approx)
            cwnd_packets_ += 1.0;
        } else {
            // congestion avoidance: increase by ~1 packet per RTT -> 1/cwnd per ACK
            cwnd_packets_ += 1.0 / std::max(1.0, cwnd_packets_);
        }

        // clamp cwnd
        if (cwnd_packets_ < 1.0) cwnd_packets_ = 1.0;
        if (cwnd_packets_ > 1e6) cwnd_packets_ = 1e6;

        update_target_rate();
    }

    /**
     * @brief Periodic maintenance hook.
     *
     * Detects RTT inflation (a sign of congestion) and applies a
     * multiplicative decrease to the congestion window and recomputes the
     * pacing rate when necessary. Ignored when `unlimited_` is true.
     *
     * @param now_ns Current time in nanoseconds.
     */
    void on_poll(uint64_t now_ns) {
        if (unlimited_) return;
        // detect RTT inflation: if last rtt sample > min_rtt * threshold, treat as congestion
        if (last_rtt_ns_ > 0 && min_rtt_ns_ > 0) {
            if (last_rtt_ns_ > min_rtt_ns_ * rtt_inflation_threshold_) {
                // multiplicative decrease
                ssthresh_ = std::max(2.0, cwnd_packets_ / 2.0);
                cwnd_packets_ = ssthresh_;
                update_target_rate();
            }
        }
        // no-op otherwise
    }

    /**
     * @brief Return the controller's target sending rate (packets-per-second).
     *
     * When `unlimited_` is true returns 0.0 to indicate no pacing guidance.
     */
    double target_rate_pps() const {
        if (unlimited_) return 0.0;
        return pacing_rate_pps_;
    }

   private:
    /**
     * @brief Recompute the pacing rate from `cwnd_packets_` and `min_rtt_ns_`.
     *
     * If `min_rtt_ns_` is unknown the pacing rate is left unchanged.
     */
    void update_target_rate() {
        if (min_rtt_ns_ == 0) {
            // keep existing pacing rate if we don't yet have a min RTT
            return;
        }
        double min_rtt_s = static_cast<double>(min_rtt_ns_) / 1e9;
        if (min_rtt_s <= 0.0) return;
        pacing_rate_pps_ = cwnd_packets_ / min_rtt_s;
        // Clamp to avoid runaway: limit to a multiplier of initial requested rate
        const double max_mult = 1.0;  // do not exceed initial requested rate
        if (initial_rate_pps_ > 0.0) {
            double cap = initial_rate_pps_ * max_mult;
            if (pacing_rate_pps_ > cap) pacing_rate_pps_ = cap;
        }
        if (pacing_rate_pps_ < 1.0) pacing_rate_pps_ = 1.0;
    }

    /**
     * @brief Whether the controller is in unlimited (disabled) mode.
     */
    bool unlimited_{false};
    /**
     * @brief Most recent RTT sample (nanoseconds).
     */
    uint64_t last_rtt_ns_{0};
    /**
     * @brief Minimum observed RTT (nanoseconds), used to compute pacing.
     */
    uint64_t min_rtt_ns_{0};
    /**
     * @brief Congestion window in packets.
     */
    double cwnd_packets_{10.0};
    /**
     * @brief Slow-start threshold (packets).
     */
    double ssthresh_{1000.0};
    /**
     * @brief Computed pacing rate in packets-per-second.
     */
    double pacing_rate_pps_{0.0};
    /**
     * @brief Map of outstanding send times by sequence number.
     */
    std::unordered_map<uint64_t, uint64_t> send_times_;
    /**
     * @brief Initial requested rate (pps) provided via `set_initial_rate`.
     */
    double initial_rate_pps_{0.0};

    /**
     * @brief Threshold multiplier for RTT inflation detection.
     */
    const double rtt_inflation_threshold_{1.6};
};

static_assert(CongestionControllerConcept<reno_congestion_controller>,
              "reno_congestion_controller does not meet "
              "CongestionControllerConcept requirements");