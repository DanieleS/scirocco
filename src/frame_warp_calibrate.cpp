/**
 * @file src/frame_warp_calibrate.cpp
 * @brief Definitions for src/frame_warp_calibrate.h.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

// local includes
#include "frame_warp_calibrate.h"

namespace frame_warp {

  namespace {
    constexpr std::uint64_t lag_step_us = 2'000;
    /// The shortest lag within this fraction of the best fit wins.
    constexpr double lag_tolerance = 0.995;
    /// Input a fit needs: mouse counts, seconds of full deflection, and thumbnail pixels moved.
    constexpr double min_mouse_counts = 300.0;
    constexpr double min_stick_seconds = 0.4;
    constexpr double min_motion = 30.0;
    /// Samples whose estimate is less sure than this are left out.
    constexpr float min_confidence = 0.35f;
    /**
     * How much more of the motion a fresh fit must explain than the locked model does over the same
     * samples to replace it. Without it every fit (5 a second) replaced the model: on smooth input
     * many lags fit almost equally, the chosen lag wandered by 10-20 ms between fits, and each
     * change moved every frame's tag by the input in between, a visible jolt while turning.
     */
    constexpr double refit_margin = 0.03;
    /// Or this share of what the locked model leaves unexplained, when that is less: at a 99% fit no
    /// fit could explain 3 points more, and a lag a few milliseconds off, picked on the first few
    /// turns, would stay for good.
    constexpr double refit_share = 0.25;
    /// A locked model is sent again when a gain moves by this fraction or the lag by this much. A gain
    /// change is seamless on the client (the shift of the input still outstanding scales with it); a
    /// lag change moves every tag.
    constexpr float gain_change = 0.02f;
    constexpr std::uint32_t lag_change_us = 3'000;
    /// Under a lens the shift at the centre also stays within this share of the squared focal length
    /// over the picture's edge: at the whole of it the turn would bring the picture's edge level with
    /// the camera, and near it the edge is stretched beyond use.
    constexpr double max_lens_offset = 0.6;
    /// A model is sent again when its focal length moves by this fraction.
    constexpr float focal_change = 0.03f;
    /// Stick dead zones tried.
    constexpr std::array<float, 5> deadzones {0.05f, 0.10f, 0.15f, 0.20f, 0.25f};

    std::uint64_t saturating_sub(std::uint64_t a, std::uint64_t b) {
      return a > b ? a - b : 0;
    }

    /**
     * The fit of `input` at `lag`: with `gain` given, how much of the motion it explains (`checking`
     * says how: against the measured and the predicted motion together, or against the measured
     * motion alone like a fresh fit); without, the least-squares gains and theirs. None without
     * enough input or motion.
     *
     * An axis with too little input of its own, under half of `min_input` or a quarter of the other
     * axis's, takes the other axis's gain (square pixels, the same sensitivity both ways, as games
     * default to): a gain fitted to a sliver of input, a turn's slight drift up or down, mostly fits
     * whatever else moves the picture that way (a ship's roll) and changes with every fit.
     */
    std::optional<fit_t> fit_at(
      const std::vector<chunk_t> &chunks,
      const input_fn &input,
      std::uint64_t lag,
      std::optional<std::array<double, 2>> gain,
      double min_input,
      bool checking
    ) {
      std::array<double, 2> uu {};
      std::array<double, 2> um {};
      std::array<double, 2> mm {};
      std::array<double, 2> used_input {};
      double moved = 0.0;
      for (const auto &chunk : chunks) {
        const auto u = input(chunk, lag);
        if (!u) {
          continue;
        }
        const std::array<double, 2> m {chunk.dx, chunk.dy};
        for (std::size_t axis = 0; axis < 2; ++axis) {
          uu[axis] += (*u)[axis] * (*u)[axis];
          um[axis] += (*u)[axis] * m[axis];
          mm[axis] += m[axis] * m[axis];
          used_input[axis] += std::abs((*u)[axis]);
        }
        moved += std::abs(m[0]) + std::abs(m[1]);
      }
      // A fresh fit needs motion to fit; checking a given gain does not, as a picture that stands
      // still while input arrives is exactly what must fail it.
      if (used_input[0] + used_input[1] < min_input || (!checking && moved < min_motion)) {
        return std::nullopt;
      }
      const auto enough = [&](std::size_t axis) {
        return used_input[axis] >= min_input * 0.5 && used_input[axis] * 4.0 >= used_input[1 - axis] && uu[axis] > 0.0;
      };
      std::array<double, 2> g {};
      if (gain) {
        g = *gain;
      } else {
        const auto own = [&](std::size_t axis) {
          return um[axis] / uu[axis];
        };
        const bool x = enough(0);
        const bool y = enough(1);
        if (x && y) {
          g = {own(0), own(1)};
        } else if (x) {
          g = {own(0), own(0)};
        } else if (y) {
          g = {own(1), own(1)};
        } else {
          g = {0.0, 0.0};
        }
      }
      // Explained share: 1 - residual / total, residual = sum (m - g u)^2. A given gain is checked
      // against the measured and the predicted motion together, so a still picture scores 0 instead
      // of having nothing to compare with.
      double residual = 0.0;
      double predicted = 0.0;
      for (std::size_t axis = 0; axis < 2; ++axis) {
        residual += mm[axis] - 2.0 * g[axis] * um[axis] + g[axis] * g[axis] * uu[axis];
        predicted += g[axis] * g[axis] * uu[axis];
      }
      residual = std::max(residual, 0.0);
      const double total = checking ? (mm[0] + mm[1] + predicted) * 0.5 : mm[0] + mm[1];
      if (total <= 0.0) {
        return std::nullopt;
      }
      fit_t out;
      out.gain = g;
      out.lag_us = lag;
      out.fit = std::clamp(1.0 - residual / total, 0.0, 1.0);
      out.deadzone = 0.0f;
      out.exponent = 1.0f;
      return out;
    }

    /// `fit_at` with `checking` following `gain`.
    std::optional<fit_t> score(const std::vector<chunk_t> &chunks, const input_fn &input, std::uint64_t lag, std::optional<std::array<double, 2>> gain, double min_input) {
      return fit_at(chunks, input, lag, gain, min_input, gain.has_value());
    }

    /// How much of the motion `gain` at `lag` explains, or without one the least-squares gain there
    /// and its share, counted as a fresh fit's share is (against the measured motion alone), so the
    /// two compare.
    std::optional<fit_t> explained(const std::vector<chunk_t> &chunks, const input_fn &input, std::uint64_t lag, std::optional<std::array<double, 2>> gain, double min_input) {
      return fit_at(chunks, input, lag, gain, min_input, false);
    }

    /// The best lag for `input`, and the gains there.
    std::optional<fit_t> best_fit(const std::vector<chunk_t> &chunks, const input_fn &input, double min_input) {
      std::vector<fit_t> fits;
      for (std::uint64_t lag = 0; lag <= max_lag_us; lag += lag_step_us) {
        if (auto fit = score(chunks, input, lag, std::nullopt, min_input)) {
          fits.push_back(*fit);
        }
      }
      double best = -std::numeric_limits<double>::infinity();
      for (const auto &f : fits) {
        best = std::max(best, f.fit);
      }
      if (!std::isfinite(best)) {
        return std::nullopt;
      }
      for (const auto &f : fits) {
        if (f.fit >= best * lag_tolerance) {
          return f;
        }
      }
      return std::nullopt;
    }

    /// `gain` in picture pixels from thumbnail pixels.
    std::array<float, 2> to_picture(const std::array<double, 2> &gain, const std::array<double, 2> &scale) {
      return {static_cast<float>(gain[0] * scale[0]), static_cast<float>(gain[1] * scale[1])};
    }
  }  // namespace

  void keep_unless_better(fit_t &model, std::optional<fit_t> &fresh, const explain_fn &at_lag) {
    if (!fresh) {
      return;
    }
    const auto found = *fresh;
    const auto now = at_lag(model, model.gain);
    if (now && now->fit + std::min(refit_margin, refit_share * std::max(1.0 - now->fit, 0.0)) >= found.fit) {
      auto settled = at_lag(model, std::nullopt);
      if (settled && !(settled->fit >= now->fit)) {
        settled.reset();
      }
      model.gain = settled ? settled->gain : model.gain;
      model.fit = settled ? settled->fit : now->fit;
      fresh.reset();
    }
  }

  calibrator_t::calibrator_t(std::pair<std::uint16_t, std::uint16_t> picture, std::pair<std::size_t, std::size_t> thumbnail):
      picture_ {picture},
      content_ {picture},
      layout_ {picture, thumbnail} {
    set_sizes(picture, picture, thumbnail);
  }

  void calibrator_t::set_sizes(std::pair<std::uint16_t, std::uint16_t> picture, std::pair<std::uint16_t, std::uint16_t> content, std::pair<std::size_t, std::size_t> thumbnail) {
    const auto old_scale = scale_;
    picture_ = {std::max<std::uint16_t>(picture.first, 1), std::max<std::uint16_t>(picture.second, 1)};
    content_ = {std::max<std::uint16_t>(content.first, 1), std::max<std::uint16_t>(content.second, 1)};
    scale_ = {
      static_cast<double>(content_.first) / static_cast<double>(std::max<std::size_t>(thumbnail.first, 1)),
      static_cast<double>(content_.second) / static_cast<double>(std::max<std::size_t>(thumbnail.second, 1)),
    };
    layout_ = layout_t {content_, thumbnail};
    if (old_scale != scale_) {
      // Samples are in the old thumbnail's pixels.
      samples_.clear();
      for (auto *fit : {&mouse_, &stick_}) {
        if (*fit) {
          for (std::size_t axis = 0; axis < 2; ++axis) {
            (*fit)->gain[axis] *= old_scale[axis] / scale_[axis];
          }
        }
      }
    }
  }

  void calibrator_t::clear_samples() {
    samples_.clear();
  }

  void calibrator_t::set_lens(std::optional<double> lens) {
    if (lens && std::isfinite(*lens) && *lens > 0.0) {
      lens_ = lens;
    } else {
      lens_.reset();
    }
  }

  void calibrator_t::set_lens_drawn(bool drawn) {
    lens_drawn_ = drawn;
  }

  turn_t calibrator_t::resolve(const motion_t &motion) const {
    return layout_.resolve(motion, lens_);
  }

  void calibrator_t::set_prior(const prior_t &prior) {
    const auto scale = [&](const std::array<float, 2> &gain) {
      const std::array<double, 2> ratio {
        static_cast<double>(content_.first) / static_cast<double>(std::max<std::uint16_t>(prior.picture_width, 1)),
        static_cast<double>(content_.second) / static_cast<double>(std::max<std::uint16_t>(prior.picture_height, 1)),
      };
      return std::array<double, 2> {
        static_cast<double>(gain[0]) * ratio[0] / scale_[0],
        static_cast<double>(gain[1]) * ratio[1] / scale_[1],
      };
    };
    if (prior.mouse_gain && prior.mouse_lag_us) {
      fit_t fit;
      fit.gain = scale(*prior.mouse_gain);
      fit.lag_us = std::min(*prior.mouse_lag_us, max_lag_us);
      fit.fit = 0.0;
      fit.deadzone = 0.0f;
      fit.exponent = 1.0f;
      mouse_ = fit;
    }
    if (prior.stick_gain && prior.stick_lag_us) {
      fit_t fit;
      fit.gain = scale(*prior.stick_gain);
      fit.lag_us = std::min(*prior.stick_lag_us, max_lag_us);
      fit.fit = 0.0;
      fit.deadzone = std::clamp(prior.stick_deadzone.value_or(0.15f), 0.0f, 0.5f);
      fit.exponent = 1.0f;
      if (prior.stick_exponent && std::find(stick_exponents.begin(), stick_exponents.end(), *prior.stick_exponent) != stick_exponents.end()) {
        fit.exponent = *prior.stick_exponent;
      }
      stick_ = fit;
    }
    if (prior.focal && std::isfinite(*prior.focal) && *prior.focal > 0.0f) {
      // A field of view is the same at every picture size.
      const double ratio = static_cast<double>(std::max<std::uint16_t>(prior.picture_width, 1)) * 0.5 / static_cast<double>(*prior.focal);
      set_lens(ratio * ratio);
    }
    prior_ = prior;
  }

  std::optional<prior_t> calibrator_t::prior() const {
    std::optional<fit_t> mouse;
    std::optional<fit_t> stick;
    if (mouse_locked_) {
      mouse = mouse_;
    }
    if (stick_locked_) {
      stick = stick_;
    }
    if (!mouse && !stick) {
      return prior_;
    }
    std::optional<float> focal;
    if (lens_) {
      focal = static_cast<float>(layout_.focal(*lens_));
    }
    const prior_t earlier = prior_.value_or(prior_t {});
    prior_t out;
    out.mouse_gain = mouse ? std::optional {to_picture(mouse->gain, scale_)} : earlier.mouse_gain;
    out.mouse_lag_us = mouse ? std::optional {mouse->lag_us} : earlier.mouse_lag_us;
    out.stick_gain = stick ? std::optional {to_picture(stick->gain, scale_)} : earlier.stick_gain;
    out.stick_lag_us = stick ? std::optional {stick->lag_us} : earlier.stick_lag_us;
    out.stick_deadzone = stick ? std::optional {stick->deadzone} : earlier.stick_deadzone;
    out.stick_exponent = stick ? std::optional {stick->exponent} : earlier.stick_exponent;
    out.focal = focal;
    out.picture_width = content_.first;
    out.picture_height = content_.second;
    return out;
  }

  std::uint64_t calibrator_t::mouse_lag_us() const {
    return mouse_ ? mouse_->lag_us : default_lag_us;
  }

  std::uint64_t calibrator_t::stick_lag_us() const {
    return stick_ ? stick_->lag_us : mouse_lag_us();
  }

  void calibrator_t::add(const sample_t &sample) {
    if (sample.t1_us <= sample.t0_us) {
      return;
    }
    samples_.push_back(sample);
    while (!samples_.empty() && saturating_sub(sample.t1_us, samples_.front().t1_us) > window_us) {
      samples_.pop_front();
    }
  }

  bool calibrator_t::due(std::uint64_t now_us) const {
    return !last_fit_us_ || saturating_sub(now_us, *last_fit_us_) >= fit_every_us;
  }

  std::optional<model_t> calibrator_t::update(const history_t &history, std::uint64_t now_us) {
    if (!due(now_us)) {
      return std::nullopt;
    }
    last_fit_us_ = now_us;
    const auto all = chunks(std::nullopt);
    const auto recent = chunks(saturating_sub(now_us, recent_us));
    update_mouse(history, all, recent);
    update_stick(history, all, recent);
    return take_model();
  }

  void calibrator_t::update_mouse(const history_t &history, const std::vector<chunk_t> &all, const std::vector<chunk_t> &recent) {
    const input_fn input = [&history](const chunk_t &c, std::uint64_t lag) -> std::optional<std::array<double, 2>> {
      const auto t0 = saturating_sub(c.t0_us, lag);
      const auto t1 = saturating_sub(c.t1_us, lag);
      if (history.stick_active(t0, t1)) {
        return std::nullopt;
      }
      const auto [x, y] = history.mouse_between(t0, t1);
      return std::array<double, 2> {static_cast<double>(x), static_cast<double>(y)};
    };
    auto fresh = best_fit(all, input, min_mouse_counts);
    if (mouse_locked_ && mouse_) {
      keep_unless_better(*mouse_, fresh, [&](const fit_t &m, std::optional<std::array<double, 2>> gain) {
        return explained(all, input, m.lag_us, gain, min_mouse_counts);
      });
    }
    // Recent input under the model in force (or the fresh fit) decides an unlock.
    std::optional<fit_t> current;
    if (mouse_locked_) {
      current = mouse_;
    } else {
      current = fresh ? fresh : mouse_;
    }
    std::optional<fit_t> recent_fit;
    if (current) {
      recent_fit = score(recent, input, current->lag_us, current->gain, min_mouse_counts);
    }
    if (fresh) {
      if (fresh->fit >= lock_fit) {
        mouse_ = fresh;
        mouse_locked_ = true;
      } else if (!mouse_locked_) {
        mouse_ = fresh;
      }
    }
    // A prior locks on recent confirmation alone.
    if (!mouse_locked_ && prior_ && recent_fit && recent_fit->fit >= lock_fit) {
      mouse_locked_ = true;
      if (mouse_) {
        mouse_->fit = recent_fit->fit;
      }
    }
    if (mouse_locked_ && recent_fit && recent_fit->fit < unlock_fit) {
      mouse_locked_ = false;
    }
  }

  void calibrator_t::update_stick(const history_t &history, const std::vector<chunk_t> &all, const std::vector<chunk_t> &recent) {
    // Each curve's running integral is built once per fit (every chunk at every lag reads it with
    // two binary searches, instead of walking the chunk's pad packets).
    const auto input = [&history](const stick_integrals_t &curve, const chunk_t &c, std::uint64_t lag) -> std::optional<std::array<double, 2>> {
      const auto t0 = saturating_sub(c.t0_us, lag);
      const auto t1 = saturating_sub(c.t1_us, lag);
      const auto [mx, my] = history.mouse_between(t0, t1);
      if (mx != 0 || my != 0) {
        return std::nullopt;
      }
      return curve.between(t0, t1);
    };
    const auto input_of = [&input](const stick_integrals_t &curve) -> input_fn {
      return [&input, &curve](const chunk_t &c, std::uint64_t lag) {
        return input(curve, c, lag);
      };
    };
    // The lag with a middling curve, then the curve at that lag.
    const auto middling = history.stick_integrals(0.15f, 1.0f);
    auto fresh = best_fit(all, input_of(middling), min_stick_seconds);
    if (fresh) {
      fresh->deadzone = 0.15f;
      fresh->exponent = 1.0f;
    }
    if (fresh) {
      const auto found = *fresh;
      for (const float deadzone : deadzones) {
        for (const float exponent : {1.0f, 1.5f, 2.0f}) {
          std::optional<stick_integrals_t> own;
          const stick_integrals_t *curve = &middling;
          if (!(deadzone == 0.15f && exponent == 1.0f)) {
            own = history.stick_integrals(deadzone, exponent);
            curve = &*own;
          }
          auto fit = score(all, input_of(*curve), found.lag_us, std::nullopt, min_stick_seconds);
          if (fit && fit->fit > (fresh ? fresh->fit : 0.0)) {
            fit->deadzone = deadzone;
            fit->exponent = exponent;
            fresh = fit;
          }
        }
      }
      // The lag again, with the curve found.
      if (fresh) {
        const auto curve = *fresh;
        const auto integrals = history.stick_integrals(curve.deadzone, curve.exponent);
        auto refit = best_fit(all, input_of(integrals), min_stick_seconds);
        if (refit) {
          refit->deadzone = curve.deadzone;
          refit->exponent = curve.exponent;
          fresh = refit;
        }
      }
    }
    if (stick_locked_ && stick_) {
      const auto integrals = history.stick_integrals(stick_->deadzone, stick_->exponent);
      keep_unless_better(*stick_, fresh, [&](const fit_t &m, std::optional<std::array<double, 2>> gain) {
        return explained(all, input_of(integrals), m.lag_us, gain, min_stick_seconds);
      });
    }
    std::optional<fit_t> current;
    if (stick_locked_) {
      current = stick_;
    } else {
      current = fresh ? fresh : stick_;
    }
    std::optional<fit_t> recent_fit;
    if (current) {
      const auto integrals = history.stick_integrals(current->deadzone, current->exponent);
      recent_fit = score(recent, input_of(integrals), current->lag_us, current->gain, min_stick_seconds);
    }
    if (fresh) {
      if (fresh->fit >= lock_fit) {
        stick_ = fresh;
        stick_locked_ = true;
      } else if (!stick_locked_) {
        stick_ = fresh;
      }
    }
    if (!stick_locked_ && prior_ && recent_fit && recent_fit->fit >= lock_fit) {
      stick_locked_ = true;
      if (stick_) {
        stick_->fit = recent_fit->fit;
      }
    }
    if (stick_locked_ && recent_fit && recent_fit->fit < unlock_fit) {
      stick_locked_ = false;
    }
  }

  std::vector<chunk_t> calibrator_t::chunks(std::optional<std::uint64_t> since) const {
    std::vector<chunk_t> out;
    std::optional<chunk_t> open;
    for (const auto &s : samples_) {
      if (since && s.t0_us < *since) {
        continue;
      }
      const auto turn = resolve(s.motion);
      const bool usable = turn.confidence >= min_confidence;
      if (open && usable && open->t1_us == s.t0_us) {
        open->t1_us = s.t1_us;
        open->dx += static_cast<double>(turn.dx);
        open->dy += static_cast<double>(turn.dy);
      } else if (usable) {
        chunk_t chunk;
        chunk.t0_us = s.t0_us;
        chunk.t1_us = s.t1_us;
        chunk.dx = static_cast<double>(turn.dx);
        chunk.dy = static_cast<double>(turn.dy);
        open = chunk;
      } else {
        open.reset();
      }
      if (open && open->t1_us - open->t0_us >= chunk_us) {
        out.push_back(*open);
        open.reset();
      }
    }
    return out;
  }

  std::optional<model_t> calibrator_t::take_model() {
    const auto next = model(static_cast<std::uint16_t>(generation_ + 1));
    bool changed = false;
    if (!sent_) {
      changed = next.locked();
    } else {
      const auto &sent = *sent_;
      const auto moved = [](const std::array<float, 2> &a, const std::array<float, 2> &b) {
        for (std::size_t i = 0; i < 2; ++i) {
          const float scale = std::max({std::abs(a[i]), std::abs(b[i]), 1e-6f});
          if (std::abs(a[i] - b[i]) / scale > gain_change) {
            return true;
          }
        }
        return false;
      };
      const auto lag_moved = (sent.lag_us > next.lag_us ? sent.lag_us - next.lag_us : next.lag_us - sent.lag_us) > lag_change_us;
      changed = sent.mouse_locked != next.mouse_locked
                || sent.stick_locked != next.stick_locked
                || (next.mouse_locked && moved(sent.mouse_gain, next.mouse_gain))
                || (next.stick_locked && moved(sent.stick_gain, next.stick_gain))
                || (next.stick_locked && (sent.stick_deadzone != next.stick_deadzone || sent.stick_exponent != next.stick_exponent))
                || (next.locked() && lag_moved)
                || (next.locked() && std::abs(sent.focal - next.focal) > focal_change * std::max(sent.focal, next.focal));
    }
    if (!changed) {
      return std::nullopt;
    }
    generation_ = next.generation;
    sent_ = next;
    return next;
  }

  model_t calibrator_t::model(std::uint16_t generation) const {
    fit_t mouse;
    if (mouse_) {
      mouse = *mouse_;
    } else {
      mouse.gain = {0.0, 0.0};
      mouse.lag_us = default_lag_us;
      mouse.fit = 0.0;
      mouse.deadzone = 0.0f;
      mouse.exponent = 1.0f;
    }
    fit_t stick;
    if (stick_) {
      stick = *stick_;
    } else {
      stick.gain = {0.0, 0.0};
      stick.lag_us = mouse.lag_us;
      stick.fit = 0.0;
      stick.deadzone = 0.15f;
      stick.exponent = 1.0f;
    }
    double confidence = 0.0;
    if (mouse_locked_ && stick_locked_) {
      confidence = std::max(mouse.fit, stick.fit);
    } else if (mouse_locked_) {
      confidence = mouse.fit;
    } else if (stick_locked_) {
      confidence = stick.fit;
    } else {
      confidence = std::max(mouse.fit, stick.fit);
    }
    // In picture pixels, which the game's part of the picture shares.
    std::optional<double> focal;
    if (lens_ && lens_drawn_) {
      const double f = layout_.focal(*lens_);
      if (f >= static_cast<double>(min_focal) && f <= static_cast<double>(max_focal)) {
        focal = f;
      }
    }
    const auto lens_limit = [&](std::uint16_t edge) {
      if (!focal) {
        return std::numeric_limits<float>::infinity();
      }
      return static_cast<float>(max_lens_offset * *focal * *focal / static_cast<double>(std::max<std::uint16_t>(edge, 1)));
    };
    const auto clamp_gain = [](std::array<float, 2> g) {
      for (auto &v : g) {
        v = std::isfinite(v) ? std::clamp(v, -max_gain, max_gain) : 0.0f;
      }
      return g;
    };
    model_t out;
    out.generation = generation;
    out.mouse_locked = mouse_locked_;
    out.stick_locked = stick_locked_;
    out.picture_width = picture_.first;
    out.picture_height = picture_.second;
    out.mouse_gain = clamp_gain(to_picture(mouse.gain, scale_));
    out.stick_gain = clamp_gain(to_picture(stick.gain, scale_));
    out.stick_deadzone = std::clamp(stick.deadzone, 0.0f, max_stick_deadzone);
    out.stick_exponent = std::find(stick_exponents.begin(), stick_exponents.end(), stick.exponent) != stick_exponents.end() ? stick.exponent : 1.0f;
    out.max_shift = {
      std::min(static_cast<float>(picture_.first) * max_offset_share, lens_limit(picture_.first)),
      std::min(static_cast<float>(picture_.second) * max_offset_share, lens_limit(picture_.second)),
    };
    out.lag_us = static_cast<std::uint32_t>(mouse_locked_ || !stick_locked_ ? mouse.lag_us : stick.lag_us);
    out.confidence = std::clamp(static_cast<float>(confidence), 0.0f, 1.0f);
    out.focal = focal ? static_cast<float>(*focal) : 0.0f;
    return out;
  }

  void to_json(nlohmann::json &j, const prior_t &prior) {
    j = nlohmann::json::object();
    const auto put = [&j](const char *key, const auto &value) {
      if (value) {
        j[key] = *value;
      } else {
        j[key] = nullptr;
      }
    };
    put("mouse_gain", prior.mouse_gain);
    put("mouse_lag_us", prior.mouse_lag_us);
    put("stick_gain", prior.stick_gain);
    put("stick_lag_us", prior.stick_lag_us);
    put("stick_deadzone", prior.stick_deadzone);
    put("stick_exponent", prior.stick_exponent);
    put("focal", prior.focal);
    j["picture_width"] = prior.picture_width;
    j["picture_height"] = prior.picture_height;
  }

  void from_json(const nlohmann::json &j, prior_t &prior) {
    // `at` throws for anything that is not an object holding the key.
    const auto width = j.at("picture_width").get<std::uint16_t>();
    const auto height = j.at("picture_height").get<std::uint16_t>();
    const auto read = [&j](const char *key, auto &out) {
      using value_t = typename std::remove_reference_t<decltype(out)>::value_type;
      out.reset();
      const auto it = j.find(key);
      if (it != j.end() && !it->is_null()) {
        out = it->template get<value_t>();
      }
    };
    prior_t out;
    read("mouse_gain", out.mouse_gain);
    read("mouse_lag_us", out.mouse_lag_us);
    read("stick_gain", out.stick_gain);
    read("stick_lag_us", out.stick_lag_us);
    read("stick_deadzone", out.stick_deadzone);
    read("stick_exponent", out.stick_exponent);
    read("focal", out.focal);
    out.picture_width = width;
    out.picture_height = height;
    prior = out;
  }

}  // namespace frame_warp
