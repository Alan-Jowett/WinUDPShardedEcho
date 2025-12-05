
/**
 * @file bbr.hpp
 * @brief Simple header-only BBR-like congestion controller (lightweight, approximate)
 *
 * This file defines a basic BBR-like congestion controller that can be plugged
 * into the client pacer. It estimates bandwidth and RTT to adjust the sending rate.
 * The implementation is simplified for demonstration purposes and may not
 * include all features of a full BBR implementation.
 *
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>

#include "congestion_controller.hpp"

/**
 * @class bbr_congestion_controller
 * @brief Lightweight, header-only BBR-like congestion controller.
 *
 * This class provides a simplified BBR-inspired rate estimator intended for
 * use by the client pacer. It maintains a short history of sent sequence
 * numbers and ACK timestamps to estimate delivered bandwidth (packets per
 * second) and a smoothed RTT estimate. The controller exposes a target
 * sending rate in packets-per-second which callers should respect when
 * scheduling sends.
 */
class bbr_congestion_controller {
   public:
    /**
     * @brief Default-construct a controller with default parameters.
     *
     * The controller starts with a conservative target rate (1000 pps by
     * default in the atomic `target_rate_`). Call `set_initial_rate` to
     * constrain the controller to a target ceiling derived from the caller's
     * configured rate.
     */
    bbr_congestion_controller() = default;

    /**
     * @brief Set an initial upper-bound rate for the controller.
     *
     * This method records the externally requested initial rate (in
     * packets-per-second) and updates the internal target and bandwidth
     * estimates to match that value. The controller will not increase its
     * computed target above this initial value.
     *
     * @param pps Initial packets-per-second rate to use as an upper bound.
     */
    void set_initial_rate(double pps) {
        initial_rate_pps_ = pps;
        target_rate_.store(pps);
        bandwidth_pps_ = pps;
    }

    /**
     * @brief Notify the controller that a packet was sent.
     *
     * The controller records the sequence number and send timestamp so that
     * delivered rate can later be estimated when ACKs arrive. Implementations
     * should call this for each transmitted packet that they expect to be
     * ACKed.
     *
     * @param now_ns Current time in nanoseconds.
     * @param seq Packet sequence number associated with the send.
     */
    void on_send(uint64_t now_ns, uint64_t seq) {
        sent_history_.emplace_back(seq, now_ns);
        while (sent_history_.size() > max_history_) sent_history_.pop_front();
    }

    /**
     * @brief Handle an incoming ACK and update bandwidth / RTT estimates.
     *
     * Updates a smoothed RTT estimate, appends the ACK timestamp into a
     * sliding window used to compute delivered packets-per-second, and
     * computes a new target sending rate. The target is smoothed and never
     * exceeds the value set by `set_initial_rate`.
     *
     * @param now_ns Current time in nanoseconds (ACK receive time).
     * @param seq Sequence number acknowledged by this ACK.
     * @param rtt_ns Measured RTT for the acknowledged packet, in
     *               nanoseconds.
     */
    void on_ack(uint64_t now_ns, uint64_t seq, uint64_t rtt_ns) {
        if (rtt_est_ns_ == 0)
            rtt_est_ns_ = rtt_ns;
        else
            rtt_est_ns_ = static_cast<uint64_t>(rtt_est_ns_ * 0.875 + rtt_ns * 0.125);

        // Add this ACK timestamp to sliding window and evict old samples
        ack_history_.push_back(now_ns);
        const uint64_t window_ns = ack_window_ns_;
        while (!ack_history_.empty() && ack_history_.front() + window_ns < now_ns)
            ack_history_.pop_front();

        // Compute sample delivered packets/sec over the window
        double sample_pps = 0.0;
        if (!ack_history_.empty()) {
            double window_s =
                static_cast<double>(std::max<uint64_t>(1, now_ns - ack_history_.front())) * 1e-9;
            if (window_s > 0.0) sample_pps = static_cast<double>(ack_history_.size()) / window_s;
        }

        if (bandwidth_pps_ == 0.0)
            bandwidth_pps_ = sample_pps;
        else
            bandwidth_pps_ = bandwidth_pps_ * 0.9 + sample_pps * 0.1;

        // Compute new target but never exceed the initially requested rate
        double new_target = std::max(bandwidth_pps_ * pacing_gain_, min_pps_);
        if (initial_rate_pps_ > 0.0) new_target = std::min(new_target, initial_rate_pps_);
        target_rate_.store(new_target);
    }

    /**
     * @brief Periodic poll hook called by the pacer/driver.
     *
     * This allows the controller to apply time-based decay to its bandwidth
     * estimate when no recent ACKs have been seen. Callers should call this
     * with a regularly advancing time value (nanoseconds) from their main
     * loop.
     *
     * @param now_ns Current time in nanoseconds.
     */
    void on_poll(uint64_t now_ns) {
        if (last_poll_ns_ != 0 && now_ns > last_poll_ns_) {
            uint64_t delta_ns = now_ns - last_poll_ns_;
            if (delta_ns > decay_interval_ns_ && bandwidth_pps_ > 0.0) {
                bandwidth_pps_ *= 0.95;
                double new_target = std::max(bandwidth_pps_ * pacing_gain_, min_pps_);
                target_rate_.store(new_target);
            }
        }
        last_poll_ns_ = now_ns;
    }

    /**
     * @brief Get the current target sending rate in packets-per-second.
     *
     * This returns the controller's smoothed target rate which callers should
     * respect when scheduling packet transmissions.
     *
     * @returns Target packets-per-second.
     */
    double target_rate_pps() const { return target_rate_.load(); }

   private:
    /**
     * @brief Atomic target rate (packets per second) exposed to callers.
     */
    std::atomic<double> target_rate_{1000.0};

    /**
     * @brief Smoothed estimated delivered bandwidth in packets-per-second.
     */
    double bandwidth_pps_ = 0.0;

    /**
     * @brief Smoothed RTT estimate in nanoseconds.
     */
    uint64_t rtt_est_ns_ = 0;

    /**
     * @brief Timestamp of the last `on_poll` call (nanoseconds).
     */
    uint64_t last_poll_ns_ = 0;

    /**
     * @brief Multiplicative pacing gain applied to the bandwidth estimate.
     */
    const double pacing_gain_ = 1.25;

    /**
     * @brief Minimum allowed target sending rate (pps).
     */
    const double min_pps_ = 100.0;

    /**
     * @brief Interval (ns) after which the bandwidth estimate decays.
     */
    const uint64_t decay_interval_ns_ = 200000000;  // 200ms

    /**
     * @brief Maximum number of recent sends to remember.
     */
    const size_t max_history_ = 1024;

    /**
     * @brief Recent sent packet records: pair<sequence, send_time_ns>.
     */
    std::deque<std::pair<uint64_t, uint64_t>> sent_history_;  // pair<seq, send_ns>

    /**
     * @brief ACK receive timestamps used to compute delivered rate over a
     * sliding window.
     */
    const uint64_t ack_window_ns_ = 200000000;  // 200ms
    std::deque<uint64_t> ack_history_;

    /**
     * @brief Optional initial rate (pps) supplied by the caller; acts as an
     * upper bound for target rate adjustments when > 0.
     */
    double initial_rate_pps_ = 0.0;
};

static_assert(CongestionControllerConcept<bbr_congestion_controller>,
              "bbr_congestion_controller does not meet "
              "CongestionControllerConcept requirements");