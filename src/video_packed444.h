/**
 * @file src/video_packed444.h
 * @brief Packed 4:4:4 video ("chroma packing v1"): a full-resolution Y/U/V picture carried as sample
 *        data inside one ordinary 4:2:0 HEVC or AV1 picture, rebuilt by the client on the GPU.
 *
 * Wire names and layout are the ones of RamazanKara/nightjar and RamazanKara/Rubylight, byte for
 * byte, so Scirocco also works with a Nightjar client and a Zefiro client also works with a
 * Rubylight host. Only the NVENC D3D11 path offers it, and only when `experimental_packed_444` is on.
 *
 * Negotiation (plain RTSP SDP lines, unknown attributes are ignored by unaware peers):
 *   DESCRIBE  host   a=x-rl-video.packed444Version:1
 *   DESCRIBE  host   a=x-rl-video.packed444Modes:<mask>   bit0 HEVC 8, bit1 HEVC 10, bit2 AV1 8, bit3 AV1 10
 *   ANNOUNCE  client a=x-rl-video[0].chromaPacking:1
 *
 * Sizes: the logical size W x H (clientViewportWd/Ht) is the only size capture, letterbox, input
 * and stats use. The coded size is Wc = (W + 63) & ~63, Hc = 2 * ((H + 7) & ~7).
 *
 * Layout (sample planes of the coded 4:2:0 picture; SY/SU/SV are the full-resolution source planes
 * with edge replication beyond W x H):
 *   CY(x, y)          = SY(x, y)             0 <= y < Hp          (all luma)
 *   CY(i, Hp + y)     = SU(2i + 1, y)        0 <= i < Wp/2        (odd-x U)
 *   CY(Wp/2 + i, Hp + y) = SV(2i + 1, y)                          (odd-x V)
 *   CU(i, j)          = SU(2i, j)            0 <= j < Hp          (even-x U)
 *   CV(i, j)          = SV(2i, j)                                 (even-x V)
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace video::packed444 {
  using namespace std::literals;

  inline constexpr std::string_view sdp_version_attribute = "x-rl-video.packed444Version"sv;
  inline constexpr std::string_view sdp_modes_attribute = "x-rl-video.packed444Modes"sv;
  inline constexpr std::string_view announce_attribute = "x-rl-video[0].chromaPacking"sv;
  inline constexpr int wire_version = 1;

  inline constexpr unsigned mode_hevc_8 = 1;
  inline constexpr unsigned mode_hevc_10 = 2;
  inline constexpr unsigned mode_av1_8 = 4;
  inline constexpr unsigned mode_av1_10 = 8;
  inline constexpr unsigned mode_mask = 15;

  inline constexpr int max_width = 16384;
  inline constexpr int max_height = 8192;

  struct dimensions_t {
    int width = 0;
    int height = 0;

    bool operator==(const dimensions_t &) const = default;
  };

  inline int padded_width(int width) {
    return (width + 63) & ~63;
  }

  inline int padded_height(int height) {
    return (height + 7) & ~7;
  }

  /// Size of the coded 4:2:0 picture for a logical size.
  inline dimensions_t coded_size(int width, int height) {
    return {padded_width(width), 2 * padded_height(height)};
  }

  inline bool logical_size_valid(int width, int height) {
    return width >= 1 && width <= max_width && height >= 1 && height <= max_height;
  }

  /**
   * Modes the host offers, from the probed codec modes (video::active_hevc_mode / active_av1_mode:
   * 1 off, 2 on in 8-bit, 3 on with Main10). Zero when no mode is available.
   */
  inline unsigned offered_modes(int hevc_mode, int av1_mode) {
    unsigned modes = 0;
    if (hevc_mode >= 2) modes |= mode_hevc_8;
    if (hevc_mode >= 3) modes |= mode_hevc_10;
    if (av1_mode >= 2) modes |= mode_av1_8;
    if (av1_mode >= 3) modes |= mode_av1_10;
    return modes;
  }

  /// Mode bit of a codec (video format 1 HEVC, 2 AV1) and depth, 0 for any other format.
  inline unsigned mode_bit(int video_format, bool ten_bit) {
    if (video_format == 1) return ten_bit ? mode_hevc_10 : mode_hevc_8;
    if (video_format == 2) return ten_bit ? mode_av1_10 : mode_av1_8;
    return 0;
  }

  /// The two DESCRIBE lines (each ending in a line break), empty when the option is off or no mode is offered.
  inline std::string describe_lines(bool option_enabled, unsigned modes) {
    modes &= mode_mask;
    if (!option_enabled || modes == 0) {
      return {};
    }
    std::string out;
    out += "a="s;
    out += sdp_version_attribute;
    out += ':';
    out += std::to_string(wire_version);
    out += '\n';
    out += "a="s;
    out += sdp_modes_attribute;
    out += ':';
    out += std::to_string(modes);
    out += '\n';
    return out;
  }

  /**
   * Value of the ANNOUNCE attribute: nullopt when the value is invalid, 0 for absent or "0" (off),
   * 1 for "1" (packing requested).
   */
  inline std::optional<int> parse_chroma_packing(std::optional<std::string_view> value) {
    if (!value || *value == "0"sv) return 0;
    if (*value == "1"sv) return 1;
    return std::nullopt;
  }

  struct announce_t {
    int video_format = 0;  ///< x-nv-vqos[0].bitStreamFormat
    int dynamic_range = 0;  ///< x-nv-video[0].dynamicRangeMode, 1 = 10-bit packed
    int chroma_sampling_type = 0;  ///< x-ss-video[0].chromaSamplingType
    int width = 0;  ///< logical size
    int height = 0;
  };

  /**
   * Validate an ANNOUNCE that requests packing. Returns a one-line reason for the 400 reply, or
   * nullptr when the request is acceptable. The real encoder limits are checked later, when the
   * encoder is created at the coded size.
   */
  inline const char *validate_announce(const announce_t &announce, bool option_enabled, unsigned host_modes) {
    if (announce.chroma_sampling_type != 0) {
      return "chroma packing cannot be combined with native 4:4:4";
    }
    if (announce.video_format != 1 && announce.video_format != 2) {
      return "chroma packing needs HEVC or AV1";
    }
    if (announce.dynamic_range != 0 && announce.dynamic_range != 1) {
      return "chroma packing dynamicRangeMode must be 0 or 1";
    }
    if (!option_enabled) {
      return "chroma packing is disabled on this host";
    }
    if ((host_modes & mode_mask & mode_bit(announce.video_format, announce.dynamic_range == 1)) == 0) {
      return "chroma packing mode is not offered by this host";
    }
    if (!logical_size_valid(announce.width, announce.height)) {
      return "chroma packing size is out of range";
    }
    return nullptr;
  }

  // CPU reference of the layout, used by the unit tests and as the specification of the GPU shaders.

  /// One full-resolution plane (row-major) of code values.
  struct plane_t {
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> samples;

    std::uint16_t at(int x, int y) const {
      return samples[(std::size_t) y * width + x];
    }
  };

  /// A coded 4:2:0 picture: Y plane Wp x 2Hp, chroma planes Wp/2 x Hp each.
  struct coded_picture_t {
    int padded_width = 0;
    int padded_height = 0;
    std::vector<std::uint16_t> y;
    std::vector<std::uint16_t> u;
    std::vector<std::uint16_t> v;

    std::uint16_t cy(int x, int yy) const {
      return y[(std::size_t) yy * padded_width + x];
    }

    std::uint16_t cu(int i, int j) const {
      return u[(std::size_t) j * (padded_width / 2) + i];
    }

    std::uint16_t cv(int i, int j) const {
      return v[(std::size_t) j * (padded_width / 2) + i];
    }
  };

  /// Source sample with edge replication.
  inline std::uint16_t padded_sample(const plane_t &plane, int x, int y) {
    const int cx = x < plane.width ? x : plane.width - 1;
    const int cy = y < plane.height ? y : plane.height - 1;
    return plane.at(cx, cy);
  }

  inline coded_picture_t pack(const plane_t &sy, const plane_t &su, const plane_t &sv) {
    coded_picture_t out;
    const int wp = padded_width(sy.width);
    const int hp = padded_height(sy.height);
    out.padded_width = wp;
    out.padded_height = hp;
    out.y.assign((std::size_t) wp * 2 * hp, 0);
    out.u.assign((std::size_t) (wp / 2) * hp, 0);
    out.v.assign((std::size_t) (wp / 2) * hp, 0);

    for (int y = 0; y < hp; ++y) {
      for (int x = 0; x < wp; ++x) {
        out.y[(std::size_t) y * wp + x] = padded_sample(sy, x, y);
      }
      for (int i = 0; i < wp / 2; ++i) {
        out.y[(std::size_t) (hp + y) * wp + i] = padded_sample(su, 2 * i + 1, y);
        out.y[(std::size_t) (hp + y) * wp + wp / 2 + i] = padded_sample(sv, 2 * i + 1, y);
        out.u[(std::size_t) y * (wp / 2) + i] = padded_sample(su, 2 * i, y);
        out.v[(std::size_t) y * (wp / 2) + i] = padded_sample(sv, 2 * i, y);
      }
    }
    return out;
  }

  /// Reconstruction of one logical pixel (the client's rule), x < W and y < H.
  struct yuv_t {
    std::uint16_t y;
    std::uint16_t u;
    std::uint16_t v;

    bool operator==(const yuv_t &) const = default;
  };

  inline yuv_t unpack(const coded_picture_t &picture, int x, int y) {
    const int wp = picture.padded_width;
    const int hp = picture.padded_height;
    yuv_t out {};
    out.y = picture.cy(x, y);
    if ((x & 1) == 0) {
      out.u = picture.cu(x / 2, y);
      out.v = picture.cv(x / 2, y);
    } else {
      out.u = picture.cy(x >> 1, hp + y);
      out.v = picture.cy(wp / 2 + (x >> 1), hp + y);
    }
    return out;
  }
}  // namespace video::packed444
