/**
 * @file src/frame_warp.h
 * @brief Frame Warp on the host: learn the running game's camera response, tag each video frame
 *        with the input it reflects, and find the HUD (src/frame_warp_wire.h has the wire formats).
 *
 * One `host_t` per stream, not thread-safe (the session wraps it in a lock). The control thread
 * hands it the client's request and every relative mouse move and controller packet as it decodes
 * them. The video side hands it each capture's luma thumbnail and, now and then, the GPU's HUD bits
 * (both a frame or more late), and asks for the tags of each frame as its number is assigned. The
 * control loop sends what `take_tags`, `take_model` and `take_mask` return.
 *
 * The learning is split as follows: src/frame_warp_history.h (input with times),
 * src/frame_warp_motion.h (picture motion from thumbnails, still pixels), src/frame_warp_lens.h
 * (field of view), src/frame_warp_calibrate.h (gains and lag, locking). The algorithms follow
 * RamazanKara/Rubylight `rust/core/src/frame_warp/` (commit b3e59a776).
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// local includes
#include "frame_warp_calibrate.h"
#include "frame_warp_history.h"
#include "frame_warp_lens.h"
#include "frame_warp_motion.h"
#include "frame_warp_prior.h"
#include "frame_warp_types.h"
#include "frame_warp_wire.h"

namespace frame_warp {

  /// The camera counts as turning for this long after the scene was last seen moving: the GPU's HUD
  /// pass only learns while it does.
  inline constexpr std::uint64_t motion_hold_us = 150'000;
  /// How far, in thumbnail pixels, the scene moves between the two captures the HUD pass compares:
  /// over less, a textured scene that moved can look as if it stayed.
  inline constexpr float motion_min = 1.0f;
  /// The confident shifts of this window add up to how far the scene moved: the camera turns when
  /// that is at least `motion_min`, and the HUD pass compares captures as far apart as the scene took
  /// to move that far.
  inline constexpr std::uint64_t motion_window_us = 100'000;
  /// Confidence a shift needs to count toward the scene moving.
  inline constexpr float motion_confidence = 0.5f;
  /// A move teaches the lens when the player's turning arrived within this of the input the move
  /// shows: the game's own camera work (a cutscene, a walk) is not a turn.
  inline constexpr std::uint64_t lens_input_margin_us = 30'000;
  /// Encoded tag messages kept for the control loop; older ones are dropped (the next carries them
  /// again).
  inline constexpr std::size_t pending_tags = 8;
  /// How often a changed HUD mask may be sent.
  inline constexpr std::uint64_t mask_every_us = 1'000'000;
  /// A mask that differs from the last sent in fewer cells than this is not sent.
  inline constexpr std::size_t mask_change = 16;
  /// HUD pieces up to twice this share of the picture's height apart are joined into one region (16
  /// pixels each way at 1080p). A translucent HUD, grey text on a panel or a ring of icons, changes
  /// with the scene behind it, so the GPU finds it only where it is nearly opaque, in pieces; left in
  /// pieces, it would break up as the scene shifts between them.
  inline constexpr float join_height = 0.015f;

  /// The learner's state, for logs and tests.
  struct status_t {
    bool enabled = false;
    std::optional<model_t> model;
    std::uint64_t samples = 0;
    std::size_t mask_cells = 0;
  };

  /**
   * Where the GPU's HUD grid, laid over the capture, lies on the stream's picture, which the client
   * masks: a capture of another shape is letterboxed into it.
   */
  struct placement_t {
    std::pair<std::uint16_t, std::uint16_t> picture {0, 0};
    /// Where the capture sits in the picture; the rest is black bars.
    rect_t content;
    /// The size of the mask the client gets, or 0 wide for none: its cells cover the picture.
    std::pair<std::uint16_t, std::uint16_t> mask {0, 0};

    /**
     * @brief `grid`, over the capture, as the client's mask over the picture: a cell is HUD where a
     *        grid cell whose centre falls in it is (as `mask_t::halved` would make it), or, when none
     *        does, the grid cell under its centre. The bars count as HUD, so they hold still and the
     *        picture is not shifted in from them. A grid over the whole picture at the mask's size is
     *        already that mask.
     */
    mask_t place(const mask_t &grid) const;

    bool operator==(const placement_t &) const = default;
  };

  /**
   * @brief The mask the client gets from the GPU's HUD cells: pieces close together joined
   *        (`join_height`), then grown by one cell all round, since the HUD's anti-aliased edges
   *        change a little with what is behind them and left out they would shift with the scene.
   */
  mask_t shape(const mask_t &mask);

  /// The thumbnail pixels (one byte each, row by row) whose centre falls in a HUD cell of `mask`.
  std::vector<std::uint8_t> hud_pixels(const mask_t &mask, std::pair<std::size_t, std::size_t> size);

  /// Decides which HUD masks go out.
  class mask_tracker_t {
  public:
    /// Sends the last mask again (a client that just asked has none).
    void resend();
    void clear_pending();

    /// A HUD mask, already shaped and placed (`shape`, `placement_t::place`).
    void offer(const mask_t &shaped, std::uint16_t max_width, std::uint64_t now_us);

    std::optional<std::vector<std::uint8_t>> take() {
      auto out = std::move(pending_);
      pending_.reset();
      return out;
    }

    const std::optional<mask_t> &sent() const {
      return sent_;
    }

  private:
    std::uint16_t generation_ = 0;
    std::optional<mask_t> sent_;
    std::optional<std::uint64_t> sent_at_us_;
    std::optional<std::vector<std::uint8_t>> pending_;
  };

  class host_t {
  public:
    /// Frame Warp for a stream of `picture_width` x `picture_height` pixels; off until a client asks.
    explicit host_t(std::uint16_t picture_width = 1920, std::uint16_t picture_height = 1080);

    /**
     * @brief Applies a client's request (the 0x5543 payload).
     * @return Whether Frame Warp is on, none for a malformed request, which changes nothing.
     */
    std::optional<bool> on_request(std::span<const std::uint8_t> payload);

    bool enabled() const {
      return enabled_;
    }

    /// The last request's flags bit 0: the client draws the lens.
    bool lens_requested() const {
      return lens_requested_;
    }

    /**
     * @brief The stream's picture size changed (a reconfigure), or where the capture sits in it: a
     *        capture of another shape is letterboxed. `content` is the whole picture when the capture
     *        fills it.
     */
    void set_picture(std::uint16_t width, std::uint16_t height, rect_t content);

    /// The thumbnail size the GPU should make for this picture's content.
    std::pair<std::size_t, std::size_t> thumbnail_size() const {
      return thumbnail_;
    }

    void set_prior(const prior_t &prior);

    /// What to remember for this game.
    std::optional<prior_t> prior() const;

    /// A relative mouse move, as the control thread decoded it.
    void on_mouse(std::int16_t dx, std::int16_t dy, std::uint64_t at_us);

    /// A controller packet, as the control thread decoded it.
    void on_controller(std::uint16_t number, std::int16_t right_x, std::int16_t right_y, std::uint64_t at_us);

    /**
     * @brief A capture's luma thumbnail: measures the shift from the previous one, learns, and fits
     *        again when it is time (synchronously: the caller runs this off the frame path).
     */
    void on_thumbnail(thumbnail_t &&thumbnail, std::uint64_t now_us);

    /// Whether the camera is turning: the GPU's HUD pass learns only then.
    bool camera_moving(std::uint64_t now_us) const;

    /// While the camera turns, how far apart the captures are that the GPU's HUD pass compares, so
    /// that the scene moves about `motion_min` between them; none while it stands still.
    std::optional<std::uint64_t> hud_every_us(std::uint64_t now_us) const;

    /// The width of the HUD grid the GPU should make over the capture, or 0 for none: as wide as the
    /// capture's part of the mask the client gets.
    std::uint16_t mask_width() const;

    /// How the GPU's HUD grid over the capture lies on the picture the client masks.
    placement_t placement() const;

    /**
     * @brief The GPU's HUD bits (one per mask cell, laid out as `mask_t::words`) over the capture:
     *        shaped, laid over the picture, and sent when it changed enough; also left out of the
     *        motion measurement.
     */
    void on_hud(mask_t &&mask, std::uint64_t now_us);

    /// Tags frame `frame`, captured at `capture_us`, and queues the tags message (this frame's tag
    /// and up to three before it).
    void tag(std::uint32_t frame, std::uint64_t capture_us);

    /// The next tags message to send (unreliable).
    std::optional<std::vector<std::uint8_t>> take_tags();

    /// The model to send (reliable), when it changed.
    std::optional<std::array<std::uint8_t, model_bytes>> take_model();

    /// The HUD mask to send (reliable), when it changed.
    std::optional<std::vector<std::uint8_t>> take_mask();

    status_t status() const;

    /// A confident shift between captures at `t0` and `t1`: whether, with those of the last
    /// `motion_window_us`, the scene moved far enough for the camera to count as turning, and how
    /// long it took to move `motion_min` at that speed. Public for the unit tests.
    void add_shift(std::uint64_t t0_us, std::uint64_t t1_us, float dx, float dy, std::uint64_t now_us);

    const calibrator_t &calibrator() const {
      return calibrator_;
    }

  private:
    struct shift_t {
      std::uint64_t t0_us;
      std::uint64_t t1_us;
      float dx;
      float dy;
    };

    std::uint16_t calibrator_generation() const;
    /// Puts the learned lens in force.
    void apply_lens();
    /// Whether the player's mouse or stick was turning the camera in the input that the captures from
    /// `t0_us` to `t1_us` show.
    bool turning(std::uint64_t t0_us, std::uint64_t t1_us) const;

    bool enabled_ = false;
    bool lens_requested_ = false;
    std::uint16_t mask_max_width_ = 0;
    history_t history_;
    calibrator_t calibrator_;
    /// The game's field of view, from the moves the player's turns made.
    lens_t lens_;
    /// The previous thumbnail, the HUD's pixels and what stands still.
    measurer_t measurer_;
    std::pair<std::uint16_t, std::uint16_t> picture_;
    /// Where the capture sits in the picture.
    rect_t content_;
    std::pair<std::size_t, std::size_t> thumbnail_;
    std::deque<tag_t> recent_;
    std::deque<std::vector<std::uint8_t>> tags_;
    std::optional<model_t> model_;
    std::optional<model_t> last_model_;
    mask_tracker_t mask_;
    /// The confident shifts of the last `motion_window_us`.
    std::deque<shift_t> shifts_;
    std::optional<std::uint64_t> moving_until_us_;
    /// While the camera turns, how far apart the captures the HUD pass compares are.
    std::uint64_t hud_every_us_ = 0;
    std::uint64_t samples_ = 0;
  };

}  // namespace frame_warp
