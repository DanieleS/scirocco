/**
 * @file tests/unit/platform/windows/test_frame_warp_d3d11.cpp
 * @brief The Frame Warp thumbnail and HUD passes and their readback ring, run on the WARP
 *        software rasterizer so no GPU is involved.
 */
#include "../../../tests_common.h"
#include "src/frame_warp_session.h"
#include "src/frame_warp_types.h"
#include "src/frame_warp_wire.h"
#include "src/platform/windows/frame_warp_d3d11.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using platf::dxgi::frame_warp_d3d11_t;
using hud_t = frame_warp_d3d11_t::hud_t;
using pass_t = frame_warp_d3d11_t::pass_t;
using source_t = frame_warp_d3d11_t::source_t;
using transfer_e = frame_warp_d3d11_t::transfer_e;

namespace {
  constexpr std::uint16_t half_1 = 0x3C00;  // 80 nits as scRGB

  ComPtr<ID3DBlob> compile_frame_warp() {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const std::filesystem::path path {FRAME_WARP_TEST_SHADER_PATH};
    const auto status = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main_cs", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(status)) {
      ADD_FAILURE() << "frame_warp_cs.hlsl failed to compile: "
                    << (errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string {});
      return nullptr;
    }
    return code;
  }

  /// The ST 2084 encoding of `nits`, as common.hlsl's NitsToPQ does it.
  float pq(float nits) {
    const float m1 = 2610.0f / 4096.0f / 4.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;
    const float lp = std::pow(nits / 10000.0f, m1);
    return std::pow((c1 + c2 * lp) / (1.0f + c3 * lp), m2);
  }

  /// A byte that looks random but is the same for the same (x, y).
  std::uint8_t noise(std::uint32_t x, std::uint32_t y) {
    std::uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<std::uint8_t>((h ^ (h >> 16)) >> 8);
  }

  std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return 0xff000000u | static_cast<std::uint32_t>(b) << 16 | static_cast<std::uint32_t>(g) << 8 | r;
  }

  class FrameWarpD3D11: public testing::Test {
  protected:
    void SetUp() override {
      const auto status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
      if (FAILED(status)) {
        GTEST_SKIP() << "No WARP device";
      }
      shader = compile_frame_warp();
      ASSERT_TRUE(shader);
      auto created = frame_warp_d3d11_t::create(device.Get(), shader->GetBufferPointer(), shader->GetBufferSize());
      ASSERT_TRUE(created) << created.error();
      pass.emplace(std::move(*created));
    }

    /// A texture of `pixels`, row by row, and a view of it.
    template<class T>
    ComPtr<ID3D11ShaderResourceView> texture(DXGI_FORMAT format, std::uint32_t width, std::uint32_t height, const std::vector<T> &pixels) {
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = width;
      desc.Height = height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      const D3D11_SUBRESOURCE_DATA data {pixels.data(), static_cast<UINT>(sizeof(T) * pixels.size() / height), 0};
      ComPtr<ID3D11Texture2D> texture;
      EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)));
      ComPtr<ID3D11ShaderResourceView> view;
      if (texture) {
        EXPECT_TRUE(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)));
      }
      return view;
    }

    /// An 8-bit picture of `value(x, y)` in every colour channel.
    template<class F>
    ComPtr<ID3D11ShaderResourceView> gray(std::uint32_t width, std::uint32_t height, F value) {
      std::vector<std::uint32_t> pixels;
      for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
          const auto v = static_cast<std::uint8_t>(value(x, y));
          pixels.push_back(rgba(v, v, v));
        }
      }
      return texture(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, pixels);
    }

    /// Poll until nothing is in flight, or give up after a few seconds; what finished, the
    /// last mask included.
    frame_warp_d3d11_t::readout_t drain() {
      frame_warp_d3d11_t::readout_t all;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {10};
      while (pass->in_flight() > 0 && std::chrono::steady_clock::now() < deadline) {
        auto polled = pass->poll(context.Get());
        if (!polled) {
          ADD_FAILURE() << polled.error();
          break;
        }
        for (auto &thumbnail : polled->thumbnails) {
          all.thumbnails.push_back(std::move(thumbnail));
        }
        if (polled->mask) {
          all.mask = std::move(polled->mask);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
      }
      return all;
    }

    /// One capture, queued, flushed and read back.
    frame_warp_d3d11_t::readout_t run(const source_t &source, const pass_t &request, std::uint64_t at_us) {
      EXPECT_TRUE(pass->queue(context.Get(), source, request, at_us));
      context->Flush();
      return drain();
    }

    /// The thumbnail of `view`, `width` x `height`.
    frame_warp::thumbnail_t thumbnail_of(ID3D11ShaderResourceView *view, std::uint32_t source_width, std::uint32_t source_height, transfer_e transfer, std::uint32_t width, std::uint32_t height) {
      pass_t request;
      request.thumbnail_width = width;
      request.thumbnail_height = height;
      const auto readout = run({view, source_width, source_height, transfer}, request, 77);
      EXPECT_EQ(readout.thumbnails.size(), 1u);
      if (readout.thumbnails.empty()) {
        return {};
      }
      EXPECT_EQ(readout.thumbnails.front().width, width);
      EXPECT_EQ(readout.thumbnails.front().height, height);
      EXPECT_EQ(readout.thumbnails.front().luma.size(), static_cast<std::size_t>(width) * height);
      EXPECT_EQ(readout.thumbnails.front().at_us, 77u);
      return readout.thumbnails.front();
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3DBlob> shader;
    std::optional<frame_warp_d3d11_t> pass;
  };

  /// A mask is all clear.
  bool clear(const frame_warp::mask_t &mask) {
    return mask.is_clear();
  }

  /// 128x64 frame n: the left quarter is a texture that stays; the rest is another that pans
  /// five pixels a frame.
  std::uint8_t turning(std::uint32_t n, std::uint32_t x, std::uint32_t y) {
    return x < 32 ? noise(x, y) : noise(x + 5 * n, y);
  }
}  // namespace

