/**
 * @file src/frame_warp_session.cpp
 * @brief One stream's Frame Warp, see frame_warp_session.h.
 */
// this include
#include "frame_warp_session.h"

// standard includes
#include <algorithm>
#include <fstream>
#include <system_error>

// lib includes
#include <nlohmann/json.hpp>

namespace frame_warp {

  namespace {
    /// Readbacks waiting for the worker; past it they are dropped (a thumbnail costs its sample).
    constexpr std::size_t max_queued_work = 8;

    /// One file for every stream: saves take turns.
    std::mutex &save_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    session_t::clock::time_point epoch() {
      static const auto first_use = session_t::clock::now();
      return first_use;
    }

    /// The file's content, or nothing learned when it is missing or unreadable.
    nlohmann::json read_file(const std::filesystem::path &path) {
      std::ifstream in {path, std::ios::binary};
      if (!in) {
        return nlohmann::json::object();
      }
      auto parsed = nlohmann::json::parse(in, nullptr, false);
      if (!parsed.is_object()) {
        return nlohmann::json::object();
      }
      return parsed;
    }
  }  // namespace

  store_t::store_t(std::filesystem::path path, std::string key):
      path_ {std::move(path)},
      key_ {std::move(key)} {
  }

  std::string store_t::key_for(std::string_view uuid, std::string_view name) {
    if (!uuid.empty()) {
      return "uuid:" + std::string {uuid};
    }
    if (!name.empty()) {
      return "name:" + std::string {name};
    }
    return {};
  }

  std::optional<prior_t> store_t::load() const {
    if (key_.empty()) {
      return std::nullopt;
    }
    std::lock_guard lock {save_mutex()};
    const auto file = read_file(path_);
    try {
      const auto games = file.find("games");
      if (games == file.end() || !games->is_object()) {
        return std::nullopt;
      }
      const auto game = games->find(key_);
      if (game == games->end() || !game->is_object() || !game->contains("prior")) {
        return std::nullopt;
      }
      return game->at("prior").get<prior_t>();
    } catch (const nlohmann::json::exception &) {
      return std::nullopt;
    }
  }

  void store_t::save(const prior_t &prior) const {
    if (key_.empty()) {
      return;
    }
    std::lock_guard lock {save_mutex()};
    auto file = read_file(path_);
    if (!file.contains("games") || !file["games"].is_object()) {
      file["games"] = nlohmann::json::object();
    }
    auto &games = file["games"];
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    games[key_] = {{"prior", prior}, {"used", now}};

    // The least recently used games go first.
    while (games.size() > max_games) {
      auto oldest = games.begin();
      for (auto it = games.begin(); it != games.end(); ++it) {
        const auto used = [](const nlohmann::json &game) {
          return game.is_object() && game.contains("used") && game["used"].is_number_unsigned() ? game["used"].get<std::uint64_t>() : std::uint64_t {0};
        };
        if (used(it.value()) < used(oldest.value())) {
          oldest = it;
        }
      }
      games.erase(oldest);
    }

    std::error_code ec;
    if (path_.has_parent_path()) {
      std::filesystem::create_directories(path_.parent_path(), ec);
    }
    auto temporary = path_;
    temporary += ".tmp";
    {
      std::ofstream out {temporary, std::ios::binary | std::ios::trunc};
      if (!out) {
        return;
      }
      out << file.dump(2);
      if (!out) {
        return;
      }
    }
    std::filesystem::rename(temporary, path_, ec);
    if (ec) {
      std::filesystem::remove(temporary, ec);
    }
  }

  session_t::session_t(store_t store):
      store_ {std::move(store)} {
    (void) epoch();
  }

  session_t::~session_t() {
    stop_worker();
  }

