/**
 * @file src/frame_warp_types.h
 * @brief What the Frame Warp learner and the GPU passes hand each other.
 *
 * The GPU passes (src/platform/windows/frame_warp_d3d11.h) read a small luma thumbnail of each
 * capture and, while the camera turns, a HUD mask back; the learner (src/frame_warp.h) measures
 * the picture's motion from the thumbnails. Both are plain data so the learner builds and tests
 * without a GPU.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace frame_warp {

  /// Width of the luma thumbnail of a capture, in pixels.
  inline constexpr std::size_t thumbnail_width = 256;

  /// The thumbnail height for a picture, `thumbnail_width` wide: its aspect, 32 to 256 rows.
  inline constexpr std::size_t thumbnail_height(std::uint16_t picture_width, std::uint16_t picture_height) {
    const std::size_t w = std::max<std::size_t>(picture_width, 1);
    const std::size_t h = std::max<std::size_t>(picture_height, 1);
    return std::clamp<std::size_t>(thumbnail_width * h / w, 32, 256);
  }

  /// A rectangle in stream picture pixels: where a capture shows inside the encoded picture. A
  /// capture of another shape than the stream is letterboxed into it, and the rest is black bars.
  struct rect_t {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;

    bool operator==(const rect_t &) const = default;
  };

  /// The luma of one capture, `width * height` bytes, rows top to bottom.
  struct thumbnail_t {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<std::uint8_t> luma;
    /// The capture's time, microseconds on the session clock (`session_t::us`).
    std::uint64_t at_us = 0;
  };

}  // namespace frame_warp
