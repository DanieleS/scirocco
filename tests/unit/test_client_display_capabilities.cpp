/**
 * @file tests/unit/test_client_display_capabilities.cpp
 * @brief Client HDR luminance report parsing, driver clamp and display-reuse tolerance.
 */
#include "../tests_common.h"

#include <src/hdr/client_display_capabilities.h>

#include <array>
#include <limits>
#include <string_view>

namespace {
  using hdr::client_display_parse_status_e;
  using hdr::parse_client_display_capabilities;
}  // namespace

TEST(ClientDisplayCapabilities, NoParametersIsSilentlyAbsent) {
  EXPECT_EQ(parse_client_display_capabilities("", "", "").status, client_display_parse_status_e::absent);
}

TEST(ClientDisplayCapabilities, OneOrTwoParametersAreIncomplete) {
  EXPECT_EQ(parse_client_display_capabilities("1000", "", "").status, client_display_parse_status_e::incomplete);
  EXPECT_EQ(parse_client_display_capabilities("", "0.001", "").status, client_display_parse_status_e::incomplete);
  EXPECT_EQ(parse_client_display_capabilities("1000", "0.001", "").status, client_display_parse_status_e::incomplete);
  EXPECT_EQ(parse_client_display_capabilities("", "0.001", "600").status, client_display_parse_status_e::incomplete);
}

TEST(ClientDisplayCapabilities, ThreeValidParametersParse) {
  const auto result = parse_client_display_capabilities("1000.0", "0.001", "600.0");
  ASSERT_EQ(result.status, client_display_parse_status_e::valid);
  EXPECT_FLOAT_EQ(result.capabilities.max_nits, 1000.0f);
  EXPECT_FLOAT_EQ(result.capabilities.min_nits, 0.001f);
  EXPECT_FLOAT_EQ(result.capabilities.max_average_nits, 600.0f);
}

TEST(ClientDisplayCapabilities, ExponentFormAndZeroBlackAreAccepted) {
  const auto result = parse_client_display_capabilities("4.0E2", "4.0E-4", "2e2");
  ASSERT_EQ(result.status, client_display_parse_status_e::valid);
  EXPECT_FLOAT_EQ(result.capabilities.max_nits, 400.0f);
  EXPECT_FLOAT_EQ(result.capabilities.min_nits, 0.0004f);
  EXPECT_EQ(parse_client_display_capabilities("1000", "0", "600").status, client_display_parse_status_e::valid);
}

TEST(ClientDisplayCapabilities, MalformedValuesAreRejected) {
  constexpr std::array<std::string_view, 7> bad_values = {"abc", "1000nits", "NaN", "inf", "-inf", " 1000", "1000 "};
  for (const auto bad : bad_values) {
    EXPECT_EQ(parse_client_display_capabilities(bad, "0.001", "600").status, client_display_parse_status_e::malformed) << bad;
    EXPECT_EQ(parse_client_display_capabilities("1000", bad, "600").status, client_display_parse_status_e::malformed) << bad;
    EXPECT_EQ(parse_client_display_capabilities("1000", "0.001", bad).status, client_display_parse_status_e::malformed) << bad;
  }
}

TEST(ClientDisplayCapabilities, OutOfRangeAndReversedRelationsAreRejected) {
  using status_e = client_display_parse_status_e;
  EXPECT_EQ(parse_client_display_capabilities("0.5", "0", "0.5").status, status_e::out_of_range);
  EXPECT_EQ(parse_client_display_capabilities("10001", "0", "600").status, status_e::out_of_range);
  EXPECT_EQ(parse_client_display_capabilities("1000", "-1", "600").status, status_e::out_of_range);
  EXPECT_EQ(parse_client_display_capabilities("1000", "101", "600").status, status_e::out_of_range);
  EXPECT_EQ(parse_client_display_capabilities("1000", "0.001", "0.5").status, status_e::out_of_range);
  EXPECT_EQ(parse_client_display_capabilities("1000", "0.001", "1001").status, status_e::out_of_range);
  EXPECT_EQ(parse_client_display_capabilities("1000", "700", "600").status, status_e::out_of_range);
  EXPECT_EQ(parse_client_display_capabilities("1", "0.001", "1").status, status_e::valid);
  EXPECT_EQ(parse_client_display_capabilities("10000", "100", "10000").status, status_e::valid);
}

TEST(ClientDisplayCapabilities, PeakIsClampedToDriverRange) {
  EXPECT_EQ(hdr::clamp_peak_for_driver(100.0f), 400u);
  EXPECT_EQ(hdr::clamp_peak_for_driver(400.0f), 400u);
  EXPECT_EQ(hdr::clamp_peak_for_driver(1000.4f), 1000u);
  EXPECT_EQ(hdr::clamp_peak_for_driver(1000.6f), 1001u);
  EXPECT_EQ(hdr::clamp_peak_for_driver(2000.0f), 2000u);
  EXPECT_EQ(hdr::clamp_peak_for_driver(10000.0f), 2000u);
  EXPECT_EQ(hdr::clamp_peak_for_driver(std::numeric_limits<float>::quiet_NaN()), 400u);
}

TEST(ClientDisplayCapabilities, ReuseToleratesJitterForTheSameClient) {
  EXPECT_TRUE(hdr::peak_matches_for_reuse(1000, 1000, false));
  EXPECT_TRUE(hdr::peak_matches_for_reuse(1000, 1025, false));
  EXPECT_TRUE(hdr::peak_matches_for_reuse(1000, 975, false));
  // The 25 nit floor applies at the low end, where 5 % is smaller.
  EXPECT_TRUE(hdr::peak_matches_for_reuse(400, 425, false));
  EXPECT_FALSE(hdr::peak_matches_for_reuse(400, 426, false));
  // 5 % of 2000 is 100 nits, wider than the floor.
  EXPECT_TRUE(hdr::peak_matches_for_reuse(2000, 1905, false));
  EXPECT_FALSE(hdr::peak_matches_for_reuse(2000, 1890, false));
  EXPECT_FALSE(hdr::peak_matches_for_reuse(400, 1000, false));
}

TEST(ClientDisplayCapabilities, ReuseNeedsExactMatchWhenTheClientChanged) {
  EXPECT_TRUE(hdr::peak_matches_for_reuse(1000, 1000, true));
  EXPECT_FALSE(hdr::peak_matches_for_reuse(1000, 1010, true));
  EXPECT_FALSE(hdr::peak_matches_for_reuse(1000, 999, true));
}

TEST(ClientDisplayCapabilities, DriftAccumulatesAgainstTheProgrammedPeak) {
  const std::uint32_t programmed = 1000;
  for (std::uint32_t requested = 1010; requested <= 1050; requested += 10) {
    EXPECT_TRUE(hdr::peak_matches_for_reuse(programmed, requested, false));
  }
  EXPECT_FALSE(hdr::peak_matches_for_reuse(programmed, 1060, false));
}