TEST(FrameWarpHudGrid, KeepsTheShapeOfThePicture) {
  using platf::dxgi::hud_grid;
  EXPECT_FALSE(hud_grid(0, 1920, 1080));
  EXPECT_FALSE(hud_grid(256, 0, 1080));
  EXPECT_FALSE(hud_grid(256, 1920, 0));

  const auto wide = hud_grid(256, 1920, 1080);
  ASSERT_TRUE(wide);
  EXPECT_EQ(wide->first, 256u);
  EXPECT_EQ(wide->second, 144u);

  // No more cells than pixels, nor than the wire's edge.
  const auto small = hud_grid(5000, 100, 50);
  ASSERT_TRUE(small);
  EXPECT_EQ(small->first, 100u);
  EXPECT_EQ(small->second, 50u);
  const auto huge = hud_grid(5000, 4096, 2048);
  ASSERT_TRUE(huge);
  EXPECT_EQ(huge->first, 2048u);
  EXPECT_EQ(huge->second, 1024u);

  // At least a row.
  const auto thin = hud_grid(64, 4000, 3);
  ASSERT_TRUE(thin);
  EXPECT_EQ(thin->first, 64u);
  EXPECT_EQ(thin->second, 1u);
}

TEST_F(FrameWarpD3D11, ThumbnailOfAUniformPictureIsItsLuma) {
  const auto view = gray(64, 36, [](auto, auto) {
    return 128;
  });
  const auto thumbnail = thumbnail_of(view.Get(), 64, 36, transfer_e::encoded, 16, 9);
  for (const auto luma : thumbnail.luma) {
    EXPECT_NEAR(luma, 128, 1);
  }
}

TEST_F(FrameWarpD3D11, ThumbnailCoversTheWholeCapture) {
  // Black left half, white right half: the box edges meet at x = 32.
  const auto view = gray(64, 16, [](auto x, auto) {
    return x < 32 ? 0 : 255;
  });
  const auto thumbnail = thumbnail_of(view.Get(), 64, 16, transfer_e::encoded, 16, 4);
  for (std::size_t y = 0; y < 4; ++y) {
    for (std::size_t x = 0; x < 16; ++x) {
      EXPECT_EQ(thumbnail.luma[y * 16 + x], x < 8 ? 0 : 255) << "column " << x << " row " << y;
    }
  }
}

