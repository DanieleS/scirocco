/**
 * @file tests/unit/test_capture_action.cpp
 * @brief Test src/capture_action.*.
 */
#include "../tests_common.h"
#include "src/capture_action.h"
#include "src/focus_hints.h"

#include <cstdint>
#include <string>
#include <vector>

using capture_action::action_t;
using capture_action::provider_t;

namespace {
  std::string payload(std::initializer_list<std::uint8_t> bytes) {
    return {bytes.begin(), bytes.end()};
  }

  std::vector<std::uint16_t> keys(provider_t provider, action_t action) {
    const auto chord = capture_action::chord(provider, action);
    return {chord.begin(), chord.end()};
  }
}  // namespace

// These are shared with moonlight-common-c and a mismatch is silent on the wire, so they are
// pinned here rather than trusted.
TEST(CaptureActionWire, ActionsAreTheWireValues) {
  EXPECT_EQ(static_cast<std::uint8_t>(action_t::screenshot), 1);
  EXPECT_EQ(static_cast<std::uint8_t>(action_t::save_replay), 2);
  EXPECT_EQ(static_cast<std::uint8_t>(action_t::toggle_recording), 3);
}

TEST(CaptureActionWire, PacketTypeAvoidsEveryKnownControlMessage) {
  EXPECT_EQ(capture_action::packet_type, 0x3021);
  EXPECT_NE(capture_action::packet_type, focus_hints::packet_type);

  // Moonlight/Sunshine, Apollo, Vibeshine haptics and the IDs other forks are known to use
  for (const std::uint16_t taken : {0x0100, 0x0109, 0x010b, 0x010e, 0x0200, 0x0201, 0x0204, 0x0206, 0x0301, 0x0302, 0x0305, 0x0307, 0x3000, 0x3001, 0x3002, 0x3003, 0x5500, 0x5501, 0x5502, 0x5503, 0x5504, 0x550c, 0x5601}) {
    EXPECT_NE(capture_action::packet_type, taken) << std::hex << taken;
  }
}

TEST(CaptureActionWire, FeatureBitsAvoidEveryKnownFlag) {
  EXPECT_EQ(capture_action::client_feature_flag, 0x20000u);
  EXPECT_EQ(capture_action::host_feature_flag, 0x20000u);
  EXPECT_EQ(capture_action::host_available_flag, 0x40000u);

  EXPECT_EQ(capture_action::client_feature_flag & focus_hints::client_feature_flag, 0u);
  EXPECT_EQ((capture_action::host_feature_flag | capture_action::host_available_flag) & focus_hints::host_feature_flag, 0u);
  EXPECT_EQ(capture_action::client_feature_flag & 0xFFu, 0u);
  EXPECT_EQ((capture_action::host_feature_flag | capture_action::host_available_flag) & 0x3FFu, 0u);
}

TEST(CaptureActionWire, DecodesEveryAction) {
  EXPECT_EQ(capture_action::decode_payload(payload({1, 1, 0, 0})), action_t::screenshot);
  EXPECT_EQ(capture_action::decode_payload(payload({1, 2, 0, 0})), action_t::save_replay);
  EXPECT_EQ(capture_action::decode_payload(payload({1, 3, 0, 0})), action_t::toggle_recording);
}

TEST(CaptureActionWire, RejectsWhatItCannotTrust) {
  EXPECT_EQ(capture_action::decode_payload(""), std::nullopt);
  EXPECT_EQ(capture_action::decode_payload(payload({1, 1, 0})), std::nullopt);  // Short
  EXPECT_EQ(capture_action::decode_payload(payload({0, 1, 0, 0})), std::nullopt);  // Version 0
  EXPECT_EQ(capture_action::decode_payload(payload({1, 0, 0, 0})), std::nullopt);
  EXPECT_EQ(capture_action::decode_payload(payload({1, 4, 0, 0})), std::nullopt);  // From a newer client
  EXPECT_EQ(capture_action::decode_payload(payload({1, 0xFF, 0, 0})), std::nullopt);
}

