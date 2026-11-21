// Frame Warp's passes over a capture (src/platform/windows/frame_warp_d3d11.h): a luma
// thumbnail of the whole capture for the host's motion measurement and, while the camera
// turns, the HUD: the pixels that stay the same within a small tolerance, scored per mask
// cell. Both read the capture as the conversion read it.
//
// Ported from RamazanKara/Rubylight rust/windows/src/shaders/color.hlsl (frame_warp_*_cs,
// commits aa1d312bf and 79921e3b9). One entry point serves all four passes, selected by
// `fw_pass`, so the host compiles and keeps a single blob:
//   0 thumbnail  one thread per four thumbnail pixels, a byte each in one word
//   1 score      one thread per mask cell: compares the cell's pixels with the codes of the
//                earlier capture and updates the cell's score
//   2 codes      one thread per pixel: keeps the capture's codes for the next comparison
//   3 bits       one thread per word of the mask: a bit per cell that reached fw_threshold
//
// Unlike the Rust, the grid covers the whole stream picture and the capture lies in
// `fw_content` of it: a cell that reaches into a bar is HUD (so bars hold still and nothing
// is shifted in from them); every other cell scores the capture pixels that show in it. The
// reduction is a gather per cell, so no pass needs atomics or a counts buffer.

#include "include/common.hlsl"

static const uint PASS_THUMBNAIL = 0;
static const uint PASS_SCORE = 1;
static const uint PASS_CODES = 2;
static const uint PASS_BITS = 3;

// The capture's pixel kinds (frame_warp_d3d11_t::transfer_e).
static const uint PIXEL_SDR = 0;  // UNORM display-referred values, 8 bits a channel
static const uint PIXEL_SCRGB = 1;  // FP16 linear scRGB, taken as PQ of scRGB / 125
static const uint PIXEL_PQ = 2;  // UNORM PQ codes (HDR10), 10 bits a channel

// How far, in 10-bit steps, a channel may move between passes with its pixel still counted
// as the same: 8 is 2/255. Games add faint noise or dither to their HUD.
static const uint TOLERANCE = 8;

Texture2D image : register(t0);

cbuffer frame_warp_params_cbuffer : register(b0) {
    uint2 fw_source;  // capture texels across and down
    uint2 fw_thumb;  // thumbnail pixels across (a multiple of 4) and down
    uint2 fw_grid;  // mask cells across and down, over the stream picture
    uint2 fw_picture;  // stream picture pixels
    uint4 fw_content;  // where the capture shows in the picture: x, y, width, height
    uint fw_pixel;
    uint fw_pass;
    uint fw_stride;  // mask words per row
    uint fw_threshold;  // score from which a cell is HUD
};

// The thumbnail's words; the HUD's code per pixel; its score per cell; the mask bits.
RWStructuredBuffer<uint> thumbnail : register(u0);
RWStructuredBuffer<uint> codes : register(u1);
RWStructuredBuffer<uint> scores : register(u2);
RWStructuredBuffer<uint> bits : register(u3);

// A captured pixel's channels, 0 to 1 each: the codes of an SDR or HDR10 capture as they
// are, PQ of scRGB / 125 for scRGB.
float3 fw_channels(uint2 p)
{
    float3 rgb = image.Load(int3(p, 0)).rgb;
    if (fw_pixel == PIXEL_SCRGB) {
        rgb = NitsToPQ(max(rgb, 0.0) * 80.0);
    }
    return rgb;
}

float fw_luma(float3 channels)
{
    if (fw_pixel == PIXEL_PQ) {
        return dot(channels, float3(0.2627, 0.6780, 0.0593));
    }
    return dot(channels, float3(0.2126, 0.7152, 0.0722));
}

