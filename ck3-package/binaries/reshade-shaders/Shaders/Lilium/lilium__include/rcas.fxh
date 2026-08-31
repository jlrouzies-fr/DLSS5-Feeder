// Original functions are part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//_____________________________________________________________/\_______________________________________________________________
//==============================================================================================================================
//
//                                      FSR - [RCAS] ROBUST CONTRAST ADAPTIVE SHARPENING
//
//------------------------------------------------------------------------------------------------------------------------------
// CAS uses a simplified mechanism to convert local contrast into a variable amount of sharpness.
// RCAS uses a more exact mechanism, solving for the maximum local sharpness possible before clipping.
// RCAS also has a built in process to limit sharpening of what it detects as possible noise.
// RCAS sharper does not support scaling, as it should be applied after EASU scaling.
// Pass EASU output straight into RCAS, no color conversions necessary.
//------------------------------------------------------------------------------------------------------------------------------
// RCAS is based on the following logic.
// RCAS uses a 5 tap filter in a cross pattern (same as CAS),
//    w                n
//  w 1 w  for taps  w m e
//    w                s
// Where 'w' is the negative lobe weight.
//  output = (w*(n+e+w+s)+m)/(4*w+1)
// RCAS solves for 'w' by seeing where the signal might clip out of the {0 to 1} input range,
//  0 == (w*(n+e+w+s)+m)/(4*w+1) -> w = -m/(n+e+w+s)
//  1 == (w*(n+e+w+s)+m)/(4*w+1) -> w = (1-m)/(n+e+w+s-4*1)
// Then chooses the 'w' which results in no clipping, limits 'w', and multiplies by the 'sharp' amount.
// This solution above has issues with MSAA input as the steps along the gradient cause edge detection issues.
// So RCAS uses 4x the maximum and 4x the minimum (depending on equation) in place of the individual taps.
// As well as switching from 'm' to either the minimum or maximum (depending on side), to help in energy conservation.
// This stabilizes RCAS.
// RCAS does a simple highpass which is normalized against the local contrast then shaped,
//       0.25
//  0.25  -1  0.25
//       0.25
// This is used as a noise detection filter, to reduce the effect of RCAS on grain, and focus on real edges.
//
//  GLSL example for the required callbacks :
//
//  FfxFloat16x4 FsrRcasLoadH(FfxInt16x2 p){return FfxFloat16x4(imageLoad(imgSrc,FfxInt32x2(p)));}
//  void FsrRcasInputH(inout FfxFloat16 r,inout FfxFloat16 g,inout FfxFloat16 b)
//  {
//    //do any simple input color conversions here or leave empty if none needed
//  }
//
//  FsrRcasCon need to be called from the CPU or GPU to set up constants.
//  Including a GPU example here, the 'con' value would be stored out to a constant buffer.
//
//  FfxUInt32x4 con;
//  FsrRcasCon(con,
//   0.0); // The scale is {0.0 := maximum sharpness, to N>0, where N is the number of stops (halving) of the reduction of sharpness}.
// ---------------
// RCAS sharpening supports a CAS-like pass-through alpha via,
//  #define FSR_RCAS_PASSTHROUGH_ALPHA 1
// RCAS also supports a define to enable a more expensive path to avoid some sharpening of noise.
// Would suggest it is better to apply film grain after RCAS sharpening (and after scaling) instead of using this define,
//  #define FSR_RCAS_DENOISE 1


#include "include_main.fxh"


HDR10_TO_LINEAR_LUT()


uniform uint RCAS_MODE
<
  ui_label   = "RCAS mode";
  ui_tooltip = "to do the sharpening:"
          "\n" " - luminance uses the relative luminance of linearised RGB values"
          "\n" " - linear uses linearised RGB values"
          "\n" " - RGB (linear) uses linearised RGB values"
          "\n" " - RGB (shaped) and original use \"in-gamma\" RGB values"
          "\n"
          "\n" "for the noise removal calculation:"
          "\n" " - luminance uses the already calculated relative luminance"
          "\n" " - linear uses the relative luminance"
          "\n" " - gamma uses luma"
          "\n" " - original uses an approximated luma";
  ui_type    = "combo";
  ui_items   = "luminance\0"
               "RGB (linear)\0"
               "RGB (shaped)\0"
               "original\0";
> = 0u;

#define RCAS_MODE_LUMINANCE  0u
#define RCAS_MODE_RGB_LINEAR 1u
#define RCAS_MODE_RGB_SHAPED 2u
#define RCAS_MODE_ORIGINAL   3u

uniform float SHARPEN_AMOUNT
<
  ui_label   = "sharpness amount";
  ui_tooltip = "Even a value of 0 applies a bit of sharpness!";
  ui_type    = "drag";
  ui_min     = 0.f;
  ui_max     = 1.f;
  ui_step    = 0.001f;
