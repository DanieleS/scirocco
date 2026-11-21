/**
 * @file src/frame_warp_wire.cpp
 * @brief Frame Warp wire formats, see frame_warp_wire.h.
 */
// this include
#include "frame_warp_wire.h"

// standard includes
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace frame_warp {

  namespace {
    void put_u16(std::uint8_t *out, std::uint16_t value) {
      out[0] = static_cast<std::uint8_t>(value & 0xff);
      out[1] = static_cast<std::uint8_t>(value >> 8);
    }

    void put_u32(std::uint8_t *out, std::uint32_t value) {
      for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
      }
    }

    void put_f32(std::uint8_t *out, float value) {
      put_u32(out, std::bit_cast<std::uint32_t>(value));
    }

    std::uint16_t get_u16(const std::uint8_t *in) {
      return static_cast<std::uint16_t>(in[0] | (in[1] << 8));
    }

    std::uint32_t get_u32(const std::uint8_t *in) {
      return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
             (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
    }

    float get_f32(const std::uint8_t *in) {
      return std::bit_cast<float>(get_u32(in));
    }

    bool gain_ok(float value) {
      return std::isfinite(value) && std::fabs(value) <= max_gain;
    }

    bool exponent_ok(float value) {
      return std::find(stick_exponents.begin(), stick_exponents.end(), value) != stick_exponents.end();
    }

    bool shift_ok(float value) {
      return std::isfinite(value) && value >= 0.0f && value <= 65535.0f;
    }

    void put_leb128(std::vector<std::uint8_t> &out, std::uint32_t value) {
      while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
      }
      out.push_back(static_cast<std::uint8_t>(value));
    }

    /// Reads one LEB128 value of at most 5 bytes.
    std::optional<std::uint32_t> get_leb128(std::span<const std::uint8_t> data, std::size_t &offset) {
      std::uint64_t value = 0;
      for (int i = 0; i < 5; ++i) {
        if (offset >= data.size()) {
          return std::nullopt;
        }
        const auto byte = data[offset++];
        value |= static_cast<std::uint64_t>(byte & 0x7f) << (7 * i);
        if ((byte & 0x80) == 0) {
          if (value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
          }
          return static_cast<std::uint32_t>(value);
        }
      }
      return std::nullopt;
    }
  }  // namespace

  std::array<std::uint8_t, request_bytes> request_t::encode() const {
    std::array<std::uint8_t, request_bytes> out {};
    out[0] = static_cast<std::uint8_t>(wire_version);
    out[1] = enabled ? 1 : 0;
    out[2] = controller.value_or(0xFF);
    out[3] = lens ? request_flag_lens : 0;
    put_u16(out.data() + 4, mask_max_width);
    return out;
  }

  std::optional<request_t> request_t::decode(std::span<const std::uint8_t> data) {
    if (data.size() < request_bytes || data[0] == 0) {
      return std::nullopt;
    }
    // Any version above 1 is read with the version 1 layout.
    if (data[1] > 1) {
      return std::nullopt;
    }
    request_t request;
    request.enabled = data[1] == 1;
    if (data[2] != 0xFF) {
      if (data[2] > 15) {
        return std::nullopt;
      }
      request.controller = data[2];
    }
    request.lens = (data[3] & request_flag_lens) != 0;
    request.mask_max_width = std::min<std::uint16_t>(get_u16(data.data() + 4), max_mask_edge);
    return request;
  }

  std::optional<float> model_t::lens() const {
    if (std::isfinite(focal) && focal >= min_focal && focal <= max_focal) {
      return focal;
    }
    return std::nullopt;
  }

  std::optional<std::array<std::uint8_t, model_bytes>> model_t::encode() const {
    if (picture_width == 0 || picture_height == 0) {
      return std::nullopt;
    }
    if (!gain_ok(mouse_gain[0]) || !gain_ok(mouse_gain[1]) || !gain_ok(stick_gain[0]) || !gain_ok(stick_gain[1])) {
      return std::nullopt;
    }
    if (!std::isfinite(stick_deadzone) || stick_deadzone < 0.0f || stick_deadzone > max_stick_deadzone) {
      return std::nullopt;
    }
    if (!exponent_ok(stick_exponent) || !shift_ok(max_shift[0]) || !shift_ok(max_shift[1])) {
      return std::nullopt;
    }
    if (!std::isfinite(confidence) || confidence < 0.0f || confidence > 1.0f) {
      return std::nullopt;
    }
    if (focal != 0.0f && !(std::isfinite(focal) && focal >= min_focal && focal <= max_focal)) {
      return std::nullopt;
    }

    std::array<std::uint8_t, model_bytes> out {};
    out[0] = static_cast<std::uint8_t>(wire_version);
    out[1] = static_cast<std::uint8_t>((mouse_locked ? 1 : 0) | (stick_locked ? 2 : 0));
    put_u16(out.data() + 2, generation);
    put_u16(out.data() + 4, picture_width);
    put_u16(out.data() + 6, picture_height);
    put_f32(out.data() + 8, mouse_gain[0]);
    put_f32(out.data() + 12, mouse_gain[1]);
    put_f32(out.data() + 16, stick_gain[0]);
    put_f32(out.data() + 20, stick_gain[1]);
    put_f32(out.data() + 24, stick_deadzone);
    put_f32(out.data() + 28, stick_exponent);
    put_f32(out.data() + 32, max_shift[0]);
    put_f32(out.data() + 36, max_shift[1]);
    put_u32(out.data() + 40, lag_us);
    put_f32(out.data() + 44, confidence);
    put_f32(out.data() + 48, focal);
    return out;
  }

  std::optional<model_t> model_t::decode(std::span<const std::uint8_t> data) {
    if (data.size() < model_flat_bytes || data[0] == 0) {
      return std::nullopt;
    }
    model_t model;
    model.mouse_locked = (data[1] & 1) != 0;
    model.stick_locked = (data[1] & 2) != 0;
    model.generation = get_u16(data.data() + 2);
    model.picture_width = get_u16(data.data() + 4);
    model.picture_height = get_u16(data.data() + 6);
    model.mouse_gain = {get_f32(data.data() + 8), get_f32(data.data() + 12)};
    model.stick_gain = {get_f32(data.data() + 16), get_f32(data.data() + 20)};
    model.stick_deadzone = get_f32(data.data() + 24);
    model.stick_exponent = get_f32(data.data() + 28);
    model.max_shift = {get_f32(data.data() + 32), get_f32(data.data() + 36)};
    model.lag_us = get_u32(data.data() + 40);
    model.confidence = get_f32(data.data() + 44);
    model.focal = data.size() >= model_bytes ? get_f32(data.data() + 48) : 0.0f;

    if (model.picture_width == 0 || model.picture_height == 0) {
      return std::nullopt;
    }
    if (!gain_ok(model.mouse_gain[0]) || !gain_ok(model.mouse_gain[1]) || !gain_ok(model.stick_gain[0]) || !gain_ok(model.stick_gain[1])) {
      return std::nullopt;
    }
    if (!std::isfinite(model.stick_deadzone) || model.stick_deadzone < 0.0f || model.stick_deadzone > max_stick_deadzone) {
      return std::nullopt;
    }
    if (!exponent_ok(model.stick_exponent) || !shift_ok(model.max_shift[0]) || !shift_ok(model.max_shift[1])) {
      return std::nullopt;
    }
    if (!std::isfinite(model.confidence) || model.confidence < 0.0f || model.confidence > 1.0f) {
      return std::nullopt;
    }
    if (model.focal != 0.0f && !(std::isfinite(model.focal) && model.focal >= min_focal && model.focal <= max_focal)) {
      return std::nullopt;
    }
    return model;
  }

  std::optional<std::vector<std::uint8_t>> tags_t::encode() const {
    if (tags.empty() || tags.size() > max_tags) {
      return std::nullopt;
    }
    std::vector<std::uint8_t> out(tags_header_bytes + tag_bytes * tags.size());
    out[0] = static_cast<std::uint8_t>(wire_version);
    out[1] = static_cast<std::uint8_t>(tags.size());
    put_u16(out.data() + 2, model_generation);
    auto *at = out.data() + tags_header_bytes;
    for (const auto &tag : tags) {
      put_u32(at, tag.frame);
      put_u32(at + 4, static_cast<std::uint32_t>(tag.mouse_x));
      put_u32(at + 8, static_cast<std::uint32_t>(tag.mouse_y));
      put_u32(at + 12, tag.controller_seq);
      put_u32(at + 16, tag.held_us);
      at += tag_bytes;
    }
    return out;
  }

  std::optional<tags_t> tags_t::decode(std::span<const std::uint8_t> data) {
    if (data.size() < tags_header_bytes || data[0] == 0) {
      return std::nullopt;
    }
    const std::size_t count = data[1];
    if (count == 0 || count > max_tags || data.size() < tags_header_bytes + tag_bytes * count) {
      return std::nullopt;
    }
    tags_t out;
    out.model_generation = get_u16(data.data() + 2);
    const auto *at = data.data() + tags_header_bytes;
    for (std::size_t i = 0; i < count; ++i) {
      tag_t tag;
      tag.frame = get_u32(at);
      tag.mouse_x = static_cast<std::int32_t>(get_u32(at + 4));
      tag.mouse_y = static_cast<std::int32_t>(get_u32(at + 8));
      tag.controller_seq = get_u32(at + 12);
      tag.held_us = get_u32(at + 16);
      out.tags.push_back(tag);
      at += tag_bytes;
    }
    return out;
  }

  mask_t::mask_t(std::uint16_t generation, std::uint16_t width, std::uint16_t height):
      generation {generation},
      width {width},
      height {height},
      words(stride(width) * height, 0u) {
  }

  bool mask_t::get(std::uint16_t x, std::uint16_t y) const {
    if (x >= width || y >= height) {
      return false;
    }
    return (words[static_cast<std::size_t>(y) * stride(width) + x / 32] >> (x % 32)) & 1u;
  }

  void mask_t::set(std::uint16_t x, std::uint16_t y, bool value) {
    if (x >= width || y >= height) {
      return;
    }
    auto &word = words[static_cast<std::size_t>(y) * stride(width) + x / 32];
    const std::uint32_t bit = 1u << (x % 32);
    word = value ? (word | bit) : (word & ~bit);
  }

  std::size_t mask_t::count() const {
    std::size_t total = 0;
    for (const auto word : words) {
      total += static_cast<std::size_t>(std::popcount(word));
    }
    return total;
  }

  bool mask_t::is_clear() const {
    return std::all_of(words.begin(), words.end(), [](std::uint32_t word) {
      return word == 0;
    });
  }

  mask_t mask_t::halved() const {
    const auto half_width = static_cast<std::uint16_t>((width + 1) / 2);
    const auto half_height = static_cast<std::uint16_t>((height + 1) / 2);
    mask_t out {generation, half_width, half_height};
    for (std::uint16_t y = 0; y < height; ++y) {
      for (std::uint16_t x = 0; x < width; ++x) {
        if (get(x, y)) {
          out.set(x / 2, y / 2, true);
        }
      }
    }
    return out;
  }

  std::optional<std::vector<std::uint8_t>> mask_t::encode() const {
    if (width > max_mask_edge || height > max_mask_edge) {
      return std::nullopt;
    }
    if ((width == 0) != (height == 0)) {
      return std::nullopt;
    }
    std::vector<std::uint8_t> out(mask_header_bytes);
    out[0] = static_cast<std::uint8_t>(wire_version);
    out[1] = width == 0 ? 0 : 1;
    put_u16(out.data() + 2, generation);
    put_u16(out.data() + 4, width);
    put_u16(out.data() + 6, height);
    for (std::uint16_t y = 0; y < height; ++y) {
      // Runs alternate clear, set, clear..., starting with clear: a row that starts set begins with a 0.
      bool state = false;
      std::uint32_t run = 0;
      for (std::uint16_t x = 0; x < width; ++x) {
        if (get(x, y) == state) {
          ++run;
          continue;
        }
        put_leb128(out, run);
        state = !state;
        run = 1;
      }
      put_leb128(out, run);
      if (out.size() > max_mask_message_bytes) {
        return std::nullopt;
      }
    }
    return out;
  }

  std::optional<mask_t> mask_t::decode(std::span<const std::uint8_t> data, std::uint16_t max_width) {
    if (data.size() < mask_header_bytes || data[0] == 0 || data.size() > max_mask_message_bytes) {
      return std::nullopt;
    }
    const auto kind = data[1];
    const auto generation = get_u16(data.data() + 2);
    const auto width = get_u16(data.data() + 4);
    const auto height = get_u16(data.data() + 6);
    if (kind == 0) {
      if (width != 0 || height != 0) {
        return std::nullopt;
      }
      return mask_t {generation, 0, 0};
    }
    if (kind != 1 || width == 0 || height == 0 || width > max_mask_edge || height > max_mask_edge || width > max_width) {
      return std::nullopt;
    }

    mask_t out {generation, width, height};
    std::size_t offset = mask_header_bytes;
    for (std::uint16_t y = 0; y < height; ++y) {
      std::uint32_t x = 0;
      bool state = false;
      bool first = true;
      while (x < width) {
        const auto run = get_leb128(data, offset);
        if (!run) {
          return std::nullopt;
        }
        if (*run == 0 && !first) {
          return std::nullopt;
        }
        if (*run > width - x) {
          return std::nullopt;
        }
        if (state) {
          for (std::uint32_t i = 0; i < *run; ++i) {
            out.set(static_cast<std::uint16_t>(x + i), y, true);
          }
        }
        x += *run;
        state = !state;
        first = false;
      }
    }
    return out;
  }

  std::array<float, 2> stick_curve(std::int16_t x, std::int16_t y, float deadzone, float exponent) {
    const auto unit = [](std::int16_t axis) {
      return std::clamp(static_cast<float>(axis) / 32767.0f, -1.0f, 1.0f);
    };
    const float vx = unit(x);
    const float vy = unit(y);
    const float r = std::sqrt(vx * vx + vy * vy);
    if (r <= deadzone || r <= 0.0f) {
      return {0.0f, 0.0f};
    }
    const float scaled = std::min((r - deadzone) / (1.0f - deadzone), 1.0f);
    const float magnitude = std::pow(scaled, exponent);
    return {vx / r * magnitude, vy / r * magnitude};
  }

}  // namespace frame_warp