  std::uint64_t session_t::us(clock::time_point at) {
    const auto since = at - epoch();
    // Before the epoch (a capture older than the first use) counts as 0.
    return since.count() < 0 ? 0 : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(since).count());
  }

  std::uint64_t session_t::now_us() {
    return us(clock::now());
  }

  void session_t::set_observer(std::function<void(std::string)> observer) {
    std::lock_guard lock {mutex_};
    observer_ = std::move(observer);
  }

  void session_t::drain_locked() {
    if (auto model = host_.take_model()) {
      outbox_.model = *model;
    }
    if (auto mask = host_.take_mask()) {
      outbox_.mask = std::move(*mask);
    }
    while (auto tags = host_.take_tags()) {
      if (outbox_.tags.size() >= pending_tags) {
        outbox_.tags.erase(outbox_.tags.begin());
      }
      outbox_.tags.push_back(std::move(*tags));
    }
    pending_.store(!outbox_.empty(), std::memory_order_release);
  }

  std::optional<bool> session_t::on_request(std::span<const std::uint8_t> payload) {
    std::optional<bool> on;
    bool starting = false;
    {
      std::lock_guard lock {mutex_};
      const bool was = enabled_.load(std::memory_order_relaxed);
      on = host_.on_request(payload);
      if (!on) {
        return std::nullopt;
      }
      starting = *on && !was;
      if (!*on) {
        // A stop ends the tags and the model: nothing more is sent for this client.
        outbox_ = {};
      }
      enabled_.store(*on, std::memory_order_release);
      drain_locked();
    }
    if (starting) {
      start_worker();
    }
    return on;
  }

  void session_t::on_mouse(std::int16_t dx, std::int16_t dy) {
    if (!enabled()) {
      return;
    }
    const auto at = now_us();
    std::lock_guard lock {mutex_};
    host_.on_mouse(dx, dy, at);
  }

  void session_t::on_controller(std::uint16_t number, std::int16_t right_stick_x, std::int16_t right_stick_y) {
    if (!enabled()) {
      return;
    }
    const auto at = now_us();
    std::lock_guard lock {mutex_};
    host_.on_controller(number, right_stick_x, right_stick_y, at);
  }

  outbox_t session_t::take() {
    std::lock_guard lock {mutex_};
    drain_locked();
    auto out = std::move(outbox_);
    outbox_ = {};
    pending_.store(false, std::memory_order_release);
    return out;
  }

  void session_t::set_measurable(bool measurable) {
    measurable_.store(measurable, std::memory_order_release);
  }

  void session_t::set_picture(std::uint16_t width, std::uint16_t height, rect_t content) {
    std::uint64_t key = 1469598103934665603ull;
    for (const std::uint64_t part : {width, height, content.x, content.y, content.width, content.height}) {
      key = (key ^ part) * 1099511628211ull;
    }
    if (picture_key_.load(std::memory_order_acquire) == key) {
      return;
    }
    std::lock_guard lock {mutex_};
    picture_width_ = width;
    picture_height_ = height;
    content_ = content;
    host_.set_picture(width, height, content);
    picture_key_.store(key, std::memory_order_release);
    if (enabled_.load(std::memory_order_relaxed)) {
      drain_locked();
    }
  }

  gpu_params_t session_t::params() const {
    if (!enabled()) {
      return {};
    }
    const auto now = now_us();
    std::lock_guard lock {mutex_};
    gpu_params_t params;
    params.enabled = enabled_.load(std::memory_order_relaxed);
    const auto thumbnail = host_.thumbnail_size();
    params.thumbnail_width = thumbnail.first;
    params.thumbnail_height = thumbnail.second;
    params.mask_width = host_.mask_width();
    params.moving = host_.camera_moving(now);
    params.hud_every_us = host_.hud_every_us(now).value_or(0);
    // The learner lays the GPU's HUD cells over the picture itself (placement_t::place): the passes
    // see the capture alone, as the picture the cells cover, and the bars of a letterboxed stream
    // are added when the mask is placed.
    const bool has_content = content_.width > 0 && content_.height > 0;
    params.picture_width = has_content ? content_.width : picture_width_;
    params.picture_height = has_content ? content_.height : picture_height_;
    params.content = rect_t {0, 0, params.picture_width, params.picture_height};
    return params;
  }

  void session_t::submit_thumbnail(thumbnail_t thumbnail) {
    {
      std::lock_guard lock {work_mutex_};
      if (worker_stop_ || work_.size() >= max_queued_work) {
        return;
      }
      work_.push_back({std::move(thumbnail)});
    }
    work_cv_.notify_one();
  }

  void session_t::submit_hud(mask_t mask) {
    {
      std::lock_guard lock {work_mutex_};
      if (worker_stop_ || work_.size() >= max_queued_work) {
        return;
      }
      work_.push_back({std::move(mask)});
    }
    work_cv_.notify_one();
  }

  void session_t::tag(std::uint32_t frame, clock::time_point capture) {
    if (!enabled() || !measurable()) {
      return;
    }
    const auto at = us(capture);
    std::lock_guard lock {mutex_};
    host_.tag(frame, at);
    drain_locked();
  }

  status_t session_t::status() const {
    std::lock_guard lock {mutex_};
    return host_.status();
  }

  void session_t::start_worker() {
    std::lock_guard lock {work_mutex_};
    if (worker_.joinable() || worker_stop_) {
      return;
    }
    worker_ = std::thread {[this] {
      worker_loop();
    }};
  }

  void session_t::stop_worker() {
    {
      std::lock_guard lock {work_mutex_};
      worker_stop_ = true;
    }
    work_cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void session_t::worker_loop() {
    // What this game taught the host before starts the learner; the file is read off the control thread.
    if (const auto prior = store_.load()) {
      std::function<void(std::string)> observer;
      {
        std::lock_guard lock {mutex_};
        host_.set_prior(*prior);
        observer = observer_;
      }
      if (observer) {
        observer("Frame Warp starts from what it learned for this game before");
      }
    }

    bool locked = false;
    bool saved = false;
    while (true) {
      work_t work;
      {
        std::unique_lock lock {work_mutex_};
        work_cv_.wait(lock, [this] {
          return worker_stop_ || !work_.empty();
        });
        if (work_.empty()) {
          break;  // stopped and drained
        }
        work = std::move(work_.front());
        work_.pop_front();
      }

      const auto now = now_us();
      std::optional<prior_t> to_save;
      std::string note;
      std::function<void(std::string)> observer;
      {
        std::lock_guard lock {mutex_};
        if (auto *thumbnail = std::get_if<thumbnail_t>(&work.item)) {
          host_.on_thumbnail(std::move(*thumbnail), now);
        } else if (auto *mask = std::get_if<mask_t>(&work.item)) {
          host_.on_hud(std::move(*mask), now);
        }
        drain_locked();

        const auto status = host_.status();
        const bool now_locked = status.model && status.model->locked();
        if (now_locked != locked) {
          locked = now_locked;
          observer = observer_;
          if (locked) {
            const auto &model = *status.model;
            note = "Frame Warp model locked: mouse " + std::to_string(model.mouse_locked) + ", stick " + std::to_string(model.stick_locked) +
                   ", lag " + std::to_string(model.lag_us / 1000.0) + " ms, fit " + std::to_string(model.confidence);
            if (!saved) {
              to_save = host_.prior();
            }
          } else {
            note = "Frame Warp model unlocked";
          }
        }
      }
      if (observer && !note.empty()) {
        observer(note);
      }
      if (to_save) {
        store_.save(*to_save);
        saved = true;
      }
    }

    std::optional<prior_t> final_prior;
    {
      std::lock_guard lock {mutex_};
      final_prior = host_.prior();
    }
    if (final_prior) {
      store_.save(*final_prior);
    }
  }

}  // namespace frame_warp
