/**
 * @file src/frame_warp_history.h
 * @brief What input the host received and when, for tagging frames and for fitting the model.
 *
 * Relative mouse motion is kept as running sums after each move; the tracked pad's packets as
 * their count and right stick. Times are microseconds on the session's clock, taken when the
 * control thread decoded the packet (injection follows within the same pass).
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

// local includes
#include "frame_warp_wire.h"

namespace frame_warp {

  /// History kept: longer than the calibrator's window plus its largest lag.
  inline constexpr std::uint64_t history_keep_us = 6'000'000;

  class stick_integrals_t;

  class history_t {
  public:
    struct mouse_entry_t {
      std::uint64_t at_us = 0;
      /// Running sums after this move, and the move.
      std::int64_t x = 0;
      std::int64_t y = 0;
      std::int16_t dx = 0;
      std::int16_t dy = 0;
    };

    struct pad_entry_t {
      std::uint64_t at_us = 0;
      /// Packets received for the tracked pad, this one included.
      std::uint32_t seq = 0;
      std::int16_t stick_x = 0;
      std::int16_t stick_y = 0;
    };

    explicit history_t(std::optional<std::uint8_t> controller = std::nullopt);

    /// Tracks another pad from now on; its packets count from zero.
    void set_controller(std::optional<std::uint8_t> controller);
    std::optional<std::uint8_t> controller() const;

    void on_mouse(std::int16_t dx, std::int16_t dy, std::uint64_t at_us);
    void on_controller(std::uint16_t number, std::int16_t stick_x, std::int16_t stick_y, std::uint64_t at_us);

    /// The running mouse sums as of `at_us`.
    std::pair<std::int64_t, std::int64_t> mouse_at(std::uint64_t at_us) const;

    /// Mouse counts received in `t0..t1`.
    std::pair<std::int64_t, std::int64_t> mouse_between(std::uint64_t t0, std::uint64_t t1) const;

    /// Whether a mouse move arrived after `t0` and by `t1` (moves that cancel count).
    bool mouse_moved(std::uint64_t t0, std::uint64_t t1) const;

    /// The tag of a frame that reflects mouse input up to `mouse_us` and pad input up to `pad_us`.
    tag_t tag(std::uint32_t frame, std::uint64_t mouse_us, std::uint64_t pad_us) const;

    /// The integral of the stick curve over `t0..t1`, in seconds of full deflection.
    std::array<double, 2> stick_integral(std::uint64_t t0, std::uint64_t t1, float deadzone, float exponent) const;

    /// `stick_integral` for many spans under one curve: the curve's running integral over the
    /// whole history, built once, so each span costs two binary searches. Valid until the next
    /// controller packet.
    stick_integrals_t stick_integrals(float deadzone, float exponent) const;

    /// Whether the tracked pad's right stick was off centre at any time in `t0..t1`.
    bool stick_active(std::uint64_t t0, std::uint64_t t1) const;

    /// When the last mouse move or pad packet arrived.
    std::optional<std::uint64_t> last_input_us() const;

    std::size_t mouse_entries() const {
      return mouse_.size();
    }

    const std::deque<pad_entry_t> &pads() const {
      return pads_;
    }

  private:
    std::optional<std::uint8_t> controller_;
    std::deque<mouse_entry_t> mouse_;
    std::pair<std::int64_t, std::int64_t> mouse_sum_ {0, 0};
    std::deque<pad_entry_t> pads_;
    std::uint32_t pad_seq_ = 0;
  };

  /// One stick curve's running integral over the pad history (`history_t::stick_integrals`).
  class stick_integrals_t {
  public:
    stick_integrals_t(const std::deque<history_t::pad_entry_t> *pads, std::vector<std::array<float, 2>> curve, std::vector<std::array<double, 2>> running);

    /// `history_t::stick_integral` over `t0..t1` with this curve.
    std::array<double, 2> between(std::uint64_t t0, std::uint64_t t1) const;

  private:
    /// The integral from the first packet to `t`; 0 before it.
    std::array<double, 2> until(std::uint64_t t) const;

    const std::deque<history_t::pad_entry_t> *pads_;
    /// Each packet's curved stick, held until the next packet (the last one indefinitely).
    std::vector<std::array<float, 2>> curve_;
    /// The integral from the first packet to each packet, in seconds of full deflection.
    std::vector<std::array<double, 2>> running_;
  };

}  // namespace frame_warp
