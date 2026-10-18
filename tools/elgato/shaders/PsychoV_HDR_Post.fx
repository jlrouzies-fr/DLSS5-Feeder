// PsychoV-style HDR post-process for ReShade
// Target: HDR10/PQ input and output (BT.2020 assumed).
//
// IMPORTANT LIMITATION:
// This is a standalone post-process adaptation, NOT a bit-exact port of RenoDX
// PsychoV-17. RenoDX PsychoV expects linear scene-referred input before the
// game's native tonemapper. A normal ReShade FX pass usually sees the completed
// frame, so the original scene values cannot be recovered exactly.
//
// Use only if ReShade's sampled backbuffer is actually PQ-encoded HDR10.
// If your ReShade buffer is scRGB/linear or SDR, disable this shader: the input
// decoding will be wrong. Also ensure ReShade's output path preserves HDR.
//
// Place in: reshade-shaders/Shaders/PsychoV_HDR_Post.fx
// Requires a current ReShade version supporting COLOR semantic and tex2D.
//
// SPDX-License-Identifier: CC0-1.0

#include "ReShade.fxh"

uniform float Exposure <
    ui_type = "slider";
    ui_label = "Exposure";
    ui_min = -4.0; ui_max = 4.0; ui_step = 0.05;
> = 0.0;

uniform float PeakLuminance <
    ui_type = "slider";
    ui_label = "Target peak (nits)";
    ui_min = 400.0; ui_max = 4000.0; ui_step = 50.0;
> = 1000.0;

uniform float PaperWhite <
    ui_type = "slider";
    ui_label = "Paper white (nits)";
    ui_min = 80.0; ui_max = 500.0; ui_step = 5.0;
> = 203.0;

uniform float Contrast <
    ui_type = "slider";
    ui_label = "Perceptual contrast";
    ui_min = 0.50; ui_max = 1.50; ui_step = 0.01;
> = 1.0;

uniform float HighlightRollOff <
    ui_type = "slider";
    ui_label = "Highlight roll-off";
    ui_min = 0.25; ui_max = 3.0; ui_step = 0.01;
> = 1.0;

uniform float Saturation <
    ui_type = "slider";
    ui_label = "Saturation";
    ui_min = 0.0; ui_max = 1.5; ui_step = 0.01;
> = 1.0;

uniform float Strength <
    ui_type = "slider";
    ui_label = "Effect strength";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 1.0;

// ST.2084 / PQ constants.
static const float PQ_m1 = 0.1593017578125;
static const float PQ_m2 = 78.84375;
static const float PQ_c1 = 0.8359375;
static const float PQ_c2 = 18.8515625;
static const float PQ_c3 = 18.6875;
static const float MAX_PQ_NITS = 10000.0;

float3 PQToNits(float3 pq)
{
    float3 p = pow(saturate(pq), 1.0 / PQ_m2);
    float3 num = max(p - PQ_c1, 0.0);
    float3 den = max(PQ_c2 - PQ_c3 * p, 1e-6);
    return pow(num / den, 1.0 / PQ_m1) * MAX_PQ_NITS;
}

float3 NitsToPQ(float3 nits)
{
    float3 y = saturate(max(nits, 0.0) / MAX_PQ_NITS);
    float3 p = pow(y, PQ_m1);
    return pow((PQ_c1 + PQ_c2 * p) / (1.0 + PQ_c3 * p), PQ_m2);
}

// BT.2020 luminance coefficients.
float Luma2020(float3 c)
{
    return dot(c, float3(0.2627, 0.6780, 0.0593));
}

// Smoothly compress luminance above paper white toward target peak.
// This is a practical display-referred fallback curve, not RenoDX PsychoV17.
float MapLuminance(float y)
{
    float paper = max(PaperWhite, 1.0);
    float peak = max(PeakLuminance, paper + 1.0);
    float exposure = exp2(Exposure);
    float x = max(y * exposure, 0.0) / paper;
    float peakRatio = (peak - paper) / paper;
    float k = max(HighlightRollOff, 0.001);

    // Keeps near-black and paper-white stable; compresses highlights smoothly.
    float mapped = x <= 1.0
        ? pow(max(x, 0.0), max(Contrast, 0.001))
        : 1.0 + peakRatio * (1.0 - exp(-(x - 1.0) * k / max(peakRatio, 0.001)));

    return min(mapped * paper, peak);
}

float3 PS_PsychoV_HDR(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 srcPQ = tex2D(ReShade::BackBuffer, texcoord).rgb;
    float3 srcNits = PQToNits(srcPQ);

    float y = max(Luma2020(srcNits), 1e-5);
    float mappedY = MapLuminance(y);

    // Preserve channel ratios while mapping luminance; then gently control saturation.
    float3 mapped = srcNits * (mappedY / y);
    float mappedLuma = Luma2020(mapped);
    float3 gray = mappedLuma.xxx;
    mapped = lerp(gray, mapped, Saturation);

    float3 outPQ = NitsToPQ(max(mapped, 0.0));
    return float4(lerp(srcPQ, outPQ, Strength), 1.0);
}

technique PsychoV_HDR_Post
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_PsychoV_HDR;
    }
}
