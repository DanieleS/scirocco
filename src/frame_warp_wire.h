/**
 * @file src/frame_warp_wire.h
 * @brief Frame Warp wire formats, version 1 (control messages 0x5543 to 0x5546).
 *
 * Frame Warp lets a client show a camera turn on its next display refresh: the host learns, per
 * game, how far the picture moves per mouse count and per second of stick, and tags every video
 * frame with the input it reflects; the client shifts each picture by the input that has not
 * reached it yet. The layouts are those of RamazanKara/Rubylight `docs/frame-warp.md` and
 * RamazanKara/nightjar `docs/protocol/frame-warp.md`, byte for byte, so a Scirocco host also serves
 * a Nightjar client. Integers are little-endian, floats IEEE-754 binary32.
 *
 *   0x5543 request  client -> host   8 bytes, reliable
 *   0x5544 model    host -> client   52 bytes (48 without the focal length), reliable
 *   0x5545 tags     host -> client   4 + 20 x count bytes, unreliable and unsequenced
 *   0x5546 mask     host -> client   8 bytes + LEB128 runs, reliable
 *
 * Everything is announced by the host (serverinfo `FrameWarpVersion`, DESCRIBE
 * `a=x-rl-control:`) and asked for by the client; a host does nothing for a client that did not
 * send a request with `enabled` 1. This file only encodes and decodes; every decoder rejects what
 * the format forbids and ignores bytes past the layout it knows.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace frame_warp {

  inline constexpr std::uint16_t request_type = 0x5543;
  inline constexpr std::uint16_t model_type = 0x5544;
  inline constexpr std::uint16_t tags_type = 0x5545;
  inline constexpr std::uint16_t mask_type = 0x5546;

  /// serverinfo element, and the highest version of the document the host speaks.
  inline constexpr std::string_view serverinfo_field = "FrameWarpVersion";
  inline constexpr int wire_version = 1;
  /// DESCRIBE line (without the line end): all four ids, which is what a client looks for.
  inline constexpr std::string_view sdp_attribute = "a=x-rl-control:0x5543,0x5544,0x5545,0x5546";

  inline constexpr std::size_t request_bytes = 8;
  inline constexpr std::size_t model_bytes = 52;
  inline constexpr std::size_t model_flat_bytes = 48;
  inline constexpr std::size_t tags_header_bytes = 4;
  inline constexpr std::size_t tag_bytes = 20;
  inline constexpr std::size_t max_tags = 4;
  inline constexpr std::size_t mask_header_bytes = 8;
  inline constexpr std::uint16_t max_mask_edge = 2048;
  /// The whole mask message; the host halves the mask until it fits.
  inline constexpr std::size_t max_mask_message_bytes = 48 * 1024;
  inline constexpr float max_gain = 100000.0f;
  inline constexpr float max_stick_deadzone = 0.5f;
  inline constexpr float min_focal = 1.0f;
  inline constexpr float max_focal = 1000000.0f;
  inline constexpr std::array<float, 5> stick_exponents {1.0f, 1.5f, 2.0f, 2.5f, 3.0f};
  /// `held_us` of a tag that has no controller packet.
  inline constexpr std::uint32_t no_held = 0xFFFFFFFFu;
  /// Request `flags` bit 0: the client draws the picture through the lens.
  inline constexpr std::uint8_t request_flag_lens = 0x01;

  /// 0x5543.
  struct request_t {
    bool enabled = false;
    /// Input-stream controller number whose right stick turns the camera, none for 0xFF.
    std::optional<std::uint8_t> controller;
    /// The client draws the lens: it is sent the focal length.
    bool lens = false;
    /// Widest HUD mask the client takes; 0 asks for none.
    std::uint16_t mask_max_width = 0;

    std::array<std::uint8_t, request_bytes> encode() const;

    /**
     * @brief Decode a request. Version 0, a short payload, an `enabled` other than 0 or 1 and a
     *        controller number past 15 (other than 0xFF) are rejected; a version above 1 is read
     *        as version 1 and its extra bytes are ignored.
     */
    static std::optional<request_t> decode(std::span<const std::uint8_t> data);

    bool operator==(const request_t &) const = default;
  };

  /// 0x5544.
  struct model_t {
    bool mouse_locked = false;
    bool stick_locked = false;
    std::uint16_t generation = 0;
    std::uint16_t picture_width = 0;
    std::uint16_t picture_height = 0;
    /// Picture pixels per mouse count.
    std::array<float, 2> mouse_gain {};
    /// Picture pixels per second of full deflection, after the curve.
    std::array<float, 2> stick_gain {};
    float stick_deadzone = 0.0f;
    float stick_exponent = 1.0f;
    std::array<float, 2> max_shift {};
    std::uint32_t lag_us = 0;
    /// How much of the motion the model explains, 0..1 (statistics).
    float confidence = 0.0f;
    /// The game camera's focal length in picture pixels, 0 when the picture moves alike everywhere.
    float focal = 0.0f;

    bool locked() const {
      return mouse_locked || stick_locked;
    }

    /// The focal length when there is one.
    std::optional<float> lens() const;

    /// 52 bytes, or none when a field is out of its range.
    std::optional<std::array<std::uint8_t, model_bytes>> encode() const;

    /// 48 and 52 bytes both decode (the 48-byte form has no focal length).
    static std::optional<model_t> decode(std::span<const std::uint8_t> data);

    bool operator==(const model_t &) const = default;
  };

  /// One frame's tag inside 0x5545.
  struct tag_t {
    /// Video frame number of the NV video packet header.
    std::uint32_t frame = 0;
    /// The host's running sums of the relative mouse motion it received, mod 2^32.
    std::int32_t mouse_x = 0;
    std::int32_t mouse_y = 0;
    /// Packets received from the tracked controller by then, counted from the enabling request.
    std::uint32_t controller_seq = 0;
    /// How long the last of them had been held, `no_held` when none had arrived.
    std::uint32_t held_us = no_held;

    bool operator==(const tag_t &) const = default;
  };

  /// 0x5545: the newest frame's tag and up to three before it, oldest first.
  struct tags_t {
    std::uint16_t model_generation = 0;
    std::vector<tag_t> tags;

    /// None for no tags or more than `max_tags`.
    std::optional<std::vector<std::uint8_t>> encode() const;
    static std::optional<tags_t> decode(std::span<const std::uint8_t> data);

    bool operator==(const tags_t &) const = default;
  };

  /// 0x5546, and the HUD the GPU finds: one bit per cell, `height` rows of `stride(width)` words,
  /// bit `i` of word `w` being column `32 * w + i`.
  struct mask_t {
    std::uint16_t generation = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::vector<std::uint32_t> words;

    static std::size_t stride(std::uint16_t width) {
      return (static_cast<std::size_t>(width) + 31) / 32;
    }

    mask_t() = default;
    /// An all-clear mask.
    mask_t(std::uint16_t generation, std::uint16_t width, std::uint16_t height);

    bool get(std::uint16_t x, std::uint16_t y) const;
    void set(std::uint16_t x, std::uint16_t y, bool value);
    std::size_t count() const;
    bool is_clear() const;

    /// Half the resolution (rounding up): a cell is set when any of the four it covers is.
    mask_t halved() const;

    /// A mask of width and height 0 encodes as kind 0 (no mask). None when the message would
    /// exceed `max_mask_message_bytes` or a side exceeds `max_mask_edge`.
    std::optional<std::vector<std::uint8_t>> encode() const;
    static std::optional<mask_t> decode(std::span<const std::uint8_t> data, std::uint16_t max_width = max_mask_edge);

    bool operator==(const mask_t &) const = default;
  };

  /**
   * @brief The stick curve both sides apply: a dead zone on the stick's radius, then `s^exponent`.
   * @param x,y The stick's axes as the input stream carries them.
   * @return The deflection after the curve, each axis in -1..1.
   */
  std::array<float, 2> stick_curve(std::int16_t x, std::int16_t y, float deadzone, float exponent);

}  // namespace frame_warp
