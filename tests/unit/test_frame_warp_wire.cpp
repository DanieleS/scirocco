/**
 * @file tests/unit/test_frame_warp_wire.cpp
 * @brief Test src/frame_warp_wire.*, the Frame Warp messages 0x5543 to 0x5546.
 */
#include "../tests_common.h"
#include "src/frame_warp_wire.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using namespace frame_warp;

namespace {
  using bytes = std::vector<std::uint8_t>;

  model_t sample_model() {
    model_t model;
    model.mouse_locked = true;
    model.generation = 7;
    model.picture_width = 1920;
    model.picture_height = 1080;
    model.mouse_gain = {-0.9375f, 0.5f};
    model.stick_gain = {100.0f, -80.0f};
    model.stick_deadzone = 0.1f;
    model.stick_exponent = 2.0f;
    model.max_shift = {288.0f, 162.0f};
    model.lag_us = 40000;
    model.confidence = 0.9f;
    model.focal = 960.0f;
    return model;
  }
}  // namespace

TEST(FrameWarpWireRequest, GoldenBytesOfTheZefiroDefaults) {
  request_t request;
  request.enabled = true;
  request.controller = 0;
  request.lens = true;
  request.mask_max_width = 256;
  const std::array<std::uint8_t, 8> golden {0x01, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
  EXPECT_EQ(request.encode(), golden);
  EXPECT_EQ(request_t::decode(golden), request);
}

TEST(FrameWarpWireRequest, NoControllerIs0xFFAndTrailingBytesAreIgnored) {
  const bytes data {0x01, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB};
  const auto request = request_t::decode(data);
  ASSERT_TRUE(request);
  EXPECT_FALSE(request->enabled);
  EXPECT_FALSE(request->controller);
  EXPECT_FALSE(request->lens);
  EXPECT_EQ(request->mask_max_width, 0);
}

TEST(FrameWarpWireRequest, FutureVersionsAreReadAsVersionOne) {
  const bytes data {0x02, 0x01, 0x03, 0x01, 0x00, 0x01, 0x00, 0x00, 0x11, 0x22, 0x33};
  const auto request = request_t::decode(data);
  ASSERT_TRUE(request);
  EXPECT_TRUE(request->enabled);
  EXPECT_EQ(request->controller, 3);
  EXPECT_TRUE(request->lens);
  EXPECT_EQ(request->mask_max_width, 256);
}

TEST(FrameWarpWireRequest, RejectsWhatTheFormatForbids) {
  EXPECT_FALSE(request_t::decode(bytes {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));  // version 0
  EXPECT_FALSE(request_t::decode(bytes {0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}));  // short
  EXPECT_FALSE(request_t::decode(bytes {0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));  // enabled 2
  EXPECT_FALSE(request_t::decode(bytes {0x01, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00}));  // controller 16
}

TEST(FrameWarpWireModel, RoundTripsAt52Bytes) {
  const auto model = sample_model();
  const auto wire = model.encode();
  ASSERT_TRUE(wire);
  EXPECT_EQ(wire->size(), model_bytes);
  EXPECT_EQ((*wire)[0], 1);
  EXPECT_EQ((*wire)[1], 1);
  EXPECT_EQ(model_t::decode(*wire), model);
  EXPECT_EQ(model.lens(), 960.0f);
}

TEST(FrameWarpWireModel, A48ByteModelHasNoFocal) {
  const auto wire = sample_model().encode();
  ASSERT_TRUE(wire);
  const auto flat = model_t::decode(std::span<const std::uint8_t> {wire->data(), model_flat_bytes});
  ASSERT_TRUE(flat);
  EXPECT_EQ(flat->focal, 0.0f);
  EXPECT_FALSE(flat->lens());
  EXPECT_EQ(flat->generation, 7);
  // Trailing bytes past 52 are ignored.
  bytes longer(wire->begin(), wire->end());
  longer.push_back(0x55);
  EXPECT_EQ(model_t::decode(longer), sample_model());
  // Shorter than 48 is no model.
  EXPECT_FALSE(model_t::decode(std::span<const std::uint8_t> {wire->data(), model_flat_bytes - 1}));
}

TEST(FrameWarpWireModel, RejectsEveryOutOfRangeField) {
  auto base = sample_model();
  auto reject = [](model_t model) {
    EXPECT_FALSE(model.encode());
  };

  auto model = base;
  model.picture_width = 0;
  reject(model);
  model = base;
  model.picture_height = 0;
  reject(model);
  model = base;
  model.mouse_gain[0] = std::nanf("");
  reject(model);
  model = base;
  model.mouse_gain[1] = 100001.0f;
  reject(model);
  model = base;
  model.stick_gain[0] = -100001.0f;
  reject(model);
  model = base;
  model.stick_deadzone = 0.51f;
  reject(model);
  model = base;
  model.stick_exponent = 1.75f;
  reject(model);
  model = base;
  model.max_shift[0] = -1.0f;
  reject(model);
  model = base;
  model.max_shift[1] = 65536.0f;
  reject(model);
  model = base;
  model.confidence = 1.5f;
  reject(model);
  model = base;
  model.focal = 0.5f;
  reject(model);
  model = base;
  model.focal = 2000000.0f;
  reject(model);

  // Decoding refuses the same.
  auto wire = *base.encode();
  wire[7] = 0;
  wire[6] = 0;
  EXPECT_FALSE(model_t::decode(wire));
  wire = *base.encode();
  wire[28] = 0x00;  // exponent 2.75 (0x40300000)
  wire[29] = 0x00;
  wire[30] = 0x30;
  wire[31] = 0x40;
  EXPECT_FALSE(model_t::decode(wire));
}

TEST(FrameWarpWireModel, NoLensEncodesFocalZero) {
  auto model = sample_model();
  model.focal = 0.0f;
  const auto wire = model.encode();
  ASSERT_TRUE(wire);
  EXPECT_EQ(model_t::decode(*wire)->focal, 0.0f);
  EXPECT_FALSE(model.lens());
}

TEST(FrameWarpWireTags, GoldenBytes) {
  // Frame 16, mouse (-3, 2), no controller packet, model generation 2.
  tags_t tags;
  tags.model_generation = 2;
  tags.tags.push_back({16, -3, 2, 0, no_held});
  const std::array<std::uint8_t, 24> golden {
    0x01, 0x01, 0x02, 0x00,
    0x10, 0x00, 0x00, 0x00,
    0xFD, 0xFF, 0xFF, 0xFF,
    0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF};
  const auto wire = tags.encode();
  ASSERT_TRUE(wire);
  EXPECT_EQ(*wire, bytes(golden.begin(), golden.end()));
  EXPECT_EQ(tags_t::decode(*wire), tags);
}

TEST(FrameWarpWireTags, OneToFourTagsOnly) {
  tags_t tags;
  EXPECT_FALSE(tags.encode());
  for (std::uint32_t i = 1; i <= 4; ++i) {
    tags.tags.push_back({i, 0, 0, 0, no_held});
    const auto wire = tags.encode();
    ASSERT_TRUE(wire);
    EXPECT_EQ(wire->size(), tags_header_bytes + tag_bytes * i);
    EXPECT_EQ(tags_t::decode(*wire), tags);
  }
  tags.tags.push_back({5, 0, 0, 0, no_held});
  EXPECT_FALSE(tags.encode());

  // A count of 0 or 5, or a payload shorter than its count says, is no message.
  auto wire = *tags_t {1, {{1, 0, 0, 0, no_held}}}.encode();
  wire[1] = 0;
  EXPECT_FALSE(tags_t::decode(wire));
  wire[1] = 5;
  EXPECT_FALSE(tags_t::decode(wire));
  wire[1] = 2;
  EXPECT_FALSE(tags_t::decode(wire));
}

TEST(FrameWarpWireTags, MouseSumsWrapAsSigned32Bit) {
  tags_t tags;
  tags.tags.push_back({1, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(), 0, 5});
  const auto decoded = tags_t::decode(*tags.encode());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->tags[0].mouse_x, std::numeric_limits<std::int32_t>::min());
  EXPECT_EQ(decoded->tags[0].mouse_y, std::numeric_limits<std::int32_t>::max());
  const std::int32_t sent = std::numeric_limits<std::int32_t>::min();
  const std::int32_t tagged = std::numeric_limits<std::int32_t>::max();
  EXPECT_EQ(static_cast<std::int32_t>(static_cast<std::uint32_t>(sent) - static_cast<std::uint32_t>(tagged)), 1);
}

TEST(FrameWarpWireMask, GoldenBytes) {
  mask_t mask {1, 5, 2};
  mask.set(2, 0, true);
  for (std::uint16_t x = 0; x < 5; ++x) {
    mask.set(x, 1, true);
  }
  const std::array<std::uint8_t, 13> golden {0x01, 0x01, 0x01, 0x00, 0x05, 0x00, 0x02, 0x00, 0x02, 0x01, 0x02, 0x00, 0x05};
  const auto wire = mask.encode();
  ASSERT_TRUE(wire);
  EXPECT_EQ(*wire, bytes(golden.begin(), golden.end()));
  EXPECT_EQ(mask_t::decode(*wire), mask);
}

TEST(FrameWarpWireMask, NoMaskIsKindZero) {
  const auto wire = mask_t {3, 0, 0}.encode();
  ASSERT_TRUE(wire);
  EXPECT_EQ(wire->size(), mask_header_bytes);
  EXPECT_EQ((*wire)[1], 0);
  const auto decoded = mask_t::decode(*wire);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->generation, 3);
  EXPECT_EQ(decoded->width, 0);
}

TEST(FrameWarpWireMask, RowsThatStartSetAndRunsAcross32Columns) {
  mask_t mask {9, 70, 3};
  for (std::uint16_t x = 0; x < 70; ++x) {
    mask.set(x, 0, x < 3 || x >= 60);
    mask.set(x, 1, x >= 31 && x < 34);
    mask.set(x, 2, false);
  }
  const auto decoded = mask_t::decode(*mask.encode());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, mask);
  EXPECT_EQ(decoded->count(), 3u + 10u + 3u);
  EXPECT_FALSE(decoded->is_clear());
}