TEST_F(FrameWarpD3D11, ThumbnailKeepsTheRowsInOrder) {
  // Row bands of 0, 64, 128 and 192 over four thumbnail rows.
  const auto view = gray(32, 16, [](auto, auto y) {
    return y / 4 * 64;
  });
  const auto thumbnail = thumbnail_of(view.Get(), 32, 16, transfer_e::encoded, 8, 4);
  for (std::size_t y = 0; y < 4; ++y) {
    for (std::size_t x = 0; x < 8; ++x) {
      EXPECT_NEAR(thumbnail.luma[y * 8 + x], y * 64, 1);
    }
  }
}

TEST_F(FrameWarpD3D11, ThumbnailWeighsTheChannelsAsRec709) {
  const std::vector<std::uint32_t> red(16 * 16, rgba(255, 0, 0));
  const std::vector<std::uint32_t> green(16 * 16, rgba(0, 255, 0));
  const std::vector<std::uint32_t> blue(16 * 16, rgba(0, 0, 255));
  const auto from = [&](const std::vector<std::uint32_t> &pixels) {
    const auto view = texture(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, pixels);
    return thumbnail_of(view.Get(), 16, 16, transfer_e::encoded, 4, 4).luma.front();
  };
  EXPECT_NEAR(from(red), 0.2126 * 255.0, 1.0);
  EXPECT_NEAR(from(green), 0.7152 * 255.0, 1.0);
  EXPECT_NEAR(from(blue), 0.0722 * 255.0, 1.0);
}

TEST_F(FrameWarpD3D11, ThumbnailOfScRgbIsPqOfScRgbOver125) {
  // 1.0 is 80 nits, 4.0 is 320 nits; the left half of each row is one, the right the other.
  constexpr std::uint16_t half_4 = 0x4400;
  std::vector<std::uint64_t> pixels;
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 16; ++x) {
      const std::uint64_t value = x < 8 ? half_1 : half_4;
      pixels.push_back(value | value << 16 | value << 32 | std::uint64_t {half_1} << 48);
    }
  }
  const auto view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 8, pixels);
  const auto thumbnail = thumbnail_of(view.Get(), 16, 8, transfer_e::scrgb, 4, 2);
  EXPECT_NEAR(thumbnail.luma[0], pq(80.0f) * 255.0f, 1.5f);
  EXPECT_NEAR(thumbnail.luma[1], pq(80.0f) * 255.0f, 1.5f);
  EXPECT_NEAR(thumbnail.luma[2], pq(320.0f) * 255.0f, 1.5f);
  EXPECT_NEAR(thumbnail.luma[3], pq(320.0f) * 255.0f, 1.5f);
  EXPECT_GT(thumbnail.luma[2], thumbnail.luma[0]);
}

TEST_F(FrameWarpD3D11, ThumbnailOfHdr10UsesTheRec2020Weights) {
  // R10G10B10A2: red full, green and blue 0; and the reverse for blue.
  const std::vector<std::uint32_t> red(16 * 16, 1023u | 3u << 30);
  const std::vector<std::uint32_t> blue(16 * 16, 1023u << 20 | 3u << 30);
  const auto from = [&](const std::vector<std::uint32_t> &pixels) {
    const auto view = texture(DXGI_FORMAT_R10G10B10A2_UNORM, 16, 16, pixels);
    return thumbnail_of(view.Get(), 16, 16, transfer_e::pq, 4, 4).luma.front();
  };
  EXPECT_NEAR(from(red), 0.2627 * 255.0, 1.0);
  EXPECT_NEAR(from(blue), 0.0593 * 255.0, 1.0);
}

TEST_F(FrameWarpD3D11, NoMaskWithoutAHudPass) {
  const auto view = gray(16, 16, [](auto x, auto y) {
    return noise(x, y);
  });
  pass_t request;
  request.thumbnail_width = 16;
  request.thumbnail_height = 16;
  const auto readout = run({view.Get(), 16, 16, transfer_e::encoded}, request, 1);
  EXPECT_EQ(readout.thumbnails.size(), 1u);
  EXPECT_FALSE(readout.mask);
  EXPECT_FALSE(pass->mask_started());
}

