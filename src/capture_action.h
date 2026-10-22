/**
 * @file src/capture_action.h
 * @brief Platform-neutral protocol and policy for client-requested captures.
 *
 * A client's in-game menu can ask for a screenshot, the last few seconds of gameplay or a
 * recording. The tool that does it runs on the host (Xbox Game Bar or the NVIDIA app), and
 * which one is a property of the PC, not of each client. So the client sends what it wants
 * and the host, configured with `capture_provider`, presses that tool's shortcut itself.
 *
 * This is a negotiated extension. The host advertises `host_feature_flag` in
 * `x-ss-general.featureFlags` when it understands capture actions, and adds
 * `host_available_flag` when a provider is configured. A client that sees the first flag
 * sends `packet_type` messages instead of pressing shortcuts, and shows its capture entries
 * only when it also sees the second. A client that asks with `client_feature_flag` in
 * `x-ml-general.featureFlags` gets its actions handled; any other one is ignored, so it
 * keeps whatever it did before.
 *
 * Nothing here touches the operating system, so it is unit tested on every platform. The
 * shortcut is injected by `input::press_chord()`.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace capture_action {

  /**
   * @brief What the client asks for.
   * @details These are the wire values, shared with moonlight-common-c's LI_CAPTURE_*.
   */
  enum class action_t : std::uint8_t {
    screenshot = 1,  ///< Save a screenshot
    save_replay = 2,  ///< Save the replay buffer (the last few seconds)
    toggle_recording = 3,  ///< Start or stop a recording. Neither tool says which it did.
  };

  /// The host tool that carries the action out.
  enum class provider_t : std::uint8_t {
    off,  ///< Captures are disabled
    gamebar,  ///< Xbox Game Bar, on every Windows PC
    nvidia,  ///< The NVIDIA app or GeForce Experience, with its default shortcuts
  };

  /// `x-ml-general.featureFlags` bit: the client sends capture actions.
  /// Next to focus hints' 0x10000, far from the low bits for the same reasons.
  inline constexpr std::uint32_t client_feature_flag = 0x20000;

  /// `x-ss-general.featureFlags` bit: the host understands capture actions.
  inline constexpr std::uint32_t host_feature_flag = 0x20000;

  /// `x-ss-general.featureFlags` bit: the host has a provider configured for this session.
  /// Never set without `host_feature_flag`.
  inline constexpr std::uint32_t host_available_flag = 0x40000;

  /// Control stream message type, client to host. Unused by Moonlight, Sunshine, Apollo and
  /// Vibeshine; next to focus hints' 0x3020.
  inline constexpr std::uint16_t packet_type = 0x3021;

  /// First byte of the payload. A later version may append fields, never reorder these.
  inline constexpr std::uint8_t wire_version = 1;

  /// Version (u8), action (u8), reserved (u16).
  inline constexpr std::size_t payload_size = 4;

  /**
   * @brief Read one request.
   * @return The action, or nothing for a short payload, version 0 or an action this host
   *         does not know. Fields a later version appends are ignored.
   */
  std::optional<action_t> decode_payload(std::string_view payload);

  /// Parse a `capture_provider` value. Nothing when it is not one.
  std::optional<provider_t> parse_provider(std::string_view value);

  /// Human-readable names, for logs.
  const char *to_string(action_t action);
  const char *to_string(provider_t provider);

  /**
   * @brief The provider's shortcut for an action, as Windows virtual-key codes.
   * @details Modifiers first, in the order they go down; they come up in reverse.
   *          Empty when the provider is off.
   */
  std::span<const std::uint16_t> chord(provider_t provider, action_t action);

  /// Whether this build can press the tools' shortcuts. Both tools are Windows only.
#ifdef _WIN32
  inline constexpr bool host_supported = true;
#else
  inline constexpr bool host_supported = false;
#endif

  /// The `x-ss-general.featureFlags` value to advertise.
  [[nodiscard]] constexpr std::uint32_t host_feature_flags(const std::uint32_t platform_caps, const bool supported, const provider_t provider) {
    if (!supported) {
      return platform_caps;
    }
    return platform_caps | host_feature_flag | (provider != provider_t::off ? host_available_flag : 0);
  }

  /// The provider a session's actions go to: only when the host has one and the client asked.
  [[nodiscard]] constexpr provider_t negotiated(const bool supported, const provider_t provider, const std::uint32_t client_feature_flags) {
    return supported && (client_feature_flags & client_feature_flag) != 0 ? provider : provider_t::off;
  }

}  // namespace capture_action
