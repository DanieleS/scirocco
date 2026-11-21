/**
 * @file src/frame_warp_host.cpp
 * @brief Definitions for src/frame_warp.h.
 */
// standard includes
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

// local includes
#include "frame_warp.h"

namespace frame_warp {

  namespace {
    std::uint64_t saturating_sub(std::uint64_t a, std::uint64_t b) {
      return a > b ? a - b : 0;
    }

    /// Along one axis, the grid cells a mask cell takes (`placement_t::place`): mask cell `cell` of
    /// `cells` over `picture` pixels, and `grid` cells over the `length` pixels from `start`. None
    /// when the cell reaches past them, into a bar. The range is the first cell and the one past the
    /// last.
    std::optional<std::pair<std::uint16_t, std::uint16_t>> cells_of(
      std::uint16_t cell_index,
      std::uint16_t cell_count,
      std::uint16_t picture_size,
      std::uint16_t start_pixel,
      std::uint16_t length_pixels,
      std::uint16_t grid_count
    ) {
      const std::uint64_t cell = cell_index;
      const std::uint64_t cells = cell_count;
      const std::uint64_t picture = picture_size;
      const std::uint64_t start = start_pixel;
      const std::uint64_t length = length_pixels;
      const std::uint64_t grid = grid_count;
      const std::uint64_t a = cell * picture / cells;
      const std::uint64_t b = (cell + 1) * picture / cells;
      if (a < start || b > start + length) {
        return std::nullopt;
      }
      // Grid cell k's centre is (2k + 1) * length / (2 * grid) pixels from the start: the first whose
      // centre is at `p` or past it.
      const auto first = [&](std::uint64_t p) {
        const std::uint64_t scaled = 2 * grid * (p - start);
        const std::uint64_t over = scaled > length ? scaled - length : 0;
        const std::uint64_t index = (over + 2 * length - 1) / (2 * length);
        return std::min(index, grid);
      };
      std::uint64_t low = first(a);
      std::uint64_t high = first(b);
      if (!(low < high)) {
        const std::uint64_t at = std::min((a + b - 2 * start) * grid / (2 * length), grid - 1);
        low = at;
        high = at + 1;
      }
      return std::pair {static_cast<std::uint16_t>(low), static_cast<std::uint16_t>(high)};
    }

    /// Calls `put` for each cell of `line` without `value` that has a cell with it within `reach`.
    template<class F>
    void spread(const std::vector<std::uint8_t> &line, std::size_t reach, bool value, F put) {
      const auto n = line.size();
      const auto is = [&](std::size_t i) {
        return (line[i] != 0) == value;
      };
      // How many cells of the window [i - reach, i + reach] have `value`.
      std::size_t count = 0;
      for (std::size_t i = 0; i < std::min(reach, n); ++i) {
        if (is(i)) {
          count += 1;
        }
      }
      for (std::size_t i = 0; i < n; ++i) {
        if (i + reach < n && is(i + reach)) {
          count += 1;
        }
        if (i > reach && is(i - reach - 1)) {
          count -= 1;
        }
        if (count > 0 && !is(i)) {
          put(i);
        }
      }
    }

