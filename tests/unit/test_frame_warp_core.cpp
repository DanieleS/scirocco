/**
 * @file tests/unit/test_frame_warp_core.cpp
 * @brief Test src/frame_warp.* (the Frame Warp learner): input history, motion measurement, lens,
 *        calibration and the host that ties them together, on synthetic thumbnails.
 */
#include "../tests_common.h"
#include "src/frame_warp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace frame_warp;

namespace {

  // ---- The synthetic scene ----

  /// Value noise: a hashed lattice every `cell` pixels, bilinear between, 0 to 1.
  float noise(float u, float v, float cell, std::uint32_t seed) {
    const auto lattice = [seed](std::int32_t x, std::int32_t y) {
      std::uint32_t h = (static_cast<std::uint32_t>(x) * 0x8da6b343u) ^ (static_cast<std::uint32_t>(y) * 0xd8163841u) ^ (seed * 0xcb1ab31fu);
      h ^= h >> 13;
      h *= 0x5bd1e995u;
      h ^= h >> 15;
      return static_cast<float>(h & 0xffffu) / 65535.0f;
    };
    const float fu = u / cell;
    const float fv = v / cell;
    const float fx = std::floor(fu);
    const float fy = std::floor(fv);
    const float tx = fu - fx;
    const float ty = fv - fy;
    const auto x0 = static_cast<std::int32_t>(fx);
    const auto y0 = static_cast<std::int32_t>(fy);
    const float top = lattice(x0, y0) * (1.0f - tx) + lattice(x0 + 1, y0) * tx;
    const float bottom = lattice(x0, y0 + 1) * (1.0f - tx) + lattice(x0 + 1, y0 + 1) * tx;
    return top * (1.0f - ty) + bottom * ty;
  }

  /// The textured world's luma at `(u, v)`.
  std::uint8_t world(float u, float v) {
    const float value = 128.0f + 70.0f * (noise(u, v, 4.0f, 1) - 0.5f) + 60.0f * (noise(u, v, 9.0f, 2) - 0.5f) + 50.0f * (noise(u, v, 23.0f, 3) - 0.5f);
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
  }

  /// A textured scene without repeats, sampled at `(x + ox, y + oy)` so the content moves by `(-ox, -oy)`.
  thumbnail_t scene(std::size_t width, std::size_t height, float ox, float oy, std::uint64_t at_us) {
    thumbnail_t out;
    out.width = width;
    out.height = height;
    out.at_us = at_us;
    out.luma.reserve(width * height);
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        out.luma.push_back(world(static_cast<float>(x) + ox, static_cast<float>(y) + oy));
      }
    }
    return out;
  }

  /// Where `with_hud` draws: wide bars at the bottom left and right and a box at the top right.
  bool hud_at(std::size_t x, std::size_t y) {
    return ((y >= 106 && y < 122) && !(x >= 104 && x < 166)) || ((y >= 12 && y < 52) && (x >= 190 && x < 246));
  }

  /// `t` with a static HUD as a game draws one, the same in every capture.
  thumbnail_t with_hud(thumbnail_t t) {
    for (std::size_t y = 0; y < t.height; ++y) {
      for (std::size_t x = 0; x < t.width; ++x) {
        if (!hud_at(x, y)) {
          continue;
        }
        const bool bar = (y >= 110 && y < 118) && !(x >= 100 && x < 170);
        std::uint8_t value = 20;
        if (bar) {
          value = 235;
        } else if ((x + y) % 6 < 3) {
          value = 40;
        }
        t.luma[y * t.width + x] = value;
      }
    }
    return t;
  }

  std::array<std::uint8_t, request_bytes> request(bool enabled, std::optional<std::uint8_t> controller, std::uint16_t mask_max_width, bool lens = true) {
    request_t r;
    r.enabled = enabled;
    r.controller = controller;
    r.mask_max_width = mask_max_width;
    r.lens = lens;
    return r.encode();
  }

  host_t enabled_host(std::uint16_t width = 1920, std::uint16_t height = 1080) {
    host_t host {width, height};
    EXPECT_EQ(host.on_request(request(true, 0, 480)), std::optional<bool> {true});
    return host;
  }

  constexpr rect_t whole(std::uint16_t width, std::uint16_t height) {
    return rect_t {0, 0, width, height};
  }
}  // namespace

// ---- History ----

TEST(FrameWarpHistory, MouseSumsAreLookedUpByTime) {
  history_t h;
  EXPECT_EQ(h.mouse_at(5), (std::pair<std::int64_t, std::int64_t> {0, 0}));
  h.on_mouse(10, -2, 100);
  h.on_mouse(5, 1, 200);
  EXPECT_EQ(h.mouse_at(99), (std::pair<std::int64_t, std::int64_t> {0, 0}));
  EXPECT_EQ(h.mouse_at(100), (std::pair<std::int64_t, std::int64_t> {10, -2}));
  EXPECT_EQ(h.mouse_at(150), (std::pair<std::int64_t, std::int64_t> {10, -2}));
  EXPECT_EQ(h.mouse_at(1000), (std::pair<std::int64_t, std::int64_t> {15, -1}));
  EXPECT_EQ(h.mouse_between(150, 250), (std::pair<std::int64_t, std::int64_t> {5, 1}));
  EXPECT_TRUE(h.mouse_moved(150, 250));
  EXPECT_FALSE(h.mouse_moved(200, 250));
}

TEST(FrameWarpHistory, TagsCarryTheSumsAndTheHeldPadPacket) {
  history_t h {std::uint8_t {0}};
  h.on_mouse(7, 3, 10);
  EXPECT_EQ(h.tag(1, 20, 20).held_us, no_held);
  h.on_controller(0, 100, 0, 30);
  h.on_controller(2, 9, 9, 35);
  h.on_controller(0, 200, 0, 50);
  const auto tag = h.tag(4, 40, 60);
  EXPECT_EQ(tag.frame, 4u);
  EXPECT_EQ(tag.mouse_x, 7);
  EXPECT_EQ(tag.mouse_y, 3);
  EXPECT_EQ(tag.controller_seq, 2u);
  EXPECT_EQ(tag.held_us, 10u);
  const auto early = h.tag(5, 5, 40);
  EXPECT_EQ(early.mouse_x, 0);
  EXPECT_EQ(early.controller_seq, 1u);
  EXPECT_EQ(early.held_us, 10u);
}

TEST(FrameWarpHistory, MouseSumsWrapAsSigned32Bit) {
  history_t h;
  for (int i = 0; i < 70000; ++i) {
    h.on_mouse(32767, 0, static_cast<std::uint64_t>(i));
  }
  const auto tag = h.tag(1, 100000, 100000);
  const auto expected = static_cast<std::int32_t>(static_cast<std::uint32_t>(70000ull * 32767ull));
  EXPECT_EQ(tag.mouse_x, expected);
}