TEST_F(FrameWarpD3D11, AStaticPatchBecomesHudWhileTheRestScrolls) {
  const auto grid = platf::dxgi::hud_grid(32, 128, 64);
  ASSERT_TRUE(grid);
  EXPECT_EQ(grid->first, 32u);
  EXPECT_EQ(grid->second, 16u);

  std::optional<frame_warp::mask_t> early;
  std::optional<frame_warp::mask_t> mask;
  const std::uint32_t last = frame_warp_d3d11_t::hud_score + 2;
  for (std::uint32_t n = 0; n <= last; ++n) {
    const auto view = gray(128, 64, [n](auto x, auto y) {
      return turning(n, x, y);
    });
    pass_t request;
    request.thumbnail_width = 32;
    request.thumbnail_height = 16;
    request.hud = hud_t {grid->first, grid->second, n == 10 || n == last, 0};
    auto readout = run({view.Get(), 128, 64, transfer_e::encoded}, request, n);
    ASSERT_EQ(readout.thumbnails.size(), 1u);
    if (n == 10) {
      early = std::move(readout.mask);
    } else if (n == last) {
      mask = std::move(readout.mask);
    } else {
      EXPECT_FALSE(readout.mask) << "no mask unless read is set";
    }
  }

  ASSERT_TRUE(early);
  EXPECT_TRUE(clear(*early)) << "a few comparisons do not make HUD";

  ASSERT_TRUE(mask);
  EXPECT_EQ(mask->width, 32u);
  EXPECT_EQ(mask->height, 16u);
  ASSERT_EQ(mask->words.size(), 16u);
  for (const auto word : mask->words) {
    // The left quarter, 8 of the 32 cells, is the HUD.
    EXPECT_EQ(word, 0x000000ffu);
  }
}

TEST_F(FrameWarpD3D11, AHudPassWithoutAnEarlierCaptureOnlyTakesCodes) {
  // The very first capture of a size has nothing to compare with: even with read set, its mask
  // has no scored cells.
  const auto view = gray(128, 64, [](auto x, auto y) {
    return turning(0, x, y);
  });
  pass_t request;
  request.thumbnail_width = 32;
  request.thumbnail_height = 16;
  request.hud = hud_t {32, 16, true, 0};
  const auto readout = run({view.Get(), 128, 64, transfer_e::encoded}, request, 0);
  ASSERT_TRUE(readout.mask);
  EXPECT_TRUE(clear(*readout.mask));
  EXPECT_TRUE(pass->mask_started());
}

TEST_F(FrameWarpD3D11, ALetterboxedCaptureHasItsGridWhereItShowsAndBarsAreHud) {
  // A 128x64 capture in the middle of a 192x64 picture: 32 pixel bars either side. 48x16
  // cells of 4x4 picture pixels; the capture's cells are 8 to 39 and the static left quarter
  // of it (picture pixels 32 to 63) is cells 8 to 15.
  const auto grid = platf::dxgi::hud_grid(48, 192, 64);
  ASSERT_TRUE(grid);
  EXPECT_EQ(grid->first, 48u);
  EXPECT_EQ(grid->second, 16u);

  std::optional<frame_warp::mask_t> mask;
  const std::uint32_t last = frame_warp_d3d11_t::hud_score + 2;
  for (std::uint32_t n = 0; n <= last; ++n) {
    const auto view = gray(128, 64, [n](auto x, auto y) {
      return turning(n, x, y);
    });
    pass_t request;
    request.thumbnail_width = 32;
    request.thumbnail_height = 16;
    request.picture_width = 192;
    request.picture_height = 64;
    request.content = {32, 0, 128, 64};
    request.hud = hud_t {grid->first, grid->second, n == last, 0};
    auto readout = run({view.Get(), 128, 64, transfer_e::encoded}, request, n);
    ASSERT_EQ(readout.thumbnails.size(), 1u);
    if (n == last) {
      mask = std::move(readout.mask);
    }
  }

  ASSERT_TRUE(mask);
  EXPECT_EQ(mask->width, 48u);
  EXPECT_EQ(mask->height, 16u);
  ASSERT_EQ(mask->words.size(), 32u);
  for (std::size_t row = 0; row < 16; ++row) {
    // Cells 0 to 15: bar and HUD; 16 to 39: the scrolling scene; 40 to 47: bar.
    EXPECT_EQ(mask->words[row * 2], 0x0000ffffu) << "row " << row;
    EXPECT_EQ(mask->words[row * 2 + 1], 0x0000ff00u) << "row " << row;
  }
}