TEST(FrameWarpWireMask, RejectsWhatTheFormatForbids) {
  const auto good = *mask_t {1, 5, 2}.encode();
  // A row whose runs add up to less than the width.
  auto bad = good;
  bad.pop_back();
  EXPECT_FALSE(mask_t::decode(bad));
  // A run past the width.
  bad = good;
  bad[mask_header_bytes] = 0x09;
  EXPECT_FALSE(mask_t::decode(bad));
  // A zero run that is not a row's first.
  bad = {0x01, 0x01, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03};
  EXPECT_FALSE(mask_t::decode(bad));
  // Wider than the client asked for.
  EXPECT_FALSE(mask_t::decode(good, 4));
  EXPECT_TRUE(mask_t::decode(good, 5));
  // Kind 0 with a size, an unknown kind, version 0.
  EXPECT_FALSE(mask_t::decode(bytes {0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x01, 0x00}));
  EXPECT_FALSE(mask_t::decode(bytes {0x01, 0x02, 0x00, 0x00, 0x05, 0x00, 0x01, 0x00}));
  bad = good;
  bad[0] = 0;
  EXPECT_FALSE(mask_t::decode(bad));
  // An overlong LEB128 value.
  bad = {0x01, 0x01, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
  EXPECT_FALSE(mask_t::decode(bad));
}

TEST(FrameWarpWireMask, HalvingSetsACellWhenAnyOfTheFourIs) {
  mask_t mask {4, 5, 3};
  mask.set(4, 2, true);
  mask.set(1, 0, true);
  const auto half = mask.halved();
  EXPECT_EQ(half.width, 3);
  EXPECT_EQ(half.height, 2);
  EXPECT_TRUE(half.get(2, 1));
  EXPECT_TRUE(half.get(0, 0));
  EXPECT_EQ(half.count(), 2u);
  EXPECT_EQ(half.generation, 4);
}

TEST(FrameWarpWireMask, ATooBigMaskDoesNotEncode) {
  // A checkerboard of 2048 x 2048 cells needs far more than 48 KiB.
  mask_t mask {1, 2048, 2048};
  for (std::uint16_t y = 0; y < 2048; ++y) {
    for (std::uint16_t x = 0; x < 2048; ++x) {
      mask.set(x, y, ((x + y) & 1) != 0);
    }
  }
  EXPECT_FALSE(mask.encode());
  auto half = mask;
  while (!half.encode() && half.width > 1) {
    half = half.halved();
  }
  const auto wire = half.encode();
  ASSERT_TRUE(wire);
  EXPECT_LE(wire->size(), max_mask_message_bytes);
}

TEST(FrameWarpWireStick, CurveHasADeadZoneAndAnExponent) {
  EXPECT_EQ(stick_curve(0, 0, 0.1f, 2.0f), (std::array<float, 2> {0.0f, 0.0f}));
  // Inside the dead zone.
  EXPECT_EQ(stick_curve(3000, 0, 0.1f, 1.0f), (std::array<float, 2> {0.0f, 0.0f}));
  // Full deflection reaches 1 whatever the exponent.
  const auto full = stick_curve(32767, 0, 0.1f, 3.0f);
  EXPECT_NEAR(full[0], 1.0f, 1e-5f);
  EXPECT_NEAR(full[1], 0.0f, 1e-5f);
  // Halfway between the dead zone and the edge, squared.
  const auto half = stick_curve(static_cast<std::int16_t>(32767 * 0.55), 0, 0.1f, 2.0f);
  EXPECT_NEAR(half[0], 0.25f, 1e-3f);
  // The direction is kept on both axes and the sign carries.
  const auto diagonal = stick_curve(-23170, 23170, 0.0f, 1.0f);
  EXPECT_NEAR(diagonal[0], -std::sqrt(0.5f), 1e-3f);
  EXPECT_NEAR(diagonal[1], std::sqrt(0.5f), 1e-3f);
  // -32768 clamps to -1.
  EXPECT_NEAR(stick_curve(-32768, 0, 0.0f, 1.0f)[0], -1.0f, 1e-6f);
}