> = 0.5f;

uniform bool NOISE_REMOVAL_ENABLE
<
  ui_label   = "enable noise removal";
  ui_tooltip = "Makes high frequency detail look less oversharpened and suppresses ringing and halos a bit."
          "\n" "Can also remove film grain and other noise!";
  ui_type    = "radio";
> = true;


void Preprocess_RGB
(
  inout float3 B,
  inout float3 D,
  inout float3 E,
  inout float3 F,
  inout float3 H
)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  B = Csp::Mat::scRGB_To::BT2020_normalised(B);
  D = Csp::Mat::scRGB_To::BT2020_normalised(D);
  E = Csp::Mat::scRGB_To::BT2020_normalised(E);
  F = Csp::Mat::scRGB_To::BT2020_normalised(F);
  H = Csp::Mat::scRGB_To::BT2020_normalised(H);

  BRANCH()
  if (RCAS_MODE == RCAS_MODE_RGB_SHAPED)
  {
    B = Csp::Trc::Linear_To::PQ(B);
    D = Csp::Trc::Linear_To::PQ(D);
    E = Csp::Trc::Linear_To::PQ(E);
    F = Csp::Trc::Linear_To::PQ(F);
    H = Csp::Trc::Linear_To::PQ(H);
  }

#endif // ACTUAL_COLOUR_SPACE == CSP_SCRGB

  return;
}

float3 Process_RGB_For_Output
(
  float3 RGB
)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  BRANCH()
  if (RCAS_MODE == RCAS_MODE_RGB_SHAPED)
  {
    RGB = Csp::Trc::PQ_To::Linear(RGB);
  }

  RGB = Csp::Mat::BT2020_normalised_To::scRGB(RGB);

#endif // ACTUAL_COLOUR_SPACE == CSP_SCRGB

  return RGB;
}

void Get_Luminance
(
  in    float3 B,
  in    float3 D,
  inout float3 E,
  in    float3 F,
  in    float3 H,
  out   float  B_lum,
  out   float  D_lum,
  out   float  E_lum,
  out   float  F_lum,
  out   float  H_lum
)
{
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)

  B = FetchFromHdr10ToLinearLUT(B);
  D = FetchFromHdr10ToLinearLUT(D);
  E = FetchFromHdr10ToLinearLUT(E);
  F = FetchFromHdr10ToLinearLUT(F);
  H = FetchFromHdr10ToLinearLUT(H);

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

  B = DECODE_SDR(B);
  D = DECODE_SDR(D);
  E = DECODE_SDR(E);
  F = DECODE_SDR(F);
  H = DECODE_SDR(H);

#elif (ACTUAL_COLOUR_SPACE != CSP_SCRGB) // fallback for shader permutations

  B = 0.f;
  D = 0.f;
  E = 0.f;
  F = 0.f;
  H = 0.f;

#endif // ACTUAL_COLOUR_SPACE == CSP_HDR10

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  B_lum = dot(B, Csp::Mat::scRGB_To_XYZ_normalised[1]);
  D_lum = dot(D, Csp::Mat::scRGB_To_XYZ_normalised[1]);
  E_lum = dot(E, Csp::Mat::scRGB_To_XYZ_normalised[1]);
  F_lum = dot(F, Csp::Mat::scRGB_To_XYZ_normalised[1]);
  H_lum = dot(H, Csp::Mat::scRGB_To_XYZ_normalised[1]);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

  B_lum = dot(B, Csp::Mat::BT2020_To_XYZ[1]);
  D_lum = dot(D, Csp::Mat::BT2020_To_XYZ[1]);
  E_lum = dot(E, Csp::Mat::BT2020_To_XYZ[1]);
  F_lum = dot(F, Csp::Mat::BT2020_To_XYZ[1]);
  H_lum = dot(H, Csp::Mat::BT2020_To_XYZ[1]);

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

  B_lum = dot(B, Csp::Mat::BT709_To_XYZ[1]);
  D_lum = dot(D, Csp::Mat::BT709_To_XYZ[1]);
  E_lum = dot(E, Csp::Mat::BT709_To_XYZ[1]);
  F_lum = dot(F, Csp::Mat::BT709_To_XYZ[1]);
  H_lum = dot(H, Csp::Mat::BT709_To_XYZ[1]);

#else // fallback for shader permutations

  B_lum = 0.f;
  D_lum = 0.f;
  E_lum = 0.f;
  F_lum = 0.f;
  H_lum = 0.f;

#endif // ACTUAL_COLOUR_SPACE == CSP_SCRGB

  return;
}