TEST_F(FrameWarpD3D11, ACaptureTooSoonAfterTheComparedOneRunsNoHudPass) {
  const auto view = gray(128, 64, [](auto x, auto y) {
    return noise(x, y);
  });
  const auto request = [](bool read) {
    pass_t request;
    request.thumbnail_width = 32;
    request.thumbnail_height = 16;
    request.hud = hud_t {32, 16, read, 1000};
    return request;
  };
  const source_t source {view.Get(), 128, 64, transfer_e::encoded};

  // Takes the codes.
  EXPECT_EQ(run(source, request(false), 0).thumbnails.size(), 1u);
  // 10 us later is too soon: the thumbnail is made, the HUD is not, and no mask is read.
  const auto soon = run(source, request(true), 10);
  EXPECT_EQ(soon.thumbnails.size(), 1u);
  EXPECT_FALSE(soon.mask);
  EXPECT_FALSE(pass->mask_started());
  // 1000 us after the capture the codes are from it compares, still with that one.
  const auto later = run(source, request(true), 1000);
  EXPECT_EQ(later.thumbnails.size(), 1u);
  ASSERT_TRUE(later.mask);
  EXPECT_TRUE(clear(*later.mask)) << "one comparison is no HUD";
}

TEST_F(FrameWarpD3D11, NeverQueuesMoreThanTheRing) {
  const auto view = gray(64, 36, [](auto x, auto y) {
    return noise(x, y);
  });
  const source_t source {view.Get(), 64, 36, transfer_e::encoded};
  pass_t request;
  request.thumbnail_width = 16;
  request.thumbnail_height = 9;

  for (std::uint64_t i = 0; i < frame_warp_d3d11_t::ring_size; ++i) {
    EXPECT_TRUE(pass->queue(context.Get(), source, request, 100 + i));
  }
  EXPECT_FALSE(pass->queue(context.Get(), source, request, 200));
  EXPECT_EQ(pass->in_flight(), frame_warp_d3d11_t::ring_size);
  context->Flush();

  const auto readout = drain();
  ASSERT_EQ(readout.thumbnails.size(), frame_warp_d3d11_t::ring_size);
  for (std::size_t i = 0; i < readout.thumbnails.size(); ++i) {
    EXPECT_EQ(readout.thumbnails[i].at_us, 100 + i) << "oldest first";
  }
  EXPECT_EQ(pass->in_flight(), 0u);
  const auto again = run(source, request, 300);
  ASSERT_EQ(again.thumbnails.size(), 1u);
  EXPECT_EQ(again.thumbnails.front().at_us, 300u);
}

TEST_F(FrameWarpD3D11, ARingBusyWithAMaskDoesNotReadAnotherUntilItIsRead) {
  const auto view = gray(128, 64, [](auto x, auto y) {
    return noise(x, y);
  });
  const source_t source {view.Get(), 128, 64, transfer_e::encoded};
  pass_t request;
  request.thumbnail_width = 32;
  request.thumbnail_height = 16;
  request.hud = hud_t {32, 16, true, 0};

  ASSERT_TRUE(pass->queue(context.Get(), source, request, 1));
  EXPECT_TRUE(pass->mask_started());
  ASSERT_TRUE(pass->queue(context.Get(), source, request, 2));
  EXPECT_FALSE(pass->mask_started()) << "the first mask copy is still in flight";
  context->Flush();
  const auto readout = drain();
  EXPECT_EQ(readout.thumbnails.size(), 2u);
  EXPECT_TRUE(readout.mask);

  ASSERT_TRUE(pass->queue(context.Get(), source, request, 3));
  EXPECT_TRUE(pass->mask_started());
  context->Flush();
  drain();
}