TEST(CaptureActionWire, ANewerVersionKeepsTheLayout) {
  EXPECT_EQ(capture_action::decode_payload(payload({2, 2, 0, 0, 0xAA, 0xBB})), action_t::save_replay);
}

TEST(CaptureActionNegotiation, HostAdvertisesSupportAndAvailabilitySeparately) {
  using capture_action::host_available_flag;
  using capture_action::host_feature_flag;

  EXPECT_EQ(capture_action::host_feature_flags(0x03, false, provider_t::gamebar), 0x03u);
  EXPECT_EQ(capture_action::host_feature_flags(0x03, true, provider_t::off), 0x03u | host_feature_flag);
  EXPECT_EQ(capture_action::host_feature_flags(0x03, true, provider_t::gamebar), 0x03u | host_feature_flag | host_available_flag);
  EXPECT_EQ(capture_action::host_feature_flags(0x03, true, provider_t::nvidia), 0x03u | host_feature_flag | host_available_flag);
}

TEST(CaptureActionNegotiation, OnlyAClientThatAskedGetsAProvider) {
  using capture_action::client_feature_flag;

  EXPECT_EQ(capture_action::negotiated(true, provider_t::nvidia, 0x07), provider_t::off);  // A stock Moonlight client
  EXPECT_EQ(capture_action::negotiated(false, provider_t::nvidia, client_feature_flag), provider_t::off);
  EXPECT_EQ(capture_action::negotiated(true, provider_t::off, client_feature_flag), provider_t::off);
  EXPECT_EQ(capture_action::negotiated(true, provider_t::nvidia, 0x03 | client_feature_flag), provider_t::nvidia);
  EXPECT_EQ(capture_action::negotiated(true, provider_t::gamebar, client_feature_flag), provider_t::gamebar);
}

TEST(CaptureActionProvider, ParsesTheConfigValues) {
  EXPECT_EQ(capture_action::parse_provider("off"), provider_t::off);
  EXPECT_EQ(capture_action::parse_provider("gamebar"), provider_t::gamebar);
  EXPECT_EQ(capture_action::parse_provider("nvidia"), provider_t::nvidia);
  EXPECT_EQ(capture_action::parse_provider("NVIDIA"), std::nullopt);
  EXPECT_EQ(capture_action::parse_provider(""), std::nullopt);
  EXPECT_EQ(capture_action::parse_provider("shadowplay"), std::nullopt);
}

TEST(CaptureActionProvider, NamesRoundTrip) {
  for (const auto provider : {provider_t::off, provider_t::gamebar, provider_t::nvidia}) {
    EXPECT_EQ(capture_action::parse_provider(capture_action::to_string(provider)), provider);
  }
}

// The tools' own default shortcuts, as Windows virtual-key codes
TEST(CaptureActionChord, GameBar) {
  EXPECT_EQ(keys(provider_t::gamebar, action_t::screenshot), (std::vector<std::uint16_t> {0x5B, 0xA4, 0x2C}));  // Win+Alt+PrtScn
  EXPECT_EQ(keys(provider_t::gamebar, action_t::save_replay), (std::vector<std::uint16_t> {0x5B, 0xA4, 0x47}));  // Win+Alt+G
  EXPECT_EQ(keys(provider_t::gamebar, action_t::toggle_recording), (std::vector<std::uint16_t> {0x5B, 0xA4, 0x52}));  // Win+Alt+R
}

TEST(CaptureActionChord, Nvidia) {
  EXPECT_EQ(keys(provider_t::nvidia, action_t::screenshot), (std::vector<std::uint16_t> {0xA4, 0x70}));  // Alt+F1
  EXPECT_EQ(keys(provider_t::nvidia, action_t::save_replay), (std::vector<std::uint16_t> {0xA4, 0x79}));  // Alt+F10
  EXPECT_EQ(keys(provider_t::nvidia, action_t::toggle_recording), (std::vector<std::uint16_t> {0xA4, 0x78}));  // Alt+F9
}

TEST(CaptureActionChord, OffPressesNothing) {
  for (const auto action : {action_t::screenshot, action_t::save_replay, action_t::toggle_recording}) {
    EXPECT_TRUE(capture_action::chord(provider_t::off, action).empty());
  }
}
