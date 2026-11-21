/**
 * @file src/frame_warp_session.h
 * @brief One stream's Frame Warp: the learner behind a lock, its worker and what a game taught.
 *
 * A session is made for every stream the host offered Frame Warp to, and does nothing until the
 * client's request with `enabled` 1 arrives: no thread, no GPU resource, no message. After that
 *
 *   - the control thread hands it the client's request and each relative mouse move and
 *     controller packet as it receives them (`on_request`, `on_mouse`, `on_controller`);
 *   - the video side hands it the stream's size (`set_picture`), asks what the GPU passes should
 *     make (`params`) and gives it each capture's thumbnail and the HUD bits, which a worker
 *     thread measures off the frame path (`submit_thumbnail`, `submit_hud`), and tags each frame
 *     as it goes out (`tag`);
 *   - the control loop collects what is to be sent (`take`), always on its own thread: ENet is
 *     not used from anywhere else.
 *
 * What a game taught the host is kept in `frame_warp.json` (key `uuid:<app uuid>`, else
 * `name:<app name>`, the 256 most recent games) and starts the next stream of that game.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// local includes
#include "frame_warp.h"
#include "frame_warp_input.h"
#include "frame_warp_types.h"
#include "frame_warp_wire.h"

namespace frame_warp {

  /// What the GPU passes should make for the next capture.
  struct gpu_params_t {
    /// False until a client asked, and after it stopped: the passes are not run.
    bool enabled = false;
    std::size_t thumbnail_width = 0;
    std::size_t thumbnail_height = 0;
    /// The width of the HUD mask to read back, 0 for none.
    std::uint16_t mask_width = 0;
    /// The camera turned recently: the HUD pass learns only then.
    bool moving = false;
    /// While it turns, how far apart the captures are that the HUD pass compares (microseconds), 0 for each against the last.
    std::uint64_t hud_every_us = 0;
    /// The picture the HUD cells cover (the capture's part of the stream: the learner places the
    /// cells on the encoded picture), and `content` the whole of it.
    std::uint16_t picture_width = 0;
    std::uint16_t picture_height = 0;
    rect_t content;
  };

  /// What the control loop sends for a session, in this order of importance.
  struct outbox_t {
    /// A model, reliable.
    std::optional<std::array<std::uint8_t, model_bytes>> model;
    /// A HUD mask, reliable.
    std::optional<std::vector<std::uint8_t>> mask;
    /// Tags messages, oldest first, unreliable and unsequenced.
    std::vector<std::vector<std::uint8_t>> tags;

    bool empty() const {
      return !model && !mask && tags.empty();
    }
  };

  /// Reads and writes `frame_warp.json`; one file for every stream, saves take turns.
  class store_t {
  public:
    store_t(std::filesystem::path path, std::string key);

    std::optional<prior_t> load() const;
    void save(const prior_t &prior) const;

    /// The key of a game: `uuid:<uuid>`, else `name:<name>`, empty for neither.
    static std::string key_for(std::string_view uuid, std::string_view name);

    /// Games kept; the least recently used goes first.
    static constexpr std::size_t max_games = 256;
    static constexpr std::string_view file_name = "frame_warp.json";

  private:
    std::filesystem::path path_;
    std::string key_;
  };

  class session_t final: public input_sink_t {
  public:
    using clock = std::chrono::steady_clock;

    /**
     * @param store What a game taught the host before, and where this stream's lesson goes; an
     *        empty key remembers nothing.
     */
    explicit session_t(store_t store);
    ~session_t();

    session_t(const session_t &) = delete;
    session_t &operator=(const session_t &) = delete;

    /// Microseconds on the session clock (monotonic, from the process's first use).
    static std::uint64_t us(clock::time_point at);
    static std::uint64_t now_us();

    // ---- Control thread ----

    /**
     * @brief Apply a client's request (the 0x5543 payload).
     * @return Whether Frame Warp is on now, none for a malformed request, which changes nothing.
     *         A request that turns it on queues the model and the last mask for `take`.
     */
    std::optional<bool> on_request(std::span<const std::uint8_t> payload);

    /// One relative mouse move as it was received and accepted. Cheap while Frame Warp is off.
    void on_mouse(std::int16_t dx, std::int16_t dy) override;

    /// One controller packet of controller `number`. Cheap while Frame Warp is off.
    void on_controller(std::uint16_t number, std::int16_t right_stick_x, std::int16_t right_stick_y) override;

    /// A client asked: the passes and tags run. One atomic load.
    bool enabled() const {
      return enabled_.load(std::memory_order_acquire);
    }

    bool active() const override {
      return enabled();
    }

    /// Called from the worker when the model locks or unlocks, with a line for the log. Set before use.
    void set_observer(std::function<void(std::string)> observer);

    /// Something is waiting for `take`. One atomic load.
    bool pending() const {
      return pending_.load(std::memory_order_acquire);
    }

    /// What is to be sent now; the control thread alone sends it.
    outbox_t take();

    // ---- Video side ----

    /**
     * @brief Whether this stream's frames can be measured at all (the encoder path has the GPU
     *        passes). A stream that cannot is answered with an unlocked model and never tagged.
     */
    void set_measurable(bool measurable);
    bool measurable() const {
      return measurable_.load(std::memory_order_acquire);
    }

    /// The encoded picture's size and where the capture shows inside it; cheap when unchanged.
    void set_picture(std::uint16_t width, std::uint16_t height, rect_t content);

    gpu_params_t params() const;

    /// A finished GPU readback. The worker measures it; a full queue drops it.
    void submit_thumbnail(thumbnail_t thumbnail);
    void submit_hud(mask_t mask);

    /// Tag frame `frame`, captured at `capture`. Does nothing for a stream not measured or off.
    void tag(std::uint32_t frame, clock::time_point capture);

    /// The learner's state, for logs and tests.
    status_t status() const;

  private:
    struct work_t {
      std::variant<thumbnail_t, mask_t> item;
    };

    void start_worker();
    void stop_worker();
    void worker_loop();
    /// Moves what the learner has to send into `outbox_`. Call with `mutex_` held.
    void drain_locked();

    store_t store_;

    mutable std::mutex mutex_;
    host_t host_;
    std::atomic_bool enabled_ {false};
    std::atomic_bool pending_ {false};
    std::atomic_bool measurable_ {true};
    /// What the learner has to send, until `take`. Tags beyond `pending_tags` drop the oldest.
    outbox_t outbox_;
    std::function<void(std::string)> observer_;
    /// A hash of the picture size and content rectangle last given to `set_picture`, to skip the lock.
    std::atomic<std::uint64_t> picture_key_ {0};
    std::uint16_t picture_width_ = 0;
    std::uint16_t picture_height_ = 0;
    rect_t content_;

    std::mutex work_mutex_;
    std::condition_variable work_cv_;
    std::deque<work_t> work_;
    bool worker_stop_ = false;
    std::thread worker_;
  };

}  // namespace frame_warp
