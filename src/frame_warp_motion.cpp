/**
 * @file src/frame_warp_motion.cpp
 * @brief Definitions for src/frame_warp_motion.h.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

// local includes
#include "frame_warp_motion.h"

namespace frame_warp {

  namespace {
    constexpr std::size_t min_edge = 32;
    /// Edges left out: vignettes, letterboxing and edge HUD.
    constexpr float margin_share = 0.04f;
    /// Largest shift searched, as a fraction of the profile's length.
    constexpr float search_share = 0.2f;
    /// Correlation a cell needs to count.
    constexpr float min_correlation = 0.6f;
    /// How much a cell's best shift must beat any other more than 2 pixels away.
    constexpr float min_uniqueness = 0.04f;
    /// Profile variance below this is a flat cell (sky, a wall in shadow).
    constexpr float min_variance = 0.5f;
    /// Cells within this many pixels of the median agree with it.
    constexpr float agree_distance = 1.5f;
    /// A pixel this far from its right and its lower neighbour (in luma codes) is texture: it changes
    /// whichever way it moves (an edge along the move would not).
    constexpr std::uint8_t texture_step = 8;
    /// A cell whose texture is this much unchanged stands still.
    constexpr float still_share = 0.9f;
    /// A cell needs this many texture pixels to tell whether it stands still.
    constexpr std::size_t min_texture = 16;

    /// A pixel that stays within this many luma codes of what it was has not changed: a HUD's pixel
    /// in the thumbnail is the mean of many, so a game's dither over it comes to less.
    constexpr std::uint8_t still_change = 2;
    /// A move that should have changed a pixel by at least this much tells whether the pixel moved
    /// along; less is within `still_change` and the capture's noise.
    constexpr float still_step = 3.0f;
    /// A pixel stands still once the moves since it last changed should have changed it by this many
    /// luma codes in all, of which one move counts for a quarter at most: where the measured shift
    /// is wrong for once (a thin pole that passes over a pixel of sky), it is not four times.
    constexpr float still_total = 24.0f;
    /// Moves shorter than this (thumbnail pixels) say nothing about what stands still.
    constexpr float still_min_move = 1.0f;

    constexpr float nan_value = std::numeric_limits<float>::quiet_NaN();

    bool valid(const thumbnail_t &t) {
      return t.width >= min_edge && t.height >= min_edge && t.luma.size() == t.width * t.height;
    }

    float at(const thumbnail_t &t, std::size_t x, std::size_t y) {
      return static_cast<float>(t.luma[y * t.width + x]);
    }

    /// Cell boundaries along an edge of `len` pixels, margins left out.
    std::array<std::size_t, grid_edge + 1> edges(std::size_t len) {
      const auto margin = static_cast<std::size_t>(static_cast<float>(len) * margin_share);
      const auto inner = len - 2 * margin;
      std::array<std::size_t, grid_edge + 1> out {};
      for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = margin + inner * i / grid_edge;
      }
      return out;
    }

    /// The median of at most a handful of values.
    std::optional<float> median(std::vector<float> values) {
      if (values.empty()) {
        return std::nullopt;
      }
      std::sort(values.begin(), values.end());
      const auto mid = values.size() / 2;
      if (values.size() % 2 == 1) {
        return values[mid];
      }
      return 0.5f * (values[mid - 1] + values[mid]);
    }

    /// Where the cells are and how far each searches.
    struct grid_t {
      std::array<std::size_t, grid_edge + 1> x_edges;
      std::array<std::size_t, grid_edge + 1> y_edges;
      std::size_t search_x;
      std::size_t search_y;
    };

    struct shift_t {
      /// Sub-pixel shift: `after[i + at] ~ before[i]`.
      float at = 0.0f;
      float correlation = 0.0f;
      /// Where the shift was measured.
      std::optional<place_t> place;
    };

    /// Adds a detrended line's known samples to the profile's sums; `at_across` is the line's place
    /// across the profile, from the thumbnail's centre.
    void gather(const std::vector<float> &line, float at_across, std::vector<profile_sum_t> &sums) {
      for (std::size_t i = 0; i < line.size(); ++i) {
        if (!std::isnan(line[i])) {
          sums[i].sum += line[i];
          sums[i].n += 1;
          sums[i].across += at_across;
        }
      }
    }

    void means(const std::vector<profile_sum_t> &sums, std::vector<float> &out) {
      out.clear();
      for (const auto &s : sums) {
        out.push_back(s.n == 0 ? nan_value : s.sum / static_cast<float>(s.n));
      }
    }

    template<class S>
    void profile_rows_impl(const thumbnail_t &t, std::size_t x0, std::size_t x1, std::ptrdiff_t shift, S scene, lines_t &lines, std::vector<float> &out) {
      const auto last = static_cast<std::ptrdiff_t>(t.width) - 1;
      lines.sums.assign(t.height, profile_sum_t {});
      for (std::size_t column = x0; column < x1; ++column) {
        const auto x = static_cast<std::size_t>(std::clamp(static_cast<std::ptrdiff_t>(column) + shift, std::ptrdiff_t {0}, last));
        lines.line.clear();
        for (std::size_t y = 0; y < t.height; ++y) {
          lines.line.push_back(scene(x, y) ? at(t, x, y) : nan_value);
        }
        detrend(lines.line, lines.prefix, lines.count);
        gather(lines.line, static_cast<float>(x) + 0.5f - static_cast<float>(t.width) * 0.5f, lines.sums);
      }
      means(lines.sums, out);
    }

    template<class S>
    void profile_columns_impl(const thumbnail_t &t, std::size_t y0, std::size_t y1, std::ptrdiff_t shift, S scene, lines_t &lines, std::vector<float> &out) {
      const auto last = static_cast<std::ptrdiff_t>(t.height) - 1;
      lines.sums.assign(t.width, profile_sum_t {});
      for (std::size_t row = y0; row < y1; ++row) {
        const auto y = static_cast<std::size_t>(std::clamp(static_cast<std::ptrdiff_t>(row) + shift, std::ptrdiff_t {0}, last));
        lines.line.clear();
        for (std::size_t x = 0; x < t.width; ++x) {
          lines.line.push_back(scene(x, y) ? at(t, x, y) : nan_value);
        }
        detrend(lines.line, lines.prefix, lines.count);
        gather(lines.line, static_cast<float>(y) + 0.5f - static_cast<float>(t.height) * 0.5f, lines.sums);
      }
      means(lines.sums, out);
    }

    /**
     * Normalised correlation of `before[from..to]` with `after[from + s..to + s]`, over the part
     * that stays inside `after` and saw some of the scene in both; none when that is less than 60%
     * of the cell.
     */
    std::optional<float> correlation(std::span<const float> before, std::span<const float> after, std::size_t from, std::size_t to, std::ptrdiff_t s) {
      const auto lo = static_cast<std::size_t>(std::max<std::ptrdiff_t>({static_cast<std::ptrdiff_t>(from), -s, 0}));
      const auto hi = static_cast<std::size_t>(std::max<std::ptrdiff_t>(std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(to), static_cast<std::ptrdiff_t>(after.size()) - s), 0));
      if (hi <= lo) {
        return std::nullopt;
      }
      // The pairs where both profiles saw some of the scene.
      const float *a = before.data() + lo;
      const float *b = after.data() + (static_cast<std::ptrdiff_t>(lo) + s);
      const auto n_pairs = hi - lo;
      std::size_t count = 0;
      float sa = 0.0f;
      float sb = 0.0f;
      for (std::size_t i = 0; i < n_pairs; ++i) {
        if (!std::isnan(a[i]) && !std::isnan(b[i])) {
          count += 1;
          sa += a[i];
          sb += b[i];
        }
      }
      if (count * 10 < (to - from) * 6) {
        return std::nullopt;
      }
      const auto n = static_cast<float>(count);
      const float ma = sa / n;
      const float mb = sb / n;
      float ab = 0.0f;
      float aa = 0.0f;
      float bb = 0.0f;
      for (std::size_t i = 0; i < n_pairs; ++i) {
        if (!std::isnan(a[i]) && !std::isnan(b[i])) {
          const float da = a[i] - ma;
          const float db = b[i] - mb;
          ab += da * db;
          aa += da * da;
          bb += db * db;
        }
      }
      if (aa <= std::numeric_limits<float>::epsilon() || bb <= std::numeric_limits<float>::epsilon()) {
        return std::nullopt;
      }
      return ab / std::sqrt(aa * bb);
    }

    /**
     * The sub-pixel shift, from `start` by gradient steps (Lucas-Kanade on the profiles): a parabola
     * through three correlation samples pulls shifts under half a pixel toward the nearest whole one,
     * which would teach the model too small a gain for slow turns. Stays within a pixel of `whole`,
     * the best whole shift, and keeps `start` when the profile has no slope to follow.
     */
    float refine(std::span<const float> before, std::span<const float> after, std::size_t from, std::size_t to, float start, float whole) {
      // `after` at fractional index `x`, linearly interpolated; NaN outside or where unknown.
      const auto sample = [&](float x) -> float {
        if (x < 0.0f || x > static_cast<float>(after.size() - 1)) {
          return nan_value;
        }
        const auto i = std::min(static_cast<std::size_t>(std::floor(x)), after.size() - 1);
        const auto j = std::min(i + 1, after.size() - 1);
        const float t = x - static_cast<float>(i);
        return after[i] * (1.0f - t) + after[j] * t;
      };
      float position = start;
      for (int iteration = 0; iteration < 4; ++iteration) {
        float num = 0.0f;
        float den = 0.0f;
        const auto end = std::min(to, before.size());
        for (std::size_t i = from; i < end; ++i) {
          const float a = before[i];
          const float x = static_cast<float>(i) + position;
          const float b = sample(x);
          const float slope = 0.5f * (sample(x + 1.0f) - sample(x - 1.0f));
          if (std::isnan(a) || std::isnan(b) || std::isnan(slope)) {
            continue;
          }
          num += slope * (a - b);
          den += slope * slope;
        }
        if (den <= std::numeric_limits<float>::epsilon()) {
          return start;
        }
        const float step = std::clamp(num / den, -0.5f, 0.5f);
        position = std::clamp(position + step, whole - 1.0f, whole + 1.0f);
        if (std::abs(step) < 0.005f) {
          break;
        }
      }
      return position;
    }

    /**
     * The shift of `before[from..to]` within `after`, searched over `-search..=search`, by
     * normalised correlation and refined to sub-pixel by a parabola through the peak. None for a
     * flat cell, a weak or an ambiguous peak. `across` is where across `before` its samples' pixels
     * lie; `scores` is scratch.
     */
    std::optional<shift_t> best_shift(
      std::span<const float> before,
      std::span<const float> across,
      std::span<const float> after,
      std::size_t from,
      std::size_t to,
      std::size_t search,
      std::vector<std::optional<float>> &scores
    ) {
      if (to < from) {
        return std::nullopt;
      }
      const auto len = to - from;
      std::size_t count = 0;
      float total = 0.0f;
      for (std::size_t i = from; i < to; ++i) {
        if (!std::isnan(before[i])) {
          count += 1;
          total += before[i];
        }
      }
      if (len < 8 || count < 8) {
        return std::nullopt;
      }
      const float mean = total / static_cast<float>(count);
      float spread = 0.0f;
      for (std::size_t i = from; i < to; ++i) {
        if (!std::isnan(before[i])) {
          spread += (before[i] - mean) * (before[i] - mean);
        }
      }
      const float variance = spread / static_cast<float>(count);
      if (variance < min_variance) {
        return std::nullopt;
      }
      const auto reach = static_cast<std::ptrdiff_t>(search);
      scores.clear();
      for (std::ptrdiff_t s = -reach; s <= reach; ++s) {
        scores.push_back(correlation(before, after, from, to, s));
      }
      // The last of equal maxima, as `Iterator::max_by` has it.
      std::optional<std::pair<std::size_t, float>> top;
      for (std::size_t i = 0; i < scores.size(); ++i) {
        if (scores[i] && (!top || *scores[i] >= top->second)) {
          top = std::pair {i, *scores[i]};
        }
      }
      if (!top) {
        return std::nullopt;
      }
      const auto [best, peak] = *top;
      if (peak < min_correlation) {
        return std::nullopt;
      }
      float runner_up = -std::numeric_limits<float>::infinity();
      for (std::size_t i = 0; i < scores.size(); ++i) {
        const auto distance = i > best ? i - best : best - i;
        if (distance > 2 && scores[i]) {
          runner_up = std::max(runner_up, *scores[i]);
        }
      }
      if (peak - runner_up < min_uniqueness) {
        return std::nullopt;
      }
      // A peak at the edge of the search is not a peak.
      if (best == 0 || best + 1 == scores.size()) {
        return std::nullopt;
      }
      if (!scores[best - 1] || !scores[best + 1]) {
        return std::nullopt;
      }
      const float l = *scores[best - 1];
      const float r = *scores[best + 1];
      const float curvature = l - 2.0f * peak + r;
      const float offset = curvature < 0.0f ? std::clamp(0.5f * (l - r) / curvature, -0.5f, 0.5f) : 0.0f;
      const float whole = static_cast<float>(best) - static_cast<float>(search);
      shift_t out;
      out.at = refine(before, after, from, to, whole + offset, whole);
      out.correlation = peak;
      out.place = place_of(before, across, from, to);
      return out;
    }

    /// The vertical shift of each cell in column band `band`: `before` (the previous thumbnail's
    /// row profile over the band) found in the current one's, its columns taken `column_shift` across.
    template<class S>
    std::array<std::optional<shift_t>, grid_edge> vertical_band(
      const thumbnail_t &cur,
      S scene,
      const grid_t &grid,
      std::size_t band,
      std::span<const float> before,
      std::span<const float> before_across,
      std::ptrdiff_t column_shift,
      scratch_t &scratch
    ) {
      profile_rows_impl(cur, grid.x_edges[band], grid.x_edges[band + 1], column_shift, scene, scratch.lines, scratch.after);
      std::array<std::optional<shift_t>, grid_edge> out {};
      for (std::size_t cell = 0; cell < grid_edge; ++cell) {
        out[cell] = best_shift(before, before_across, scratch.after, grid.y_edges[cell], grid.y_edges[cell + 1], grid.search_y, scratch.scores);
      }
      return out;
    }

    /// How many grid cells stand still: their texture in `prev` where `scene` is at least
    /// `still_share` exactly the same in `cur`.
    template<class S>
    std::size_t still_cells(const thumbnail_t &prev, const thumbnail_t &cur, S scene, const grid_t &grid) {
      const auto w = prev.width;
      const auto texture = [&](std::size_t x, std::size_t y) {
        const auto v = prev.luma[y * w + x];
        const auto right = prev.luma[y * w + std::min(x + 1, w - 1)];
        const auto below = prev.luma[std::min(y + 1, prev.height - 1) * w + x];
        const auto diff = [](std::uint8_t a, std::uint8_t b) {
          return static_cast<std::uint8_t>(a > b ? a - b : b - a);
        };
        return diff(v, right) >= texture_step && diff(v, below) >= texture_step;
      };
      std::size_t still = 0;
      for (std::size_t row = 0; row < grid_edge; ++row) {
        for (std::size_t column = 0; column < grid_edge; ++column) {
          std::size_t textured = 0;
          std::size_t unchanged = 0;
          for (std::size_t y = grid.y_edges[row]; y < grid.y_edges[row + 1]; ++y) {
            for (std::size_t x = grid.x_edges[column]; x < grid.x_edges[column + 1]; ++x) {
              if (scene(x, y) && texture(x, y)) {
                textured += 1;
                unchanged += prev.luma[y * w + x] == cur.luma[y * w + x] ? 1 : 0;
              }
            }
          }
          if (textured >= min_texture && static_cast<float>(unchanged) >= static_cast<float>(textured) * still_share) {
            still += 1;
          }
        }
      }
      return still;
    }

    /// `estimate_around` where `scene` is the picture (not the HUD).
    template<class S>
    std::optional<motion_t> measure(const thumbnail_t &prev, const thumbnail_t &cur, S scene, scratch_t &scratch) {
      const auto w = cur.width;
      const auto h = cur.height;
      const grid_t grid {
        edges(w),
        edges(h),
        std::max<std::size_t>(static_cast<std::size_t>(static_cast<float>(w) * search_share), 2),
        std::max<std::size_t>(static_cast<std::size_t>(static_cast<float>(h) * search_share), 2),
      };
      const auto &x_edges = grid.x_edges;
      const auto &y_edges = grid.y_edges;

      // A picture whose texture (the HUD left out) is mostly unchanged stands still, whatever moves
      // in one corner of it.
      const auto still = still_cells(prev, cur, scene, grid);
      if (still * 2 > grid_cells) {
        motion_t out;
        out.dx = 0.0f;
        out.dy = 0.0f;
        out.confidence = static_cast<float>(still) / static_cast<float>(grid_cells);
        return out;
      }

      // Vertical first: one row profile per column band, full height, correlated per row band.
      // Then horizontal: one column profile per row band, the current one taken that band's vertical
      // shift lower so a tilt does not blur it. Then vertical again, the current column band taken
      // that band's horizontal shift across, so a fast pan does not blur it.
      for (std::size_t band = 0; band < grid_edge; ++band) {
        profile_rows_impl(prev, x_edges[band], x_edges[band + 1], 0, scene, scratch.lines, scratch.before_rows[band]);
        scratch.lines.across(scratch.before_across[band]);
      }
      std::array<std::array<std::optional<shift_t>, grid_edge>, grid_edge> first {};
      for (std::size_t band = 0; band < grid_edge; ++band) {
        first[band] = vertical_band(cur, scene, grid, band, scratch.before_rows[band], scratch.before_across[band], 0, scratch);
      }
      std::array<std::array<std::optional<shift_t>, grid_edge>, grid_edge> horizontal {};
      for (std::size_t cell = 0; cell < grid_edge; ++cell) {
        const auto y0 = y_edges[cell];
        const auto y1 = y_edges[cell + 1];
        std::vector<float> shifts;
        for (std::size_t band = 0; band < grid_edge; ++band) {
          if (first[band][cell]) {
            shifts.push_back(first[band][cell]->at);
          }
        }
        const auto shift_y = static_cast<std::ptrdiff_t>(std::round(median(shifts).value_or(0.0f)));
        profile_columns_impl(prev, y0, y1, 0, scene, scratch.lines, scratch.before);
        scratch.lines.across(scratch.across);
        profile_columns_impl(cur, y0, y1, shift_y, scene, scratch.lines, scratch.after);
        for (std::size_t band = 0; band < grid_edge; ++band) {
          horizontal[band][cell] = best_shift(scratch.before, scratch.across, scratch.after, x_edges[band], x_edges[band + 1], grid.search_x, scratch.scores);
        }
      }
      std::array<std::ptrdiff_t, grid_edge> column_shift {};
      for (std::size_t band = 0; band < grid_edge; ++band) {
        std::vector<float> shifts;
        for (const auto &s : horizontal[band]) {
          if (s) {
            shifts.push_back(s->at);
          }
        }
        column_shift[band] = static_cast<std::ptrdiff_t>(std::round(median(shifts).value_or(0.0f)));
      }
      // A band that did not move across measures as it did the first time.
      auto vertical = first;
      for (std::size_t band = 0; band < grid_edge; ++band) {
        if (column_shift[band] != 0) {
          vertical[band] = vertical_band(cur, scene, grid, band, scratch.before_rows[band], scratch.before_across[band], column_shift[band], scratch);
        }
      }
      struct found_t {
        float dx;
        float dy;
        float correlation;
      };
      std::vector<found_t> found;
      found.reserve(grid_cells);
      std::array<std::optional<cell_shift_t>, grid_cells> each {};
      for (std::size_t band = 0; band < grid_edge; ++band) {
        for (std::size_t cell = 0; cell < grid_edge; ++cell) {
          const auto &x = horizontal[band][cell];
          const auto &y = vertical[band][cell];
          if (x && y) {
            const float corr = std::min(x->correlation, y->correlation);
            found.push_back({x->at, y->at, corr});
            cell_shift_t shift;
            shift.dx = x->at;
            shift.dy = y->at;
            shift.correlation = corr;
            if (x->place && y->place) {
              shift.place = std::array<place_t, 2> {*x->place, *y->place};
            }
            each[cell * grid_edge + band] = shift;
          }
        }
      }
      if (found.size() < 3) {
        return std::nullopt;
      }
      std::vector<float> xs;
      std::vector<float> ys;
      for (const auto &c : found) {
        xs.push_back(c.dx);
        ys.push_back(c.dy);
      }
      const auto dx = median(xs);
      const auto dy = median(ys);
      if (!dx || !dy) {
        return std::nullopt;
      }
      std::size_t agreeing = 0;
      float correlation_sum = 0.0f;
      for (const auto &c : found) {
        if (std::abs(c.dx - *dx) <= agree_distance && std::abs(c.dy - *dy) <= agree_distance) {
          agreeing += 1;
          correlation_sum += c.correlation;
        }
      }
      if (agreeing < 3) {
        return std::nullopt;
      }
      const float mean_correlation = correlation_sum / static_cast<float>(agreeing);
      const float confidence = (static_cast<float>(agreeing) / static_cast<float>(grid_cells)) * mean_correlation;
      motion_t out;
      out.dx = *dx;
      out.dy = *dy;
      out.confidence = std::clamp(confidence, 0.0f, 1.0f);
      out.cells = each;
      return out;
    }

    /// The thumbnail at a place between its pixels; none outside it.
    std::optional<float> sample(const thumbnail_t &t, float x, float y) {
      if (x < 0.0f || y < 0.0f || x > static_cast<float>(t.width - 1) || y > static_cast<float>(t.height - 1)) {
        return std::nullopt;
      }
      const auto x0 = static_cast<std::size_t>(x);
      const auto y0 = static_cast<std::size_t>(y);
      const auto x1 = std::min(x0 + 1, t.width - 1);
      const auto y1 = std::min(y0 + 1, t.height - 1);
      const float fx = x - static_cast<float>(x0);
      const float fy = y - static_cast<float>(y0);
      const float top = at(t, x0, y0) * (1.0f - fx) + at(t, x1, y0) * fx;
      const float bottom = at(t, x0, y1) * (1.0f - fx) + at(t, x1, y1) * fx;
      return top * (1.0f - fy) + bottom * fy;
    }
  }  // namespace

  void lines_t::across(std::vector<float> &out) const {
    out.clear();
    for (const auto &s : sums) {
      out.push_back(s.n == 0 ? nan_value : s.across / static_cast<float>(s.n));
    }
  }

  motion_t motion_t::uniform(float dx, float dy, float confidence) {
    motion_t out;
    out.dx = dx;
    out.dy = dy;
    out.confidence = confidence;
    cell_shift_t cell;
    cell.dx = dx;
    cell.dy = dy;
    cell.correlation = confidence;
    out.cells.fill(cell);
    return out;
  }

  void detrend(std::span<float> profile, std::vector<float> &prefix, std::vector<std::uint32_t> &count) {
    constexpr std::size_t radius = 6;
    const auto n = profile.size();
    prefix.clear();
    count.clear();
    prefix.push_back(0.0f);
    count.push_back(0);
    for (const float v : profile) {
      const bool known = !std::isnan(v);
      prefix.push_back(prefix.back() + (known ? v : 0.0f));
      count.push_back(count.back() + (known ? 1u : 0u));
    }
    // Each sample's average comes from the running sums, not its neighbours, so the profile can take
    // the result as it goes.
    for (std::size_t i = 0; i < n; ++i) {
      const auto a = i > radius ? i - radius : std::size_t {0};
      const auto b = std::min(i + radius + 1, n);
      const auto known = count[b] - count[a];
      if (static_cast<std::size_t>(known) == b - a) {
        profile[i] = profile[i] - (prefix[b] - prefix[a]) / static_cast<float>(known);
      } else {
        profile[i] = nan_value;
      }
    }
  }

  void profile_rows(const thumbnail_t &t, std::size_t x0, std::size_t x1, std::ptrdiff_t shift, const std::function<bool(std::size_t, std::size_t)> &scene, lines_t &lines, std::vector<float> &out) {
    profile_rows_impl(t, x0, x1, shift, scene, lines, out);
  }

  std::optional<place_t> place_of(std::span<const float> before, std::span<const float> across, std::size_t from, std::size_t to) {
    const float middle = static_cast<float>(before.size()) * 0.5f;
    std::array<double, 5> sums {};
    const auto end = std::min(to, before.size() > 0 ? before.size() - 1 : std::size_t {0});
    for (std::size_t i = std::max<std::size_t>(from, 1); i < end; ++i) {
      const float slope = before[i + 1] - before[i - 1];
      const float beside_value = i < across.size() ? across[i] : nan_value;
      if (std::isnan(slope) || std::isnan(beside_value)) {
        continue;
      }
      const double weight = static_cast<double>(slope * slope);
      const double along = static_cast<double>(static_cast<float>(i) + 0.5f - middle);
      const double beside = static_cast<double>(beside_value);
      const std::array<double, 5> terms {1.0, along, along * along, beside, along * beside};
      for (std::size_t k = 0; k < sums.size(); ++k) {
        sums[k] += weight * terms[k];
      }
    }
    const double weight = sums[0];
    if (!(weight > static_cast<double>(std::numeric_limits<float>::epsilon()))) {
      return std::nullopt;
    }
    place_t out;
    out.along = static_cast<float>(sums[1] / weight);
    out.squared = static_cast<float>(sums[2] / weight);
    out.across = static_cast<float>(sums[3] / weight);
    out.cross = static_cast<float>(sums[4] / weight);
    return out;
  }

  std::array<std::pair<std::size_t, std::size_t>, grid_edge> cell_spans(std::size_t len) {
    const auto e = edges(len);
    std::array<std::pair<std::size_t, std::size_t>, grid_edge> out {};
    for (std::size_t i = 0; i < grid_edge; ++i) {
      out[i] = {e[i], e[i + 1]};
    }
    return out;
  }

  std::optional<motion_t> estimate(const thumbnail_t &prev, const thumbnail_t &cur) {
    return estimate_around(prev, cur, {});
  }

  std::optional<motion_t> estimate_around(const thumbnail_t &prev, const thumbnail_t &cur, std::span<const std::uint8_t> hud) {
    scratch_t scratch;
    return estimate_with(prev, cur, hud, scratch);
  }

  std::optional<motion_t> estimate_with(const thumbnail_t &prev, const thumbnail_t &cur, std::span<const std::uint8_t> hud, scratch_t &scratch) {
    if (!valid(prev) || !valid(cur) || prev.width != cur.width || prev.height != cur.height) {
      return std::nullopt;
    }
    const auto w = cur.width;
    // One branch here, so the loops inline the test (or drop it without a HUD).
    if (hud.size() != cur.width * cur.height) {
      return measure(prev, cur, [](std::size_t, std::size_t) {
        return true;
      }, scratch);
    }
    return measure(prev, cur, [hud, w](std::size_t x, std::size_t y) {
      return hud[y * w + x] == 0;
    }, scratch);
  }

  bool still_t::observe(const thumbnail_t &prev, const thumbnail_t &cur, const motion_t &motion) {
    const auto w = cur.width;
    const auto h = cur.height;
    if (!valid(prev) || !valid(cur) || prev.width != w || prev.height != h) {
      return false;
    }
    if (size_ != std::pair {w, h}) {
      size_ = {w, h};
      pixels_.clear();
      for (const auto value : prev.luma) {
        pixels_.emplace_back(value, 0.0f);
      }
      mask_.assign(w * h, 0);
      marked_ = 0;
    }
    const auto x_edges = edges(w);
    const auto y_edges = edges(h);
    // A cell's pixels, and the margin beside an outer cell with it.
    const auto span_of = [](const std::array<std::size_t, grid_edge + 1> &e, std::size_t cell, std::size_t len) {
      const auto from = cell == 0 ? std::size_t {0} : e[cell];
      const auto to = cell + 1 == grid_edge ? len : e[cell + 1];
      return std::pair {from, to};
    };
    bool changed = false;
    for (std::size_t row = 0; row < grid_edge; ++row) {
      for (std::size_t column = 0; column < grid_edge; ++column) {
        // The cell's own shift while it goes with the picture's: a lens moves a corner up to half
        // again as far as the median cell. A cell that is mostly HUD finds no shift, or the HUD's,
        // which is none.
        const float whole = std::hypot(motion.dx, motion.dy);
        float dx = motion.dx;
        float dy = motion.dy;
        const auto &cell = motion.cells[row * grid_edge + column];
        if (cell && std::hypot(cell->dx - motion.dx, cell->dy - motion.dy) <= 0.5f + 0.6f * whole) {
          dx = cell->dx;
          dy = cell->dy;
        }
        const bool moved = std::hypot(dx, dy) >= still_min_move;
        const auto [y_from, y_to] = span_of(y_edges, row, h);
        const auto [x_from, x_to] = span_of(x_edges, column, w);
        for (std::size_t y = y_from; y < y_to; ++y) {
          for (std::size_t x = x_from; x < x_to; ++x) {
            const auto index = y * w + x;
            auto &[was, should] = pixels_[index];
            const bool stood = std::abs(should) >= still_total;
            const auto now = cur.luma[index];
            const auto change = static_cast<std::uint8_t>(was > now ? was - now : now - was);
            if (change > still_change) {
              was = now;
              should = 0.0f;
            } else if (moved) {
              if (const auto brought = sample(prev, static_cast<float>(x) - dx, static_cast<float>(y) - dy)) {
                const float step = *brought - static_cast<float>(prev.luma[index]);
                if (std::abs(step) >= still_step) {
                  constexpr float most = still_total * 0.25f;
                  should += std::clamp(step, -most, most);
                }
              }
            }
            changed = changed || (stood != (std::abs(should) >= still_total));
          }
        }
      }
    }
    if (changed) {
      std::fill(mask_.begin(), mask_.end(), std::uint8_t {0});
      marked_ = 0;
      for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
          if (std::abs(pixels_[y * w + x].second) >= still_total) {
            marked_ += 1;
            const auto row_from = y > 0 ? y - 1 : std::size_t {0};
            const auto row_to = std::min(y + 1, h - 1);
            const auto col_from = x > 0 ? x - 1 : std::size_t {0};
            const auto col_to = std::min(x + 1, w - 1);
            for (auto row = row_from; row <= row_to; ++row) {
              std::fill(mask_.begin() + static_cast<std::ptrdiff_t>(row * w + col_from), mask_.begin() + static_cast<std::ptrdiff_t>(row * w + col_to + 1), std::uint8_t {1});
            }
          }
        }
      }
    }
    return changed;
  }

  std::span<const std::uint8_t> still_t::mask() const {
    if (marked_ == 0) {
      return {};
    }
    return {mask_.data(), mask_.size()};
  }

  void still_t::clear() {
    *this = still_t {};
  }

  std::optional<std::vector<std::uint8_t>> union_of(std::span<const std::uint8_t> hud, std::span<const std::uint8_t> still) {
    if (hud.empty() || still.empty() || hud.size() != still.size()) {
      return std::nullopt;
    }
    std::vector<std::uint8_t> out(hud.size());
    for (std::size_t i = 0; i < hud.size(); ++i) {
      out[i] = (hud[i] != 0 || still[i] != 0) ? 1 : 0;
    }
    return out;
  }

  measured_t measurer_t::measure(const thumbnail_t &thumbnail) {
    measured_t out;
    out.size = {thumbnail.width, thumbnail.height};
    out.at_us = thumbnail.at_us;
    if (previous_ && thumbnail.at_us > previous_->at_us) {
      std::span<const std::uint8_t> left_out;
      if (both_) {
        left_out = *both_;
      } else if (!still_.mask().empty()) {
        left_out = still_.mask();
      } else if (hud_) {
        left_out = *hud_;
      }
      const auto motion = estimate_with(*previous_, thumbnail, left_out, scratch_);
      if (motion) {
        if (still_.observe(*previous_, thumbnail, *motion)) {
          both_ = union_of(hud_ ? std::span<const std::uint8_t> {*hud_} : std::span<const std::uint8_t> {}, still_.mask());
        }
        out.shift = std::pair {previous_->at_us, *motion};
      }
    }
    return out;
  }

  void measurer_t::keep(thumbnail_t &&thumbnail, bool keep) {
    if (keep) {
      previous_ = std::move(thumbnail);
    } else {
      previous_.reset();
    }
  }

  void measurer_t::set_hud(std::vector<std::uint8_t> hud) {
    both_ = union_of(hud, still_.mask());
    hud_ = std::move(hud);
  }

  void measurer_t::reset_thumbnail() {
    previous_.reset();
  }

  void measurer_t::reset_hud() {
    hud_.reset();
    both_.reset();
  }

}  // namespace frame_warp
