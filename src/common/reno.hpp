// reno.hpp
// Simple Reno-like congestion controller (window-based) for experiments.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <algorithm>

class reno_congestion_controller {
public:
    reno_congestion_controller() = default;

    void set_initial_rate(double pps) {
        if (pps <= 0.0) {
            unlimited_ = true;
            return;
        }
        unlimited_ = false;
        // initialize min RTT unknown; cwnd start small
        cwnd_packets_ = 10.0; // start with modest cwnd
        ssthresh_ = 1000.0;
        // store initial request rate and seed pacing rate
        initial_rate_pps_ = pps;
        pacing_rate_pps_ = pps;
        // approximate initial window -> target pps when min RTT known
        // leave ewma and min_rtt unset until acks arrive
    }

    void on_send(uint64_t now_ns, uint64_t seq) {
        if (unlimited_) return;
        send_times_[seq] = now_ns;
    }

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

    double target_rate_pps() const {
        if (unlimited_) return 0.0;
        return pacing_rate_pps_;
    }

private:
    void update_target_rate() {
        if (min_rtt_ns_ == 0) {
            // keep existing pacing rate if we don't yet have a min RTT
            return;
        }
        double min_rtt_s = static_cast<double>(min_rtt_ns_) / 1e9;
        if (min_rtt_s <= 0.0) return;
        pacing_rate_pps_ = cwnd_packets_ / min_rtt_s;
        // Clamp to avoid runaway: limit to a multiplier of initial requested rate
        const double max_mult = 1.0; // do not exceed initial requested rate
        if (initial_rate_pps_ > 0.0) {
            double cap = initial_rate_pps_ * max_mult;
            if (pacing_rate_pps_ > cap) pacing_rate_pps_ = cap;
        }
        if (pacing_rate_pps_ < 1.0) pacing_rate_pps_ = 1.0;
    }

    bool unlimited_{false};
    uint64_t last_rtt_ns_{0};
    uint64_t min_rtt_ns_{0};
    double cwnd_packets_{10.0};
    double ssthresh_{1000.0};
    double pacing_rate_pps_{0.0};
    std::unordered_map<uint64_t, uint64_t> send_times_;
    double initial_rate_pps_{0.0};

    const double rtt_inflation_threshold_{1.6};
};
