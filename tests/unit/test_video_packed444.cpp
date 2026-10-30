/**
 * @file tests/unit/test_video_packed444.cpp
 * @brief Test src/video_packed444.h: size math, DESCRIBE offer, ANNOUNCE validation and the
 *        layout conformance vectors of the packed 4:4:4 contract (8-bit and 10-bit codes).
 */
#include "../tests_common.h"
#include "src/video_packed444.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace std::literals;
namespace packed = video::packed444;

namespace {
  packed::plane_t make_plane(int width, int height, std::uint16_t fill = 0) {
    packed::plane_t plane;
    plane.width = width;
    plane.height = height;
    plane.samples.assign((std::size_t) width * height, fill);
    return plane;
  }

  void set(packed::plane_t &plane, int x, int y, std::uint16_t value) {
    plane.samples[(std::size_t) y * plane.width + x] = value;
  }

  // Distinct code values per plane and position, within `max`.
  packed::plane_t pattern_plane(int width, int height, int salt, int max) {
    auto plane = make_plane(width, height);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        set(plane, x, y, (std::uint16_t) ((x * 7 + y * 31 + salt * 101 + (x * y) % 13) & max));
      }
    }
    return plane;
  }

  packed::announce_t announce(int format, int dynamic_range, int width = 1920, int height = 1080) {
    return {format, dynamic_range, 0, width, height};
  }
}  // namespace

TEST(Packed444Size, CodedSizeExamples) {
  EXPECT_EQ(packed::coded_size(1920, 1080), (packed::dimensions_t {1920, 2160}));
  EXPECT_EQ(packed::coded_size(2560, 1440), (packed::dimensions_t {2560, 2880}));
  EXPECT_EQ(packed::coded_size(1280, 720), (packed::dimensions_t {1280, 1440}));
  EXPECT_EQ(packed::coded_size(3840, 2160), (packed::dimensions_t {3840, 4320}));
  EXPECT_EQ(packed::coded_size(66, 66), (packed::dimensions_t {128, 144}));
  EXPECT_EQ(packed::coded_size(1, 1), (packed::dimensions_t {64, 16}));
}

TEST(Packed444Size, LogicalLimits) {
  EXPECT_TRUE(packed::logical_size_valid(1, 1));
  EXPECT_TRUE(packed::logical_size_valid(16384, 8192));
  EXPECT_TRUE(packed::logical_size_valid(1001, 563));  // odd sizes are accepted
  EXPECT_FALSE(packed::logical_size_valid(0, 1080));
  EXPECT_FALSE(packed::logical_size_valid(1920, 0));
  EXPECT_FALSE(packed::logical_size_valid(16385, 1080));
  EXPECT_FALSE(packed::logical_size_valid(1920, 8193));
}

TEST(Packed444Offer, ModeBitsFollowTheProbedCodecModes) {
  EXPECT_EQ(packed::offered_modes(1, 1), 0u);
  EXPECT_EQ(packed::offered_modes(0, 0), 0u);
  EXPECT_EQ(packed::offered_modes(2, 1), packed::mode_hevc_8);
  EXPECT_EQ(packed::offered_modes(3, 1), packed::mode_hevc_8 | packed::mode_hevc_10);
  EXPECT_EQ(packed::offered_modes(1, 2), packed::mode_av1_8);
  EXPECT_EQ(packed::offered_modes(1, 3), packed::mode_av1_8 | packed::mode_av1_10);
  EXPECT_EQ(packed::offered_modes(3, 3), 15u);
  EXPECT_EQ(packed::offered_modes(2, 3), 1u | 4u | 8u);
}

