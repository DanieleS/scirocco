/**
 * @file src/frame_warp_lens.h
 * @brief The game's field of view, learned from how a camera turn moves the picture.
 *
 * A game draws through a perspective camera, so a turn does not move its picture alike everywhere.
 * With the focal length `f` (the distance, in pixels, at which the picture's width spans the
 * horizontal field of view), a small turn that moves the centre by `(a, b)` moves the point
 * `(x, y)` from the centre by
 *
 *     u = a (1 + x^2/f^2) + b xy/f^2
 *     v = b (1 + y^2/f^2) + a xy/f^2
 *
 * At a 90 degree field of view the left and right edges move twice as far as the centre. A shift
 * that is right in the middle of the picture is then far too short at its edges, and the median of
 * the grid's cells (src/frame_warp_motion.h), most of which lie off centre, is too long in the
 * middle. A flat picture (a map, a side-scroller) moves alike everywhere: `1/f^2` is 0.
 *
 * Each measured move gives every grid cell's own shift, and where in the cell its profile measured
 * it (`place_t`): a lens stretches the picture toward its edge, so an outer cell's texture, and
 * with it the shift, lies nearer the middle than the cell does, by enough to read a 90 degree lens
 * as 78. For every candidate field of view the cells are fitted with the camera's turn `(a, b)`, a
 * zoom about the centre (walking forward) and a roll, and the squared remainder is kept, fading
 * over the last few seconds of turning. The candidate that leaves the least wins once it leaves
 * clearly less than a flat picture does; until then, and for a game that pans a flat picture,
 * there is no lens and the model's shift stays a translation.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

// local includes
#include "frame_warp_motion.h"

namespace frame_warp {

  /// A camera turn read from a move's cells (`layout_t::resolve`).
  struct turn_t {
    /// How far the picture's centre moved, in thumbnail pixels.
    float dx = 0.0f;
    float dy = 0.0f;
    /// 0 to 1, as `motion_t::confidence`.
    float confidence = 0.0f;

    bool operator==(const turn_t &) const = default;
  };

  /// Where the grid's cells lie in the game's picture, for a thumbnail of some size.
  struct layout_t {
    /// One cell, with positions from the centre in half picture widths: its centre and the means of
    /// x^2, y^2 and xy over it.
    struct cell_t {
      double x = 0.0;
      double y = 0.0;
      double xx = 0.0;
      double yy = 0.0;
      double xy = 0.0;

      bool operator==(const cell_t &) const = default;
    };

    /// Per cell, rows top to bottom, columns left to right.
    std::array<cell_t, grid_cells> cells {};
    /// Picture pixels per thumbnail pixel, x and y.
    std::array<double, 2> scale {1.0, 1.0};
    /// Half the picture's width in picture pixels.
    double half_width = 0.5;

    layout_t() = default;

    /// The layout of a `thumbnail` (width, height) of a game picture `content` pixels in size.
    layout_t(std::pair<std::uint16_t, std::uint16_t> content, std::pair<std::size_t, std::size_t> thumbnail);

    /// The focal length, in picture pixels, of a lens whose `tangent` is the squared tangent of
    /// half the horizontal field of view.
    double focal(double tangent) const;

    /// The squared tangent of half the field of view of a focal length in picture pixels.
    double tangent(double focal) const;

    /**
     * @brief The camera's turn in `motion`: under `lens` (the squared tangent of half the
     *        horizontal field of view), how far the picture's centre moved, fitted to the cells
     *        with a zoom and a roll when enough of them found a shift. Without a lens, or for a
     *        move without cells (a still picture), the flat estimate as it is.
     */
    turn_t resolve(const motion_t &motion, std::optional<double> lens) const;

    /// What a fit of `motion`'s cells leaves (`fit`).
    struct fitted_t {
      /// The centre's shift in picture pixels.
      std::array<double, 2> turn {};
      /// The cells within the tolerance of the fit.
      std::array<bool, grid_cells> agree {};
      /// The squared remainder, picture pixels.
      double rest = 0.0;
    };

    /**
     * @brief `motion`'s cells fitted under `lens`, leaving out up to two cells farther than
     *        `tolerance` thumbnail pixels (plus a tenth of the move) from where the fit puts them.
     *        None with fewer than three cells.
     */
    std::optional<fitted_t> fit(const motion_t &motion, double lens, double tolerance) const;

  private:
    struct rows_t {
      std::array<double, 2> shift {};
      std::array<std::array<double, 4>, 2> terms {};
    };

    std::optional<rows_t> rows(const motion_t &motion, double lens, std::size_t i) const;
    std::array<double, 2> remainder(const motion_t &motion, double lens, std::size_t i, const std::array<double, 4> &solution) const;
    std::optional<std::array<double, 4>> solve(const motion_t &motion, double lens, const std::array<bool, grid_cells> &used, std::size_t terms) const;
  };

  /// The squared tangent of half a field of view of `degrees`.
  double tangent_of(double degrees);

  /// Chooses the lens from the moves the camera's turns made.
  class lens_t {
  public:
    /// Fields of view tried: 30 to 120 degrees across the picture's width, in steps of 2.
    static constexpr double first_degrees = 30.0;
    static constexpr double step_degrees = 2.0;
    static constexpr std::size_t candidates = 46;

    /// Forgets the moves; the lens in force stays until new moves choose.
    void clear();

    /// Starts from a lens an earlier session learned.
    void set_prior(std::optional<double> tangent);

    /// The lens in force: the squared tangent of half the horizontal field of view, or none for a
    /// flat picture or one not learned yet.
    std::optional<double> tangent() const;

    /// Takes a move the player's turning made. Returns whether the lens in force changed.
    bool observe(const layout_t &layout, const motion_t &motion);

  private:
    /// Whether the moves have said enough to choose (a lens or a flat picture).
    bool decided() const;
    void choose();
    /// What a field of view of `degrees` leaves, between the candidates beside it.
    double left_at(double degrees) const;

    /// What a flat picture and each candidate left of the moves, faded.
    double flat_ = 0.0;
    std::array<double, candidates> rest_ {};
    double moves_ = 0.0;
    double travel_ = 0.0;
    /// The field of view in force, in degrees: between the candidates, where they leave least.
    std::optional<double> chosen_;
    /// A lens from an earlier session, in force until the moves choose.
    std::optional<double> prior_;
  };

}  // namespace frame_warp
