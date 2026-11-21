/**
 * @file tests/unit/test_frame_warp_session.cpp
 * @brief Test src/frame_warp_session.* and src/frame_warp_policy.h: nothing happens until a client
 *        asks, requests are handled, and what a game taught the host is kept per game.
 */
#include "../tests_common.h"
#include "src/frame_warp_policy.h"
#include "src/frame_warp_session.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace frame_warp;

namespace {
  using bytes = std::vector<std::uint8_t>;

  std::filesystem::path temp_dir(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("frame-warp-test-" + name);
    std::filesystem::remove_all(path);
    return path;
  }

  bytes request(bool enabled, std::uint8_t controller = 0, bool lens = true, std::uint16_t mask_width = 256) {
    request_t value;
    value.enabled = enabled;
    value.controller = controller;
    value.lens = lens;
    value.mask_max_width = mask_width;
    const auto wire = value.encode();
    return bytes(wire.begin(), wire.end());
  }

  prior_t sample_prior(float gain) {
    prior_t prior;
    prior.mouse_gain = std::array<float, 2> {gain, gain};
    prior.mouse_lag_us = 32000;
    prior.picture_width = 1920;
    prior.picture_height = 1080;
    return prior;
  }
}  // namespace

TEST(FrameWarpPolicy, TheSettingGatesBothAnnouncements) {
  EXPECT_TRUE(policy::offered_in_serverinfo(true, true));
  EXPECT_FALSE(policy::offered_in_serverinfo(false, true));
  EXPECT_FALSE(policy::offered_in_serverinfo(true, false));

  EXPECT_TRUE(policy::offered_in_describe(true, true, true));
  EXPECT_FALSE(policy::offered_in_describe(false, true, true));
  // Not for input-only, monitor or secondary streams, and not where the passes do not exist.
  EXPECT_FALSE(policy::offered_in_describe(true, false, true));
  EXPECT_FALSE(policy::offered_in_describe(true, true, false));
  EXPECT_EQ(policy::serverinfo_value(), "1");
  EXPECT_EQ(sdp_attribute, "a=x-rl-control:0x5543,0x5544,0x5545,0x5546");
}

TEST(FrameWarpSession, DoesNothingUntilAClientAsks) {
  session_t session {store_t {{}, {}}};
  EXPECT_FALSE(session.enabled());
  session.on_mouse(10, 10);
  session.on_controller(0, 100, 100);
  session.tag(1, session_t::clock::now());
  EXPECT_FALSE(session.pending());
  EXPECT_TRUE(session.take().empty());
  EXPECT_FALSE(session.params().enabled);
  // A malformed request changes nothing.
  EXPECT_FALSE(session.on_request(bytes {0x01, 0x01}));
  EXPECT_FALSE(session.enabled());
  // Neither does a stop.
  EXPECT_EQ(session.on_request(request(false)), std::optional<bool> {false});
  EXPECT_TRUE(session.take().empty());
}

TEST(FrameWarpSession, AnEnableIsAnsweredWithAnUnlockedModelAndTagsFollow) {
  session_t session {store_t {{}, {}}};
  session.set_picture(1920, 1080, rect_t {0, 0, 1920, 1080});
  EXPECT_EQ(session.on_request(request(true)), std::optional<bool> {true});
  EXPECT_TRUE(session.enabled());
  EXPECT_TRUE(session.pending());

  auto first = session.take();
  ASSERT_TRUE(first.model);
  const auto model = model_t::decode(*first.model);
  ASSERT_TRUE(model);
  EXPECT_FALSE(model->locked());
  EXPECT_EQ(model->picture_width, 1920);
  EXPECT_EQ(model->picture_height, 1080);
  EXPECT_FALSE(session.pending());

  const auto params = session.params();
  EXPECT_TRUE(params.enabled);
  EXPECT_GT(params.thumbnail_width, 0u);
  EXPECT_EQ(params.picture_width, 1920);

  session.on_mouse(30, 0);
  session.on_controller(0, 0, 0);
  session.tag(7, session_t::clock::now() + std::chrono::milliseconds {100});
  auto next = session.take();
  ASSERT_FALSE(next.tags.empty());
  const auto tags = tags_t::decode(next.tags.back());
  ASSERT_TRUE(tags);
  EXPECT_EQ(tags->tags.back().frame, 7u);
  EXPECT_EQ(tags->tags.back().mouse_x, 30);
  EXPECT_EQ(tags->tags.back().controller_seq, 1u);

  // A stop ends it: nothing more is queued.
  EXPECT_EQ(session.on_request(request(false)), std::optional<bool> {false});
  session.tag(8, session_t::clock::now());
  EXPECT_TRUE(session.take().empty());
  EXPECT_FALSE(session.enabled());
}