TEST(Packed444Offer, DescribeLinesOnlyWithOptionAndMode) {
  EXPECT_EQ(packed::describe_lines(true, 0), "");
  EXPECT_EQ(packed::describe_lines(false, 15), "");
  EXPECT_EQ(packed::describe_lines(false, 0), "");
  EXPECT_EQ(packed::describe_lines(true, 15), "a=x-rl-video.packed444Version:1\na=x-rl-video.packed444Modes:15\n");
  EXPECT_EQ(packed::describe_lines(true, 5), "a=x-rl-video.packed444Version:1\na=x-rl-video.packed444Modes:5\n");
  // The reader masks with 15, so the host never emits bits above it.
  EXPECT_EQ(packed::describe_lines(true, 0x10), "");
  EXPECT_EQ(packed::describe_lines(true, 0x11), "a=x-rl-video.packed444Version:1\na=x-rl-video.packed444Modes:1\n");
}

TEST(Packed444Announce, AttributeValues) {
  EXPECT_EQ(packed::parse_chroma_packing(std::nullopt), std::optional<int> {0});
  EXPECT_EQ(packed::parse_chroma_packing("0"sv), std::optional<int> {0});
  EXPECT_EQ(packed::parse_chroma_packing("1"sv), std::optional<int> {1});
  EXPECT_FALSE(packed::parse_chroma_packing("2"sv).has_value());
  EXPECT_FALSE(packed::parse_chroma_packing(""sv).has_value());
  EXPECT_FALSE(packed::parse_chroma_packing("true"sv).has_value());
}

TEST(Packed444Announce, AcceptsAnOfferedMode) {
  EXPECT_EQ(packed::validate_announce(announce(1, 0), true, 15), nullptr);
  EXPECT_EQ(packed::validate_announce(announce(1, 1), true, 15), nullptr);
  EXPECT_EQ(packed::validate_announce(announce(2, 0), true, 15), nullptr);
  EXPECT_EQ(packed::validate_announce(announce(2, 1), true, 15), nullptr);
  EXPECT_EQ(packed::validate_announce(announce(1, 0, 1001, 563), true, 1), nullptr);
}

TEST(Packed444Announce, RejectsWhatTheHostDoesNotOffer) {
  // Option off.
  EXPECT_NE(packed::validate_announce(announce(1, 0), false, 15), nullptr);
  // The mode bit of (codec, depth) is missing from the current mask.
  EXPECT_NE(packed::validate_announce(announce(1, 1), true, packed::mode_hevc_8), nullptr);
  EXPECT_NE(packed::validate_announce(announce(1, 0), true, packed::mode_hevc_10), nullptr);
  EXPECT_NE(packed::validate_announce(announce(2, 0), true, packed::mode_hevc_8 | packed::mode_hevc_10), nullptr);
  EXPECT_NE(packed::validate_announce(announce(2, 1), true, packed::mode_av1_8), nullptr);
  EXPECT_NE(packed::validate_announce(announce(1, 0), true, 0), nullptr);
}

TEST(Packed444Announce, RejectsMalformedRequests) {
  // H.264, PyroWave and unknown formats.
  EXPECT_NE(packed::validate_announce(announce(0, 0), true, 15), nullptr);
  EXPECT_NE(packed::validate_announce(announce(3, 0), true, 15), nullptr);
  // Depth other than 0/1.
  EXPECT_NE(packed::validate_announce(announce(1, 2), true, 15), nullptr);
  EXPECT_NE(packed::validate_announce(announce(1, -1), true, 15), nullptr);
  // Packing is not the host's native 4:4:4.
  auto native = announce(1, 0);
  native.chroma_sampling_type = 1;
  EXPECT_NE(packed::validate_announce(native, true, 15), nullptr);
  // Sizes.
  EXPECT_NE(packed::validate_announce(announce(1, 0, 0, 1080), true, 15), nullptr);
  EXPECT_NE(packed::validate_announce(announce(1, 0, 1920, 8193), true, 15), nullptr);
  EXPECT_NE(packed::validate_announce(announce(1, 0, 16385, 1080), true, 15), nullptr);
}