TEST(FrameWarpHistory, StickIntegralFollowsTheHeldStates) {
  history_t h {std::uint8_t {1}};
  h.on_controller(1, 0, 0, 0);
  h.on_controller(1, 32767, 0, 1'000'000);
  h.on_controller(1, 0, -32768, 1'500'000);
  const auto full = h.stick_integral(1'000'000, 1'500'000, 0.0f, 1.0f);
  EXPECT_NEAR(full[0], 0.5, 1e-6);
  EXPECT_NEAR(full[1], 0.0, 1e-9);
  const auto spanning = h.stick_integral(500'000, 2'000'000, 0.0f, 1.0f);
  EXPECT_NEAR(spanning[0], 0.5, 1e-6);
  EXPECT_NEAR(spanning[1], -0.5, 1e-4);
  EXPECT_TRUE(h.stick_active(1'200'000, 1'300'000));
  EXPECT_FALSE(h.stick_active(100'000, 900'000));
}

TEST(FrameWarpHistory, OldHistoryIsDropped) {
  history_t h;
  h.on_mouse(1, 0, 0);
  h.on_mouse(1, 0, 1);
  h.on_mouse(1, 0, history_keep_us + 10);
  EXPECT_EQ(h.mouse_entries(), 2u);
  EXPECT_EQ(h.mouse_at(history_keep_us + 10), (std::pair<std::int64_t, std::int64_t> {3, 0}));
}

TEST(FrameWarpHistory, RunningStickIntegralsMatchTheDirectSum) {
  std::uint32_t seed = 0x9e3779b9u;
  const auto next = [&seed]() {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  history_t h {std::uint8_t {0}};
  std::uint64_t t = 50'000;
  for (int i = 0; i < 3'000; ++i) {
    // 0 to 4 ms apart (repeated times included), the stick anywhere, centred often.
    const std::uint64_t gap = next() % 4'001;
    const std::uint64_t divisor = 1 + next() % 2;
    t += gap / divisor;
    std::int16_t x = 0;
    std::int16_t y = 0;
    if (next() % 4 != 0) {
      x = static_cast<std::int16_t>(next() & 0xffffu);
      y = static_cast<std::int16_t>(next() & 0xffffu);
    }
    h.on_controller(0, x, y, t);
  }
  const auto end = t;
  const std::array<std::pair<float, float>, 4> curves {{{0.0f, 1.0f}, {0.15f, 1.0f}, {0.2f, 1.5f}, {0.25f, 2.0f}}};
  for (const auto &[deadzone, exponent] : curves) {
    const auto running = h.stick_integrals(deadzone, exponent);
    for (int i = 0; i < 2'000; ++i) {
      // Spans before the first packet, past the last, empty and reversed included.
      const std::uint64_t t0 = static_cast<std::uint64_t>(next()) % (end + 100'000);
      std::uint64_t t1 = t0;
      switch (next() % 8) {
        case 0:
          break;
        case 1:
          {
            const std::uint64_t back = next() % 10'000;
            t1 = t0 > back ? t0 - back : 0;
            break;
          }
        default:
          t1 = t0 + next() % 230'000;
          break;
      }
      const auto direct = h.stick_integral(t0, t1, deadzone, exponent);
      const auto fast = running.between(t0, t1);
      for (std::size_t axis = 0; axis < 2; ++axis) {
        EXPECT_NEAR(direct[axis], fast[axis], 1e-9) << t0 << ".." << t1 << " with " << deadzone << "/" << exponent;
      }
    }
  }
  // No pad packets: nothing either way.
  const history_t empty {std::uint8_t {0}};
  const auto none = empty.stick_integrals(0.1f, 1.0f).between(0, 1'000'000);
  EXPECT_EQ(none[0], 0.0);
  EXPECT_EQ(none[1], 0.0);
}

// ---- Motion ----

TEST(FrameWarpMotion, FindsAPanInBothDirections) {
  const std::array<std::pair<float, float>, 5> pans {{{6.0f, 0.0f}, {-11.0f, 3.0f}, {0.0f, -7.0f}, {23.0f, -9.0f}, {2.5f, 1.5f}}};
  for (const auto &[ox, oy] : pans) {
    const auto a = scene(256, 144, 0.0f, 0.0f, 0);
    const auto b = scene(256, 144, ox, oy, 1);
    const auto m = estimate(a, b);
    ASSERT_TRUE(m.has_value()) << "no estimate for " << ox << "," << oy;
    EXPECT_LT(std::abs(m->dx + ox), 0.6f) << "dx " << m->dx << " for " << ox;
    EXPECT_LT(std::abs(m->dy + oy), 0.6f) << "dy " << m->dy << " for " << oy;
    EXPECT_GT(m->confidence, 0.5f);
  }
}

TEST(FrameWarpMotion, SlowTurnsAreMeasuredToATenthOfAPixel) {
  const std::array<std::pair<float, float>, 5> pans {{{0.2f, 0.0f}, {0.0f, 0.3f}, {0.0f, -0.15f}, {0.45f, -0.4f}, {1.3f, 0.7f}}};
  for (const auto &[ox, oy] : pans) {
    const auto a = scene(256, 144, 0.0f, 0.0f, 0);
    const auto b = scene(256, 144, ox, oy, 1);
    const auto m = estimate(a, b);
    ASSERT_TRUE(m.has_value()) << "no estimate for " << ox << "," << oy;
    EXPECT_LT(std::abs(m->dx + ox), 0.1f) << "dx " << m->dx << " for " << ox;
    EXPECT_LT(std::abs(m->dy + oy), 0.1f) << "dy " << m->dy << " for " << oy;
  }
}

TEST(FrameWarpMotion, ASlowTiltOverUprightEdgesIsNotAStillPicture) {
  // Poles and walls: strong upright edges, which a tilt leaves unchanged, over texture.
  const auto upright = [](float oy) {
    auto t = scene(256, 144, 0.0f, oy, 0);
    for (std::size_t i = 0; i < t.luma.size(); ++i) {
      // Irregular widths: a periodic pattern has no one shift.
      const std::uint32_t column = static_cast<std::uint32_t>(i % 256 / 5);
      if (((column * 2654435761u) >> 31) == 1) {
        t.luma[i] = static_cast<std::uint8_t>(std::min(255, t.luma[i] + 70));
      }
    }
    return t;
  };
  const auto m = estimate(upright(0.0f), upright(0.25f));
  ASSERT_TRUE(m.has_value());
  EXPECT_LT(std::abs(m->dy + 0.25f), 0.1f) << m->dy;
}

TEST(FrameWarpMotion, StillPicturesHaveNoMotion) {
  const auto a = scene(256, 144, 0.0f, 0.0f, 0);
  const auto m = estimate(a, a);
  ASSERT_TRUE(m.has_value());
  EXPECT_LT(std::abs(m->dx), 0.1f);
  EXPECT_LT(std::abs(m->dy), 0.1f);
}

TEST(FrameWarpMotion, AStaticHudCellDoesNotMoveTheMedian) {
  const auto a = scene(256, 144, 0.0f, 0.0f, 0);
  auto b = scene(256, 144, 9.0f, 0.0f, 1);
  // Copy the top-left cell unchanged, as a HUD would stay.
  for (std::size_t y = 0; y < 48; ++y) {
    for (std::size_t x = 0; x < 85; ++x) {
      b.luma[y * 256 + x] = a.luma[y * 256 + x];
    }
  }
  const auto m = estimate(a, b);
  ASSERT_TRUE(m.has_value());
  EXPECT_LT(std::abs(m->dx + 9.0f), 0.6f) << m->dx;
}

TEST(FrameWarpMotion, AKnownHudDoesNotHoldBackATilt) {
  std::vector<std::uint8_t> hud(256 * 144);
  for (std::size_t i = 0; i < hud.size(); ++i) {
    hud[i] = hud_at(i % 256, i / 256) ? 1 : 0;
  }
  for (const float oy : {0.4f, 0.8f, 1.5f, -1.2f, 3.0f}) {
    const auto a = with_hud(scene(256, 144, 0.0f, 0.0f, 0));
    const auto b = with_hud(scene(256, 144, 4.0f, oy, 1));
    const auto m = estimate_around(a, b, hud);
    ASSERT_TRUE(m.has_value()) << "no estimate for " << oy;
    EXPECT_LT(std::abs(m->dy + oy), 0.35f) << "dy " << m->dy << " for " << oy;
    EXPECT_LT(std::abs(m->dx + 4.0f), 0.35f) << "dx " << m->dx << " for " << oy;
  }
}

TEST(FrameWarpMotion, AStillCameraWithOneThingMovingStandsStill) {
  const auto a = with_hud(scene(256, 144, 0.0f, 0.0f, 0));
  auto b = a;
  // Something crosses the middle cell; the rest of the picture holds.
  const auto mover = scene(256, 144, 7.0f, 2.0f, 1);
  for (std::size_t y = 55; y < 90; ++y) {
    for (std::size_t x = 100; x < 160; ++x) {
      b.luma[y * 256 + x] = mover.luma[y * 256 + x];
    }
  }
  const auto m = estimate(a, b);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->dx, 0.0f);
  EXPECT_EQ(m->dy, 0.0f);
  EXPECT_GT(m->confidence, 0.5f);
}

TEST(FrameWarpMotion, FlatPicturesAndMismatchedSizesGiveNothing) {
  thumbnail_t flat;
  flat.width = 64;
  flat.height = 36;
  flat.luma.assign(64 * 36, 90);
  EXPECT_FALSE(estimate(flat, flat).has_value());
  const auto a = scene(64, 36, 0.0f, 0.0f, 0);
  const auto b = scene(64, 40, 0.0f, 0.0f, 0);
  EXPECT_FALSE(estimate(a, b).has_value());
}

TEST(FrameWarpMotion, ACellIsMeasuredWhereItsTextureIs) {
  // A profile flat but for one ramp: its slopes, and so its shift, are the ramp's.
  std::vector<float> profile(100, 0.0f);
  for (std::size_t i = 60; i < 70; ++i) {
    profile[i] = static_cast<float>(i - 60);
  }
  for (std::size_t i = 70; i < 100; ++i) {
    profile[i] = 10.0f;
  }
  const std::vector<float> beside(100, 7.0f);
  const auto found = place_of(profile, beside, 40, 90);
  ASSERT_TRUE(found.has_value());
  EXPECT_NEAR(found->across, 7.0f, 1e-4f);
  EXPECT_NEAR(found->cross, 7.0f * found->along, 1e-2f);
  // The ramp runs from sample 60 to 70, 10 to 20 past the middle.
  EXPECT_NEAR(found->along, 15.0f, 0.6f);
  EXPECT_NEAR(found->squared, 15.0f * 15.0f, 25.0f);
  // Textured alike all over, it is the cell's middle; unknown samples are left out.
  std::vector<float> even(100);
  for (std::size_t i = 0; i < 100; ++i) {
    even[i] = (i % 2 == 0 ? 1.0f : -1.0f) * static_cast<float>(i % 4);
  }
  const auto mean = place_of(even, beside, 40, 90);
  ASSERT_TRUE(mean.has_value());
  EXPECT_NEAR(mean->along, 15.0f, 1.5f);
  auto holed = profile;
  holed[65] = std::nanf("");
  EXPECT_TRUE(place_of(holed, beside, 40, 90).has_value());
  EXPECT_FALSE(place_of(std::vector<float>(100, 0.0f), beside, 40, 90).has_value());
  // A measured move carries each cell's place.
  const auto motion = estimate(scene(256, 144, 0.0f, 0.0f, 0), scene(256, 144, 3.0f, 0.0f, 1));
  ASSERT_TRUE(motion.has_value());
  ASSERT_TRUE(motion->cells[0].has_value());
  ASSERT_TRUE(motion->cells[0]->place.has_value());
  const auto &x = (*motion->cells[0]->place)[0];
  const auto &y = (*motion->cells[0]->place)[1];
  EXPECT_TRUE(x.along < -40.0f && y.along < -20.0f && x.squared > x.along * x.along * 0.9f);
  // Each across the other's axis in the cell's middle: all of it is scene.
  EXPECT_LT(std::abs(x.across + 44.0f), 3.0f) << x.across;
  EXPECT_LT(std::abs(y.across + 78.0f), 3.0f) << y.across;
}

TEST(FrameWarpMotion, WhatStandsStillWhileThePictureMovesIsFoundAndLetGo) {
  const auto near_hud = [](std::size_t x, std::size_t y) {
    for (std::size_t ny = y > 2 ? y - 2 : 0; ny <= y + 2; ++ny) {
      for (std::size_t nx = x > 2 ? x - 2 : 0; nx <= x + 2; ++nx) {
        if (hud_at(nx, ny)) {
          return true;
        }
      }
    }
    return false;
  };
  still_t still;
  float ox = 0.0f;
  float oy = 0.0f;
  auto previous = with_hud(scene(256, 144, ox, oy, 0));
  // Pans with a little tilt, to and fro; then the HUD goes (a menu closes).
  for (std::uint64_t i = 1; i <= 60; ++i) {
    ox += (i / 10) % 2 == 0 ? 3.3f : -2.7f;
    oy += (i / 7) % 2 == 0 ? 0.8f : -0.6f;
    auto current = scene(256, 144, ox, oy, i);
    if (i <= 40) {
      current = with_hud(std::move(current));
    }
    const auto motion = estimate_around(previous, current, still.mask());
    ASSERT_TRUE(motion.has_value()) << i;
    still.observe(previous, current, *motion);
    if (i == 40) {
      const auto mask = still.mask();
      ASSERT_FALSE(mask.empty()) << "a HUD";
      std::size_t hud = 0;
      std::size_t marked = 0;
      std::size_t astray = 0;
      for (std::size_t p = 0; p < 256 * 144; ++p) {
        const bool is_hud = hud_at(p % 256, p / 256);
        hud += is_hud ? 1 : 0;
        marked += (is_hud && mask[p] != 0) ? 1 : 0;
        astray += (mask[p] != 0 && !near_hud(p % 256, p / 256)) ? 1 : 0;
      }
      // Its rims and its pattern; the middle of its plain bar says nothing.
      EXPECT_GE(marked * 10, hud * 8) << marked << " of " << hud;
      // Of the scene, only the pixels at its rim.
      EXPECT_LE(astray, 8u) << astray << " of the scene";
      // And the shift is the scene's, though the host was told of no HUD.
      EXPECT_LT(std::abs(motion->dx + 3.3f), 0.35f) << motion->dx;
      EXPECT_LT(std::abs(motion->dy - 0.6f), 0.35f) << motion->dy;
    }
    previous = std::move(current);
  }
  std::size_t left = 0;
  for (const auto m : still.mask()) {
    left += m != 0 ? 1 : 0;
  }
  EXPECT_LE(left, 8u) << left << " still marked";
}

TEST(FrameWarpMotion, APanTellsNothingOfAHorizonOrASky) {
  // A plain sky over a plain strip of ground with a straight horizon between, texture below: the
  // pan brings every pixel of the upper two more of the same.
  const auto seen = [](float ox, std::uint64_t at_us) {
    auto t = scene(256, 144, ox, 0.0f, at_us);
    for (std::size_t i = 0; i < 256 * 70; ++i) {
      t.luma[i] = i / 256 < 60 ? 200 : 90;
    }
    return t;
  };
  still_t still;
  auto previous = seen(0.0f, 0);
  for (std::uint64_t i = 1; i <= 60; ++i) {
    auto current = seen(static_cast<float>(i) * 3.5f, i);
    const auto motion = estimate(previous, current);
    ASSERT_TRUE(motion.has_value()) << i;
    EXPECT_FALSE(still.observe(previous, current, *motion)) << i;
    previous = std::move(current);
  }
  EXPECT_TRUE(still.mask().empty());
}

TEST(FrameWarpMotion, ALineIsNotMeasuredBesideAHud) {
  // A slow gradient: all of it goes, but where the average would lean away from the HUD.
  std::vector<float> line(40);
  for (std::size_t i = 0; i < line.size(); ++i) {
    line[i] = 3.0f * static_cast<float>(i);
  }
  line[20] = std::nanf("");
  std::vector<float> prefix;
  std::vector<std::uint32_t> count;
  detrend(line, prefix, count);
  for (std::size_t i = 14; i <= 26; ++i) {
    EXPECT_TRUE(std::isnan(line[i])) << i;
  }
  EXPECT_LT(std::abs(line[13]), 1e-3f);
  EXPECT_LT(std::abs(line[27]), 1e-3f);
  // At the picture's edge it leans and stays: the cells keep their margin from it.
  EXPECT_LT(std::abs(line[0] + 9.0f), 1e-3f);

  // Rows half under a HUD are the mean of other columns than their neighbours, here darker ones:
  // no step for that in the profile.
  thumbnail_t t;
  t.width = 64;
  t.height = 64;
  for (std::size_t i = 0; i < 64 * 64; ++i) {
    t.luma.push_back(i % 64 < 32 ? 200 : 50);
  }
  const auto hud = [](std::size_t x, std::size_t y) {
    return x < 32 && y >= 24 && y < 40;
  };
  lines_t lines;
  std::vector<float> profile;
  profile_rows(
    t, 0, 64, 0, [&hud](std::size_t x, std::size_t y) {
      return !hud(x, y);
    },
    lines,
    profile
  );
  for (const auto v : profile) {
    EXPECT_LT(std::abs(v), 1e-3f);
  }
  // And those rows lie where their columns do.
  std::vector<float> across;
  lines.across(across);
  EXPECT_LT(std::abs(across[10] - 0.0f), 1e-3f);
  EXPECT_LT(std::abs(across[30] - 16.0f), 1e-3f);
}

// ---- The lens ----

namespace {
  constexpr std::pair<std::uint16_t, std::uint16_t> lens_content {2560, 1440};
  constexpr std::pair<std::size_t, std::size_t> lens_thumbnail {256, 144};

  double tangent(double degrees) {
    return tangent_of(degrees);
  }

  /**
   * The move a camera turning by `turn` thumbnail pixels at the centre makes through a lens of
   * `degrees` (0: a flat picture), with a zoom, a roll and noise of `noise` pixels.
   */
  motion_t turned(const layout_t &layout, double degrees, std::pair<double, double> turn, double zoom, double roll, double noise, std::uint32_t seed) {
    const double lens = degrees > 0.0 ? tangent(degrees) : 0.0;
    std::uint32_t state = (seed * 0x9e3779b9u) | 1u;
    const auto random = [&state]() {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      return static_cast<double>(state) / static_cast<double>(0xffffffffu) - 0.5;
    };
    const double a = turn.first * layout.scale[0];
    const double b = turn.second * layout.scale[1];
    motion_t out;
    for (std::size_t i = 0; i < grid_cells; ++i) {
      const auto &c = layout.cells[i];
      const double u = a * (1.0 + lens * c.xx) + b * lens * c.xy + zoom * c.x - roll * c.y;
      const double v = a * lens * c.xy + b * (1.0 + lens * c.yy) + zoom * c.y + roll * c.x;
      cell_shift_t shift;
      const double nx = noise * 2.0 * random();
      const double ny = noise * 2.0 * random();
      shift.dx = static_cast<float>(u / layout.scale[0] + nx);
      shift.dy = static_cast<float>(v / layout.scale[1] + ny);
      shift.correlation = 0.9f;
      out.cells[i] = shift;
    }
    const auto median = [&out](bool x_axis) {
      std::vector<float> values;
      for (const auto &c : out.cells) {
        values.push_back(x_axis ? c->dx : c->dy);
      }
      std::sort(values.begin(), values.end());
      return values[values.size() / 2];
    };
    out.dx = median(true);
    out.dy = median(false);
    out.confidence = 0.9f;
    return out;
  }

  std::pair<layout_t, lens_t> learn(double degrees, double noise, std::size_t moves) {
    const layout_t layout {lens_content, lens_thumbnail};
    lens_t lens;
    for (std::size_t i = 0; i < moves; ++i) {
      // Turns left and right, a little up and down, some while walking or rolling.
      const double phase = static_cast<double>(i) * 0.37;
      const std::pair<double, double> turn {4.0 * std::sin(phase) + 1.5, 1.2 * std::cos(phase * 0.6)};
      const double zoom = i % 3 == 0 ? 3.0 : 0.0;
      const double roll = i % 5 == 0 ? -2.0 : 0.0;
      lens.observe(layout, turned(layout, degrees, turn, zoom, roll, noise, static_cast<std::uint32_t>(i + 1)));
    }
    return {layout, lens};
  }

  std::optional<double> degrees_of(const lens_t &lens) {
    const auto t = lens.tangent();
    if (!t) {
      return std::nullopt;
    }
    return 2.0 * std::atan(std::sqrt(*t)) * 180.0 / std::numbers::pi;
  }

  /**
   * The textured world on a cylinder around the camera, seen through a lens of `focal` thumbnail
   * pixels by a camera pitched by `pitch` and then turned by `yaw` (radians), as a game's is.
   */
  thumbnail_t seen(double focal, double yaw, double pitch, std::uint64_t at_us) {
    const auto [width, height] = lens_thumbnail;
    thumbnail_t out;
    out.width = width;
    out.height = height;
    out.at_us = at_us;
    out.luma.reserve(width * height);
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        double px = static_cast<double>(x) + 0.5 - static_cast<double>(width) * 0.5;
        double py = static_cast<double>(y) + 0.5 - static_cast<double>(height) * 0.5;
        double pz = focal;
        const double py2 = py * std::cos(pitch) + focal * std::sin(pitch);
        const double pz2 = focal * std::cos(pitch) - py * std::sin(pitch);
        py = py2;
        pz = pz2;
        const double px2 = px * std::cos(yaw) + pz * std::sin(yaw);
        const double pz3 = pz * std::cos(yaw) - px * std::sin(yaw);
        px = px2;
        pz = pz3;
        const double u = focal * std::atan2(px, pz);
        const double v = focal * py / std::hypot(px, pz);
        out.luma.push_back(world(static_cast<float>(u) + 4000.0f, static_cast<float>(v) + 4000.0f));
      }
    }
    return out;
  }

  /// Where `lens_with_hud` draws: a framed minimap in the top right corner, a counter in the bottom
  /// right, a bar in the bottom left, and in the top left a stamp that changes with every capture.
  std::optional<int> lens_hud_at(std::size_t x, std::size_t y) {
    if (x >= 218 && x < 253 && y >= 3 && y < 38) {
      return 0;
    }
    if (x >= 230 && x < 253 && y >= 132 && y < 141) {
      return 1;
    }
    if (x >= 3 && x < 44 && y >= 137 && y < 141) {
      return 2;
    }
    if (x < 58 && y >= 1 && y < 11) {
      return 3;
    }
    return std::nullopt;
  }

  /// `t` under a HUD as a game draws one, the same in every capture but for its stamp.
  thumbnail_t lens_with_hud(thumbnail_t t, std::uint64_t capture) {
    for (std::size_t y = 0; y < t.height; ++y) {
      for (std::size_t x = 0; x < t.width; ++x) {
        const auto part = lens_hud_at(x, y);
        if (!part) {
          continue;
        }
        std::uint8_t value = 0;
        switch (*part) {
          // A light frame around dark ground with blocks on it.
          case 0:
            if (!(x >= 220 && x < 251) || !(y >= 5 && y < 36)) {
              value = 200;
            } else if ((x / 9 + y / 11) % 3 == 0) {
              value = 120;
            } else {
              value = 40;
            }
            break;
          case 1:
            value = (x / 3) % 2 == 0 ? 220 : 25;
            break;
          case 2:
            value = x < 32 ? 90 : 30;
            break;
          default:
            {
              const std::uint64_t bit = ((static_cast<std::uint64_t>(x) / 2) * 0x9e3779b97f4a7c15ull) ^ ((capture + static_cast<std::uint64_t>(y) / 3 * 77) * 0xbf58476d1ce4e5b9ull);
              value = ((bit ^ (bit >> 31)) & 1) == 0 ? 0 : 255;
              break;
            }
        }
        t.luma[y * t.width + x] = value;
      }
    }
    return t;
  }
}  // namespace

