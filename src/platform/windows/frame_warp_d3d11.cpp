/**
 * @file src/platform/windows/frame_warp_d3d11.cpp
 * @brief The Direct3D 11 passes behind Frame Warp.
 */
// standard includes
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

// platform includes
#include <d3dcompiler.h>

// local includes
#include "frame_warp_d3d11.h"

namespace platf::dxgi {
  namespace {
    /// frame_warp_params_cbuffer in frame_warp_cs.hlsl.
    struct alignas(16) params_t {
      std::uint32_t source[2];
      std::uint32_t thumbnail[2];
      std::uint32_t grid[2];
      std::uint32_t picture[2];
      std::uint32_t content[4];
      std::uint32_t pixel;
      std::uint32_t pass;
      std::uint32_t stride;
      std::uint32_t threshold;
    };

    static_assert(sizeof(params_t) % 16 == 0, "Constant buffers are sized in 16-byte units");

    /// The `fw_pass` values of frame_warp_cs.hlsl.
    enum class pass_e : std::uint32_t {
      thumbnail = 0,
      score = 1,
      codes = 2,
      bits = 3,
    };

    /// The threads of one group along each side.
    constexpr std::uint32_t group_size = 8;

    constexpr std::uint32_t thumbnail_bytes = frame_warp_d3d11_t::thumbnail_edge * frame_warp_d3d11_t::thumbnail_edge;

    /// The largest HUD buffer made, in bytes.
    constexpr std::uint64_t max_buffer_bytes = 0x7fffffffull;

    std::string failed(const char *what, HRESULT status) {
      char code[16];
      std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(status));
      return std::string {what} + " [" + code + ']';
    }

    std::uint32_t groups(std::uint32_t count) {
      return (count + group_size - 1) / group_size;
    }

    HRESULT make_words(ID3D11Device *device, std::uint64_t words, ID3D11Buffer **buffer, ID3D11UnorderedAccessView **view) {
      D3D11_BUFFER_DESC desc {};
      desc.ByteWidth = static_cast<UINT>(words * sizeof(std::uint32_t));
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      desc.StructureByteStride = sizeof(std::uint32_t);
      auto status = device->CreateBuffer(&desc, nullptr, buffer);
      if (FAILED(status)) {
        return status;
      }

      D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc {};
      view_desc.Format = DXGI_FORMAT_UNKNOWN;
      view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      view_desc.Buffer.FirstElement = 0;
      view_desc.Buffer.NumElements = static_cast<UINT>(words);
      return device->CreateUnorderedAccessView(*buffer, &view_desc, view);
    }

    HRESULT make_staging(ID3D11Device *device, std::uint32_t bytes, ID3D11Buffer **buffer) {
      D3D11_BUFFER_DESC desc {};
      desc.ByteWidth = bytes;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      return device->CreateBuffer(&desc, nullptr, buffer);
    }

