/**
 * @file src/frame_warp_history.cpp
 * @brief Definitions for src/frame_warp_history.h.
 */
// standard includes
#include <algorithm>
#include <cstdint>
#include <utility>

// local includes
#include "frame_warp_history.h"

namespace frame_warp {

  namespace {
    /// Mouse moves and pad packets kept at most, whatever their age.
    constexpr std::size_t max_entries = 65'536;
    /// A stick past this fraction of full deflection is in use.
    constexpr float stick_active_level = 0.08f;

    std::uint64_t saturating_sub(std::uint64_t a, std::uint64_t b) {
      return a > b ? a - b : 0;
    }

    /// How many entries of `entries` (sorted by time) are at or before `at_us`.
    template<class T>
    std::size_t count_until(const std::deque<T> &entries, std::uint64_t at_us) {
      const auto it = std::partition_point(entries.begin(), entries.end(), [at_us](const T &e) {
        return e.at_us <= at_us;
      });
      return static_cast<std::size_t>(it - entries.begin());
    }
  }  // namespace

  history_t::history_t(std::optional<std::uint8_t> controller):
      controller_ {controller} {
  }

  void history_t::set_controller(std::optional<std::uint8_t> controller) {
    if (controller_ != controller) {
      controller_ = controller;
      pads_.clear();
      pad_seq_ = 0;
    }
  }

  std::optional<std::uint8_t> history_t::controller() const {
    return controller_;
  }

  void history_t::on_mouse(std::int16_t dx, std::int16_t dy, std::uint64_t at_us) {
    if (dx == 0 && dy == 0) {
      return;
    }
    mouse_sum_.first += dx;
    mouse_sum_.second += dy;
    if (!mouse_.empty()) {
      at_us = std::max(at_us, mouse_.back().at_us);
    }
    mouse_.push_back(mouse_entry_t {at_us, mouse_sum_.first, mouse_sum_.second, dx, dy});
    // Keep the newest entry before the window: it is the sum at the window's start.
    while (mouse_.size() > max_entries || (mouse_.size() > 1 && saturating_sub(at_us, mouse_[1].at_us) > history_keep_us)) {
      mouse_.pop_front();
    }
  }

  void history_t::on_controller(std::uint16_t number, std::int16_t stick_x, std::int16_t stick_y, std::uint64_t at_us) {
    if (!controller_ || static_cast<std::uint16_t>(*controller_) != number) {
      return;
    }
    pad_seq_ = pad_seq_ + 1;
    if (!pads_.empty()) {
      at_us = std::max(at_us, pads_.back().at_us);
    }
    pads_.push_back(pad_entry_t {at_us, pad_seq_, stick_x, stick_y});
    while (pads_.size() > max_entries || (pads_.size() > 1 && saturating_sub(at_us, pads_[1].at_us) > history_keep_us)) {
      pads_.pop_front();
    }
  }

  std::pair<std::int64_t, std::int64_t> history_t::mouse_at(std::uint64_t at_us) const {
    const auto after = count_until(mouse_, at_us);
    if (after > 0) {
      return {mouse_[after - 1].x, mouse_[after - 1].y};
    }
    if (mouse_.empty()) {
      return mouse_sum_;
    }
    // Before the oldest kept move: its sums less the move itself.
    const auto &first = mouse_.front();
    return {first.x - first.dx, first.y - first.dy};
  }

  std::pair<std::int64_t, std::int64_t> history_t::mouse_between(std::uint64_t t0, std::uint64_t t1) const {
    const auto a = mouse_at(t0);
    const auto b = mouse_at(t1);
    return {b.first - a.first, b.second - a.second};
  }

  bool history_t::mouse_moved(std::uint64_t t0, std::uint64_t t1) const {
    return count_until(mouse_, t0) < count_until(mouse_, t1);
  }