TEST(FrameWarpLens, APerspectivePictureTeachesItsFieldOfView) {
  for (const double field : {50.0, 70.0, 90.0, 104.0}) {
    const auto [layout, lens] = learn(field, 0.15, 240);
    const auto found = degrees_of(lens);
    ASSERT_TRUE(found.has_value()) << "no lens for " << field;
    EXPECT_LE(std::abs(*found - field), 4.0) << *found << " for " << field;
  }
}

TEST(FrameWarpLens, AFlatPictureHasNoLens) {
  const auto flat = learn(0.0, 0.15, 240);
  EXPECT_FALSE(flat.second.tangent().has_value());
  // And noise alone, at a narrow field of view's size, does not make one.
  const auto noisy = learn(0.0, 0.4, 240);
  EXPECT_FALSE(noisy.second.tangent().has_value());
}

TEST(FrameWarpLens, ALensWaitsForEnoughTurningAndStartsFromThePrior) {
  auto [layout, lens] = learn(90.0, 0.15, 12);
  EXPECT_FALSE(lens.tangent().has_value());
  lens.set_prior(tangent(80.0));
  ASSERT_TRUE(degrees_of(lens).has_value());
  EXPECT_EQ(std::round(*degrees_of(lens)), 80.0);
  // The moves then say otherwise.
  bool changed = false;
  for (std::uint32_t i = 0; i < 240; ++i) {
    const std::pair<double, double> turn {4.0 * std::sin(static_cast<double>(i) * 0.37) + 1.5, 1.0};
    changed = lens.observe(layout, turned(layout, 90.0, turn, 0.0, 0.0, 0.15, i + 7)) || changed;
  }
  EXPECT_TRUE(changed);
  const auto found = degrees_of(lens);
  ASSERT_TRUE(found.has_value());
  EXPECT_LE(std::abs(*found - 90.0), 4.0) << *found;
  // Cleared, the lens in force stays as the prior.
  lens.clear();
  ASSERT_TRUE(degrees_of(lens).has_value());
  EXPECT_LT(std::abs(*degrees_of(lens) - *found), 1e-9);
}

