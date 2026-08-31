// Original functions are part of the FidelityFX SDK.
//
// Copyright (C) 2023 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the “Software”), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.


#include "cas_helpers.fxh"


uniform bool CAS_LUMINANCE_MODE
<
  ui_label   = "use luminance mode";
  ui_tooltip = "uses the relative luminance instead linear RGB values to do the sharpening";
> = true;

uniform float SHARPEN_AMOUNT
<
  ui_label   = "sharpness amount";
  ui_type    = "drag";
  ui_min     = 0.f;
  ui_max     = 0.2f;
  ui_step    = 0.0001f;
> = 0.1f;


float3 CasSharpenOnly
(
        SPixelsToProcess Ptp,
  const float            Peak
)
{
  float3 output;

  BRANCH()
  if (CAS_LUMINANCE_MODE)
  {
    SLumianceOfPixels lop;

    GetLuminance(Ptp, lop);

    static const float aLum = lop.aLum;
    static const float bLum = lop.bLum;
    static const float cLum = lop.cLum;
    static const float dLum = lop.dLum;
    static const float eLum = lop.eLum;
    static const float fLum = lop.fLum;
    static const float gLum = lop.gLum;
    static const float hLum = lop.hLum;
    static const float iLum = lop.iLum;

    // Soft min and max.
    //  a b c             b
    //  d e f * 0.5  +  d e f * 0.5
    //  g h i             h
    // These are 2.0x bigger (factored out the extra multiply).
    float minLum = MIN3(MIN3(dLum, eLum, fLum), bLum, hLum);
    float maxLum = MAX3(MAX3(dLum, eLum, fLum), bLum, hLum);

    minLum += MIN3(MIN3(minLum, aLum, cLum), gLum, iLum);
    maxLum += MAX3(MAX3(maxLum, aLum, cLum), gLum, iLum);

    // Smooth minimum distance to signal limit divided by smooth max.
    float rcpMaxLum = rcp(maxLum);

    // Shaping amount of sharpening.
    float amplifyLum = sqrt(saturate(min(minLum, 2.f - maxLum) * rcpMaxLum));

    // Filter shape.
    //  0 w 0
    //  w 1 w
    //  0 w 0

    float weight = amplifyLum * Peak;

    float rcpWeight = rcp(4.f * weight + 1.f);

    float outputLum = ((bLum + dLum + fLum + hLum) * weight + eLum) * rcpWeight;

    output = max(outputLum / eLum, 0.f) * Ptp.e;

    output = LuminanceModePrepareForOutput(output);
  }
  else
  {
    static const float3 a = RgbModePrepareForProcessing(Ptp.a);
    static const float3 b = RgbModePrepareForProcessing(Ptp.b);
    static const float3 c = RgbModePrepareForProcessing(Ptp.c);
    static const float3 d = RgbModePrepareForProcessing(Ptp.d);
    static const float3 e = RgbModePrepareForProcessing(Ptp.e);
    static const float3 f = RgbModePrepareForProcessing(Ptp.f);
    static const float3 g = RgbModePrepareForProcessing(Ptp.g);
    static const float3 h = RgbModePrepareForProcessing(Ptp.h);
    static const float3 i = RgbModePrepareForProcessing(Ptp.i);

    // Soft min and max.
    //  a b c             b
    //  d e f * 0.5  +  d e f * 0.5
    //  g h i             h
    // These are 2.0x bigger (factored out the extra multiply).
    float3 minRgb = MIN3(MIN3(d, e, f), b, h);
    float3 maxRgb = MAX3(MAX3(d, e, f), b, h);

    minRgb += MIN3(MIN3(minRgb, a, c), g, i);
    maxRgb += MAX3(MAX3(maxRgb, a, c), g, i);

    // Smooth minimum distance to signal limit divided by smooth max.
    float3 rcpMaxRgb = rcp(maxRgb);

    // Shaping amount of sharpening.
    float3 amplifyRgb = sqrt(saturate(min(minRgb, 2.f - maxRgb) * rcpMaxRgb));

    // Filter shape.
    //  0 w 0
    //  w 1 w
    //  0 w 0

    float3 weight = amplifyRgb * Peak;

    float3 rcpWeight = rcp(4.f * weight + 1.f);

    output = max(((b + d + f + h) * weight + e) * rcpWeight, 0.f);

    output = RgbModePrepareForOutput(output);
  }

  return output;
}
