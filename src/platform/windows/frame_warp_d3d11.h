/**
 * @file src/platform/windows/frame_warp_d3d11.h
 * @brief The Direct3D 11 passes behind Frame Warp: a luma thumbnail of each capture and the
 *        HUD mask of the pixels that stay put while the camera turns.
 *
 * frame_warp_cs.hlsl runs on the device context the conversion used, inside the conversion's
 * keyed-mutex hold, and each pass's readout is copied into one of a ring of staging buffers.
 * poll() maps only finished buffers (D3D11_MAP_FLAG_DO_NOT_WAIT), so nothing on the
 * capture-to-send path ever waits for a pass: a thumbnail that is late arrives with a later
 * frame, and the learner works from the capture times it carries.
 *
 * The passes follow RamazanKara/Rubylight rust/windows/src/compute_frame_warp.rs and its
 * shaders/color.hlsl (commits aa1d312bf and 79921e3b9), on a D3D11 immediate context instead
 * of a D3D12 compute queue.
 *
 * This file has no Sunshine dependencies besides src/frame_warp_types.h, src/frame_warp_wire.h
 * and, for the session class, src/frame_warp_session.h, so a standalone test can drive the
 * pass class. src/platform/windows/display_vram.cpp wires the session class to the encode
 * devices.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// platform includes
#include <d3d11.h>
#include <wrl/client.h>

// local includes
#include "src/frame_warp_session.h"
#include "src/frame_warp_types.h"
#include "src/frame_warp_wire.h"

namespace platf::dxgi {
  class frame_warp_d3d11_t {
  public:
    /// The capture's values, as frame_warp_cs.hlsl numbers them.
    enum class transfer_e : std::uint32_t {
      encoded = 0,  ///< UNORM display-referred values (sRGB, 8 bits a channel): luma straight from them.
      scrgb = 1,  ///< FP16 linear scRGB: taken as PQ of scRGB / 125, as Rubylight does.
      pq = 2,  ///< UNORM PQ codes (HDR10, 10 bits a channel): luma with the Rec. 2020 weights.
    };

    /// The capture a pass reads, as the conversion read it: the whole texture, before any scaling.
    struct source_t {
      ID3D11ShaderResourceView *view = nullptr;
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      transfer_e transfer = transfer_e::encoded;
    };

    /// The HUD work while the camera turns.
    struct hud_t {
      /// The mask cells across and down, over the stream picture (hud_grid()).
      std::uint32_t grid_width = 0;
      std::uint32_t grid_height = 0;
      /// Copy the mask bits back for poll(); ignored while one copy is still in flight.
      bool read = false;
      /// How far apart, in the `at_us` of queue(), the captures are at least that the pass
      /// compares: a capture sooner than this after the one the codes are from runs no HUD
      /// pass, so that a slow turn at a high frame rate still moves the scene far enough
      /// between the two for its pixels to change. 0 compares each capture with the one before.
      std::uint64_t every_us = 0;
    };

    struct pass_t {
      /// The thumbnail's width, a multiple of 4, and height, each at most `thumbnail_edge`.
      std::uint32_t thumbnail_width = 0;
      std::uint32_t thumbnail_height = 0;
      std::optional<hud_t> hud;
      /// The stream's encoded picture, which the mask covers, and where the capture shows in
      /// it (the rest is black bars, which count as HUD). 0 means the picture is the capture.
      std::uint32_t picture_width = 0;
      std::uint32_t picture_height = 0;
      frame_warp::rect_t content;
    };

    struct readout_t {
      /// Finished thumbnails, oldest first.
      std::vector<frame_warp::thumbnail_t> thumbnails;
      /// The HUD mask, once its copy finished.
      std::optional<frame_warp::mask_t> mask;
    };

    /// The largest thumbnail edge (frame_warp::thumbnail_width by the tallest thumbnail_height).
    static constexpr std::uint32_t thumbnail_edge = 256;
    /// HUD cells across or down at most (frame_warp::max_mask_edge).
    static constexpr std::uint32_t max_grid_edge = 2048;
    /// The score from which a cell is HUD: it stayed through about 24 comparisons while the
    /// camera turned, and every change halves it.
    static constexpr std::uint32_t hud_score = 24;
    /// Readbacks in flight at most. A capture that finds them all busy goes unmeasured.
    static constexpr std::size_t ring_size = 4;

    /**
     * @brief Create the passes on `device` from frame_warp_cs.hlsl compiled for cs_5_0.
     */
    static std::expected<frame_warp_d3d11_t, std::string> create(ID3D11Device *device, const void *shader, std::size_t size);

    /**
     * @brief Why `pass` cannot run, none when it can.
     */
    static std::optional<std::string> check(const pass_t &pass);

    /**
     * @brief Record the passes over `source` and their readback copies on `context`. Nothing is
     *        submitted or waited for.
     * @param at_us The capture's time, handed back with its thumbnail.
     * @return False, with nothing queued, when every readback buffer is still in flight, the
     *         source or the pass is unusable, or a buffer could not be made (poll() then
     *         reports why).
     */
    bool queue(ID3D11DeviceContext *context, const source_t &source, const pass_t &pass, std::uint64_t at_us);

    /**
     * @brief Whether the last queue() that returned true started a mask copy.
     */
    bool mask_started() const {
      return started_mask;
    }

    /**
     * @brief The readouts that have finished since the last call. Never waits.
     * @return An error when a readback cannot be mapped or a buffer could not be made.
     */
    std::expected<readout_t, std::string> poll(ID3D11DeviceContext *context);

    /**
     * @brief Passes queued and not read back yet.
     */
    std::size_t in_flight() const;

  private:
    struct slot_t {
      Microsoft::WRL::ComPtr<ID3D11Buffer> thumbnail_staging;
      Microsoft::WRL::ComPtr<ID3D11Buffer> mask_staging;
      std::uint32_t mask_staging_bytes = 0;
      bool pending = false;
      std::uint64_t at_us = 0;
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      bool has_mask = false;
      std::uint32_t mask_width = 0;
      std::uint32_t mask_height = 0;
    };

    /// What the HUD buffers were made for: any change starts the scores over.
    struct hud_key_t {
      std::uint32_t source_width = 0;
      std::uint32_t source_height = 0;
      std::uint32_t grid_width = 0;
      std::uint32_t grid_height = 0;
      std::uint32_t picture_width = 0;
      std::uint32_t picture_height = 0;
      frame_warp::rect_t content;

      bool operator==(const hud_key_t &) const = default;
    };

    struct hud_buffers_t {
      hud_key_t key;
      Microsoft::WRL::ComPtr<ID3D11Buffer> codes;
      Microsoft::WRL::ComPtr<ID3D11Buffer> scores;
      Microsoft::WRL::ComPtr<ID3D11Buffer> bits;
      Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> codes_uav;
      Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> scores_uav;
      Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> bits_uav;
    };

    /// The HUD buffers for `key`; true when they are new, none when they could not be made.
    std::optional<bool> prepare_hud(const hud_key_t &key);
    bool mask_in_flight() const;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> params;
    Microsoft::WRL::ComPtr<ID3D11Buffer> thumbnail;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> thumbnail_uav;
    std::optional<hud_buffers_t> hud;
    std::array<slot_t, ring_size> ring;
    std::size_t next = 0;  ///< The slot the next pass writes; the oldest in flight when busy.
    /// The codes in the HUD buffer are a capture's that the next HUD pass compares with.
    bool primed = false;
    /// The `at_us` of the capture whose codes those are.
    std::uint64_t taken_at = 0;
    bool started_mask = false;
    std::optional<std::string> failure;
  };

  /**
   * @brief The HUD mask's cells over a picture: `mask_width` across (no wider than the picture
   *        or `frame_warp_d3d11_t::max_grid_edge`) and as many rows as keep the cells' shape the
   *        picture's. None for no mask (`mask_width` or a picture side 0).
   */
  std::optional<std::pair<std::uint32_t, std::uint32_t>> hud_grid(std::uint16_t mask_width, std::uint16_t picture_width, std::uint16_t picture_height);

  /**
   * @brief frame_warp_cs.hlsl at `path` (entry point main_cs), compiled for cs_5_0; kept for
   *        later streams.
   */
  std::expected<Microsoft::WRL::ComPtr<ID3DBlob>, std::string> compile_frame_warp_shader(const std::filesystem::path &path);

  /**
   * @brief One encode device's Frame Warp passes for the stream attached to it.
   * @details Like hdr_stats_session_t: the passes are recorded by converted() right after the
   *          conversion's draw, inside its keyed-mutex hold, and submitted() flushes, polls
   *          (never waits) and hands finished readbacks to the frame_warp::session_t.
   *
   *          Nothing is compiled or created, and a frame costs one atomic load, until the
   *          client asked for Frame Warp (`session->enabled()`); the passes are made at the
   *          first capture after that and their buffers go again when it stops. A failure
   *          then stops the stream's measuring, never its frames.
   */
  class frame_warp_session_t {
  public:
    explicit frame_warp_session_t(std::filesystem::path shader_path);

    /**
     * @brief Measure frames for `session` from now on on `device`; null detaches. Creates
     *        nothing: the passes are made by the first converted() after a client asked.
     * @return Why the session cannot be served (a null device); nothing is measured then.
     */
    std::optional<std::string> attach(ID3D11Device *device, std::shared_ptr<::frame_warp::session_t> session);

    /**
     * @brief How far apart the captures are that the HUD pass compares (frame_warp_d3d11_t::hud_t::every_us),
     *        from the learner's motion measurement; 0 compares each capture with the one before.
     */
    void set_hud_every_us(std::uint64_t every_us) {
      hud_every_us = every_us;
    }

    /**
     * @brief Call from the conversion, right after its draw read `source`, while it holds the
     *        capture's keyed mutex. Records the passes when Frame Warp is on and `captured` is
     *        newer than the last capture measured; never submits or waits.
     * @param captured When the capture was made; none when it is not known (measured, as now).
     */
    void converted(ID3D11DeviceContext *context, const frame_warp_d3d11_t::source_t &source, std::optional<std::chrono::steady_clock::time_point> captured);

    /**
     * @brief Call from a conversion whose source is not measured, such as a blank frame.
     */
    void converted_unmeasured();

    /**
     * @brief Call once the encoder took the last converted frame: flushes what converted()
     *        recorded and forwards the finished readbacks. Never waits.
     * @return Why measuring stopped for good, when it stopped now.
     */
    std::optional<std::string> submitted(ID3D11DeviceContext *context);

    /**
     * @brief A session is attached and measuring has not failed.
     */
    bool active() const;

    /**
     * @brief The passes exist: Frame Warp is on and a capture has been seen.
     */
    bool has_pass() const {
      return pass.has_value();
    }

    std::size_t in_flight() const {
      return pass ? pass->in_flight() : 0;
    }

    /// The HUD mask is read back at most this often.
    static constexpr std::chrono::milliseconds mask_every {250};

  private:
    std::string stop(std::string reason);

    std::filesystem::path shader_path;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    std::shared_ptr<::frame_warp::session_t> session;
    std::optional<frame_warp_d3d11_t> pass;
    bool failed = false;
    /// Why the passes could not be made, reported once by submitted().
    std::optional<std::string> failure;
    /// Something was recorded that submitted() has not flushed.
    bool recorded = false;
    std::optional<std::chrono::steady_clock::time_point> last_measured;
    std::optional<std::chrono::steady_clock::time_point> mask_due;
    std::uint64_t hud_every_us = 0;
  };
}  // namespace platf::dxgi