  tag_t history_t::tag(std::uint32_t frame, std::uint64_t mouse_us, std::uint64_t pad_us) const {
    const auto [x, y] = mouse_at(mouse_us);
    const auto after = count_until(pads_, pad_us);
    std::uint32_t seq = 0;
    std::uint32_t held = no_held;
    if (after > 0) {
      const auto &pad = pads_[after - 1];
      held = static_cast<std::uint32_t>(std::min<std::uint64_t>(pad_us - pad.at_us, no_held - 1));
      seq = pad.seq;
    } else {
      seq = pads_.empty() ? pad_seq_ : pads_.front().seq - 1;
      held = no_held;
    }
    tag_t out;
    out.frame = frame;
    // The wire carries the sums modulo 2^32; the client takes differences.
    out.mouse_x = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(x)));
    out.mouse_y = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(y)));
    out.controller_seq = seq;
    out.held_us = seq == 0 ? no_held : held;
    return out;
  }

  std::array<double, 2> history_t::stick_integral(std::uint64_t t0, std::uint64_t t1, float deadzone, float exponent) const {
    std::array<double, 2> sum {0.0, 0.0};
    if (t1 <= t0) {
      return sum;
    }
    const auto start = [&] {
      const auto n = count_until(pads_, t0);
      return n > 0 ? n - 1 : std::size_t {0};
    }();
    for (std::size_t i = start; i < pads_.size(); ++i) {
      const auto &pad = pads_[i];
      if (pad.at_us >= t1) {
        break;
      }
      const auto end = std::min(i + 1 < pads_.size() ? pads_[i + 1].at_us : t1, t1);
      const auto from = std::max(pad.at_us, t0);
      if (end <= from) {
        continue;
      }
      const double seconds = static_cast<double>(end - from) / 1e6;
      const auto curve = stick_curve(pad.stick_x, pad.stick_y, deadzone, exponent);
      sum[0] += static_cast<double>(curve[0]) * seconds;
      sum[1] += static_cast<double>(curve[1]) * seconds;
    }
    return sum;
  }

  stick_integrals_t history_t::stick_integrals(float deadzone, float exponent) const {
    std::vector<std::array<float, 2>> curve;
    curve.reserve(pads_.size());
    for (const auto &pad : pads_) {
      curve.push_back(stick_curve(pad.stick_x, pad.stick_y, deadzone, exponent));
    }
    std::vector<std::array<double, 2>> running;
    running.reserve(curve.size());
    std::array<double, 2> sum {0.0, 0.0};
    for (std::size_t i = 0; i < curve.size(); ++i) {
      running.push_back(sum);
      if (i + 1 < pads_.size()) {
        const double seconds = static_cast<double>(pads_[i + 1].at_us - pads_[i].at_us) / 1e6;
        sum[0] += static_cast<double>(curve[i][0]) * seconds;
        sum[1] += static_cast<double>(curve[i][1]) * seconds;
      }
    }
    return stick_integrals_t {&pads_, std::move(curve), std::move(running)};
  }

  bool history_t::stick_active(std::uint64_t t0, std::uint64_t t1) const {
    const auto start = [&] {
      const auto n = count_until(pads_, t0);
      return n > 0 ? n - 1 : std::size_t {0};
    }();
    for (std::size_t i = start; i < pads_.size() && pads_[i].at_us < t1; ++i) {
      const float x = static_cast<float>(pads_[i].stick_x) / 32767.0f;
      const float y = static_cast<float>(pads_[i].stick_y) / 32767.0f;
      if (x * x + y * y > stick_active_level * stick_active_level) {
        return true;
      }
    }
    return false;
  }

  std::optional<std::uint64_t> history_t::last_input_us() const {
    std::optional<std::uint64_t> out;
    if (!mouse_.empty()) {
      out = mouse_.back().at_us;
    }
    if (!pads_.empty()) {
      out = out ? std::max(*out, pads_.back().at_us) : pads_.back().at_us;
    }
    return out;
  }

  stick_integrals_t::stick_integrals_t(const std::deque<history_t::pad_entry_t> *pads, std::vector<std::array<float, 2>> curve, std::vector<std::array<double, 2>> running):
      pads_ {pads},
      curve_ {std::move(curve)},
      running_ {std::move(running)} {
  }

  std::array<double, 2> stick_integrals_t::between(std::uint64_t t0, std::uint64_t t1) const {
    if (t1 <= t0) {
      return {0.0, 0.0};
    }
    const auto a = until(t0);
    const auto b = until(t1);
    return {b[0] - a[0], b[1] - a[1]};
  }

  std::array<double, 2> stick_integrals_t::until(std::uint64_t t) const {
    const auto after = count_until(*pads_, t);
    if (after == 0) {
      return {0.0, 0.0};
    }
    const auto i = after - 1;
    const double seconds = static_cast<double>(t - (*pads_)[i].at_us) / 1e6;
    return {
      running_[i][0] + static_cast<double>(curve_[i][0]) * seconds,
      running_[i][1] + static_cast<double>(curve_[i][1]) * seconds,
    };
  }

}  // namespace frame_warp