void Get_Luma
(
  const float3 B,
  const float3 D,
  const float3 E,
  const float3 F,
  const float3 H,
  out   float  B_luma,
  out   float  D_luma,
  out   float  E_luma,
  out   float  F_luma,
  out   float  H_luma
)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB \
  || ACTUAL_COLOUR_SPACE == CSP_HDR10)

  B_luma = dot(B, Csp::Mat::BT2020_To_XYZ[1]);
  D_luma = dot(D, Csp::Mat::BT2020_To_XYZ[1]);
  E_luma = dot(E, Csp::Mat::BT2020_To_XYZ[1]);
  F_luma = dot(F, Csp::Mat::BT2020_To_XYZ[1]);
  H_luma = dot(H, Csp::Mat::BT2020_To_XYZ[1]);

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

  B_luma = dot(B, Csp::Mat::BT709_To_XYZ[1]);
  D_luma = dot(D, Csp::Mat::BT709_To_XYZ[1]);
  E_luma = dot(E, Csp::Mat::BT709_To_XYZ[1]);
  F_luma = dot(F, Csp::Mat::BT709_To_XYZ[1]);
  H_luma = dot(H, Csp::Mat::BT709_To_XYZ[1]);

#else // fallback for shader permutations

  B_luma = 0.f;
  D_luma = 0.f;
  E_luma = 0.f;
  F_luma = 0.f;
  H_luma = 0.f;

#endif // ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10

  return;
}

void Get_Luma_2x
(
  const float3 B,
  const float3 D,
  const float3 E,
  const float3 F,
  const float3 H,
  out   float  B_luma_2x,
  out   float  D_luma_2x,
  out   float  E_luma_2x,
  out   float  F_luma_2x,
  out   float  H_luma_2x
)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB \
  || ACTUAL_COLOUR_SPACE == CSP_HDR10)

  B_luma_2x = dot(B, Csp::Mat::BT2020_To_XYZ[1] * 2.f);
  D_luma_2x = dot(D, Csp::Mat::BT2020_To_XYZ[1] * 2.f);
  E_luma_2x = dot(E, Csp::Mat::BT2020_To_XYZ[1] * 2.f);
  F_luma_2x = dot(F, Csp::Mat::BT2020_To_XYZ[1] * 2.f);
  H_luma_2x = dot(H, Csp::Mat::BT2020_To_XYZ[1] * 2.f);

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

  B_luma_2x = dot(B, Csp::Mat::BT709_To_XYZ[1] * 2.f);
  D_luma_2x = dot(D, Csp::Mat::BT709_To_XYZ[1] * 2.f);
  E_luma_2x = dot(E, Csp::Mat::BT709_To_XYZ[1] * 2.f);
  F_luma_2x = dot(F, Csp::Mat::BT709_To_XYZ[1] * 2.f);
  H_luma_2x = dot(H, Csp::Mat::BT709_To_XYZ[1] * 2.f);

#else // fallback for shader permutations

  B_luma_2x = 0.f;
  D_luma_2x = 0.f;
  E_luma_2x = 0.f;
  F_luma_2x = 0.f;
  H_luma_2x = 0.f;

#endif

  return;
}


