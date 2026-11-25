/**
 * @file src/frame_warp_calibrate.h
 * @brief Learning the running game's camera response: how far the picture moves per mouse count
 *        and per second of stick, and how long after the input it moves (the game's lag).
 *
 * Each pair of analysed captures gives a measured content shift over a time span. Consecutive pairs
 * are summed into chunks of at least `chunk_us`, so a game that renders slower than the capture
 * (many still pairs, then a jump) still lines up with its input. For every candidate lag `L`, each
 * chunk's input is what arrived in its span moved `L` earlier, and a least-squares gain through the
 * origin fits shift to input on each axis. The lag whose fit explains the most of the measured
 * motion wins, preferring the shortest lag that comes within `lag_tolerance` of the best: a lag too
 * long would shift too far, a lag too short only falls short.
 *
 * A fit locks once it explains `lock_fit` of the motion over enough input, and unlocks when recent
 * input with the locked model explains less than `unlock_fit` (a menu, a cutscene, the mouse moving
 * a cursor): a shift that matches nothing on screen is worse than none.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

// local includes
#include "frame_warp_history.h"
#include "frame_warp_lens.h"
#include "frame_warp_motion.h"
#include "frame_warp_prior.h"
#include "frame_warp_wire.h"

namespace frame_warp {

  /// Samples kept and fitted over.
  inline constexpr std::uint64_t window_us = 4'000'000;
  /// The recent span an unlock decision looks at.
  inline constexpr std::uint64_t recent_us = 1'200'000;
  /// Shortest span a chunk of samples covers.
  inline constexpr std::uint64_t chunk_us = 30'000;
  /// Lags tried: 0 to 200 ms.
  inline constexpr std::uint64_t max_lag_us = 200'000;
  /// Fit (explained share of the motion) that locks, and that recent input must keep.
  inline constexpr double lock_fit = 0.80;
  inline constexpr double unlock_fit = 0.50;
  /// How often the fit runs.
  inline constexpr std::uint64_t fit_every_us = 200'000;
  /// Largest shift, as a fraction of the picture's width and height.
  inline constexpr float max_offset_share = 0.15f;
  /// The lag before anything is fitted.
  inline constexpr std::uint64_t default_lag_us = 40'000;

  /// One measured shift between two analysed captures.
  struct sample_t {
    std::uint64_t t0_us = 0;
    std::uint64_t t1_us = 0;
    motion_t motion;
  };

  /// Consecutive confident samples summed, in thumbnail pixels as the camera's turn at the centre.
  struct chunk_t {
    std::uint64_t t0_us = 0;
    std::uint64_t t1_us = 0;
    double dx = 0.0;
    double dy = 0.0;
  };

  /// A fit of one input kind.
  struct fit_t {
    /// Thumbnail pixels per mouse count, or per second of full deflection.
    std::array<double, 2> gain {};
    std::uint64_t lag_us = 0;
    /// Share of the motion explained, 0 to 1.
    double fit = 0.0;
    float deadzone = 0.0f;
    float exponent = 1.0f;

    bool operator==(const fit_t &) const = default;
  };

  /// The input a chunk saw at a lag, or none when it does not count for this fit.
  using input_fn = std::function<std::optional<std::array<double, 2>>(const chunk_t &, std::uint64_t)>;

  /// How a locked `model` explains the motion at its lag, with the given gain or the least-squares one.
  using explain_fn = std::function<std::optional<fit_t>(const fit_t &, std::optional<std::array<double, 2>>)>;

  /**
   * @brief A locked `model` keeps its lag unless `fresh` explains at least 3 points more of the
   *        motion than `model` does over the same samples, or a quarter of what `model` leaves
   *        unexplained when that is less; otherwise `fresh` is dropped and `model` takes the
   *        least-squares gain at its lag when that explains as much.
   */
  void keep_unless_better(fit_t &model, std::optional<fit_t> &fresh, const explain_fn &at_lag);

  class calibrator_t {
  public:
    /// A calibrator for a stream of `picture` pixels whose thumbnails are `thumbnail` pixels.
    calibrator_t(std::pair<std::uint16_t, std::uint16_t> picture, std::pair<std::size_t, std::size_t> thumbnail);

    /**
     * @brief A new stream or thumbnail size: the stream's `picture`, the `content` of it the game
     *        fills (the picture less black bars) and the `thumbnail` of that content. What was
     *        learned carries over in picture pixels.
     */
    void set_sizes(std::pair<std::uint16_t, std::uint16_t> picture, std::pair<std::uint16_t, std::uint16_t> content, std::pair<std::size_t, std::size_t> thumbnail);

    /// Forgets the measured moves (their input history is gone); what was learned stays.
    void clear_samples();

    /// The game's lens changed (`lens_t`): the squared tangent of half its horizontal field of view.
    void set_lens(std::optional<double> lens);

    /// Whether the client draws the picture through the lens. One that slides it is sent no focal
    /// length and no limit for one; its gains are the picture centre's either way.
    void set_lens_drawn(bool drawn);

    std::optional<double> lens() const {
      return lens_;
    }

    /// Where the motion estimate's cells lie in the game's picture.
    const layout_t &layout() const {
      return layout_;
    }

    /// The camera's turn in a measured move, under the lens in force.
    turn_t resolve(const motion_t &motion) const;

    /// Starts from what an earlier session learned: the lags apply at once and the gains lock as
    /// soon as recent input confirms them.
    void set_prior(const prior_t &prior);

    /// What to remember for this game: the locked fits, in picture pixels.
    std::optional<prior_t> prior() const;

    /// The lag to tag mouse input with: the fitted one, else the default.
    std::uint64_t mouse_lag_us() const;
    std::uint64_t stick_lag_us() const;

    bool locked() const {
      return mouse_locked_ || stick_locked_;
    }

    void add(const sample_t &sample);

    /// Whether `update` would fit at `now_us`.
    bool due(std::uint64_t now_us) const;

    /// Fits again if it is time, and returns the model to send when it changed enough to.
    std::optional<model_t> update(const history_t &history, std::uint64_t now_us);

    /// The model for generation `generation`.
    model_t model(std::uint16_t generation) const;

    // The fits as they stand, for the unit tests.
    const std::optional<fit_t> &mouse_fit() const {
      return mouse_;
    }

    void set_mouse_fit(const std::optional<fit_t> &fit) {
      mouse_ = fit;
    }

    bool mouse_locked() const {
      return mouse_locked_;
    }

  private:
    void update_mouse(const history_t &history, const std::vector<chunk_t> &all, const std::vector<chunk_t> &recent);
    void update_stick(const history_t &history, const std::vector<chunk_t> &all, const std::vector<chunk_t> &recent);

    /// Confident samples since `since`, summed into chunks of at least `chunk_us`. A gap or an
    /// unsure sample ends a chunk.
    std::vector<chunk_t> chunks(std::optional<std::uint64_t> since) const;

    /// The model as it stands, if it differs enough from the last one sent.
    std::optional<model_t> take_model();

    std::deque<sample_t> samples_;
    /// Picture pixels per thumbnail pixel, x and y.
    std::array<double, 2> scale_ {1.0, 1.0};
    /// The stream's picture, which the model is sent for.
    std::pair<std::uint16_t, std::uint16_t> picture_;
    /// The game's part of it, which the thumbnails cover: the picture less any black bars.
    std::pair<std::uint16_t, std::uint16_t> content_;
    /// Where the motion estimate's cells lie in that part.
    layout_t layout_;
    /// The game's lens, as the squared tangent of half its horizontal field of view; none for a flat
    /// picture or one not learned yet. With one, samples count as the turn at the picture's centre
    /// and so do the gains.
    std::optional<double> lens_;
    /// Whether the model carries the lens (`set_lens_drawn`).
    bool lens_drawn_ = true;
    std::optional<fit_t> mouse_;
    std::optional<fit_t> stick_;
    bool mouse_locked_ = false;
    bool stick_locked_ = false;
    std::optional<std::uint64_t> last_fit_us_;
    std::uint16_t generation_ = 0;
    std::optional<model_t> sent_;
    std::optional<prior_t> prior_;
  };

}  // namespace frame_warp