    /// Spreads `value` from every cell that has it to the cells within `reach` (a square), `width`
    /// cells to a row: grows the set cells, or with `false` shrinks them. Nothing spreads from past
    /// the edges, so a HUD at an edge keeps its size.
    void grow(std::vector<std::uint8_t> &cells, std::size_t width, std::size_t reach, bool value) {
      if (width == 0) {
        return;
      }
      const std::uint8_t byte = value ? 1 : 0;
      const auto height = cells.size() / width;
      std::vector<std::uint8_t> line;
      for (std::size_t y = 0; y < height; ++y) {
        auto row = cells.begin() + static_cast<std::ptrdiff_t>(y * width);
        line.assign(row, row + static_cast<std::ptrdiff_t>(width));
        spread(line, reach, value, [&](std::size_t x) {
          row[static_cast<std::ptrdiff_t>(x)] = byte;
        });
      }
      // Down the columns a row at a time, which reads memory in order: per column, how many cells of
      // the window [y - reach, y + reach] had `value` before this pass.
      const auto before = cells;
      std::vector<std::size_t> counts(width, 0);
      const auto count = [&](std::size_t y, bool add) {
        for (std::size_t x = 0; x < width; ++x) {
          if ((before[y * width + x] != 0) == value) {
            if (add) {
              counts[x] += 1;
            } else {
              counts[x] -= 1;
            }
          }
        }
      };
      for (std::size_t y = 0; y < std::min(reach, height); ++y) {
        count(y, true);
      }
      for (std::size_t y = 0; y < height; ++y) {
        if (y + reach < height) {
          count(y + reach, true);
        }
        if (y > reach) {
          count(y - reach - 1, false);
        }
        for (std::size_t x = 0; x < width; ++x) {
          if (counts[x] > 0) {
            cells[y * width + x] = byte;
          }
        }
      }
    }
  }  // namespace

  mask_t placement_t::place(const mask_t &grid) const {
    const std::uint16_t x0 = content.x;
    const std::uint16_t y0 = content.y;
    const std::uint16_t cw = content.width;
    const std::uint16_t ch = content.height;
    const std::uint16_t mw = mask.first;
    const std::uint16_t mh = mask.second;
    if (x0 == 0 && y0 == 0 && cw == picture.first && ch == picture.second && grid.width == mw && grid.height == mh) {
      return grid;
    }
    mask_t out(grid.generation, mw, mh);
    if (grid.width == 0 || grid.height == 0 || cw == 0 || ch == 0) {
      return out;
    }
    std::vector<std::optional<std::pair<std::uint16_t, std::uint16_t>>> across;
    across.reserve(mw);
    for (std::uint16_t x = 0; x < mw; ++x) {
      across.push_back(cells_of(x, mw, picture.first, x0, cw, grid.width));
    }
    std::vector<std::uint8_t> row(grid.width, 0);
    for (std::uint16_t y = 0; y < mh; ++y) {
      const auto rows = cells_of(y, mh, picture.second, y0, ch, grid.height);
      if (!rows) {
        for (std::uint16_t x = 0; x < mw; ++x) {
          out.set(x, y, true);
        }
        continue;
      }
      for (std::uint16_t gx = 0; gx < grid.width; ++gx) {
        bool hud = false;
        for (auto gy = rows->first; gy < rows->second && !hud; ++gy) {
          hud = grid.get(gx, gy);
        }
        row[gx] = hud ? 1 : 0;
      }
      for (std::uint16_t x = 0; x < mw; ++x) {
        bool hud = true;
        if (const auto &columns = across[x]) {
          hud = false;
          for (auto gx = columns->first; gx < columns->second && !hud; ++gx) {
            hud = row[gx] != 0;
          }
        }
        if (hud) {
          out.set(x, y, true);
        }
      }
    }
    return out;
  }