TEST_F(FrameWarpD3D11, PollNeverWaitsForThePass) {
  // A pass over a large capture that has not even been flushed to the GPU: poll() answers at
  // once with nothing instead of waiting for it.
  const std::vector<std::uint64_t> pixels(1920 * 1080, std::uint64_t {half_1} * 0x0001000100010001ull);
  const auto view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1920, 1080, pixels);
  pass_t request;
  request.thumbnail_width = 256;
  request.thumbnail_height = 144;
  ASSERT_TRUE(pass->queue(context.Get(), {view.Get(), 1920, 1080, transfer_e::scrgb}, request, 5));

  const auto start = std::chrono::steady_clock::now();
  const auto early = pass->poll(context.Get());
  const auto took = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(early) << early.error();
  EXPECT_LT(took, std::chrono::milliseconds {250});
  EXPECT_LE(early->thumbnails.size(), 1u);

  context->Flush();
  std::size_t finished = early->thumbnails.size();
  finished += drain().thumbnails.size();
  EXPECT_EQ(finished, 1u);
}

TEST_F(FrameWarpD3D11, RefusesWhatCannotRun) {
  const auto view = gray(16, 16, [](auto, auto) {
    return 0;
  });
  const source_t source {view.Get(), 16, 16, transfer_e::encoded};
  pass_t request;
  request.thumbnail_width = 16;
  request.thumbnail_height = 16;

  EXPECT_FALSE(pass->queue(context.Get(), {}, request, 1)) << "no view";
  request.thumbnail_width = 6;
  EXPECT_TRUE(frame_warp_d3d11_t::check(request)) << "a thumbnail width that is not a multiple of 4";
  EXPECT_FALSE(pass->queue(context.Get(), source, request, 1));
  request.thumbnail_width = 260;
  EXPECT_TRUE(frame_warp_d3d11_t::check(request));
  request.thumbnail_width = 16;
  request.thumbnail_height = 0;
  EXPECT_TRUE(frame_warp_d3d11_t::check(request));
  request.thumbnail_height = 16;
  EXPECT_FALSE(frame_warp_d3d11_t::check(request));

  request.hud = hud_t {2049, 10, false, 0};
  EXPECT_TRUE(frame_warp_d3d11_t::check(request)) << "a grid wider than the wire's edge";
  request.hud = hud_t {10, 0, false, 0};
  EXPECT_TRUE(frame_warp_d3d11_t::check(request));
  request.hud.reset();

  request.picture_width = 100;
  request.picture_height = 50;
  request.content = {50, 0, 100, 50};
  EXPECT_TRUE(frame_warp_d3d11_t::check(request)) << "content past the picture";
  request.content = {0, 0, 100, 50};
  EXPECT_FALSE(frame_warp_d3d11_t::check(request));
  EXPECT_EQ(pass->in_flight(), 0u);
}

namespace {
  class FrameWarpSession: public FrameWarpD3D11 {
  protected:
    void SetUp() override {
      FrameWarpD3D11::SetUp();
      if (IsSkipped() || HasFatalFailure()) {
        return;
      }
      view = gray(128, 72, [](auto x, auto y) {
        return noise(x, y);
      });
      source = {view.Get(), 128, 72, transfer_e::encoded};
    }

    /// A learner session, asked for Frame Warp when `enabled`.
    std::shared_ptr<frame_warp::session_t> learner(bool enabled) {
      auto made = std::make_shared<frame_warp::session_t>(frame_warp::store_t {std::filesystem::path {}, std::string {}});
      made->set_picture(128, 72, {0, 0, 128, 72});
      if (enabled) {
        const frame_warp::request_t request {true, std::nullopt, false, 256};
        made->on_request(request.encode());
      }
      return made;
    }

    /// Run submitted() until no pass is in flight, or give up.
    bool settle(platf::dxgi::frame_warp_session_t &session) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {10};
      while (std::chrono::steady_clock::now() < deadline) {
        EXPECT_FALSE(session.submitted(context.Get()));
        if (session.in_flight() == 0) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
      }
      return false;
    }

    ComPtr<ID3D11ShaderResourceView> view;
    source_t source;
  };
}  // namespace

TEST_F(FrameWarpSession, NothingExistsWithoutASession) {
  platf::dxgi::frame_warp_session_t session {FRAME_WARP_TEST_SHADER_PATH};
  const auto now = std::chrono::steady_clock::now();
  session.converted(context.Get(), source, now);
  session.converted_unmeasured();
  EXPECT_FALSE(session.submitted(context.Get()));
  EXPECT_FALSE(session.has_pass());
  EXPECT_FALSE(session.active());
  EXPECT_FALSE(session.attach(device.Get(), nullptr));
  EXPECT_FALSE(session.active());
  EXPECT_TRUE(session.attach(nullptr, learner(false))) << "a session needs a device";
  EXPECT_FALSE(session.active());
}