// Four thumbnail pixels a thread, a byte each in one word, rows top to bottom: the mean of
// at most 8x8 samples spread over each pixel's box of the capture.
void fw_thumbnail(uint2 id)
{
    uint words = fw_thumb.x / 4;
    if (id.x >= words || id.y >= fw_thumb.y) {
        return;
    }
    uint y0 = id.y * fw_source.y / fw_thumb.y;
    uint span_y = max((id.y + 1) * fw_source.y / fw_thumb.y, y0 + 1) - y0;
    uint ny = min(span_y, 8u);
    uint quad = 0;
    for (uint i = 0; i < 4; i++) {
        uint x0 = (id.x * 4 + i) * fw_source.x / fw_thumb.x;
        uint span_x = max((id.x * 4 + i + 1) * fw_source.x / fw_thumb.x, x0 + 1) - x0;
        uint nx = min(span_x, 8u);
        float sum = 0.0;
        for (uint sy = 0; sy < ny; sy++) {
            uint y = y0 + (2 * sy + 1) * span_y / (2 * ny);
            for (uint sx = 0; sx < nx; sx++) {
                uint x = x0 + (2 * sx + 1) * span_x / (2 * nx);
                sum += fw_luma(fw_channels(uint2(x, y)));
            }
        }
        quad |= uint(saturate(sum / float(nx * ny)) * 255.0 + 0.5) << (8 * i);
    }
    thumbnail[id.y * words + id.x] = quad;
}

// A captured pixel's channels, 10 bits each in one word. 8-bit codes land on every fourth
// step, so 2/255 is exactly TOLERANCE; 10-bit codes land on every step.
uint fw_code(float3 channels)
{
    uint3 steps = uint3(saturate(channels) * (fw_pixel == PIXEL_SDR ? 1020.0 : 1023.0) + 0.5);
    return steps.r | (steps.g << 10) | (steps.b << 20);
}

int3 fw_split(uint code)
{
    return int3(int(code & 1023u), int((code >> 10) & 1023u), int((code >> 20) & 1023u));
}

// Whether any channel of two codes differs by more than TOLERANCE.
bool fw_changed(uint a, uint b)
{
    return any(abs(fw_split(a) - fw_split(b)) > int3(TOLERANCE, TOLERANCE, TOLERANCE));
}

// Whether the step from code a to its neighbour b is the step from c to d, the same two
// pixels in the earlier pass: every channel within TOLERANCE or half the step.
bool fw_same_step(uint a, uint b, uint c, uint d)
{
    int3 now = fw_split(a) - fw_split(b);
    int3 then = fw_split(c) - fw_split(d);
    return all(abs(now - then) <= max(int3(TOLERANCE, TOLERANCE, TOLERANCE), abs(now) / 2));
}

// Whether cell `cell` of `cells` along one axis of the picture reaches past the capture's
// `length` pixels from `start`, into a bar.
bool fw_reaches_bar(uint cell, uint cells, uint picture, uint start, uint length)
{
    uint a = cell * picture / cells;
    uint b = (cell + 1) * picture / cells;
    return length == 0 || a < start || b > start + length;
}

// The capture pixels [lo, hi) along one axis that show in a cell that does not reach a bar:
// at least one, also when the capture is shown larger than it is.
uint2 fw_span(uint cell, uint cells, uint picture, uint start, uint length, uint size)
{
    uint a = cell * picture / cells;
    uint b = (cell + 1) * picture / cells;
    uint lo = min(((a - start) * size + length - 1) / length, size - 1);
    uint hi = min(max(((b - start) * size + length - 1) / length, lo + 1), size);
    return uint2(lo, hi);
}

