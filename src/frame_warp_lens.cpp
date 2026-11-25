/**
 * @file src/frame_warp_lens.cpp
 * @brief Definitions for src/frame_warp_lens.h.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <numbers>

// local includes
#include "frame_warp_lens.h"

namespace frame_warp {

  namespace {
    /// What a move's remainders keep of their weight when the next move comes: half after about 140
    /// moves, a little over two seconds of turning at 60 captures a second.
    constexpr double fade = 0.995;
    /// Moves (faded) and their squared length (thumbnail pixels, faded) before a lens is chosen.
    constexpr double min_moves = 30.0;
    constexpr double min_travel = 200.0;
    /// A move shorter than this (thumbnail pixels at the median cell) says too little.
    constexpr float min_move = 1.0f;
    /// Cells a move needs to tell a lens from a zoom or a roll.
    constexpr std::size_t min_cells = 6;
    /// A lens is taken when it leaves at most this share of what a flat picture leaves, and given up
    /// when it leaves more than `drop` of it.
    constexpr double take = 0.75;
    constexpr double drop = 0.9;
    /// A lens in force gives way to one at least this many degrees away that leaves at most this
    /// share of what it leaves: the estimate wobbles by a degree as moves fade.
    constexpr double change_degrees = 1.5;
    constexpr double change = 0.98;
    /// A cell this far (thumbnail pixels, plus a tenth of the move) from where a candidate puts it
    /// is left out of that candidate's fit, and no cell costs a candidate more than this far would:
    /// something else moves there, and a move that is no camera turn at all (a cut, a menu opening)
    /// then costs every candidate alike instead of choosing among them by chance.
    constexpr double outlier = 0.5;
    /// Cells within this many thumbnail pixels of the fitted turn agree with it
    /// (`layout_t::resolve`), as cells within it of the median do in the flat estimate.
    constexpr double agree_distance = 1.5;

    /// The squared tangent of half the field of view of candidate `index`.
    double candidate(std::size_t index) {
      return tangent_of(lens_t::first_degrees + lens_t::step_degrees * static_cast<double>(index));
    }
  }  // namespace

  double tangent_of(double degrees) {
    const double half = degrees * (std::numbers::pi / 180.0) * 0.5;
    return std::tan(half) * std::tan(half);
  }

  layout_t::layout_t(std::pair<std::uint16_t, std::uint16_t> content, std::pair<std::size_t, std::size_t> thumbnail) {
    const auto tw = std::max<std::size_t>(thumbnail.first, 1);
    const auto th = std::max<std::size_t>(thumbnail.second, 1);
    const double content_width = static_cast<double>(std::max<std::uint16_t>(content.first, 1));
    const double content_height = static_cast<double>(std::max<std::uint16_t>(content.second, 1));
    scale = {content_width / static_cast<double>(tw), content_height / static_cast<double>(th)};
    half_width = content_width * 0.5;
    // A thumbnail pixel edge's place from the centre, in half picture widths.
    const auto place = [this](std::size_t edge, std::size_t len, double edge_scale) {
      return (static_cast<double>(edge) - static_cast<double>(len) * 0.5) * edge_scale / half_width;
    };
    const auto columns = cell_spans(tw);
    const auto rows = cell_spans(th);
    for (std::size_t row = 0; row < grid_edge; ++row) {
      for (std::size_t column = 0; column < grid_edge; ++column) {
        const double x0 = place(columns[column].first, tw, scale[0]);
        const double x1 = place(columns[column].second, tw, scale[0]);
        const double y0 = place(rows[row].first, th, scale[1]);
        const double y1 = place(rows[row].second, th, scale[1]);
        const double x = (x0 + x1) * 0.5;
        const double y = (y0 + y1) * 0.5;
        cell_t cell;
        cell.x = x;
        cell.y = y;
        cell.xx = (x0 * x0 + x0 * x1 + x1 * x1) / 3.0;
        cell.yy = (y0 * y0 + y0 * y1 + y1 * y1) / 3.0;
        cell.xy = x * y;
        cells[row * grid_edge + column] = cell;
      }
    }
  }

  double layout_t::focal(double tangent) const {
    return half_width / std::sqrt(tangent);
  }

  double layout_t::tangent(double focal) const {
    const double ratio = half_width / focal;
    return ratio * ratio;
  }

  turn_t layout_t::resolve(const motion_t &motion, std::optional<double> lens) const {
    turn_t flat;
    flat.dx = motion.dx;
    flat.dy = motion.dy;
    flat.confidence = motion.confidence;
    if (!lens) {
      return flat;
    }
    const auto fitted = fit(motion, *lens, agree_distance);
    if (!fitted) {
      return flat;
    }
    std::size_t agreeing = 0;
    double correlation = 0.0;
    for (std::size_t i = 0; i < grid_cells; ++i) {
      if (fitted->agree[i]) {
        agreeing += 1;
        if (motion.cells[i]) {
          correlation += static_cast<double>(motion.cells[i]->correlation);
        }
      }
    }
    if (agreeing < 3) {
      return flat;
    }
    correlation /= static_cast<double>(agreeing);
    turn_t out;
    out.dx = static_cast<float>(fitted->turn[0] / scale[0]);
    out.dy = static_cast<float>(fitted->turn[1] / scale[1]);
    out.confidence = static_cast<float>(std::clamp((static_cast<double>(agreeing) / static_cast<double>(grid_cells)) * correlation, 0.0, 1.0));
    return out;
  }

  std::optional<layout_t::fitted_t> layout_t::fit(const motion_t &motion, double lens, double tolerance) const {
    std::array<bool, grid_cells> used {};
    for (std::size_t i = 0; i < grid_cells; ++i) {
      used[i] = motion.cells[i].has_value();
    }
    const double limit = (tolerance + 0.1 * static_cast<double>(std::hypot(motion.dx, motion.dy))) * scale[0];
    std::size_t dropped = 0;
    for (;;) {
      const auto count = static_cast<std::size_t>(std::count(used.begin(), used.end(), true));
      if (count < 3) {
        return std::nullopt;
      }
      // With few cells a zoom and a roll would fit anything.
      const std::size_t terms = count >= 5 ? 4 : 2;
      const auto solution = solve(motion, lens, used, terms);
      if (!solution) {
        return std::nullopt;
      }
      double worst = 0.0;
      std::optional<std::size_t> worst_at;
      double rest = 0.0;
      std::array<bool, grid_cells> agree {};
      for (std::size_t i = 0; i < grid_cells; ++i) {
        if (!used[i]) {
          continue;
        }
        const auto r = remainder(motion, lens, i, *solution);
        const double squared = r[0] * r[0] + r[1] * r[1];
        rest += std::min(squared, limit * limit);
        agree[i] = squared <= limit * limit;
        if (squared > worst) {
          worst = squared;
          worst_at = i;
        }
      }
      if (worst_at && worst > limit * limit && dropped < 2 && count > 3) {
        used[*worst_at] = false;
        dropped += 1;
        continue;
      }
      fitted_t out;
      out.turn = {(*solution)[0], (*solution)[1]};
      out.agree = agree;
      // A cell left out costs what it may at most be off by.
      out.rest = rest + static_cast<double>(dropped) * limit * limit;
      return out;
    }
  }

  std::optional<layout_t::rows_t> layout_t::rows(const motion_t &motion, double lens, std::size_t i) const {
    const auto &shift = motion.cells[i];
    if (!shift) {
      return std::nullopt;
    }
    const auto &c = cells[i];
    rows_t out;
    if (shift->place) {
      // Each shift where its profile measured it (`place_t`).
      const double kx = scale[0] / half_width;
      const double ky = scale[1] / half_width;
      const auto at = [](float place, double place_scale) {
        return static_cast<double>(place) * place_scale;
      };
      const auto &x = (*shift->place)[0];
      const auto &y = (*shift->place)[1];
      out.terms = {{
        {1.0 + lens * at(x.squared, kx * kx), lens * at(x.cross, kx * ky), at(x.along, kx), -at(x.across, ky)},
        {lens * at(y.cross, kx * ky), 1.0 + lens * at(y.squared, ky * ky), at(y.along, ky), at(y.across, kx)},
      }};
    } else {
      out.terms = {{
        {1.0 + lens * c.xx, lens * c.xy, c.x, -c.y},
        {lens * c.xy, 1.0 + lens * c.yy, c.y, c.x},
      }};
    }
    out.shift = {static_cast<double>(shift->dx) * scale[0], static_cast<double>(shift->dy) * scale[1]};
    return out;
  }

  std::array<double, 2> layout_t::remainder(const motion_t &motion, double lens, std::size_t i, const std::array<double, 4> &solution) const {
    const auto r = rows(motion, lens, i);
    if (!r) {
      return {0.0, 0.0};
    }
    std::array<double, 2> out {};
    for (std::size_t axis = 0; axis < 2; ++axis) {
      double sum = 0.0;
      for (std::size_t k = 0; k < 4; ++k) {
        sum += r->terms[axis][k] * solution[k];
      }
      out[axis] = r->shift[axis] - sum;
    }
    return out;
  }

  std::optional<std::array<double, 4>> layout_t::solve(const motion_t &motion, double lens, const std::array<bool, grid_cells> &used, std::size_t terms) const {
    std::array<std::array<double, 5>, 4> normal {};
    for (std::size_t i = 0; i < grid_cells; ++i) {
      if (!used[i]) {
        continue;
      }
      const auto r = rows(motion, lens, i);
      if (!r) {
        return std::nullopt;
      }
      for (std::size_t axis = 0; axis < 2; ++axis) {
        for (std::size_t p = 0; p < terms; ++p) {
          for (std::size_t q = 0; q < terms; ++q) {
            normal[p][q] += r->terms[axis][p] * r->terms[axis][q];
          }
          normal[p][4] += r->terms[axis][p] * r->shift[axis];
        }
      }
    }
    // Gaussian elimination with partial pivoting.
    for (std::size_t p = 0; p < terms; ++p) {
      // The last of equal pivots, as `Iterator::max_by` has it.
      std::size_t pivot = p;
      for (std::size_t a = p; a < terms; ++a) {
        if (std::abs(normal[a][p]) >= std::abs(normal[pivot][p])) {
          pivot = a;
        }
      }
      if (std::abs(normal[pivot][p]) < 1e-9) {
        return std::nullopt;
      }
      std::swap(normal[p], normal[pivot]);
      for (std::size_t q = 0; q < terms; ++q) {
        if (q != p) {
          const double factor = normal[q][p] / normal[p][p];
          const auto pivot_row = normal[p];
          for (std::size_t k = p; k < 5; ++k) {
            normal[q][k] -= factor * pivot_row[k];
          }
        }
      }
    }
    std::array<double, 4> solution {};
    for (std::size_t p = 0; p < terms; ++p) {
      solution[p] = normal[p][4] / normal[p][p];
    }
    return solution;
  }

  void lens_t::clear() {
    prior_ = tangent();
    flat_ = 0.0;
    rest_.fill(0.0);
    moves_ = 0.0;
    travel_ = 0.0;
    chosen_.reset();
  }

  void lens_t::set_prior(std::optional<double> tangent) {
    if (tangent && std::isfinite(*tangent) && *tangent > 0.0) {
      prior_ = tangent;
    } else {
      prior_.reset();
    }
  }

  std::optional<double> lens_t::tangent() const {
    if (decided()) {
      if (chosen_) {
        return tangent_of(*chosen_);
      }
      return std::nullopt;
    }
    return prior_;
  }

  bool lens_t::decided() const {
    return moves_ >= min_moves && travel_ >= min_travel;
  }

  bool lens_t::observe(const layout_t &layout, const motion_t &motion) {
    std::size_t found = 0;
    for (const auto &cell : motion.cells) {
      if (cell) {
        found += 1;
      }
    }
    if (found < min_cells || std::hypot(motion.dx, motion.dy) < min_move) {
      return false;
    }
    const auto flat = layout.fit(motion, 0.0, outlier);
    if (!flat) {
      return false;
    }
    const auto before = tangent();
    // In thumbnail pixels, so the thresholds mean the same at every picture size.
    const double unit = 1.0 / (layout.scale[0] * layout.scale[0]);
    flat_ = flat_ * fade + flat->rest * unit;
    for (std::size_t index = 0; index < candidates; ++index) {
      // A candidate that cannot be fitted leaves what the flat picture does.
      const auto fitted = layout.fit(motion, candidate(index), outlier);
      const double left = fitted ? fitted->rest : flat->rest;
      rest_[index] = rest_[index] * fade + left * unit;
    }
    moves_ = moves_ * fade + 1.0;
    travel_ = travel_ * fade + static_cast<double>(motion.dx * motion.dx + motion.dy * motion.dy);
    if (decided()) {
      choose();
    }
    return tangent() != before;
  }

  void lens_t::choose() {
    // The first of equal minima, as `Iterator::min_by` has it.
    std::size_t best = 0;
    for (std::size_t i = 1; i < candidates; ++i) {
      if (rest_[i] < rest_[best]) {
        best = i;
      }
    }
    const double least = rest_[best];
    // Between the candidates: where a parabola through the best and its neighbours is least.
    double between = 0.0;
    if (best > 0 && best + 1 < candidates) {
      const double left = rest_[best - 1];
      const double right = rest_[best + 1];
      const double curve = left - 2.0 * least + right;
      if (curve > 0.0) {
        between = std::clamp(0.5 * (left - right) / curve, -0.5, 0.5);
      }
    }
    const double found = first_degrees + step_degrees * (static_cast<double>(best) + between);
    if (!chosen_) {
      if (least <= flat_ * take) {
        chosen_ = found;
      }
    } else {
      const double current = *chosen_;
      if (least > flat_ * drop) {
        chosen_.reset();
      } else if (std::abs(found - current) >= change_degrees && least <= left_at(current) * change) {
        chosen_ = found;
      }
    }
  }

  double lens_t::left_at(double degrees) const {
    const double at = std::clamp((degrees - first_degrees) / step_degrees, 0.0, static_cast<double>(candidates - 1));
    const auto below = std::min(static_cast<std::size_t>(at), candidates - 2);
    const double share = at - static_cast<double>(below);
    return rest_[below] * (1.0 - share) + rest_[below + 1] * share;
  }

}  // namespace frame_warp