  mask_t shape(const mask_t &mask) {
    const std::size_t width = mask.width;
    std::vector<std::uint8_t> cells;
    cells.reserve(width * mask.height);
    bool any = false;
    for (std::uint16_t y = 0; y < mask.height; ++y) {
      for (std::uint16_t x = 0; x < mask.width; ++x) {
        const bool set = mask.get(x, y);
        any = any || set;
        cells.push_back(set ? 1 : 0);
      }
    }
    const auto join = static_cast<std::size_t>(std::round(static_cast<float>(mask.height) * join_height));
    if (join > 0 && any) {
      // A closing: grown, then shrunk back, which fills the gaps it closed.
      grow(cells, width, join, true);
      grow(cells, width, join, false);
    }
    grow(cells, width, 1, true);
    mask_t out(mask.generation, mask.width, mask.height);
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (cells[i] != 0) {
        out.set(static_cast<std::uint16_t>(i % width), static_cast<std::uint16_t>(i / width), true);
      }
    }
    return out;
  }

  std::vector<std::uint8_t> hud_pixels(const mask_t &mask, std::pair<std::size_t, std::size_t> size) {
    const auto [width, height] = size;
    const std::size_t mw = mask.width;
    const std::size_t mh = mask.height;
    std::vector<std::uint8_t> out(width * height, 0);
    if (mw == 0 || mh == 0) {
      return out;
    }
    for (std::size_t y = 0; y < height; ++y) {
      const auto cy = std::min((2 * y + 1) * mh / (2 * height), mh - 1);
      for (std::size_t x = 0; x < width; ++x) {
        const auto cx = std::min((2 * x + 1) * mw / (2 * width), mw - 1);
        out[y * width + x] = mask.get(static_cast<std::uint16_t>(cx), static_cast<std::uint16_t>(cy)) ? 1 : 0;
      }
    }
    return out;
  }

  void mask_tracker_t::resend() {
    if (sent_) {
      pending_ = sent_->encode();
    }
  }

  void mask_tracker_t::clear_pending() {
    pending_.reset();
  }

  void mask_tracker_t::offer(const mask_t &shaped, std::uint16_t max_width, std::uint64_t now_us) {
    mask_t mask = shaped;
    while (mask.width > std::max<std::uint16_t>(max_width, 1)) {
      mask = mask.halved();
    }
    if (sent_) {
      if (sent_at_us_ && saturating_sub(now_us, *sent_at_us_) < mask_every_us) {
        return;
      }
      if (sent_->width == mask.width && sent_->height == mask.height) {
        std::size_t changed = 0;
        const auto n = std::min(sent_->words.size(), mask.words.size());
        for (std::size_t i = 0; i < n; ++i) {
          changed += static_cast<std::size_t>(std::popcount(sent_->words[i] ^ mask.words[i]));
        }
        if (changed < mask_change) {
          return;
        }
      }
    } else if (mask.is_clear()) {
      return;
    }
    generation_ = static_cast<std::uint16_t>(generation_ + 1);
    mask.generation = generation_;
    auto wire = mask.encode();
    while (!wire && mask.width > 1) {
      mask = mask.halved();
      wire = mask.encode();
    }
    if (!wire) {
      return;
    }
    pending_ = std::move(wire);
    sent_ = std::move(mask);
    sent_at_us_ = now_us;
  }

  host_t::host_t(std::uint16_t picture_width, std::uint16_t picture_height):
      history_ {std::nullopt},
      calibrator_ {{picture_width, picture_height}, {thumbnail_width, thumbnail_height(picture_width, picture_height)}},
      picture_ {picture_width, picture_height},
      content_ {0, 0, picture_width, picture_height},
      thumbnail_ {thumbnail_width, thumbnail_height(picture_width, picture_height)} {
  }

  std::optional<bool> host_t::on_request(std::span<const std::uint8_t> payload) {
    const auto request = request_t::decode(payload);
    if (!request) {
      return std::nullopt;
    }
    const bool starting = request->enabled && !enabled_;
    enabled_ = request->enabled;
    lens_requested_ = request->lens;
    mask_max_width_ = request->mask_max_width;
    calibrator_.set_lens_drawn(request->lens);
    if (starting) {
      // Both sides count input from the request that starts Frame Warp; a repeated request keeps
      // the counts.
      history_ = history_t {request->controller};
      calibrator_.clear_samples();
      recent_.clear();
      shifts_.clear();
      measurer_.reset_thumbnail();
    } else {
      history_.set_controller(request->controller);
    }
    if (enabled_) {
      // The client counts from its request on; so does the model it hears first.
      model_ = calibrator_.model(calibrator_generation());
      mask_.resend();
    } else {
      tags_.clear();
      model_.reset();
      mask_.clear_pending();
    }
    return enabled_;
  }

  std::uint16_t host_t::calibrator_generation() const {
    return last_model_ ? last_model_->generation : std::uint16_t {0};
  }

  void host_t::set_picture(std::uint16_t width, std::uint16_t height, rect_t content) {
    const std::pair<std::uint16_t, std::uint16_t> picture {width, height};
    if (picture == picture_ && content == content_) {
      return;
    }
    picture_ = picture;
    content_ = content;
    thumbnail_ = {thumbnail_width, thumbnail_height(content.width, content.height)};
    calibrator_.set_sizes(picture, {content.width, content.height}, thumbnail_);
    measurer_.reset_thumbnail();
    measurer_.reset_hud();
  }

  void host_t::set_prior(const prior_t &prior) {
    calibrator_.set_prior(prior);
    lens_.set_prior(calibrator_.lens());
    // What this stream's own turns have already said stands.
    apply_lens();
  }

  void host_t::apply_lens() {
    calibrator_.set_lens(lens_.tangent());
  }

  std::optional<prior_t> host_t::prior() const {
    return calibrator_.prior();
  }

  void host_t::on_mouse(std::int16_t dx, std::int16_t dy, std::uint64_t at_us) {
    if (enabled_) {
      history_.on_mouse(dx, dy, at_us);
    }
  }

  void host_t::on_controller(std::uint16_t number, std::int16_t right_x, std::int16_t right_y, std::uint64_t at_us) {
    if (enabled_) {
      history_.on_controller(number, right_x, right_y, at_us);
    }
  }

  void host_t::on_thumbnail(thumbnail_t &&thumbnail, std::uint64_t now_us) {
    if (!enabled_) {
      measurer_.keep(std::move(thumbnail), false);
      return;
    }
    const auto measured = measurer_.measure(thumbnail);
    bool reset = false;
    if (measured.size != thumbnail_) {
      thumbnail_ = measured.size;
      calibrator_.set_sizes(picture_, {content_.width, content_.height}, thumbnail_);
      shifts_.clear();
      reset = true;
    }
    if (!reset && measured.shift) {
      const auto t0_us = measured.shift->first;
      const auto &motion = measured.shift->second;
      samples_ += 1;
      if (turning(t0_us, measured.at_us) && lens_.observe(calibrator_.layout(), motion)) {
        apply_lens();
      }
      const auto turn = calibrator_.resolve(motion);
      if (turn.confidence >= motion_confidence) {
        add_shift(t0_us, measured.at_us, turn.dx, turn.dy, now_us);
      }
      sample_t sample;
      sample.t0_us = t0_us;
      sample.t1_us = measured.at_us;
      sample.motion = motion;
      calibrator_.add(sample);
    }
    measurer_.keep(std::move(thumbnail), true);
    if (const auto model = calibrator_.update(history_, now_us)) {
      last_model_ = model;
      model_ = model;
    }
  }

  bool host_t::turning(std::uint64_t t0_us, std::uint64_t t1_us) const {
    const auto span = [&](std::uint64_t lag) {
      return std::pair {saturating_sub(t0_us, lag + lens_input_margin_us), saturating_sub(t1_us, lag) + lens_input_margin_us};
    };
    const auto [m0, m1] = span(calibrator_.mouse_lag_us());
    const auto [s0, s1] = span(calibrator_.stick_lag_us());
    return history_.mouse_moved(m0, m1) || history_.stick_active(s0, s1);
  }

  void host_t::add_shift(std::uint64_t t0_us, std::uint64_t t1_us, float dx, float dy, std::uint64_t now_us) {
    shifts_.push_back(shift_t {t0_us, t1_us, dx, dy});
    while (!shifts_.empty() && shifts_.front().t0_us + motion_window_us < t1_us) {
      shifts_.pop_front();
    }
    float x = 0.0f;
    float y = 0.0f;
    for (const auto &s : shifts_) {
      x += s.dx;
      y += s.dy;
    }
    const float distance = std::hypot(x, y);
    const std::uint64_t span = shifts_.empty() ? 0 : saturating_sub(t1_us, shifts_.front().t0_us);
    if (distance >= motion_min && span > 0) {
      // Any captures missing from the window only make this longer, never too short.
      hud_every_us_ = static_cast<std::uint64_t>(static_cast<double>(motion_min) * static_cast<double>(span) / static_cast<double>(distance));
      moving_until_us_ = now_us + motion_hold_us;
    }
  }

  bool host_t::camera_moving(std::uint64_t now_us) const {
    return enabled_ && moving_until_us_ && now_us < *moving_until_us_;
  }

  std::optional<std::uint64_t> host_t::hud_every_us(std::uint64_t now_us) const {
    if (camera_moving(now_us)) {
      return hud_every_us_;
    }
    return std::nullopt;
  }

  std::uint16_t host_t::mask_width() const {
    const auto p = placement();
    if (p.mask.first == 0) {
      return 0;
    }
    const std::uint32_t whole = std::max<std::uint32_t>(picture_.first, 1);
    const std::uint32_t width = static_cast<std::uint32_t>(p.mask.first) * p.content.width + whole / 2;
    const auto scaled = static_cast<std::uint16_t>(std::min<std::uint32_t>(width / whole, std::numeric_limits<std::uint16_t>::max()));
    return std::clamp<std::uint16_t>(scaled, 1, p.mask.first);
  }

  placement_t host_t::placement() const {
    // The client's limit, halved from the picture's width until it fits.
    std::uint32_t width = enabled_ && mask_max_width_ > 0 ? picture_.first : 0;
    while (width > mask_max_width_) {
      width = (width + 1) / 2;
    }
    const std::uint64_t whole = std::max<std::uint32_t>(picture_.first, 1);
    const std::uint64_t height = (static_cast<std::uint64_t>(width) * picture_.second + whole / 2) / whole;
    placement_t out;
    out.picture = picture_;
    out.content = content_;
    out.mask.first = static_cast<std::uint16_t>(width);
    out.mask.second = std::max<std::uint16_t>(
      static_cast<std::uint16_t>(std::min<std::uint64_t>(height, std::numeric_limits<std::uint16_t>::max())),
      width > 0 ? std::uint16_t {1} : std::uint16_t {0}
    );
    return out;
  }

  void host_t::on_hud(mask_t &&mask, std::uint64_t now_us) {
    if (!enabled_ || mask_max_width_ == 0) {
      return;
    }
    const auto shaped = shape(mask);
    const auto where = placement();
    mask_.offer(where.place(shaped), mask_max_width_, now_us);
    measurer_.set_hud(hud_pixels(shaped, thumbnail_));
  }

  void host_t::tag(std::uint32_t frame, std::uint64_t capture_us) {
    if (!enabled_) {
      return;
    }
    const auto mouse_us = saturating_sub(capture_us, calibrator_.mouse_lag_us());
    const auto pad_us = saturating_sub(capture_us, calibrator_.stick_lag_us());
    const auto tag = history_.tag(frame, mouse_us, pad_us);
    if (!recent_.empty() && recent_.back().frame == frame) {
      recent_.pop_back();
    }
    recent_.push_back(tag);
    while (recent_.size() > max_tags) {
      recent_.pop_front();
    }
    tags_t message;
    message.model_generation = calibrator_generation();
    message.tags.assign(recent_.begin(), recent_.end());
    if (auto wire = message.encode()) {
      if (tags_.size() == pending_tags) {
        tags_.pop_front();
      }
      tags_.push_back(std::move(*wire));
    }
  }

  std::optional<std::vector<std::uint8_t>> host_t::take_tags() {
    if (tags_.empty()) {
      return std::nullopt;
    }
    auto out = std::move(tags_.front());
    tags_.pop_front();
    return out;
  }

  std::optional<std::array<std::uint8_t, model_bytes>> host_t::take_model() {
    if (!model_) {
      return std::nullopt;
    }
    const auto model = *model_;
    model_.reset();
    return model.encode();
  }

  std::optional<std::vector<std::uint8_t>> host_t::take_mask() {
    return mask_.take();
  }

  status_t host_t::status() const {
    status_t out;
    out.enabled = enabled_;
    out.model = last_model_;
    out.samples = samples_;
    out.mask_cells = mask_.sent() ? mask_.sent()->count() : 0;
    return out;
  }

}  // namespace frame_warp
