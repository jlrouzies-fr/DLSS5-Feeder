// PsychoV-style post-process for SDR back buffers (sRGB / gamma 2.2, BT.709)
//
// Twin of PsychoV_HDR_Post.fx for swap chains that are NOT HDR10: Elgato Studio 2.0.0.3184 presents
// 8-bit B8G8R8A8 sRGB, where the HDR version's PQ decode would be wrong. Same controls and the same
// idea -- map luminance only (channel ratios preserved), contrast below paper white, a soft
// shoulder above it, then saturation -- with the shoulder rolling into SDR white instead of a nit
// peak. Like the HDR one it is a display-referred adaptation, not a port of RenoDX PsychoV-17
// (that needs scene-linear input from before the game's tonemapper).
//
// SPDX-License-Identifier: CC0-1.0

#include "ReShade.fxh"

uniform int Transfer <
    ui_type = "combo";
    ui_label = "Back buffer encoding";
    ui_items = "Gamma 2.2\0sRGB piecewise\0";
    ui_tooltip = "How to decode the 8-bit buffer to linear. Windows SDR desktops are gamma 2.2 in practice.";
> = 0;

uniform float Exposure <
    ui_type = "slider";
    ui_label = "Exposure";
    ui_min = -4.0; ui_max = 4.0; ui_step = 0.05;
> = 0.0;

uniform float Contrast <
    ui_type = "slider";
    ui_label = "Perceptual contrast";
    ui_min = 0.50; ui_max = 1.50; ui_step = 0.01;
> = 1.0;

uniform float ShoulderStart <
    ui_type = "slider";
    ui_label = "Shoulder start";
    ui_tooltip = "Linear level where highlights start rolling off towards white (1.0 = no shoulder).";
    ui_min = 0.40; ui_max = 1.00; ui_step = 0.01;
> = 0.75;

uniform float HighlightRollOff <
    ui_type = "slider";
    ui_label = "Highlight roll-off";
    ui_tooltip = "How fast the shoulder approaches white. Higher keeps more highlight contrast.";
    ui_min = 0.10; ui_max = 4.00; ui_step = 0.05;
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

float3 Decode(float3 c)
{
    c = saturate(c);
    if (Transfer == 1)
        return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    return pow(c, 2.2);
}

float3 Encode(float3 c)
{
    c = saturate(c);
    if (Transfer == 1)
        return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return pow(c, 1.0 / 2.2);
}

float Luma709(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Contrast around SDR white below the shoulder, exponential shoulder into 1.0 above it.
// Continuous at the shoulder in value and slope (the shoulder starts with the curve's slope).
float MapLuminance(float y)
{
    float x = max(y * exp2(Exposure), 0.0);
    float c = max(Contrast, 0.001);
    float s = clamp(ShoulderStart, 0.4, 1.0);
    float base = pow(x, c);
    if (s >= 0.999 || x <= s)
        return min(base, 1.0);
    float ys = pow(s, c);                   // value at the shoulder
    float slope = c * pow(s, c - 1.0);      // slope at the shoulder
    float room = max(1.0 - ys, 1e-4);
    float k = max(HighlightRollOff, 0.01) * slope / room;
    return ys + room * (1.0 - exp(-(x - s) * k));
}

float3 PS_PsychoV_SDR(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 src = tex2D(ReShade::BackBuffer, texcoord).rgb;
    float3 lin = Decode(src);

    float y = max(Luma709(lin), 1e-5);
    float3 mapped = lin * (MapLuminance(y) / y);

    // channel ratios can push one channel past white: pull it back towards grey, not clip it
    float peak = max(max(mapped.r, mapped.g), mapped.b);
    float my = Luma709(mapped);
    if (peak > 1.0)
        mapped = lerp(my.xxx, mapped, saturate((1.0 - my) / max(peak - my, 1e-5)));

    mapped = lerp(Luma709(mapped).xxx, mapped, Saturation);
    return lerp(src, Encode(mapped), Strength);
}

technique PsychoV_SDR_Post <
    ui_label = "PsychoV SDR Post";
    ui_tooltip = "PsychoV-style luminance mapping for SDR (gamma 2.2 / sRGB) back buffers.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_PsychoV_SDR;
    }
}