// Per HUD cell, as the camera turns, over the capture pixels in it, each compared with the
// code the earlier capture left there: one where a quarter of the pixels or more changed
// loses half its score (a fade, a flash, the scene). Otherwise a cell with too few textured
// pixels (a code that differs by more than TOLERANCE from its right or lower neighbour, its
// left or upper one at the capture's edge) keeps its score: a smooth sky or fog looks the
// same moved or not, so whether it stayed tells nothing. A textured cell gains a point, up
// to 255, when no more than a quarter of its textured pixels changed, and loses half
// otherwise. A pixel textured in both captures whose steps to those neighbours stayed the
// same counts as unchanged: the edge of a translucent HUD keeps its place and its step while
// its value follows the scene behind it.
void fw_score(uint2 id)
{
    if (any(id >= fw_grid)) {
        return;
    }
    if (fw_reaches_bar(id.x, fw_grid.x, fw_picture.x, fw_content.x, fw_content.z)
        || fw_reaches_bar(id.y, fw_grid.y, fw_picture.y, fw_content.y, fw_content.w)) {
        return;
    }
    uint2 xs = fw_span(id.x, fw_grid.x, fw_picture.x, fw_content.x, fw_content.z, fw_source.x);
    uint2 ys = fw_span(id.y, fw_grid.y, fw_picture.y, fw_content.y, fw_content.w, fw_source.y);
    uint total = (xs.y - xs.x) * (ys.y - ys.x);
    uint differing = 0;
    uint textured = 0;
    uint moved = 0;
    for (uint y = ys.x; y < ys.y; y++) {
        // A pixel with no neighbour (a capture one pixel wide or high) is its own, which
        // never differs.
        uint below = y + 1 < fw_source.y ? y + 1 : (y > 0 ? y - 1 : y);
        for (uint x = xs.x; x < xs.y; x++) {
            uint beside = x + 1 < fw_source.x ? x + 1 : (x > 0 ? x - 1 : x);
            uint code = fw_code(fw_channels(uint2(x, y)));
            uint code_beside = fw_code(fw_channels(uint2(beside, y)));
            uint code_below = fw_code(fw_channels(uint2(x, below)));
            uint then = codes[y * fw_source.x + x];
            uint then_beside = codes[y * fw_source.x + beside];
            uint then_below = codes[below * fw_source.x + x];
            bool changed = fw_changed(then, code);
            bool has_texture = fw_changed(code, code_beside) || fw_changed(code, code_below);
            if (has_texture) {
                if (changed
                    && (fw_changed(then, then_beside) || fw_changed(then, then_below))
                    && fw_same_step(code, code_beside, then, then_beside)
                    && fw_same_step(code, code_below, then, then_below)) {
                    changed = false;
                }
                textured++;
                if (changed) {
                    moved++;
                }
            }
            if (changed) {
                differing++;
            }
        }
    }
    uint cell = id.y * fw_grid.x + id.x;
    uint score = scores[cell];
    if (differing * 4 > total) {
        score /= 2;
    } else if (textured * 8 < total) {
        return;
    } else {
        score = moved * 4 <= textured ? min(score + 1, 255u) : score / 2;
    }
    scores[cell] = score;
}

// Keeps every pixel's code for the next comparison.
void fw_codes(uint2 id)
{
    if (any(id >= fw_source)) {
        return;
    }
    codes[id.y * fw_source.x + id.x] = fw_code(fw_channels(id));
}

// The mask: a bit per cell that reached fw_threshold or reaches into a bar, bit i of word w
// being column 32w + i, rows of fw_stride words.
void fw_bits(uint2 id)
{
    if (id.x >= fw_stride || id.y >= fw_grid.y) {
        return;
    }
    bool bar_row = fw_reaches_bar(id.y, fw_grid.y, fw_picture.y, fw_content.y, fw_content.w);
    uint word = 0;
    for (uint bit = 0; bit < 32; bit++) {
        uint x = id.x * 32 + bit;
        if (x >= fw_grid.x) {
            break;
        }
        bool is_hud = bar_row
            || fw_reaches_bar(x, fw_grid.x, fw_picture.x, fw_content.x, fw_content.z)
            || scores[id.y * fw_grid.x + x] >= fw_threshold;
        if (is_hud) {
            word |= 1u << bit;
        }
    }
    bits[id.y * fw_stride + id.x] = word;
}

[numthreads(8, 8, 1)]
void main_cs(uint3 id : SV_DispatchThreadID)
{
    [branch] if (fw_pass == PASS_THUMBNAIL) {
        fw_thumbnail(id.xy);
    } else if (fw_pass == PASS_SCORE) {
        fw_score(id.xy);
    } else if (fw_pass == PASS_CODES) {
        fw_codes(id.xy);
    } else {
        fw_bits(id.xy);
    }
}
