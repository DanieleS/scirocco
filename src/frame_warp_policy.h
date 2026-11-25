/**
 * @file src/frame_warp_policy.h
 * @brief When the host offers Frame Warp (serverinfo and DESCRIBE), see frame_warp_wire.h.
 *
 * The offer costs nothing: a client that does not know Frame Warp ignores the serverinfo element
 * and the DESCRIBE line, and the host does no work for a session until its client asks.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>

// local includes
#include "frame_warp_wire.h"

namespace frame_warp::policy {

  /// This build can measure frames: the capture and conversion path with the GPU passes is Windows' D3D11.
  inline constexpr bool build_supported =
#ifdef _WIN32
    true;
#else
    false;
#endif

  /**
   * @brief Whether serverinfo carries `FrameWarpVersion`.
   * @param setting The `frame_warp` setting.
   */
  constexpr bool offered_in_serverinfo(bool setting, bool supported_build = build_supported) {
    return setting && supported_build;
  }

  /**
   * @brief Whether DESCRIBE lists the four ids for a launch.
   * @param setting The `frame_warp` setting.
   * @param video_session A normal game stream: not input-only or monitor, and not a secondary client.
   */
  constexpr bool offered_in_describe(bool setting, bool video_session, bool supported_build = build_supported) {
    return setting && video_session && supported_build;
  }

  /// The serverinfo element, `<FrameWarpVersion>1</FrameWarpVersion>` as the XML tree prints it.
  inline std::string serverinfo_value() {
    return std::to_string(wire_version);
  }

}  // namespace frame_warp::policy