TEST_F(FrameWarpSession, NothingIsMadeUntilTheClientAsks) {
  platf::dxgi::frame_warp_session_t session {FRAME_WARP_TEST_SHADER_PATH};
  ASSERT_FALSE(session.attach(device.Get(), learner(false)));
  EXPECT_TRUE(session.active());

  const auto now = std::chrono::steady_clock::now();
  session.converted(context.Get(), source, now);
  session.converted(context.Get(), source, now + std::chrono::milliseconds {16});
  EXPECT_FALSE(session.has_pass()) << "no shader, buffers or GPU work while no client asked";
  EXPECT_EQ(session.in_flight(), 0u);
  EXPECT_FALSE(session.submitted(context.Get()));
}

TEST_F(FrameWarpSession, TheFirstCaptureAfterTheRequestMakesThePassAndEachIsMeasuredOnce) {
  auto asked = learner(true);
  if (!asked->params().enabled || asked->params().thumbnail_width == 0) {
    GTEST_SKIP() << "the learner has no thumbnail size to ask for";
  }
  platf::dxgi::frame_warp_session_t session {FRAME_WARP_TEST_SHADER_PATH};
  ASSERT_FALSE(session.attach(device.Get(), asked));
  EXPECT_FALSE(session.has_pass()) << "made by the first capture, not by attach";

  const auto first = std::chrono::steady_clock::now();
  session.converted(context.Get(), source, first);
  EXPECT_TRUE(session.has_pass());
  EXPECT_EQ(session.in_flight(), 1u) << "recorded while the conversion still holds the mutex";
  ASSERT_TRUE(settle(session));

  // The same capture again (a repeated frame) is not measured twice; a newer one is.
  session.converted(context.Get(), source, first);
  EXPECT_EQ(session.in_flight(), 0u);
  session.converted(context.Get(), source, first + std::chrono::milliseconds {16});
  EXPECT_EQ(session.in_flight(), 1u);
  ASSERT_TRUE(settle(session));
  EXPECT_TRUE(session.active());
}

TEST_F(FrameWarpSession, TheBuffersGoWhenTheClientStops) {
  auto asked = learner(true);
  if (!asked->params().enabled || asked->params().thumbnail_width == 0) {
    GTEST_SKIP() << "the learner has no thumbnail size to ask for";
  }
  platf::dxgi::frame_warp_session_t session {FRAME_WARP_TEST_SHADER_PATH};
  ASSERT_FALSE(session.attach(device.Get(), asked));
  session.converted(context.Get(), source, std::chrono::steady_clock::now());
  ASSERT_TRUE(session.has_pass());
  ASSERT_TRUE(settle(session));

  const frame_warp::request_t stop {false, std::nullopt, false, 256};
  asked->on_request(stop.encode());
  session.converted(context.Get(), source, std::chrono::steady_clock::now());
  EXPECT_FALSE(session.has_pass());
  EXPECT_TRUE(session.active()) << "it may be asked again";
}

TEST_F(FrameWarpSession, AShaderThatDoesNotCompileMeasuresNothing) {
  auto asked = learner(true);
  if (!asked->params().enabled || asked->params().thumbnail_width == 0) {
    GTEST_SKIP() << "the learner has no thumbnail size to ask for";
  }
  platf::dxgi::frame_warp_session_t session {L"does-not-exist/frame_warp_cs.hlsl"};
  ASSERT_FALSE(session.attach(device.Get(), asked));

  session.converted(context.Get(), source, std::chrono::steady_clock::now());
  const auto reason = session.submitted(context.Get());
  ASSERT_TRUE(reason);
  EXPECT_NE(reason->find("frame_warp_cs.hlsl"), std::string::npos) << *reason;
  EXPECT_FALSE(session.has_pass());
  EXPECT_FALSE(session.active());
  EXPECT_FALSE(session.submitted(context.Get())) << "reported once";
}