TEST(Packed444Layout, SinglePixelVector) {
  // W=H=1: coded 64x16. Y plane rows 0..7 are all `a`; rows 8..15 are `b` for x < 32, `c` for x >= 32.
  for (const std::uint16_t max : {std::uint16_t {255}, std::uint16_t {1023}}) {
    const std::uint16_t a = max - 3;
    const std::uint16_t b = max / 2;
    const std::uint16_t c = max / 3;
    const auto picture = packed::pack(make_plane(1, 1, a), make_plane(1, 1, b), make_plane(1, 1, c));
    ASSERT_EQ(picture.padded_width, 64);
    ASSERT_EQ(picture.padded_height, 8);
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 64; ++x) {
        EXPECT_EQ(picture.cy(x, y), a);
      }
    }
    for (int y = 8; y < 16; ++y) {
      for (int x = 0; x < 64; ++x) {
        EXPECT_EQ(picture.cy(x, y), x < 32 ? b : c);
      }
    }
    for (int j = 0; j < 8; ++j) {
      for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(picture.cu(i, j), b);
        EXPECT_EQ(picture.cv(i, j), c);
      }
    }
  }
}

TEST(Packed444Layout, OddSizeVector) {
  // W=65, H=9: Wp=128, Hp=16, coded 128x32.
  auto sy = make_plane(65, 9, 11);
  auto su = make_plane(65, 9, 22);
  auto sv = make_plane(65, 9, 33);
  set(su, 1, 0, 101);
  set(sv, 3, 2, 202);
  set(su, 4, 5, 303);
  set(sv, 64, 8, 404);
  set(su, 64, 8, 505);

  const auto picture = packed::pack(sy, su, sv);
  ASSERT_EQ(picture.padded_width, 128);
  ASSERT_EQ(picture.padded_height, 16);
  EXPECT_EQ(picture.cy(0, 16), 101);  // SU(1,0) -> CY(0, Hp + 0)
  EXPECT_EQ(picture.cy(65, 18), 202);  // SV(3,2) -> CY(Wp/2 + 1, Hp + 2)
  EXPECT_EQ(picture.cu(2, 5), 303);  // SU(4,5) -> CU(2,5)
  EXPECT_EQ(picture.cv(32, 8), 404);  // SV(64,8) -> CV(32,8)
  // SU(64,8) is the corner source pixel: it replicates beyond it, in every padded even-x position.
  EXPECT_EQ(picture.cu(32, 8), 505);
  EXPECT_EQ(picture.cu(40, 15), 505);
  EXPECT_EQ(picture.cu(63, 9), 505);
  // Odd-x padded positions come from the replicated column x = 64 as well.
  EXPECT_EQ(picture.cy(32, 16 + 8), 505);  // SU(65, 8) -> SU(64, 8)
  EXPECT_EQ(picture.cy(32, 16 + 15), 505);  // SU(65, 15) -> SU(64, 8)
}

TEST(Packed444Layout, EverySourceSampleAppearsAndUnpackInvertsPack) {
  for (const int max : {255, 1023}) {
    for (const auto [w, h] : {std::pair {1, 1}, std::pair {2, 2}, std::pair {65, 9}, std::pair {66, 66}, std::pair {130, 17}}) {
      const auto sy = pattern_plane(w, h, 1, max);
      const auto su = pattern_plane(w, h, 2, max);
      const auto sv = pattern_plane(w, h, 3, max);
      const auto picture = packed::pack(sy, su, sv);
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const packed::yuv_t expected {sy.at(x, y), su.at(x, y), sv.at(x, y)};
          EXPECT_EQ(packed::unpack(picture, x, y), expected) << w << 'x' << h << " at " << x << ',' << y << " max " << max;
        }
      }
    }
  }
}

TEST(Packed444Layout, TenBitCodesStayWithinTenBits) {
  const auto picture = packed::pack(pattern_plane(70, 20, 1, 1023), pattern_plane(70, 20, 2, 1023), pattern_plane(70, 20, 3, 1023));
  for (const auto value : picture.y) EXPECT_LE(value, 1023);
  for (const auto value : picture.u) EXPECT_LE(value, 1023);
  for (const auto value : picture.v) EXPECT_LE(value, 1023);
}