TEST(FrameWarpLens, ALensFollowsAGameThatChangesItsFieldOfViewAndGoesWhenItGoesFlat) {
  auto [layout, lens] = learn(90.0, 0.15, 240);
  for (std::uint32_t i = 0; i < 900; ++i) {
    const std::pair<double, double> turn {4.0 * std::sin(static_cast<double>(i) * 0.37) + 1.5, 1.0};
    lens.observe(layout, turned(layout, 60.0, turn, 0.0, 0.0, 0.15, i + 11));
  }
  const auto found = degrees_of(lens);
  ASSERT_TRUE(found.has_value());
  EXPECT_LE(std::abs(*found - 60.0), 4.0) << *found;
  for (std::uint32_t i = 0; i < 900; ++i) {
    const std::pair<double, double> turn {4.0 * std::sin(static_cast<double>(i) * 0.37) + 1.5, 1.0};
    lens.observe(layout, turned(layout, 0.0, turn, 0.0, 0.0, 0.15, i + 13));
  }
  EXPECT_FALSE(lens.tangent().has_value());
}

TEST(FrameWarpLens, SlowOrSparseMovesTeachNothing) {
  const layout_t layout {lens_content, lens_thumbnail};
  lens_t lens;
  for (std::uint32_t i = 0; i < 400; ++i) {
    auto slow = turned(layout, 90.0, {0.4, 0.1}, 0.0, 0.0, 0.1, i + 1);
    EXPECT_FALSE(lens.observe(layout, slow));
    // A fast move seen by four cells only.
    slow = turned(layout, 90.0, {5.0, 0.0}, 0.0, 0.0, 0.1, i + 1);
    for (std::size_t c = 4; c < grid_cells; ++c) {
      slow.cells[c].reset();
    }
    EXPECT_FALSE(lens.observe(layout, slow));
  }
  EXPECT_FALSE(lens.tangent().has_value());
}

TEST(FrameWarpLens, TheTurnIsTheCentresUnderALensAndTheMedianWithout) {
  const layout_t layout {lens_content, lens_thumbnail};
  auto motion = turned(layout, 90.0, {6.0, -2.0}, 2.0, 1.0, 0.0, 1);
  // Most cells lie off centre, so the median is longer than the centre's move.
  EXPECT_GT(motion.dx, 6.5f);
  const auto flat = layout.resolve(motion, std::nullopt);
  EXPECT_EQ(flat.dx, motion.dx);
  EXPECT_EQ(flat.dy, motion.dy);
  auto turn = layout.resolve(motion, tangent(90.0));
  EXPECT_LT(std::abs(turn.dx - 6.0f), 0.01f) << turn.dx;
  EXPECT_LT(std::abs(turn.dy + 2.0f), 0.01f) << turn.dy;
  EXPECT_GT(turn.confidence, 0.85f);
  // A cell where something else moves is left out and costs confidence.
  cell_shift_t other;
  other.dx = -9.0f;
  other.dy = 4.0f;
  other.correlation = 0.9f;
  motion.cells[2] = other;
  turn = layout.resolve(motion, tangent(90.0));
  EXPECT_LT(std::abs(turn.dx - 6.0f), 0.05f) << turn.dx;
  EXPECT_LT(std::abs(turn.dy + 2.0f), 0.05f) << turn.dy;
  EXPECT_LT(turn.confidence, 0.85f);
  // A still picture has no cells and stays as it was measured.
  motion_t still;
  still.confidence = 0.8f;
  EXPECT_EQ(layout.resolve(still, tangent(90.0)).confidence, 0.8f);
}

TEST(FrameWarpLens, PicturesDrawnThroughALensTeachItAndGiveTheCentresTurn) {
  for (const double field : {60.0, 90.0}) {
    const layout_t layout {lens_content, lens_thumbnail};
    const double focal = static_cast<double>(lens_thumbnail.first) * 0.5 / std::tan(field * std::numbers::pi / 180.0 * 0.5);
    lens_t lens;
    double yaw = 0.0;
    double pitch = 0.0;
    auto previous = seen(focal, yaw, pitch, 0);
    double worst = 0.0;
    for (std::uint64_t i = 1; i <= 150; ++i) {
      // Turns of 1.5 to 5 thumbnail pixels a capture, left and right, with some pitch.
      const double step = (3.2 + 1.8 * std::sin(static_cast<double>(i) * 0.21)) * ((i / 40) % 2 == 0 ? 1.0 : -1.0);
      const double tilt = 0.6 * std::cos(static_cast<double>(i) * 0.13);
      yaw += step / focal;
      pitch += tilt / focal;
      auto current = seen(focal, yaw, pitch, i);
      const auto motion = estimate(previous, current);
      ASSERT_TRUE(motion.has_value()) << "a move at " << i;
      lens.observe(layout, *motion);
      if (i > 100) {
        // The content moves against the camera, by the turn at the picture's centre.
        const auto turn = layout.resolve(*motion, lens.tangent());
        worst = std::max(worst, std::abs(static_cast<double>(turn.dx) + step));
        EXPECT_GE(turn.confidence, 0.5f) << "at " << i << " for " << field;
      }
      previous = std::move(current);
    }
    const auto found = degrees_of(lens);
    ASSERT_TRUE(found.has_value()) << "no lens for " << field;
    EXPECT_LE(std::abs(*found - field), 2.0) << *found << " for " << field;
    EXPECT_LT(worst, 0.35) << worst << " for " << field;
  }
}

TEST(FrameWarpLens, AHudInTheCornersDoesNotNarrowTheLens) {
  // Left in the measurement, this HUD's corner cells move too little and a 90 degree lens reads as
  // 81. The host knows it from the GPU's mask, or finds what stands still.
  for (const bool known : {true, false}) {
    const layout_t layout {lens_content, lens_thumbnail};
    const double focal = static_cast<double>(lens_thumbnail.first) * 0.5;
    std::vector<std::uint8_t> hud(lens_thumbnail.first * lens_thumbnail.second);
    for (std::size_t i = 0; i < hud.size(); ++i) {
      const auto part = lens_hud_at(i % lens_thumbnail.first, i / lens_thumbnail.first);
      hud[i] = (part && *part < 3) ? 1 : 0;
    }
    still_t still;
    lens_t lens;
    double yaw = 0.0;
    double pitch = 0.0;
    auto previous = lens_with_hud(seen(focal, yaw, pitch, 0), 0);
    const std::uint64_t frames = known ? 150 : 300;
    for (std::uint64_t i = 1; i <= frames; ++i) {
      const double step = (3.2 + 1.8 * std::sin(static_cast<double>(i) * 0.21)) * ((i / 40) % 2 == 0 ? 1.0 : -1.0);
      yaw += step / focal;
      pitch += 0.6 * std::cos(static_cast<double>(i) * 0.13) / focal;
      auto current = lens_with_hud(seen(focal, yaw, pitch, i), i);
      const std::span<const std::uint8_t> left_out = known ? std::span<const std::uint8_t> {hud} : still.mask();
      if (const auto motion = estimate_around(previous, current, left_out)) {
        still.observe(previous, current, *motion);
        lens.observe(layout, *motion);
      }
      previous = std::move(current);
    }
    const auto found = degrees_of(lens);
    ASSERT_TRUE(found.has_value()) << "no lens, known " << known;
    EXPECT_LE(std::abs(*found - 90.0), 2.5) << *found << ", known " << known;
  }
}

TEST(FrameWarpLens, AMoveThatIsNoTurnDoesNotChooseALens) {
  auto [layout, lens] = learn(90.0, 0.15, 240);
  const auto before = degrees_of(lens);
  ASSERT_TRUE(before.has_value());
  // Cuts: every cell somewhere else, far from any lens's turn.
  for (std::uint32_t i = 0; i < 40; ++i) {
    auto cut = turned(layout, 90.0, {4.0, 1.0}, 0.0, 0.0, 0.15, i + 3);
    for (std::size_t index = 0; index < cut.cells.size(); ++index) {
      auto &cell = *cut.cells[index];
      const float wild = static_cast<float>((static_cast<std::uint32_t>(index) * 7 + i * 13) % 11) - 5.0f;
      cell.dx = cell.dx + 3.0f * wild;
      cell.dy = cell.dy - 2.0f * wild;
    }
    lens.observe(layout, cut);
    // And a turn between them, as play goes on.
    lens.observe(layout, turned(layout, 90.0, {4.0, 1.0}, 0.0, 0.0, 0.15, i + 90));
  }
  const auto after = degrees_of(lens);
  ASSERT_TRUE(after.has_value()) << "still a lens";
  EXPECT_LE(std::abs(*after - *before), 2.0) << *before << " then " << *after;
}

