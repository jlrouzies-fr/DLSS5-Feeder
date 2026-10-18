// Elgato_Deband.fx -- debanding for 8-bit camera / capture feeds (Elgato Studio)
//
// Camera and capture paths into Elgato Studio are 8-bit and usually compressed (NV12 / MJPEG),
// so smooth walls, skin and sky show contour bands, and the DLSS-NR pass that runs after the
// feeder sharpens them further. This pass flattens only regions that are already flat:
//
//   * multi-radius probe: each iteration samples 4 points (a randomly rotated cross) at a
//     growing radius and keeps their average only while it stays within the threshold of the
//     centre AND the 4 samples agree with each other (edge guard); the first iteration that
//     fails stops the search, so detail and edges are never blurred;
//   * luma and chroma are tested separately (BT.709 YCbCr): compressed chroma bands at a much
//     smaller step than luma, and colour fringes are what makes banding obvious on skin;
//   * triangular (TPDF) dither of about one 8-bit step is added where the pass smoothed, so the
//     8-bit back buffer cannot quantise the new gradient back into bands.
//
// Place it BEFORE DLSS5_Feed in the technique order: the neural pass then sees clean gradients,
// and animated dither is averaged out by its temporal accumulation instead of being sharpened.
//
// SPDX-License-Identifier: CC0-1.0

#include "ReShade.fxh"

uniform float Threshold <
    ui_type = "slider";
    ui_label = "Luma threshold";
    ui_tooltip = "Largest luma difference still treated as banding (1.0 = full range).\n"
                 "One 8-bit step is 0.0039; compressed camera feeds band at 1-3 steps.";
    ui_min = 0.001; ui_max = 0.050; ui_step = 0.001;
> = 0.010;

uniform float ChromaThreshold <
    ui_type = "slider";
    ui_label = "Chroma threshold";
    ui_tooltip = "Same for Cb/Cr. Lower than luma: chroma is subsampled and bands in smaller steps.";
    ui_min = 0.001; ui_max = 0.050; ui_step = 0.001;
> = 0.006;

uniform float Range <
    ui_type = "slider";
    ui_label = "Search radius (pixels)";
    ui_tooltip = "Largest probe radius at the back-buffer resolution. 4K preview: 16-32.";
    ui_min = 2.0; ui_max = 64.0; ui_step = 1.0;
> = 24.0;

uniform int Iterations <
    ui_type = "slider";
    ui_label = "Iterations";
    ui_tooltip = "Probe rings between 1/N and the full radius. More = wider bands caught.";
    ui_min = 1; ui_max = 4;
> = 3;

uniform float EdgeGuard <
    ui_type = "slider";
    ui_label = "Edge guard";
    ui_tooltip = "How strictly the 4 samples must agree with each other.\n"
                 "1 = strict (only very flat areas), 0 = loose (up to 4x the threshold).";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.6;

uniform float Grain <
    ui_type = "slider";
    ui_label = "Dither (8-bit steps)";
    ui_tooltip = "TPDF dither added where the pass smoothed. 1.0 hides re-quantisation; 0 disables it.";
    ui_min = 0.0; ui_max = 4.0; ui_step = 0.05;
> = 1.0;

uniform bool AnimateNoise <
    ui_label = "Animate noise";
    ui_tooltip = "New probe rotation and dither every frame. Leave on before DLSS5_Feed (the neural\n"
                 "pass averages it out); turn off when this runs last, to avoid visible crawl.";
> = true;

uniform int DebugView <
    ui_type = "combo";
    ui_label = "Debug view";
    ui_items = "Off\0Smoothed areas (brighter = wider radius)\0";
> = 0;

uniform int FrameCount < source = "framecount"; >;

// Dave Hoskins' hash without sine: stable on every GPU, no precision banding of its own.
float Hash(float2 p)
{
    float3 p3 = frac(p.xyx * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 ToYCC(float3 c)
{
    float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    return float3(y, (c.b - y) * 0.5389, (c.r - y) * 0.6350);
}

float3 FromYCC(float3 v)
{
    float r = v.x + v.z / 0.6350;
    float b = v.x + v.y / 0.5389;
    float g = (v.x - 0.2126 * r - 0.0722 * b) / 0.7152;
    return float3(r, g, b);
}

float3 FetchYCC(float2 uv)
{
    return ToYCC(tex2Dlod(ReShade::BackBuffer, float4(uv, 0.0, 0.0)).rgb);
}

bool WithinThreshold(float3 d, float scale)
{
    return d.x < Threshold * scale && max(d.y, d.z) < ChromaThreshold * scale;
}

float3 PS_ElgatoDeband(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float3 src = tex2D(ReShade::BackBuffer, uv).rgb;
    float3 centre = ToYCC(src);
    float3 result = centre;
    float mask = 0.0;

    float seed = AnimateNoise ? float(uint(FrameCount) % 1024u) * 1.61803 : 0.0;
    float guard = lerp(4.0, 1.0, EdgeGuard);

    [loop]
    for (int i = 1; i <= Iterations; i++)
    {
        float fi = float(i);
        float radius = Range * fi / float(Iterations) * lerp(0.5, 1.0, Hash(pos.xy + fi * 17.0 + seed));
        float angle = 6.2831853 * Hash(pos.yx * 1.37 + fi * 7.1 + seed);
        float2 d = float2(cos(angle), sin(angle)) * radius;
        float2 e = float2(-d.y, d.x);
        d *= float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
        e *= float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);

        float3 s0 = FetchYCC(uv + d);
        float3 s1 = FetchYCC(uv - d);
        float3 s2 = FetchYCC(uv + e);
        float3 s3 = FetchYCC(uv - e);
        float3 avg = (s0 + s1 + s2 + s3) * 0.25;

        float3 spread = max(max(abs(s0 - avg), abs(s1 - avg)), max(abs(s2 - avg), abs(s3 - avg)));
        if (!WithinThreshold(abs(centre - avg), 1.0) || !WithinThreshold(spread, guard))
            break;

        // average with the centre so a single ring cannot erase fine texture inside a band
        result = (avg * 4.0 + centre) * 0.2;
        mask = fi / float(Iterations);
    }

    if (DebugView == 1)
        return lerp(src * 0.35, float3(0.2, 1.0, 0.4), mask);

    float3 rgb = FromYCC(result);
    if (mask > 0.0 && Grain > 0.0)
    {
        float2 p = pos.xy + seed * 13.0;
        float tpdf = Hash(p) + Hash(p + 71.3) - 1.0;   // triangular, -1..1
        rgb += tpdf * Grain / 255.0;
    }
    return saturate(rgb);
}

technique Elgato_Deband <
    ui_label = "Elgato Deband";
    ui_tooltip = "Debanding for 8-bit compressed camera / capture feeds. Put it before DLSS5_Feed.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_ElgatoDeband;
    }
}
