/**
 * @file src/frame_warp_motion.h
 * @brief How far the picture's content moved between two captures, from small luma thumbnails.
 *
 * The camera turns the whole scene, so the estimate works on intensity profiles: for a band of
 * rows, the mean of each column (and for a band of columns, the mean of each row), with its slow
 * trend taken out. A shift of the profile is a shift of the content. The thumbnail is split into a
 * 3 by 3 grid; each cell finds its own shift by normalised correlation, refined to a tenth of a
 * pixel, and the median of the cells that found one clearly is the picture's. Cells that disagree
 * (the HUD, a character crossing) do not move the median. The estimate only teaches the model;
 * nothing on screen depends on one frame's estimate.
 *
 * `still_t` finds what stands still while the picture moves (a HUD down to the thumbnail's pixel),
 * and `measurer_t` keeps the state a stream of thumbnails is measured with.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// local includes
#include "frame_warp_types.h"

namespace frame_warp {

  inline constexpr std::size_t grid_edge = 3;
  /// Cells in the grid.
  inline constexpr std::size_t grid_cells = grid_edge * grid_edge;

  /**
   * Where one of a cell's shifts was measured, in thumbnail pixels from the thumbnail's centre. A
   * profile is followed by its slopes, each weighing in with its square, so a cell that is half sky
   * moves as its textured half does, a picture stretched toward its edge by a lens as the less
   * stretched part of the cell, and a cell half under a HUD as the half beside it.
   */
  struct place_t {
    /// The slopes' mean place along the shift's axis, and its mean square.
    float along = 0.0f;
    float squared = 0.0f;
    /// The mean place, across that axis, of the pixels behind the slopes, and the mean of the two
    /// places' product.
    float across = 0.0f;
    float cross = 0.0f;

    bool operator==(const place_t &) const = default;
  };

  /// One grid cell's shift, in thumbnail pixels.
  struct cell_shift_t {
    float dx = 0.0f;
    float dy = 0.0f;
    float correlation = 0.0f;
    /// Where `dx` and where `dy` were measured; none for the whole cell alike.
    std::optional<std::array<place_t, 2>> place;

    bool operator==(const cell_shift_t &) const = default;
  };

  /// The content's shift from one thumbnail to the next, in thumbnail pixels (x right, y down).
  struct motion_t {
    float dx = 0.0f;
    float dy = 0.0f;
    /// 0 to 1: how many cells agree with the median, and how clearly they found it.
    float confidence = 0.0f;
    /// Each grid cell's own shift where it found one clearly, rows top to bottom and columns left
    /// to right (`cell_spans` says where they are).
    std::array<std::optional<cell_shift_t>, grid_cells> cells {};

    /// A shift every cell shares, as a flat picture's is.
    static motion_t uniform(float dx, float dy, float confidence);

    bool operator==(const motion_t &) const = default;
  };

  /// One sample of a profile while it is built: its pixels' sum, their count and the sum of their
  /// places across the profile.
  struct profile_sum_t {
    float sum = 0.0f;
    std::uint32_t n = 0;
    float across = 0.0f;
  };

  /// What a profile is built in.
  struct lines_t {
    std::vector<float> line;
    std::vector<profile_sum_t> sums;
    std::vector<float> prefix;
    std::vector<std::uint32_t> count;

    /// Into `out`: where across the profile built last each sample's pixels lie on average, from the
    /// thumbnail's centre; NaN for a sample of HUD only.
    void across(std::vector<float> &out) const;
  };

  /// The buffers an estimate works in, kept from one estimate to the next.
  struct scratch_t {
    /// The previous thumbnail's row profile for each column band, unshifted: the same in both
    /// vertical passes; and where across each of those profiles' rows its pixels lie.
    std::array<std::vector<float>, grid_edge> before_rows;
    std::array<std::vector<float>, grid_edge> before_across;
    std::vector<float> before;
    std::vector<float> across;
    std::vector<float> after;
    std::vector<std::optional<float>> scores;
    lines_t lines;
  };

  /**
   * @brief The content's shift from `prev` to `cur`, or none when the thumbnails differ in size or
   *        too few cells find a clear shift.
   */
  std::optional<motion_t> estimate(const thumbnail_t &prev, const thumbnail_t &cur);

  /**
   * @brief `estimate`, leaving out the thumbnail pixels `hud` marks (row by row, the thumbnail's
   *        size): a HUD stays where it is, and a wide bar's rows left in would pull every vertical
   *        shift toward 0. Empty, or of another size, leaves nothing out.
   */
  std::optional<motion_t> estimate_around(const thumbnail_t &prev, const thumbnail_t &cur, std::span<const std::uint8_t> hud);

  /// `estimate_around` in `scratch`'s buffers.
  std::optional<motion_t> estimate_with(const thumbnail_t &prev, const thumbnail_t &cur, std::span<const std::uint8_t> hud, scratch_t &scratch);

  /// Where the grid's cells are along an edge of `len` thumbnail pixels: each cell's first pixel
  /// and the one past its last.
  std::array<std::pair<std::size_t, std::size_t>, grid_edge> cell_spans(std::size_t len);

  // The steps of the estimate, exposed for the unit tests.

  /**
   * @brief A line of the picture, in place, less its moving average over 13 samples: lighting and
   *        slow gradients out, edges and texture kept. NaN samples (HUD) stay NaN, and so does a
   *        sample with one among those 13. `prefix` and `count` are scratch.
   */
  void detrend(std::span<float> profile, std::vector<float> &prefix, std::vector<std::uint32_t> &count);

  /// Each row's profile over columns `x0..x1`, the columns taken `shift` to the right, built in `lines`.
  void profile_rows(const thumbnail_t &t, std::size_t x0, std::size_t x1, std::ptrdiff_t shift, const std::function<bool(std::size_t, std::size_t)> &scene, lines_t &lines, std::vector<float> &out);

  /**
   * @brief Where the shift of `before[from..to]` is measured: the weighted mean place of its slopes
   *        along the profile from its middle, and across it as `across` has each sample. None for a
   *        profile without slopes.
   */
  std::optional<place_t> place_of(std::span<const float> before, std::span<const float> across, std::size_t from, std::size_t to);

  /**
   * What stands still in a picture that moves: a HUD's frames, bars and letters, down to the
   * thumbnail's pixel. The host's HUD mask is what the client holds still, so it takes only cells
   * that are HUD all over and textured, and leaves out a HUD's rim and its plain parts; and a client
   * may want none. Left in the measurement, a minimap's frame or a health bar's edge in a corner cell
   * holds that cell's shift back, and the corners are where a lens shows most.
   *
   * A pixel is told by what the moves should have done to it: each brings it what its cell's shift
   * says lay that far away, and what that would change it by adds up, one way or the other, for as
   * long as the pixel stays what it was. One that should have changed by 24 luma codes and has not
   * stands still, until it changes. A pixel the moves bring more of the same (a sky, a horizon while
   * the camera pans, the middle of a plain panel) is not told and not marked.
   */
  class still_t {
  public:
    /// Takes the move `motion` from `prev` to `cur`. Returns whether the mask changed.
    bool observe(const thumbnail_t &prev, const thumbnail_t &cur, const motion_t &motion);

    /// Per thumbnail pixel, row by row, whether it stands still; empty while none does.
    std::span<const std::uint8_t> mask() const;

    /// Forgets what stood still.
    void clear();

  private:
    std::pair<std::size_t, std::size_t> size_ {0, 0};
    /// Per pixel: what it was when it last changed, and what the moves since should have changed it by.
    std::vector<std::pair<std::uint8_t, float>> pixels_;
    /// Per pixel: whether it or a neighbour stands still (a pixel on a HUD's rim is part HUD).
    std::vector<std::uint8_t> mask_;
    std::size_t marked_ = 0;
  };

  /// A thumbnail measured against the previous one (`measurer_t::measure`).
  struct measured_t {
    std::pair<std::size_t, std::size_t> size {0, 0};
    std::uint64_t at_us = 0;
    /// The shift since the previous thumbnail, and when that one was captured.
    std::optional<std::pair<std::uint64_t, motion_t>> shift;
  };

  /// The pixels either mask marks, when both are given (not empty) and of one size.
  std::optional<std::vector<std::uint8_t>> union_of(std::span<const std::uint8_t> hud, std::span<const std::uint8_t> still);

  /**
   * The motion measurement's state across a stream of thumbnails: the previous thumbnail, the HUD's
   * thumbnail pixels, what stands still, and the estimate's buffers.
   */
  class measurer_t {
  public:
    /// The shift from the previous thumbnail to `thumbnail`.
    measured_t measure(const thumbnail_t &thumbnail);

    /// After `measure`: keeps `thumbnail` as the next one's previous, or forgets the previous.
    void keep(thumbnail_t &&thumbnail, bool keep);

    /// The HUD's pixels over the thumbnail (`hud_pixels`).
    void set_hud(std::vector<std::uint8_t> hud);

    /// Forgets the previous thumbnail (a starting request, a new picture or thumbnail size).
    void reset_thumbnail();

    /// Forgets the HUD pixels (a new picture size).
    void reset_hud();

    const still_t &still() const {
      return still_;
    }

    bool has_previous() const {
      return previous_.has_value();
    }

    bool has_hud() const {
      return hud_.has_value();
    }

    bool has_union() const {
      return both_.has_value();
    }

    std::span<const std::uint8_t> union_mask() const {
      return both_ ? std::span<const std::uint8_t> {*both_} : std::span<const std::uint8_t> {};
    }

  private:
    std::optional<thumbnail_t> previous_;
    /// The HUD the GPU found, per thumbnail pixel, left out of the motion measurement.
    std::optional<std::vector<std::uint8_t>> hud_;
    /// What the measured moves leave standing, left out as well: the rims and plain parts of a HUD
    /// that the GPU's mask leaves in.
    still_t still_;
    /// `hud_` and `still_` together, while both mark something.
    std::optional<std::vector<std::uint8_t>> both_;
    scratch_t scratch_;
  };

}  // namespace frame_warp