TEST(FrameWarpLens, FocalLengthsAndTangentsConvert) {
  const layout_t layout {lens_content, lens_thumbnail};
  const double focal = layout.focal(tangent(90.0));
  EXPECT_NEAR(focal, 1280.0, 1e-6);
  EXPECT_NEAR(layout.tangent(focal), tangent(90.0), 1e-9);
}

// ---- Calibration ----

namespace {
  struct mouse_run_t {
    history_t history;
    calibrator_t cal;
    std::uint64_t end;
  };

  /**
   * A game at 60 fps captured at 120 fps: frame `i` shows the camera as of the input that arrived
   * `lag_us` before it was rendered, `gain` thumbnail pixels per count; the mouse's dy at a time and
   * turn speed given by `dy_at`, and the picture also rolling up and down by `roll` thumbnail
   * pixels, whatever the input.
   */
  mouse_run_t mouse_session_with(double gain, std::uint64_t lag_us, float noise, double (*dy_at)(double, double), double roll) {
    history_t history;
    calibrator_t cal {{1920, 1080}, {256, 144}};
    std::uint32_t seed = 7;
    const auto random = [&seed]() {
      seed = seed * 1'664'525u + 1'013'904'223u;
      return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24) - 0.5f;
    };
    // Input every 2 ms: a sweeping turn with pauses.
    const std::uint64_t end = 3'500'000;
    for (std::uint64_t t = 0; t < end; t += 2'000) {
      const double phase = static_cast<double>(t) / 1e6;
      const double speed = std::sin(phase * 2.3) * std::cos(phase * 0.7) * 9.0;
      const double dy = dy_at(phase, speed);
      history.on_mouse(static_cast<std::int16_t>(speed), static_cast<std::int16_t>(dy), t);
    }
    const auto camera = [&](std::uint64_t t) {
      const auto [x, y] = history.mouse_at(t > lag_us ? t - lag_us : 0);
      const double rolled = roll * std::sin(static_cast<double>(t) / 1e6 * 1.7);
      return std::pair<double, double> {static_cast<double>(x) * gain, static_cast<double>(y) * gain - rolled};
    };
    const std::uint64_t frame_us = 16'667;
    const std::uint64_t capture_us = 8'333;
    for (std::uint64_t t = capture_us; t < end; t += capture_us) {
      // The picture captured at t shows the game's last frame, rendered at a multiple of 16.7 ms.
      const auto rendered = [frame_us](std::uint64_t at) {
        return at / frame_us * frame_us;
      };
      const auto [x0, y0] = camera(rendered(t - capture_us));
      const auto [x1, y1] = camera(rendered(t));
      sample_t sample;
      sample.t0_us = t - capture_us;
      sample.t1_us = t;
      // The content moves against the camera.
      const float nx = noise * random();
      const float ny = noise * random();
      sample.motion.dx = static_cast<float>(-(x1 - x0)) + nx;
      sample.motion.dy = static_cast<float>(-(y1 - y0)) + ny;
      sample.motion.confidence = 0.9f;
      cal.add(sample);
    }
    return mouse_run_t {std::move(history), std::move(cal), end};
  }

  mouse_run_t mouse_session(double gain, std::uint64_t lag_us, float noise) {
    return mouse_session_with(
      gain,
      lag_us,
      noise,
      [](double phase, double) {
        return std::sin(phase * 1.1) * 4.0;
      },
      0.0
    );
  }
}  // namespace

