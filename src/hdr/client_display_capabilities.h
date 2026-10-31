/**
 * @file src/hdr/client_display_capabilities.h
 * @brief Parsing and policy for the HDR luminance a client reports for its own screen.
 *
 * Wire format (query parameters of /launch and /resume, values in nits):
 * `maxBrightness`, `minBrightness` and `maxAverageBrightness`. The three are atomic:
 * all present and valid, or the report is ignored. Parsing follows foundation-sunshine's
 * hdr::parse_client_display_capabilities, without its calibration machinery.
 */
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace hdr {

  struct client_display_capabilities_t {
    float max_nits = 0.0f;  ///< peak luminance
    float min_nits = 0.0f;  ///< black level
    float max_average_nits = 0.0f;  ///< max full-frame / average luminance
  };

  enum class client_display_parse_status_e {
    absent,  ///< no parameter present: silently no report
    incomplete,  ///< one or two parameters present
    malformed,  ///< a value is not a finite number
    out_of_range,  ///< a value or a relation between values is invalid
    valid,
  };

  struct client_display_parse_result_t {
    client_display_parse_status_e status = client_display_parse_status_e::absent;
    client_display_capabilities_t capabilities {};
  };

  inline constexpr float client_peak_min_nits = 1.0f;
  inline constexpr float client_peak_max_nits = 10000.0f;
  inline constexpr float client_black_max_nits = 100.0f;
  // The virtual display driver accepts a peak only inside this range.
  inline constexpr int driver_peak_min_nits = 400;
  inline constexpr int driver_peak_max_nits = 2000;

  inline std::optional<float> parse_nits(std::string_view text) {
    if (text.empty()) {
      return std::nullopt;
    }
    float value = 0.0f;
    const auto *first = text.data();
    const auto *last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, std::chars_format::general);
    if (ec != std::errc {} || ptr != last || !std::isfinite(value)) {
      return std::nullopt;
    }
    return value;
  }

  /**
   * @brief Validate the three optional parameters. Empty views mean "absent".
   */
  inline client_display_parse_result_t parse_client_display_capabilities(
    std::string_view max_text,
    std::string_view min_text,
    std::string_view max_average_text
  ) {
    using status_e = client_display_parse_status_e;
    client_display_parse_result_t result;

    const int present = (max_text.empty() ? 0 : 1) + (min_text.empty() ? 0 : 1) + (max_average_text.empty() ? 0 : 1);
    if (present == 0) {
      return result;
    }
    if (present != 3) {
      result.status = status_e::incomplete;
      return result;
    }

    const auto max_nits = parse_nits(max_text);
    const auto min_nits = parse_nits(min_text);
    const auto max_average_nits = parse_nits(max_average_text);
    if (!max_nits || !min_nits || !max_average_nits) {
      result.status = status_e::malformed;
      return result;
    }

    const bool in_range =
      *max_nits >= client_peak_min_nits && *max_nits <= client_peak_max_nits &&
      *min_nits >= 0.0f && *min_nits <= client_black_max_nits &&
      *max_average_nits >= client_peak_min_nits && *max_average_nits <= client_peak_max_nits &&
      *max_average_nits <= *max_nits && *min_nits <= *max_average_nits;
    if (!in_range) {
      result.status = status_e::out_of_range;
      return result;
    }

    result.status = status_e::valid;
    result.capabilities = {*max_nits, *min_nits, *max_average_nits};
    return result;
  }

  /**
   * @brief Convert a reported peak to what the virtual display driver accepts.
   */
  inline std::uint32_t clamp_peak_for_driver(float max_nits) {
    if (!std::isfinite(max_nits)) {
      return static_cast<std::uint32_t>(driver_peak_min_nits);
    }
    const auto rounded = std::clamp<float>(std::round(max_nits), 0.0f, static_cast<float>(driver_peak_max_nits));
    return static_cast<std::uint32_t>(std::max<int>(static_cast<int>(rounded), driver_peak_min_nits));
  }

  /**
   * @brief May a virtual display programmed with `programmed_nits` serve a session wanting `requested_nits`?
   *
   * Measurement jitter (the larger of 25 nits and 5 %) is tolerated for the same client only.
   * A different client needs an exact match. Callers must store the programmed value at creation,
   * never the last request, so that slow drift accumulates instead of being hidden.
   */
  inline bool peak_matches_for_reuse(std::uint32_t programmed_nits, std::uint32_t requested_nits, bool client_changed) {
    if (client_changed) {
      return programmed_nits == requested_nits;
    }
    const auto difference = programmed_nits > requested_nits ? programmed_nits - requested_nits : requested_nits - programmed_nits;
    const auto smaller = std::min(programmed_nits, requested_nits);
    const double tolerance = std::max(25.0, 0.05 * static_cast<double>(smaller));
    return static_cast<double>(difference) <= tolerance;
  }

}  // namespace hdr
