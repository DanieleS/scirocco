// Packed 4:4:4 in a 4:2:0 frame ("chroma packing v1", see src/video_packed444.h).
//
// The coded picture is Wp x 2Hp. Pass Y (PACKED444_UV undefined) renders its Y plane, pass UV
// (PACKED444_UV defined) renders its Wp/2 x Hp chroma plane. Both passes draw over the whole
// plane (viewport at the origin), so SV_Position is the plane coordinate, and the letterbox
// is applied here instead of by the viewport: that way bars carry the code values a native 4:4:4
// stream would carry (black), also in the padding and in the packed chroma half.
//
// Logical pixel (x, y), edge replicated beyond the logical size:
//   Y plane, y <  Hp : Y(x, y)
//   Y plane, y >= Hp : j = y - Hp, i = x mod (Wp/2); x < Wp/2 ? U(2i + 1, j) : V(2i + 1, j)
//   UV plane (i, j)  : (U(2i, j), V(2i, j))
// The source is sampled at the centre of the logical pixel, one sample per code value: nothing is
// averaged across the packed sample regions.

Texture2D image : register(t0);
SamplerState def_sampler : register(s0);

cbuffer color_matrix_cbuffer : register(b0) {
    float4 color_vec_y;
    float4 color_vec_u;
    float4 color_vec_v;
    float2 range_y;
    float2 range_uv;
};

cbuffer packed444_cbuffer : register(b2) {
    float4 packed_size;     // logical width, logical height, padded width, padded height
    float4 packed_box;      // letterbox: offset x, offset y, width, height (logical pixels)
    int4 packed_rotation;   // x: 90-degree steps of the captured texture, 0..3
};

#include "include/base_vs_types.hlsl"

// Y, U and V code values (range applied) of one logical pixel.
float3 packed444_codes(int2 logical)
{
    logical = clamp(logical, int2(0, 0), int2(packed_size.xy) - 1);

    float2 pos = float2(logical) + 0.5;
    float2 box_max = packed_box.xy + packed_box.zw;

    float3 src = float3(0, 0, 0);
    if (all(pos >= packed_box.xy) && all(pos < box_max)) {
        float2 t = (pos - packed_box.xy) / packed_box.zw;

        // Same quarter turns as the rotating vertex shader of the 4:2:0 path.
        int steps = ((packed_rotation.x % 4) + 4) % 4;
        if (steps == 1) {
            t = float2(1 - t.y, t.x);
        } else if (steps == 2) {
            t = float2(1 - t.x, 1 - t.y);
        } else if (steps == 3) {
            t = float2(t.y, 1 - t.x);
        }

        src = image.SampleLevel(def_sampler, t, 0).rgb;
    }

    float3 rgb = CONVERT_FUNCTION(src);

    float y = dot(color_vec_y.xyz, rgb) + color_vec_y.w;
    float u = dot(color_vec_u.xyz, rgb) + color_vec_u.w;
    float v = dot(color_vec_v.xyz, rgb) + color_vec_v.w;

    return float3(y * range_y.x + range_y.y,
                  u * range_uv.x + range_uv.y,
                  v * range_uv.x + range_uv.y);
}

#ifdef PACKED444_UV

float2 main_ps(vertex_t input) : SV_Target
{
    int2 cell = int2(input.viewpoint_pos.xy);
    float3 yuv = packed444_codes(int2(2 * cell.x, cell.y));
    return yuv.yz;
}

#else

float main_ps(vertex_t input) : SV_Target
{
    int2 cell = int2(input.viewpoint_pos.xy);
    int padded_height = int(packed_size.w);
    if (cell.y < padded_height) {
        return packed444_codes(cell).x;
    }

    int half_width = int(packed_size.z) / 2;
    int j = cell.y - padded_height;
    int i = cell.x % half_width;
    int2 logical = int2(2 * i + 1, j);
    float3 yuv = packed444_codes(logical);
    return cell.x < half_width ? yuv.y : yuv.z;
}

#endif