TEST(FrameWarpCalibrate, LearnsAMouseGainAndLag) {
  auto run = mouse_session(0.12, 50'000, 0.3f);
  const auto model = run.cal.update(run.history, run.end);
  ASSERT_TRUE(model.has_value()) << "a locked model";
  EXPECT_TRUE(model->mouse_locked && !model->stick_locked);
  // 0.12 thumbnail px per count is 0.9 picture px per count at 1920 / 256.
  EXPECT_LT(std::abs(model->mouse_gain[0] + 0.9f), 0.05f) << model->mouse_gain[0];
  EXPECT_LT(std::abs(model->mouse_gain[1] + 0.12f * 1080.0f / 144.0f), 0.08f) << model->mouse_gain[1];
  // Within a game frame of the true lag, and not later than it plus one.
  EXPECT_GE(model->lag_us, 30'000u);
  EXPECT_LE(model->lag_us, 70'000u);
  EXPECT_EQ(model->max_shift[0], 288.0f);
  EXPECT_EQ(model->max_shift[1], 162.0f);
  EXPECT_EQ(model->generation, 1);
  // Nothing new: no new model.
  EXPECT_FALSE(run.cal.update(run.history, run.end + fit_every_us).has_value());
}

TEST(FrameWarpCalibrate, ASliverOfVerticalInputTakesTheHorizontalGain) {
  // Turns that drift down a count for every 12 across, while the picture rolls by 3 thumbnail
  // pixels: the vertical gain fitted alone would mostly fit the roll.
  auto run = mouse_session_with(
    0.12,
    50'000,
    0.3f,
    [](double, double speed) {
      return std::round(speed / 12.0);
    },
    3.0
  );
  const auto model = run.cal.update(run.history, run.end);
  ASSERT_TRUE(model.has_value()) << "a locked model";
  EXPECT_TRUE(model->mouse_locked);
  EXPECT_LT(std::abs(model->mouse_gain[0] + 0.9f), 0.05f) << model->mouse_gain[0];
  EXPECT_LT(std::abs(model->mouse_gain[1] - model->mouse_gain[0]), 1e-4f);
}

TEST(FrameWarpCalibrate, ALockedModelStaysUntilAFitExplainsClearlyMore) {
  auto run = mouse_session(0.12, 50'000, 0.3f);
  ASSERT_TRUE(run.cal.update(run.history, run.end).has_value());
  const auto learned = *run.cal.mouse_fit();
  // A model a couple of milliseconds off explains the motion almost as well as the best fit, so its
  // lag stays rather than wandering with every fit.
  auto late = learned;
  late.lag_us = learned.lag_us + 4'000;
  run.cal.set_mouse_fit(late);
  run.cal.update(run.history, run.end + fit_every_us);
  EXPECT_TRUE(run.cal.mouse_locked());
  const auto kept = *run.cal.mouse_fit();
  EXPECT_EQ(kept.lag_us, late.lag_us);
  EXPECT_LT(std::abs(kept.gain[0] / learned.gain[0] - 1.0), 0.02);
  // A gain 4% off at the right lag explains almost as much too: the lag stays and the gain settles
  // on the least-squares one there.
  auto off = learned;
  off.gain = {learned.gain[0] * 0.96, learned.gain[1] * 0.96};
  run.cal.set_mouse_fit(off);
  run.cal.update(run.history, run.end + 2 * fit_every_us);
  const auto settled = *run.cal.mouse_fit();
  EXPECT_EQ(settled.lag_us, learned.lag_us);
  EXPECT_LT(std::abs(settled.gain[0] / learned.gain[0] - 1.0), 0.005);
  // One with half the gain explains clearly less: the fresh fit replaces it.
  auto weak = late;
  weak.gain = {late.gain[0] * 0.5, late.gain[1] * 0.5};
  run.cal.set_mouse_fit(weak);
  run.cal.update(run.history, run.end + 3 * fit_every_us);
  const auto refit = *run.cal.mouse_fit();
  EXPECT_TRUE(run.cal.mouse_locked());
  EXPECT_LT(std::abs(refit.gain[0] - learned.gain[0]), 0.01);
}

TEST(FrameWarpCalibrate, ALockedLagGivesWayToAFitThatExplainsAQuarterOfWhatItLeaves) {
  fit_t locked;
  locked.gain = {1.0, 1.0};
  locked.lag_us = 50'000;
  locked.fit = 0.0;
  // The locked model explains `share` of the motion; the least-squares gain at its lag no more.
  const auto at = [](double share) {
    return explain_fn {[share](const fit_t &model, std::optional<std::array<double, 2>> gain) {
      fit_t out = model;
      out.fit = share;
      out.gain = gain ? *gain : model.gain;
      return std::optional<fit_t> {out};
    }};
  };
  const auto replaced = [&](double share, double fit) {
    auto model = locked;
    fit_t fresh = locked;
    fresh.fit = fit;
    fresh.lag_us = 54'000;
    std::optional<fit_t> found = fresh;
    keep_unless_better(model, found, at(share));
    return found.has_value();
  };
  // At 99% a quarter of the rest is a quarter point.
  EXPECT_FALSE(replaced(0.990, 0.992));
  EXPECT_TRUE(replaced(0.990, 0.9935));
  // At 80% the 3 points apply.
  EXPECT_FALSE(replaced(0.80, 0.82));
  EXPECT_TRUE(replaced(0.80, 0.84));
}

TEST(FrameWarpCalibrate, MotionThatIgnoresTheMouseNeverLocks) {
  auto run = mouse_session(0.12, 50'000, 0.3f);
  calibrator_t cal {{1920, 1080}, {256, 144}};
  // A menu: the mouse moves, the picture does not (only noise).
  for (std::uint64_t t = 8'333; t < run.end; t += 8'333) {
    sample_t sample;
    sample.t0_us = t - 8'333;
    sample.t1_us = t;
    sample.motion.dx = static_cast<float>((t / 8'333) % 3) - 1.0f;
    sample.motion.dy = 0.0f;
    sample.motion.confidence = 0.9f;
    cal.add(sample);
  }
  EXPECT_FALSE(cal.update(run.history, run.end).has_value());
  EXPECT_FALSE(cal.locked());
}

TEST(FrameWarpCalibrate, ALockedModelUnlocksWhenThePictureStopsFollowing) {
  auto run = mouse_session(0.12, 50'000, 0.3f);
  const auto locked = run.cal.update(run.history, run.end);
  ASSERT_TRUE(locked.has_value());
  EXPECT_TRUE(locked->mouse_locked);
  const auto end = run.end;
  // A cutscene: input continues (the same history), the picture stands still.
  for (std::uint64_t t = end; t < end + 1'500'000; t += 2'000) {
    run.history.on_mouse(8, 0, t);
  }
  for (std::uint64_t t = end; t < end + 1'500'000; t += 8'333) {
    sample_t sample;
    sample.t0_us = t;
    sample.t1_us = t + 8'333;
    sample.motion.confidence = 0.9f;
    run.cal.add(sample);
  }
  const auto model = run.cal.update(run.history, end + 1'500'000);
  ASSERT_TRUE(model.has_value()) << "an unlock";
  EXPECT_FALSE(model->mouse_locked);
}

TEST(FrameWarpCalibrate, APriorSurvivesTheRoundTripInPicturePixels) {
  auto run = mouse_session(0.12, 50'000, 0.3f);
  run.cal.update(run.history, run.end);
  const auto prior = run.cal.prior();
  ASSERT_TRUE(prior.has_value());
  ASSERT_TRUE(prior->mouse_gain.has_value());
  EXPECT_LT(std::abs((*prior->mouse_gain)[0] + 0.9f), 0.05f);
  // A new session at a different size starts from it, scaled.
  calibrator_t next {{1280, 720}, {256, 144}};
  next.set_prior(*prior);
  const auto model = next.model(1);
  EXPECT_LT(std::abs(model.mouse_gain[0] + 0.6f), 0.04f) << model.mouse_gain[0];
  EXPECT_FALSE(model.mouse_locked);
  EXPECT_EQ(next.mouse_lag_us(), *prior->mouse_lag_us);
}

TEST(FrameWarpCalibrate, APriorGoesThroughJson) {
  prior_t prior;
  prior.mouse_gain = std::array<float, 2> {-0.9f, -0.5f};
  prior.mouse_lag_us = 30'000;
  prior.stick_deadzone = 0.15f;
  prior.focal = 960.0f;
  prior.picture_width = 1920;
  prior.picture_height = 1080;
  const nlohmann::json j = prior;
  EXPECT_TRUE(j.at("stick_gain").is_null());
  EXPECT_EQ(j.at("picture_width").get<int>(), 1920);
  EXPECT_EQ(j.get<prior_t>(), prior);
  // What an earlier version saved has no lens.
  const auto old = nlohmann::json::parse(
    R"({"mouse_gain":[1.0,1.0],"mouse_lag_us":30000,"stick_gain":null,"stick_lag_us":null,
        "stick_deadzone":null,"stick_exponent":null,"picture_width":1920,"picture_height":1080})"
  );
  const auto loaded = old.get<prior_t>();
  EXPECT_FALSE(loaded.focal.has_value());
  EXPECT_EQ(loaded.mouse_lag_us, std::optional<std::uint64_t> {30'000});
  calibrator_t flat {{1920, 1080}, {256, 144}};
  flat.set_prior(loaded);
  EXPECT_EQ(flat.model(1).focal, 0.0f);
  EXPECT_EQ(flat.model(1).max_shift[0], 288.0f);
  EXPECT_EQ(flat.model(1).max_shift[1], 162.0f);
  // Wrong types and a missing picture size are for the caller to catch.
  EXPECT_THROW(nlohmann::json::parse(R"({"mouse_gain":"x","picture_width":1,"picture_height":1})").get<prior_t>(), nlohmann::json::exception);
  EXPECT_THROW(nlohmann::json::parse(R"({"mouse_gain":[1.0],"picture_width":1,"picture_height":1})").get<prior_t>(), nlohmann::json::exception);
  EXPECT_THROW(nlohmann::json::parse(R"({"mouse_lag_us":5})").get<prior_t>(), nlohmann::json::exception);
  EXPECT_THROW(nlohmann::json::parse("[1]").get<prior_t>(), nlohmann::json::exception);
}

TEST(FrameWarpCalibrate, ALensGoesIntoTheModelAndThePriorAndLimitsTheShift) {
  auto run = mouse_session(0.12, 50'000, 0.3f);
  // 110 degrees across the picture's 1920 pixels.
  const double half = std::tan(55.0 * std::numbers::pi / 180.0);
  const double focal = 960.0 / half;
  run.cal.set_lens(half * half);
  const auto model = run.cal.update(run.history, run.end);
  ASSERT_TRUE(model.has_value()) << "a locked model";
  EXPECT_NEAR(static_cast<double>(model->focal), focal, 0.01);
  // So wide a lens stretches the picture's left and right before the flat limit does.
  const auto limit = static_cast<float>(0.6 * focal * focal / 1920.0);
  EXPECT_LT(limit, 288.0f);
  EXPECT_NEAR(model->max_shift[0], limit, 0.01f);
  EXPECT_EQ(model->max_shift[1], 162.0f);
  // A client that slides the picture is sent neither the lens nor its limit; the lens stays in
  // force for the gains.
  run.cal.set_lens_drawn(false);
  const auto slid = run.cal.model(1);
  EXPECT_EQ(slid.focal, 0.0f);
  EXPECT_EQ(slid.max_shift[0], 288.0f);
  EXPECT_EQ(slid.max_shift[1], 162.0f);
  EXPECT_EQ(slid.mouse_gain, model->mouse_gain);
  EXPECT_TRUE(run.cal.lens().has_value());
  run.cal.set_lens_drawn(true);
  // The next session, at another size, starts with the same field of view.
  const auto prior = run.cal.prior();
  ASSERT_TRUE(prior.has_value());
  ASSERT_TRUE(prior->focal.has_value());
  EXPECT_NEAR(static_cast<double>(*prior->focal), focal, 0.01);
  calibrator_t next {{1280, 720}, {256, 144}};
  next.set_prior(*prior);
  const double scaled = focal * 1280.0 / 1920.0;
  EXPECT_NEAR(static_cast<double>(next.model(1).focal), scaled, 0.01);

  // A field of view that wobbles is not sent again; another one is, and so is its loss.
  run.cal.set_lens(half * half * 1.02);
  EXPECT_FALSE(run.cal.update(run.history, run.end + fit_every_us).has_value());
  run.cal.set_lens(1.0);
  const auto turned_lens = run.cal.update(run.history, run.end + 2 * fit_every_us);
  ASSERT_TRUE(turned_lens.has_value()) << "a new lens";
  EXPECT_EQ(turned_lens->focal, 960.0f);
  EXPECT_EQ(turned_lens->generation, 2);
  EXPECT_EQ(turned_lens->max_shift[0], 288.0f);
  EXPECT_EQ(turned_lens->max_shift[1], 162.0f);
  run.cal.set_lens(std::nullopt);
  const auto lost = run.cal.update(run.history, run.end + 3 * fit_every_us);
  ASSERT_TRUE(lost.has_value()) << "no lens";
  EXPECT_EQ(lost->focal, 0.0f);
  EXPECT_EQ(lost->generation, 3);
  // Nonsense is no lens.
  run.cal.set_lens(std::nan(""));
  EXPECT_FALSE(run.cal.lens().has_value());
  run.cal.set_lens(1e-20);
  EXPECT_EQ(run.cal.model(4).focal, 0.0f) << "a focal length past the protocol's";
}

TEST(FrameWarpCalibrate, ALetterboxedCaptureKeepsItsGainsInItsOwnPixels) {
  // A 16:9 capture with bars in a 16:10 stream: the thumbnails cover the capture alone.
  calibrator_t cal {{2880, 1800}, {256, 160}};
  cal.set_sizes({2880, 1800}, {2880, 1620}, {256, 144});
  prior_t prior;
  prior.mouse_gain = std::array<float, 2> {2.0f, 2.0f};
  prior.mouse_lag_us = 20'000;
  prior.picture_width = 1920;
  prior.picture_height = 1080;
  cal.set_prior(prior);
  const auto model = cal.model(1);
  EXPECT_EQ(model.picture_width, 2880);
  EXPECT_EQ(model.picture_height, 1800);
  for (const float gain : model.mouse_gain) {
    EXPECT_LT(std::abs(gain - 3.0f), 1e-4f) << gain;
  }
  EXPECT_LT(std::abs(model.max_shift[1] - 1800.0f * max_offset_share), 1e-3f);
}

TEST(FrameWarpCalibrate, LearnsAStickGainWithADeadZone) {
  history_t history {std::uint8_t {0}};
  calibrator_t cal {{1920, 1080}, {256, 144}};
  const std::uint64_t lag = 34'000;
  const double gain = 60.0;  // thumbnail px per second of full deflection
  const std::uint64_t end = 3'500'000;
  // Flicks: the stick holds a level for 120 to 280 ms, then jumps to another.
  constexpr std::array<double, 7> levels {-1.0, -0.6, -0.3, 0.0, 0.35, 0.7, 1.0};
  std::uint32_t seed = 0x2545f491u;
  const auto next = [&seed]() {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  double level = 0.0;
  std::uint64_t until = 0;
  for (std::uint64_t t = 0; t < end; t += 8'000) {
    if (t >= until) {
      level = levels[next() % levels.size()];
      until = t + 120'000 + next() % 160'001;
    }
    history.on_controller(0, static_cast<std::int16_t>(level * 32767.0), 0, t);
  }
  for (std::uint64_t t = 8'333; t < end; t += 8'333) {
    const auto a = (t - 8'333) > lag ? (t - 8'333) - lag : 0;
    const auto b = t > lag ? t - lag : 0;
    const auto turned_by = history.stick_integral(a, b, 0.20f, 1.5f);
    sample_t sample;
    sample.t0_us = t - 8'333;
    sample.t1_us = t;
    sample.motion.dx = static_cast<float>(-turned_by[0] * gain);
    sample.motion.dy = 0.0f;
    sample.motion.confidence = 0.9f;
    cal.add(sample);
  }
  const auto model = cal.update(history, end);
  ASSERT_TRUE(model.has_value()) << "a locked model";
  EXPECT_TRUE(model->stick_locked && !model->mouse_locked);
  EXPECT_EQ(model->stick_deadzone, 0.20f);
  EXPECT_EQ(model->stick_exponent, 1.5f);
  EXPECT_LT(std::abs(model->stick_gain[0] + 60.0f * 7.5f), 15.0f) << model->stick_gain[0];
  const auto lag_error = model->lag_us > 34'000u ? model->lag_us - 34'000u : 34'000u - model->lag_us;
  EXPECT_LE(lag_error, 4'000u) << model->lag_us;
}

// ---- The host ----

TEST(FrameWarpHost, NothingHappensUntilAClientAsks) {
  host_t host {1920, 1080};
  host.on_mouse(5, 5, 10);
  host.tag(1, 100);
  EXPECT_FALSE(host.take_tags().has_value());
  EXPECT_EQ(host.mask_width(), 0);
  const std::array<std::uint8_t, 2> short_request {0, 1};
  EXPECT_FALSE(host.on_request(short_request).has_value());
  EXPECT_FALSE(host.enabled());
}

TEST(FrameWarpHost, HudCellsCoverTheThumbnailPixelsUnderThem) {
  mask_t mask(0, 8, 4);
  mask.set(7, 0, true);
  mask.set(0, 3, true);
  const auto hud = hud_pixels(mask, {16, 8});
  std::vector<std::pair<std::size_t, std::size_t>> set;
  for (std::size_t i = 0; i < 16 * 8; ++i) {
    if (hud[i] != 0) {
      set.emplace_back(i % 16, i / 16);
    }
  }
  const std::vector<std::pair<std::size_t, std::size_t>> expected {{14, 0}, {15, 0}, {14, 1}, {15, 1}, {0, 6}, {1, 6}, {0, 7}, {1, 7}};
  EXPECT_EQ(set, expected);
}

TEST(FrameWarpHost, AStartRequestRestartsTheCountsAndARepeatKeepsThem) {
  const auto newest = [](host_t &host) {
    std::optional<tags_t> last;
    while (auto wire = host.take_tags()) {
      last = tags_t::decode(*wire);
    }
    EXPECT_TRUE(last.has_value());
    return last ? last->tags.back() : tag_t {};
  };
  auto host = enabled_host();
  host.on_mouse(30, 0, 1'000);
  host.on_controller(0, 0, 0, 1'000);
  host.tag(1, 100'000);
  auto tag = newest(host);
  EXPECT_EQ(tag.mouse_x, 30);
  EXPECT_EQ(tag.controller_seq, 1u);
  // A repeated start keeps the counts.
  host.on_request(request(true, 0, 480));
  host.tag(3, 100'000);
  tag = newest(host);
  EXPECT_EQ(tag.mouse_x, 30);
  EXPECT_EQ(tag.controller_seq, 1u);
  // A stop and a new start restart them.
  host.on_request(request(false, 0, 480));
  host.on_request(request(true, 0, 480));
  host.tag(4, 200'000);
  tag = newest(host);
  EXPECT_EQ(tag.mouse_x, 0);
  EXPECT_EQ(tag.controller_seq, 0u);
}

TEST(FrameWarpHost, TagsCarryTheNewestFramesAndTheInputBeforeTheLag) {
  auto host = enabled_host();
  // The model sent on request first.
  const auto first_wire = host.take_model();
  ASSERT_TRUE(first_wire.has_value());
  const auto first = model_t::decode(*first_wire);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(first->locked());
  host.on_mouse(10, 0, 1'000);
  host.on_mouse(10, 0, 100'000);
  for (std::uint32_t frame = 1; frame <= 6; ++frame) {
    host.tag(frame, 100'000 + static_cast<std::uint64_t>(frame) * 1'000);
  }
  std::optional<tags_t> last;
  while (auto wire = host.take_tags()) {
    last = tags_t::decode(*wire);
  }
  ASSERT_TRUE(last.has_value());
  ASSERT_EQ(last->tags.size(), 4u);
  EXPECT_EQ(last->tags[0].frame, 3u);
  EXPECT_EQ(last->tags[1].frame, 4u);
  EXPECT_EQ(last->tags[2].frame, 5u);
  EXPECT_EQ(last->tags[3].frame, 6u);
  // With the default 40 ms lag, a capture at 106 ms reflects input up to 66 ms.
  EXPECT_EQ(last->tags[3].mouse_x, 10);
}

TEST(FrameWarpHost, LearnsFromThumbnailsAndFlagsTheCameraMoving) {
  auto host = enabled_host();
  host.take_model();
  const auto h = thumbnail_height(1920, 1080);
  // The camera pans 2 thumbnail pixels per 8 ms frame, for 40 ms of mouse input each.
  std::uint64_t t = 0;
  float offset = 0.0f;
  int sum = 0;
  while (t < 3'000'000) {
    host.on_mouse(16, 0, t);
    sum += 16;
    const float shown = std::max(static_cast<float>(sum) - 16.0f * 5.0f, 0.0f);
    offset = shown * 0.125f;
    host.on_thumbnail(scene(256, h, offset, 0.0f, t), t);
    t += 8'000;
  }
  EXPECT_GT(offset, 100.0f);
  EXPECT_TRUE(host.camera_moving(t - 8'000));
  EXPECT_FALSE(host.camera_moving(t + motion_hold_us));
  // Over a thumbnail pixel between captures: the HUD pass compares each with the one before.
  const auto every = host.hud_every_us(t - 8'000);
  ASSERT_TRUE(every.has_value());
  EXPECT_LE(*every, 8'000u);
  const auto model = host.status().model;
  ASSERT_TRUE(model.has_value()) << "a model";
  EXPECT_TRUE(model->mouse_locked);
  // The content moves left 0.125 thumbnail px per count: -0.9375 picture px.
  EXPECT_LT(std::abs(model->mouse_gain[0] + 0.9375f), 0.08f) << model->mouse_gain[0];
  // The model goes out, and the game's lesson is there to keep.
  const auto sent = host.take_model();
  ASSERT_TRUE(sent.has_value());
  EXPECT_EQ(model_t::decode(*sent), model);
  const auto prior = host.prior();
  ASSERT_TRUE(prior.has_value());
  EXPECT_TRUE(prior->mouse_gain.has_value());
}

TEST(FrameWarpHost, ASlowTurnCountsOverTheWindowAndNoiseDoesNot) {
  // 0.25 thumbnail px per 8 ms capture: the scene moves a pixel every 32 ms.
  auto host = enabled_host();
  for (std::uint64_t n = 0; n < 20; ++n) {
    host.add_shift(n * 8'000, (n + 1) * 8'000, 0.25f, 0.0f, (n + 1) * 8'000);
  }
  const auto every = host.hud_every_us(160'000);
  ASSERT_TRUE(every.has_value()) << "turning";
  EXPECT_GE(*every, 31'000u);
  EXPECT_LE(*every, 33'000u);
  EXPECT_FALSE(host.hud_every_us(160'000 + motion_hold_us).has_value());
  // Shifts back and forth add up to nothing: the camera stands still.
  auto fro = enabled_host();
  for (std::uint64_t n = 0; n < 40; ++n) {
    const float dx = n % 2 == 0 ? 0.4f : -0.4f;
    fro.add_shift(n * 8'000, (n + 1) * 8'000, dx, 0.0f, (n + 1) * 8'000);
  }
  EXPECT_FALSE(fro.hud_every_us(320'000).has_value());
  // Nor does a turn that moves less than a pixel in the window.
  auto slow = enabled_host();
  for (std::uint64_t n = 0; n < 40; ++n) {
    slow.add_shift(n * 8'000, (n + 1) * 8'000, 0.07f, 0.0f, (n + 1) * 8'000);
  }
  EXPECT_FALSE(slow.hud_every_us(320'000).has_value());
}

TEST(FrameWarpHost, HudMasksAreShapedHalvedToFitAndRateLimited) {
  auto host = enabled_host();
  EXPECT_EQ(host.mask_width(), 480);
  mask_t mask(0, 480, 270);
  mask.set(10, 10, true);
  host.on_hud(mask_t {mask}, 0);
  const auto first = host.take_mask();
  ASSERT_TRUE(first.has_value());
  const auto sent = mask_t::decode(*first);
  ASSERT_TRUE(sent.has_value());
  EXPECT_EQ(sent->count(), 9u);
  EXPECT_EQ(sent->generation, 1);
  // The same mask again, or a changed one too soon: nothing.
  host.on_hud(mask_t {mask}, 2'000'000);
  EXPECT_FALSE(host.take_mask().has_value());
  for (std::uint16_t x = 200; x < 205; ++x) {
    for (std::uint16_t y = 100; y < 105; ++y) {
      mask.set(x, y, true);
    }
  }
  host.on_hud(mask_t {mask}, 2'100'000);
  EXPECT_TRUE(host.take_mask().has_value());
  host.on_hud(mask_t(0, 480, 270), 2'200'000);
  EXPECT_FALSE(host.take_mask().has_value());
  // A client asking again gets the last one.
  host.on_request(request(true, std::nullopt, 480));
  EXPECT_TRUE(host.take_mask().has_value());
}

TEST(FrameWarpHost, HudPiecesCloseTogetherAreJoinedAndFarOnesAreNot) {
  // 960x540 cells: pieces up to 16 cells apart join (8 each way).
  mask_t mask(0, 960, 540);
  for (const std::uint16_t x : {100, 110, 125}) {
    mask.set(x, 50, true);
  }
  mask.set(110, 62, true);
  mask.set(170, 50, true);
  mask.set(959, 539, true);
  const auto shaped = shape(mask);
  // The three pieces of a line, and the one under it, are one region, grown a cell.
  for (std::uint16_t x = 99; x <= 126; ++x) {
    EXPECT_TRUE(shaped.get(x, 50)) << x;
  }
  for (std::uint16_t y = 49; y <= 63; ++y) {
    EXPECT_TRUE(shaped.get(110, y)) << y;
  }
  EXPECT_TRUE(!shaped.get(98, 50) && !shaped.get(127, 50));
  // The piece 45 cells on stays apart, as does the corner, grown a cell and not shrunk.
  for (std::uint16_t x = 127; x < 169; ++x) {
    EXPECT_FALSE(shaped.get(x, 50)) << x;
  }
  for (std::uint16_t x = 169; x <= 171; ++x) {
    EXPECT_TRUE(shaped.get(x, 50)) << x;
  }
  EXPECT_FALSE(shaped.get(172, 50));
  EXPECT_TRUE(shaped.get(958, 538) && shaped.get(959, 539));
  EXPECT_FALSE(shaped.get(957, 539));
}

TEST(FrameWarpHost, ANarrowClientGetsAHalvedMask) {
  host_t host {1920, 1080};
  host.on_request(request(true, std::nullopt, 300));
  EXPECT_EQ(host.mask_width(), 240);
  mask_t mask(0, 960, 540);
  mask.set(100, 100, true);
  host.on_hud(std::move(mask), 0);
  const auto wire = host.take_mask();
  ASSERT_TRUE(wire.has_value());
  const auto sent = mask_t::decode(*wire);
  ASSERT_TRUE(sent.has_value());
  EXPECT_EQ(sent->width, 240);
  EXPECT_EQ(sent->height, 135);
}

TEST(FrameWarpHost, ALetterboxedCaptureIsMeasuredAloneAndItsHudLandsWhereItShows) {
  // A 16:9 capture in a 16:10 stream, with bars 90 pixels high above and below it.
  host_t host {2880, 1800};
  host.set_picture(2880, 1800, rect_t {0, 90, 2880, 1620});
  host.on_request(request(true, std::nullopt, 720));
  EXPECT_EQ(host.thumbnail_size(), (std::pair<std::size_t, std::size_t> {256, 144}));
  const auto placement = host.placement();
  EXPECT_EQ(placement.mask, (std::pair<std::uint16_t, std::uint16_t> {720, 450}));
  EXPECT_EQ(host.mask_width(), 720);
  // The GPU's grid over the 2560x1440 capture: 720 by 405 cells of 4 picture pixels.
  mask_t grid(0, 720, 405);
  grid.set(100, 200, true);
  const auto placed = placement.place(grid);
  // Grid row 200 shows at picture rows 890 to 894, centred on 892: mask row 223 (892 to 896).
  EXPECT_TRUE(placed.get(100, 223) && !placed.get(100, 222) && !placed.get(100, 200));
  // The bars, and cells reaching into them, are HUD; the rest of the picture is not.
  for (const std::uint16_t x : {0, 359, 719}) {
    for (std::uint16_t y = 0; y <= 22; ++y) {
      EXPECT_TRUE(placed.get(x, y)) << x << "," << y;
    }
    for (std::uint16_t y = 427; y < 450; ++y) {
      EXPECT_TRUE(placed.get(x, y)) << x << "," << y;
    }
    EXPECT_TRUE(!placed.get(x, 23) && !placed.get(x, 426));
  }
  EXPECT_EQ(placed.count(), 46u * 720u + 1u);
  host.on_hud(mask_t {grid}, 0);
  const auto wire = host.take_mask();
  ASSERT_TRUE(wire.has_value());
  const auto sent = mask_t::decode(*wire);
  ASSERT_TRUE(sent.has_value());
  // The sent mask is the shaped one: its bars are HUD all the same.
  EXPECT_EQ(sent->width, 720);
  EXPECT_EQ(sent->height, 450);
  EXPECT_GE(sent->count(), 46u * 720u + 1u);
  // Pillarboxed: a 4:3 capture in a 16:9 stream, bars left and right.
  host.set_picture(1920, 1080, rect_t {240, 0, 1440, 1080});
  const auto pillar = host.placement();
  EXPECT_EQ(pillar.mask, (std::pair<std::uint16_t, std::uint16_t> {480, 270}));
  EXPECT_EQ(host.mask_width(), 360);
  const mask_t clear(0, 360, 270);
  const auto bars = pillar.place(clear);
  bool left_bar = true;
  for (std::uint16_t x = 0; x < 60; ++x) {
    left_bar = left_bar && bars.get(x, 0);
  }
  EXPECT_TRUE(left_bar && !bars.get(60, 0) && bars.get(420, 269));
  // Without bars, a grid at the mask's size is the mask itself.
  host.set_picture(1920, 1080, whole(1920, 1080));
  const auto plain = host.placement();
  mask_t full(0, 480, 270);
  full.set(3, 4, true);
  EXPECT_EQ(plain.place(full), full);
}

TEST(FrameWarpHost, ThumbnailHeightKeepsTheAspect) {
  EXPECT_EQ(thumbnail_height(1920, 1080), 144u);
  EXPECT_EQ(thumbnail_height(2560, 1600), 160u);
  EXPECT_EQ(thumbnail_height(3440, 1440), 107u);
  EXPECT_EQ(thumbnail_height(1080, 1920), 256u);
}

TEST(FrameWarpHost, OnlyAClientThatDrawsTheLensIsSentOne) {
  prior_t prior;
  prior.mouse_gain = std::array<float, 2> {1.0f, 1.0f};
  prior.mouse_lag_us = 30'000;
  prior.focal = 960.0f;
  prior.picture_width = 1920;
  prior.picture_height = 1080;
  const auto focal_of = [](host_t &host) {
    const auto wire = host.take_model();
    EXPECT_TRUE(wire.has_value()) << "a model for the request";
    const auto model = wire ? model_t::decode(*wire) : std::nullopt;
    EXPECT_TRUE(model.has_value()) << "a valid model";
    return model ? model->focal : -1.0f;
  };
  host_t host {1920, 1080};
  host.set_prior(prior);
  // A client that slides the picture is sent no lens, whatever the game's is. Its gains are the
  // picture centre's under that lens all the same: the median cell's, longer, would carry the middle
  // of its picture too far.
  EXPECT_EQ(host.on_request(request(true, std::nullopt, 480, false)), std::optional<bool> {true});
  EXPECT_FALSE(host.lens_requested());
  EXPECT_EQ(focal_of(host), 0.0f);
  EXPECT_TRUE(host.calibrator().lens().has_value());
  // And the game's lens is remembered.
  ASSERT_TRUE(host.prior().has_value());
  ASSERT_TRUE(host.prior()->focal.has_value());
  // One that draws it gets it with its request, and loses it with the next that doesn't.
  host.on_request(request(true, std::nullopt, 480, true));
  EXPECT_TRUE(host.lens_requested());
  EXPECT_NEAR(focal_of(host), 960.0f, 0.01f);
  host.on_request(request(true, std::nullopt, 480, false));
  EXPECT_EQ(focal_of(host), 0.0f);
}

TEST(FrameWarpHost, TheMeasurerLeavesOutWhatStandsStillWithTheGpusHud) {
  const std::array<std::uint8_t, 1> one {1};
  const std::array<std::uint8_t, 2> two {1, 0};
  const std::array<std::uint8_t, 3> hud3 {1, 0, 0};
  const std::array<std::uint8_t, 3> still3 {0, 0, 1};
  EXPECT_FALSE(union_of({}, one).has_value());
  EXPECT_FALSE(union_of(one, {}).has_value());
  const auto both = union_of(hud3, still3);
  ASSERT_TRUE(both.has_value());
  EXPECT_EQ(*both, (std::vector<std::uint8_t> {1, 0, 1}));
  EXPECT_FALSE(union_of(one, two).has_value());
  // A chequered box stays where it is while the scene pans behind it.
  const auto boxed = [](float ox, std::uint64_t at_us) {
    auto t = scene(256, 144, ox, 0.0f, at_us);
    for (std::size_t y = 20; y < 50; ++y) {
      for (std::size_t x = 190; x < 240; ++x) {
        t.luma[y * 256 + x] = (x / 5 + y / 5) % 2 == 0 ? 230 : 20;
      }
    }
    return t;
  };
  measurer_t measurer;
  std::optional<std::pair<std::uint64_t, motion_t>> last;
  for (std::uint64_t i = 0; i < 12; ++i) {
    auto thumbnail = boxed(static_cast<float>(i) * 3.0f, 1 + i * 16'000);
    last = measurer.measure(thumbnail).shift;
    measurer.keep(std::move(thumbnail), true);
  }
  const auto found = measurer.still().mask();
  ASSERT_FALSE(found.empty()) << "the box";
  EXPECT_TRUE(found[30 * 256 + 200] != 0 && found[100 * 256 + 100] == 0);
  ASSERT_TRUE(last.has_value()) << "a move";
  EXPECT_LT(std::abs(last->second.dx + 3.0f), 0.3f) << last->second.dx;
  // With the GPU's HUD both are left out, and what stood still stays when the HUD goes.
  std::vector<std::uint8_t> hud(256 * 144, 0);
  hud[100 * 256 + 100] = 1;
  measurer.set_hud(std::move(hud));
  ASSERT_TRUE(measurer.has_union());
  const auto merged = measurer.union_mask();
  EXPECT_TRUE(merged[30 * 256 + 200] != 0 && merged[100 * 256 + 100] != 0);
  measurer.reset_hud();
  EXPECT_TRUE(!measurer.has_union() && !measurer.has_hud());
  EXPECT_FALSE(measurer.still().mask().empty());
}

TEST(FrameWarpHost, AChangedPictureDropsWhatWasMeasuredAgainstBeforeIt) {
  auto host = enabled_host();
  const auto h = thumbnail_height(1920, 1080);
  host.on_thumbnail(scene(256, h, 0.0f, 0.0f, 1'000), 1'000);
  host.on_thumbnail(scene(256, h, 3.0f, 0.0f, 9'000), 9'000);
  EXPECT_EQ(host.status().samples, 1u);
  // The picture changed meanwhile: the next capture has no previous one to be measured against.
  host.set_picture(2560, 1440, whole(2560, 1440));
  host.on_thumbnail(scene(256, h, 6.0f, 0.0f, 17'000), 17'000);
  EXPECT_EQ(host.status().samples, 1u);
  host.on_thumbnail(scene(256, h, 9.0f, 0.0f, 25'000), 25'000);
  EXPECT_EQ(host.status().samples, 2u);
  // A thumbnail of another size than the host expected is taken as the new size.
  host.on_thumbnail(scene(256, 160, 0.0f, 0.0f, 33'000), 33'000);
  EXPECT_EQ(host.thumbnail_size(), (std::pair<std::size_t, std::size_t> {256, 160}));
  EXPECT_EQ(host.status().samples, 2u);
  // Disabled, thumbnails are not measured.
  host.on_request(request(false, std::nullopt, 0));
  host.on_thumbnail(scene(256, 160, 1.0f, 0.0f, 41'000), 41'000);
  host.on_thumbnail(scene(256, 160, 2.0f, 0.0f, 49'000), 49'000);
  EXPECT_EQ(host.status().samples, 2u);
  EXPECT_FALSE(host.status().enabled);
}