TEST(FrameWarpSession, AStreamThatCannotBeMeasuredIsNeverTagged) {
  session_t session {store_t {{}, {}}};
  session.set_measurable(false);
  EXPECT_EQ(session.on_request(request(true)), std::optional<bool> {true});
  auto first = session.take();
  ASSERT_TRUE(first.model);
  EXPECT_FALSE(model_t::decode(*first.model)->locked());
  session.tag(1, session_t::clock::now());
  EXPECT_TRUE(session.take().tags.empty());
}

TEST(FrameWarpSession, AControllerFiltersByTheRequestsNumber) {
  session_t session {store_t {{}, {}}};
  session.on_request(request(true, 2));
  session.take();
  session.on_controller(0, 0, 0);
  session.on_controller(2, 0, 0);
  session.on_controller(2, 0, 0);
  session.tag(1, session_t::clock::now());
  const auto out = session.take();
  ASSERT_FALSE(out.tags.empty());
  EXPECT_EQ(tags_t::decode(out.tags.back())->tags.back().controller_seq, 2u);
}

TEST(FrameWarpStore, KeysPreferTheUuid) {
  EXPECT_EQ(store_t::key_for("abc", "Game"), "uuid:abc");
  EXPECT_EQ(store_t::key_for("", "Game"), "name:Game");
  EXPECT_EQ(store_t::key_for("", ""), "");
}

TEST(FrameWarpStore, APriorLoadsForTheSameGameOnly) {
  const auto directory = temp_dir("store");
  const auto path = directory / std::string {store_t::file_name};
  store_t a {path, "uuid:a"};
  EXPECT_FALSE(a.load());
  a.save(sample_prior(1.5f));
  store_t {path, ""}.save(sample_prior(9.0f));
  EXPECT_EQ(a.load(), sample_prior(1.5f));
  EXPECT_FALSE((store_t {path, "uuid:b"}.load()));
  EXPECT_FALSE((store_t {path, ""}.load()));

  // A damaged file reads as nothing learned and is replaced by the next save.
  {
    std::ofstream out {path, std::ios::trunc};
    out << "{not json";
  }
  EXPECT_FALSE(a.load());
  store_t b {path, "uuid:b"};
  b.save(sample_prior(2.5f));
  EXPECT_EQ(b.load(), sample_prior(2.5f));
  EXPECT_FALSE(a.load());
  std::filesystem::remove_all(directory);
}

TEST(FrameWarpStore, TheLeastRecentlyUsedGamesGoPastTheLimit) {
  const auto directory = temp_dir("lru");
  const auto path = directory / std::string {store_t::file_name};
  for (std::size_t i = 0; i < store_t::max_games + 3; ++i) {
    store_t {path, "uuid:" + std::to_string(i)}.save(sample_prior(1.0f));
  }
  std::ifstream in {path};
  const auto file = nlohmann::json::parse(in);
  EXPECT_EQ(file["games"].size(), store_t::max_games);
  std::filesystem::remove_all(directory);
}

TEST(FrameWarpSession, WhatAGameTaughtStartsTheNextStream) {
  const auto directory = temp_dir("session");
  const auto path = directory / std::string {store_t::file_name};
  store_t {path, "uuid:g"}.save(sample_prior(1.25f));
  {
    session_t session {store_t {path, "uuid:g"}};
    session.on_request(request(true));
    // The worker applies the prior and, when the session ends, saves what it knows.
  }
  // Still there for the next stream of the game, and never invented for another.
  EXPECT_TRUE((store_t {path, "uuid:g"}.load()));
  EXPECT_FALSE((store_t {path, "uuid:other"}.load()));
  std::filesystem::remove_all(directory);
}
