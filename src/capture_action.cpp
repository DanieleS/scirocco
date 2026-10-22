/**
 * @file src/capture_action.cpp
 * @brief Platform-neutral protocol and policy for client-requested captures.
 */
// this include
#include "capture_action.h"

// standard includes
#include <array>

namespace capture_action {

  namespace {
    // Windows virtual-key codes, so the table builds everywhere
    constexpr std::uint16_t vk_lwin = 0x5B;
    constexpr std::uint16_t vk_lmenu = 0xA4;
    constexpr std::uint16_t vk_snapshot = 0x2C;
    constexpr std::uint16_t vk_g = 0x47;
    constexpr std::uint16_t vk_r = 0x52;
    constexpr std::uint16_t vk_f1 = 0x70;
    constexpr std::uint16_t vk_f9 = 0x78;
    constexpr std::uint16_t vk_f10 = 0x79;

    // Game Bar: Win+Alt+PrtScn, Win+Alt+G ("Record that"), Win+Alt+R
    constexpr std::array gamebar_screenshot {vk_lwin, vk_lmenu, vk_snapshot};
    constexpr std::array gamebar_replay {vk_lwin, vk_lmenu, vk_g};
    constexpr std::array gamebar_record {vk_lwin, vk_lmenu, vk_r};

    // NVIDIA app and GeForce Experience defaults: Alt+F1, Alt+F10, Alt+F9
    constexpr std::array nvidia_screenshot {vk_lmenu, vk_f1};
    constexpr std::array nvidia_replay {vk_lmenu, vk_f10};
    constexpr std::array nvidia_record {vk_lmenu, vk_f9};
  }  // namespace

  std::optional<action_t> decode_payload(const std::string_view payload) {
    if (payload.size() < payload_size) {
      return std::nullopt;
    }
    if (static_cast<std::uint8_t>(payload[0]) < wire_version) {
      return std::nullopt;
    }
    switch (const auto action = static_cast<action_t>(payload[1])) {
      case action_t::screenshot:
      case action_t::save_replay:
      case action_t::toggle_recording:
        return action;
    }
    return std::nullopt;
  }

  std::optional<provider_t> parse_provider(const std::string_view value) {
    if (value == "off") {
      return provider_t::off;
    }
    if (value == "gamebar") {
      return provider_t::gamebar;
    }
    if (value == "nvidia") {
      return provider_t::nvidia;
    }
    return std::nullopt;
  }

  const char *to_string(const action_t action) {
    switch (action) {
      case action_t::screenshot:
        return "screenshot";
      case action_t::save_replay:
        return "save replay";
      case action_t::toggle_recording:
        return "toggle recording";
    }
    return "unknown";
  }

  const char *to_string(const provider_t provider) {
    switch (provider) {
      case provider_t::off:
        return "off";
      case provider_t::gamebar:
        return "gamebar";
      case provider_t::nvidia:
        return "nvidia";
    }
    return "unknown";
  }

  std::span<const std::uint16_t> chord(const provider_t provider, const action_t action) {
    switch (provider) {
      case provider_t::gamebar:
        switch (action) {
          case action_t::screenshot:
            return gamebar_screenshot;
          case action_t::save_replay:
            return gamebar_replay;
          case action_t::toggle_recording:
            return gamebar_record;
        }
        break;
      case provider_t::nvidia:
        switch (action) {
          case action_t::screenshot:
            return nvidia_screenshot;
          case action_t::save_replay:
            return nvidia_replay;
          case action_t::toggle_recording:
            return nvidia_record;
        }
        break;
      case provider_t::off:
        break;
    }
    return {};
  }

}  // namespace capture_action