    /// The first `bytes` of `from` into `to`.
    void copy_bytes(ID3D11DeviceContext *context, ID3D11Buffer *to, ID3D11Buffer *from, std::uint32_t bytes) {
      const D3D11_BOX box {0, 0, 0, bytes, 1, 1};
      context->CopySubresourceRegion(to, 0, 0, 0, 0, from, 0, &box);
    }
  }  // namespace

  std::optional<std::pair<std::uint32_t, std::uint32_t>> hud_grid(std::uint16_t mask_width, std::uint16_t picture_width, std::uint16_t picture_height) {
    if (mask_width == 0 || picture_width == 0 || picture_height == 0) {
      return std::nullopt;
    }
    const std::uint32_t across = std::min<std::uint32_t>({mask_width, picture_width, frame_warp_d3d11_t::max_grid_edge});
    const std::uint64_t down = (static_cast<std::uint64_t>(across) * picture_height + picture_width / 2u) / picture_width;
    return std::pair {across, std::clamp<std::uint32_t>(static_cast<std::uint32_t>(down), 1, std::min<std::uint32_t>(picture_height, frame_warp_d3d11_t::max_grid_edge))};
  }

  std::expected<frame_warp_d3d11_t, std::string> frame_warp_d3d11_t::create(ID3D11Device *device, const void *shader_code, std::size_t shader_size) {
    if (!device || !shader_code || shader_size == 0) {
      return std::unexpected("Frame Warp passes need a device and a compiled shader");
    }

    frame_warp_d3d11_t pass;
    pass.device = device;
    auto status = device->CreateComputeShader(shader_code, shader_size, nullptr, &pass.shader);
    if (FAILED(status)) {
      return std::unexpected(failed("Could not create the Frame Warp shader", status));
    }

    D3D11_BUFFER_DESC params_desc {};
    params_desc.ByteWidth = sizeof(params_t);
    params_desc.Usage = D3D11_USAGE_DEFAULT;
    params_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    status = device->CreateBuffer(&params_desc, nullptr, &pass.params);
    if (FAILED(status)) {
      return std::unexpected(failed("Could not create the Frame Warp parameters", status));
    }

    status = make_words(device, thumbnail_bytes / sizeof(std::uint32_t), &pass.thumbnail, &pass.thumbnail_uav);
    if (FAILED(status)) {
      return std::unexpected(failed("Could not create the Frame Warp thumbnail buffer", status));
    }

    for (auto &slot : pass.ring) {
      status = make_staging(device, thumbnail_bytes, &slot.thumbnail_staging);
      if (FAILED(status)) {
        return std::unexpected(failed("Could not create a Frame Warp readback buffer", status));
      }
    }

    return pass;
  }

  std::optional<std::string> frame_warp_d3d11_t::check(const pass_t &pass) {
    if (pass.thumbnail_width % 4 != 0 || pass.thumbnail_width < 4 || pass.thumbnail_width > thumbnail_edge || pass.thumbnail_height < 1 || pass.thumbnail_height > thumbnail_edge) {
      return "Frame Warp thumbnail of " + std::to_string(pass.thumbnail_width) + 'x' + std::to_string(pass.thumbnail_height);
    }
    if (pass.hud) {
      const auto &hud = *pass.hud;
      if (hud.grid_width < 1 || hud.grid_width > max_grid_edge || hud.grid_height < 1 || hud.grid_height > max_grid_edge) {
        return "Frame Warp HUD grid of " + std::to_string(hud.grid_width) + 'x' + std::to_string(hud.grid_height);
      }
    }
    if (pass.picture_width != 0 && pass.picture_height != 0 && pass.content.width != 0 && pass.content.height != 0) {
      const auto &content = pass.content;
      if (content.x + content.width > pass.picture_width || content.y + content.height > pass.picture_height) {
        return "Frame Warp content outside its picture";
      }
    }
    return std::nullopt;
  }

  bool frame_warp_d3d11_t::mask_in_flight() const {
    return std::ranges::any_of(ring, [](const slot_t &slot) {
      return slot.pending && slot.has_mask;
    });
  }

  std::optional<bool> frame_warp_d3d11_t::prepare_hud(const hud_key_t &key) {
    if (hud && hud->key == key) {
      return false;
    }

    const std::uint64_t pixels = static_cast<std::uint64_t>(key.source_width) * key.source_height;
    const std::uint64_t cells = static_cast<std::uint64_t>(key.grid_width) * key.grid_height;
    const std::uint64_t stride = (key.grid_width + 31) / 32;
    if (pixels * sizeof(std::uint32_t) > max_buffer_bytes || cells * sizeof(std::uint32_t) > max_buffer_bytes) {
      failure = "Frame Warp HUD buffer for " + std::to_string(key.source_width) + 'x' + std::to_string(key.source_height);
      return std::nullopt;
    }

    hud_buffers_t made;
    made.key = key;
    auto status = make_words(device.Get(), pixels, &made.codes, &made.codes_uav);
    if (FAILED(status)) {
      failure = failed("Could not create the Frame Warp codes buffer", status);
      return std::nullopt;
    }
    status = make_words(device.Get(), cells, &made.scores, &made.scores_uav);
    if (FAILED(status)) {
      failure = failed("Could not create the Frame Warp scores buffer", status);
      return std::nullopt;
    }
    status = make_words(device.Get(), stride * key.grid_height, &made.bits, &made.bits_uav);
    if (FAILED(status)) {
      failure = failed("Could not create the Frame Warp mask buffer", status);
      return std::nullopt;
    }
    hud = std::move(made);
    return true;
  }

  bool frame_warp_d3d11_t::queue(ID3D11DeviceContext *context, const source_t &source, const pass_t &pass, std::uint64_t at_us) {
    started_mask = false;
    if (!context || !source.view || source.width == 0 || source.height == 0 || check(pass)) {
      return false;
    }

    auto &slot = ring[next];
    if (slot.pending) {
      return false;
    }

    // The picture the mask covers, and where the capture shows in it.
    std::uint32_t picture_width = pass.picture_width;
    std::uint32_t picture_height = pass.picture_height;
    frame_warp::rect_t content = pass.content;
    if (picture_width == 0 || picture_height == 0) {
      picture_width = source.width;
      picture_height = source.height;
      content = {};
    }
    if (content.width == 0 || content.height == 0) {
      // Not letterboxed: the capture fills the picture. Wider than 16 bits only for a capture
      // beyond them, which the clamp keeps in range.
      content = {0, 0, static_cast<std::uint16_t>(std::min<std::uint32_t>(picture_width, std::numeric_limits<std::uint16_t>::max())), static_cast<std::uint16_t>(std::min<std::uint32_t>(picture_height, std::numeric_limits<std::uint16_t>::max()))};
    }

    // A capture too soon after the one the codes are from runs no HUD pass, and the next
    // one still compares with that capture.
    bool held = false;
    if (pass.hud) {
      const auto due = taken_at > std::numeric_limits<std::uint64_t>::max() - pass.hud->every_us ? std::numeric_limits<std::uint64_t>::max() : taken_at + pass.hud->every_us;
      held = primed && at_us < due;
    }
    const hud_t *hud_pass = pass.hud && !held ? &*pass.hud : nullptr;

    bool score = false;
    bool read = false;
    if (hud_pass) {
      const auto fresh = prepare_hud({source.width, source.height, hud_pass->grid_width, hud_pass->grid_height, picture_width, picture_height, content});
      if (!fresh) {
        return false;
      }
      if (*fresh) {
        // New sizes start over: this capture only takes the codes.
        const UINT zeros[4] {};
        context->ClearUnorderedAccessViewUint(hud->scores_uav.Get(), zeros);
      }
      score = !*fresh && primed;
      read = hud_pass->read && !mask_in_flight();
    }

    const std::uint32_t stride = hud_pass ? (hud_pass->grid_width + 31) / 32 : 0;
    const std::uint32_t mask_bytes = hud_pass ? stride * hud_pass->grid_height * sizeof(std::uint32_t) : 0;
    if (read && slot.mask_staging_bytes != mask_bytes) {
      slot.mask_staging.Reset();
      slot.mask_staging_bytes = 0;
      const auto status = make_staging(device.Get(), mask_bytes, &slot.mask_staging);
      if (FAILED(status)) {
        failure = failed("Could not create a Frame Warp mask readback buffer", status);
        return false;
      }
      slot.mask_staging_bytes = mask_bytes;
    }

    params_t values {
      {source.width, source.height},
      {pass.thumbnail_width, pass.thumbnail_height},
      {hud_pass ? hud_pass->grid_width : 0, hud_pass ? hud_pass->grid_height : 0},
      {picture_width, picture_height},
      {content.x, content.y, content.width, content.height},
      static_cast<std::uint32_t>(source.transfer),
      0,
      stride,
      hud_score,
    };
    const auto run = [&](pass_e which, std::uint32_t across, std::uint32_t down) {
      values.pass = static_cast<std::uint32_t>(which);
      context->UpdateSubresource(params.Get(), 0, nullptr, &values, 0, 0);
      context->Dispatch(across, down, 1);
    };

    ID3D11UnorderedAccessView *views[4] {
      thumbnail_uav.Get(),
      hud_pass ? hud->codes_uav.Get() : nullptr,
      hud_pass ? hud->scores_uav.Get() : nullptr,
      hud_pass ? hud->bits_uav.Get() : nullptr,
    };
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, params.GetAddressOf());
    context->CSSetShaderResources(0, 1, &source.view);
    context->CSSetUnorderedAccessViews(0, 4, views, nullptr);

    run(pass_e::thumbnail, groups(pass.thumbnail_width / 4), groups(pass.thumbnail_height));
    if (hud_pass) {
      // The score pass reads the codes the earlier capture left; then they are replaced.
      if (score) {
        run(pass_e::score, groups(hud_pass->grid_width), groups(hud_pass->grid_height));
      }
      run(pass_e::codes, groups(source.width), groups(source.height));
      if (read) {
        run(pass_e::bits, groups(stride), groups(hud_pass->grid_height));
      }
    }

    ID3D11ShaderResourceView *no_view = nullptr;
    ID3D11UnorderedAccessView *no_views[4] {};
    ID3D11Buffer *no_buffer = nullptr;
    context->CSSetShaderResources(0, 1, &no_view);
    context->CSSetUnorderedAccessViews(0, 4, no_views, nullptr);
    context->CSSetConstantBuffers(0, 1, &no_buffer);
    context->CSSetShader(nullptr, nullptr, 0);

    // The mask copy goes first: a thumbnail that has finished means the mask has too.
    if (read) {
      copy_bytes(context, slot.mask_staging.Get(), hud->bits.Get(), mask_bytes);
    }
    copy_bytes(context, slot.thumbnail_staging.Get(), thumbnail.Get(), pass.thumbnail_width * pass.thumbnail_height);

    slot.pending = true;
    slot.at_us = at_us;
    slot.width = pass.thumbnail_width;
    slot.height = pass.thumbnail_height;
    slot.has_mask = read;
    slot.mask_width = hud_pass ? hud_pass->grid_width : 0;
    slot.mask_height = hud_pass ? hud_pass->grid_height : 0;
    next = (next + 1) % ring.size();

    primed = hud_pass != nullptr || held;
    if (hud_pass) {
      taken_at = at_us;
    }
    started_mask = read;
    return true;
  }

  std::expected<frame_warp_d3d11_t::readout_t, std::string> frame_warp_d3d11_t::poll(ID3D11DeviceContext *context) {
    if (failure) {
      return std::unexpected(std::exchange(failure, std::nullopt).value());
    }

    readout_t readout;
    // Slots fill in ring order from `next`, so the oldest in flight comes first, and the GPU
    // finishes them in that order: the first one still running ends the walk.
    for (std::size_t offset = 0; offset < ring.size(); ++offset) {
      auto &slot = ring[(next + offset) % ring.size()];
      if (!slot.pending) {
        continue;
      }

      D3D11_MAPPED_SUBRESOURCE mapped {};
      auto status = context->Map(slot.thumbnail_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (status == DXGI_ERROR_WAS_STILL_DRAWING) {
        break;
      }
      if (FAILED(status)) {
        slot.pending = false;
        return std::unexpected(failed("Could not map a Frame Warp readback buffer", status));
      }

      std::vector<std::uint32_t> mask_words;
      if (slot.has_mask) {
        D3D11_MAPPED_SUBRESOURCE mask_mapped {};
        status = context->Map(slot.mask_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mask_mapped);
        if (status == DXGI_ERROR_WAS_STILL_DRAWING) {
          context->Unmap(slot.thumbnail_staging.Get(), 0);
          break;
        }
        if (FAILED(status)) {
          context->Unmap(slot.thumbnail_staging.Get(), 0);
          slot.pending = false;
          return std::unexpected(failed("Could not map a Frame Warp mask readback buffer", status));
        }
        mask_words.resize(slot.mask_staging_bytes / sizeof(std::uint32_t));
        std::memcpy(mask_words.data(), mask_mapped.pData, slot.mask_staging_bytes);
        context->Unmap(slot.mask_staging.Get(), 0);
      }

      frame_warp::thumbnail_t thumbnail_readout;
      thumbnail_readout.width = slot.width;
      thumbnail_readout.height = slot.height;
      thumbnail_readout.at_us = slot.at_us;
      thumbnail_readout.luma.resize(static_cast<std::size_t>(slot.width) * slot.height);
      std::memcpy(thumbnail_readout.luma.data(), mapped.pData, thumbnail_readout.luma.size());
      context->Unmap(slot.thumbnail_staging.Get(), 0);
      readout.thumbnails.push_back(std::move(thumbnail_readout));

      if (slot.has_mask) {
        frame_warp::mask_t mask;
        mask.width = static_cast<std::uint16_t>(slot.mask_width);
        mask.height = static_cast<std::uint16_t>(slot.mask_height);
        mask.words = std::move(mask_words);
        readout.mask = std::move(mask);
      }
      slot.pending = false;
      slot.has_mask = false;
    }
    return readout;
  }

  std::size_t frame_warp_d3d11_t::in_flight() const {
    return static_cast<std::size_t>(std::ranges::count_if(ring, [](const slot_t &slot) {
      return slot.pending;
    }));
  }

  std::expected<Microsoft::WRL::ComPtr<ID3DBlob>, std::string> compile_frame_warp_shader(const std::filesystem::path &path) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, Microsoft::WRL::ComPtr<ID3DBlob>> compiled;
    std::lock_guard lock {mutex};
    if (const auto it = compiled.find(path); it != compiled.end()) {
      return it->second;
    }

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const auto status = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main_cs", "cs_5_0", flags, 0, &code, &errors);
    if (FAILED(status)) {
      const auto name = path.u8string();
      auto message = failed(("Could not compile " + std::string {name.begin(), name.end()}).c_str(), status);
      if (errors && errors->GetBufferSize() > 0) {
        std::string text {static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()};
        while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r')) {
          text.pop_back();
        }
        message += ": " + text;
      }
      return std::unexpected(std::move(message));
    }
    compiled.emplace(path, code);
    return code;
  }

  frame_warp_session_t::frame_warp_session_t(std::filesystem::path shader_path):
      shader_path {std::move(shader_path)} {
  }

  std::optional<std::string> frame_warp_session_t::attach(ID3D11Device *device_, std::shared_ptr<::frame_warp::session_t> session_) {
    session.reset();
    pass.reset();
    device.Reset();
    failed = false;
    failure.reset();
    recorded = false;
    last_measured.reset();
    mask_due.reset();
    if (!session_) {
      return std::nullopt;
    }
    if (!device_) {
      return "Frame Warp needs a device";
    }

    // Nothing is made here: a stream no client asked Frame Warp for costs nothing.
    device = device_;
    session = std::move(session_);
    return std::nullopt;
  }

  bool frame_warp_session_t::active() const {
    return session && !failed;
  }

  std::string frame_warp_session_t::stop(std::string reason) {
    failed = true;
    pass.reset();
    recorded = false;
    return reason;
  }

  void frame_warp_session_t::converted(ID3D11DeviceContext *context, const frame_warp_d3d11_t::source_t &source, std::optional<std::chrono::steady_clock::time_point> captured) {
    if (!active()) {
      return;
    }
    if (!session->enabled()) {
      // Off, or switched off: the passes' buffers go.
      if (pass) {
        pass.reset();
        recorded = false;
        last_measured.reset();
        mask_due.reset();
      }
      return;
    }
    const auto params = session->params();
    if (!params.enabled) {
      pass.reset();
      recorded = false;
      return;
    }
    if (!source.view || params.thumbnail_width == 0 || params.thumbnail_height == 0) {
      return;
    }
    // A capture is measured once: a frame that repeats the last one adds nothing.
    if (captured && last_measured && *captured <= *last_measured) {
      return;
    }

    if (!pass) {
      const auto shader = compile_frame_warp_shader(shader_path);
      if (!shader) {
        failure = shader.error();
        failed = true;
        return;
      }
      auto created = frame_warp_d3d11_t::create(device.Get(), (*shader)->GetBufferPointer(), (*shader)->GetBufferSize());
      if (!created) {
        failure = created.error();
        failed = true;
        return;
      }
      pass.emplace(std::move(*created));
    }

    const auto now = std::chrono::steady_clock::now();
    frame_warp_d3d11_t::pass_t request;
    request.thumbnail_width = static_cast<std::uint32_t>(std::min<std::size_t>(params.thumbnail_width, std::numeric_limits<std::uint32_t>::max()));
    request.thumbnail_height = static_cast<std::uint32_t>(std::min<std::size_t>(params.thumbnail_height, std::numeric_limits<std::uint32_t>::max()));
    request.picture_width = params.picture_width;
    request.picture_height = params.picture_height;
    request.content = params.content;
    if (params.moving) {
      // The mask covers the picture; one not yet known is the capture.
      const auto picture_width = static_cast<std::uint16_t>(std::min<std::uint32_t>(params.picture_width ? params.picture_width : source.width, std::numeric_limits<std::uint16_t>::max()));
      const auto picture_height = static_cast<std::uint16_t>(std::min<std::uint32_t>(params.picture_height ? params.picture_height : source.height, std::numeric_limits<std::uint16_t>::max()));
      if (const auto grid = hud_grid(params.mask_width, picture_width, picture_height)) {
        request.hud = frame_warp_d3d11_t::hud_t {grid->first, grid->second, !mask_due || now >= *mask_due, params.hud_every_us ? params.hud_every_us : hud_every_us};
      }
    }

    if (const auto why = frame_warp_d3d11_t::check(request)) {
      failure = *why;
      failed = true;
      pass.reset();
      return;
    }

    const auto at_us = ::frame_warp::session_t::us(captured.value_or(now));
    if (pass->queue(context, source, request, at_us)) {
      recorded = true;
      if (captured) {
        last_measured = captured;
      }
      if (pass->mask_started()) {
        mask_due = now + mask_every;
      }
    }
  }

  void frame_warp_session_t::converted_unmeasured() {
    // Nothing is measured on a blank frame, and the readbacks are not tied to frames.
  }

  std::optional<std::string> frame_warp_session_t::submitted(ID3D11DeviceContext *context) {
    if (failure) {
      return stop(std::exchange(failure, std::nullopt).value());
    }
    if (!active() || !pass) {
      return std::nullopt;
    }

    if (std::exchange(recorded, false)) {
      // The encoder already has its frame; this only gets the passes to the GPU before the
      // next conversion, so the readback is ready a frame or two later.
      context->Flush();
    }

    auto readout = pass->poll(context);
    if (!readout) {
      return stop(readout.error());
    }
    for (auto &thumbnail : readout->thumbnails) {
      session->submit_thumbnail(std::move(thumbnail));
    }
    if (readout->mask) {
      session->submit_hud(std::move(*readout->mask));
    }
    return std::nullopt;
  }
}  // namespace platf::dxgi
