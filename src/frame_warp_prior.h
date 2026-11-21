/**
 * @file src/frame_warp_prior.h
 * @brief What a game taught the Frame Warp learner, to start the next stream of that game from.
 *
 * The gains are in pixels of the game's picture (the stream's picture less any black bars) of the
 * size stored beside them, so a stream of another size scales them. The JSON keys are the field
 * names, `null` or absent for a value not learned.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <optional>

// lib includes
#include <nlohmann/json.hpp>

namespace frame_warp {

  struct prior_t {
    /// Picture pixels per mouse count at the picture size below, if learned.
    std::optional<std::array<float, 2>> mouse_gain;
    std::optional<std::uint64_t> mouse_lag_us;
    /// Picture pixels per second of full deflection after the curve.
    std::optional<std::array<float, 2>> stick_gain;
    std::optional<std::uint64_t> stick_lag_us;
    std::optional<float> stick_deadzone;
    std::optional<float> stick_exponent;
    /// The game camera's focal length in pixels of the picture below, if it has a lens; the gains
    /// are then the picture centre's.
    std::optional<float> focal;
    /// The size of the game's picture the gains are in: the stream's, less any black bars.
    std::uint16_t picture_width = 0;
    std::uint16_t picture_height = 0;

    bool operator==(const prior_t &) const = default;
  };

  void to_json(nlohmann::json &j, const prior_t &prior);

  /**
   * @brief Read a prior. Absent or null optional fields are none. Throws `nlohmann::json::exception`
   *        for a value of the wrong type, a gain that is not an array of two numbers, or a missing
   *        picture size.
   */
  void from_json(const nlohmann::json &j, prior_t &prior);

}  // namespace frame_warp