float3 RCas
(
  const int2  Position,
  const float Sharpness
)
{
  // Algorithm uses minimal 3x3 pixel neighborhood.
  //    b
  //  d e f
  //    h

  float3 e = tex2Dfetch(SamplerBackBuffer,    Position).rgb;

  int4 bd_position;
  bd_position = Position.xyxy + int4(0, -1, -1,  0);
  bd_position = clamp(bd_position, int4(0, 0, 0, 0), int4(BUFFER_SIZE_MINUS_1_INT, BUFFER_SIZE_MINUS_1_INT));

  float3 d = tex2Dfetch(SamplerBackBuffer, bd_position.zw).rgb;
  float3 b = tex2Dfetch(SamplerBackBuffer, bd_position.xy).rgb;

  int4 fh_position;
  fh_position = Position.xyxy + int4(1,  0,  0,  1);
  fh_position = clamp(fh_position, int4(0, 0, 0, 0), int4(BUFFER_SIZE_MINUS_1_INT, BUFFER_SIZE_MINUS_1_INT));

  float3 f = tex2Dfetch(SamplerBackBuffer, fh_position.xy).rgb;
  float3 h = tex2Dfetch(SamplerBackBuffer, fh_position.zw).rgb;

  // Immediate constants for peak range.
  static const float2 peak_c = float2(1.f, -4.f);

  float lobe_local;

  float b_lum, d_lum, e_lum, f_lum, h_lum;

  BRANCH()
  if (RCAS_MODE == RCAS_MODE_LUMINANCE)
  {
    Get_Luminance(b,     d,     e,     f,     h,
                  b_lum, d_lum, e_lum, f_lum, h_lum);

    // min and max of ring.
    float lum_min4 = MIN4(b_lum, d_lum, f_lum, h_lum);
    float lum_max4 = MAX4(b_lum, d_lum, f_lum, h_lum);

    // 0.99 found through testing -> see my latest desmos or https://www.desmos.com/calculator/ewttak3abi
    // anything higher than 0.99 makes the sharpening curve too flat
    // this helps reducing massive overshoot that would happen otherwise
    // normal CAS applies a limiter too so that there is no overshoot
    float lum_max4_limited = min(lum_max4, 0.99f);

    // limiters, these need to be high precision RCPs.
    float lum_min_hit = lum_min4
                      * rcp(4.f * lum_max4_limited);

    float lum_max_hit = (peak_c[0] - lum_max4_limited)
                      * rcp(4.f * lum_min4 + peak_c[1]);

    lobe_local = max(-lum_min_hit, lum_max_hit);
  }
  else
  {
    Preprocess_RGB(b, d, e, f, h);

    // min and max of ring.
    float3 rgb_min4 = MIN4(b, d, f, h);
    float3 rgb_max4 = MAX4(b, d, f, h);

    float3 rgb_max4_limited = min(rgb_max4, 0.99f);

    // limiters, these need to be high precision RCPs.
    float3 rgb_min_hit = rgb_min4
                       * rcp(4.f * rgb_max4_limited);

    float3 rgb_max_hit = (peak_c[0] - rgb_max4_limited)
                       * rcp(4.f * rgb_min4 + peak_c[1]);

    float3 rgb_lobe = max(-rgb_min_hit, rgb_max_hit);

    lobe_local = MAXRGB(rgb_lobe);
  }


// This is set at the limit of providing unnatural results for sharpening.
//0.25f - (1.f / 16.f)
#define FSR_RCAS_LIMIT 0.1875f

  float lobe = max(float(-FSR_RCAS_LIMIT),
                   min(lobe_local, 0.f)) * Sharpness;

  // Apply noise removal.
  BRANCH()
  if (NOISE_REMOVAL_ENABLE)
  {
    // Luminance/Luma times 2.
    float b_luma_2x;
    float d_luma_2x;
    float e_luma_2x;
    float f_luma_2x;
    float h_luma_2x;

    BRANCH()
    if (RCAS_MODE == RCAS_MODE_LUMINANCE)
    {
      b_luma_2x = b_lum * 2.f;
      d_luma_2x = d_lum * 2.f;
      e_luma_2x = e_lum * 2.f;
      f_luma_2x = f_lum * 2.f;
      h_luma_2x = h_lum * 2.f;
    }
    else
    BRANCH()
    if (RCAS_MODE == RCAS_MODE_RGB_SHAPED)
    {
      Get_Luma_2x(b,         d,         e,         f,         h,
                  b_luma_2x, d_luma_2x, e_luma_2x, f_luma_2x, h_luma_2x);
    }
    else //if (RCAS_MODE == RCAS_MODE_ORIGINAL)
    {
      b_luma_2x = b.b * 0.5f + (b.r * 0.5f + b.g);
      d_luma_2x = d.b * 0.5f + (d.r * 0.5f + d.g);
      e_luma_2x = e.b * 0.5f + (e.r * 0.5f + e.g);
      f_luma_2x = f.b * 0.5f + (f.r * 0.5f + f.g);
      h_luma_2x = h.b * 0.5f + (h.r * 0.5f + h.g);
    }

    // Noise detection.
    float nz = 0.25f * b_luma_2x
             + 0.25f * d_luma_2x
             + 0.25f * f_luma_2x
             + 0.25f * h_luma_2x
             - e_luma_2x;

    float luma_2x_max = MAX3(MAX3(b_luma_2x, d_luma_2x, e_luma_2x), f_luma_2x, h_luma_2x);
    float luma_2x_min = MIN3(MIN3(b_luma_2x, d_luma_2x, e_luma_2x), f_luma_2x, h_luma_2x);

    nz = saturate(abs(nz) * rcp(luma_2x_max - luma_2x_min));
    nz = -0.5f * nz + 1.f;

    lobe *= nz;
  }

  // Resolve, which needs the medium precision rcp approximation to avoid visible tonality changes.
  float rcp_l = rcp(4.f * lobe + 1.f);

  float3 pix;

  BRANCH()
  if (RCAS_MODE == RCAS_MODE_LUMINANCE)
  {
    float lum_pix = ((b_lum + d_lum + h_lum + f_lum) * lobe + e_lum) * rcp_l;

    pix = max(lum_pix / e_lum, 0.f) * e;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)

    pix = Csp::Trc::Linear_To::PQ(pix);

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

    pix = ENCODE_SDR(pix);

#endif
  }
  else
  {
    pix = max(((b + d + h + f) * lobe + e) * rcp_l, 0.f);

    pix = Process_RGB_For_Output(pix);
  }

  return pix;
}
