#pragma once

#include "include_main.fxh"


#if (defined(IS_ANALYSIS_CAPABLE_API)    \
  && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB  \
    || ACTUAL_COLOUR_SPACE == CSP_HDR10) \
   || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))

namespace Itmos
{

#define BT2446A_PRO_MODE_LUMINANCE  0
#define BT2446A_PRO_MODE_YCBCR_LIKE 1

  // outputs normalised values
  float3 Bt2446A
  (
    const float3 Input,
    const uint   ProcessingMode,
    const float  LHdr,
    const float  LSdr,
    const float  InputNitsFactor,
    const float  GammaIn,
    const float  GammaOut
  )
  {
    //pSDR and pHDR
    static const float2 pSdrHdr = 1.f + 32.f * pow(float2(LSdr, LHdr) / 10000.f, 1.f / 2.4f);
#define pSdr pSdrHdr[0]
#define pHdr pSdrHdr[1]

    float3 sdr = Input / InputNitsFactor;

    //YSDR
    float ySdr = dot(sdr, Csp::Mat::BT709_To_XYZ[1]);
    //clamp to avoid invalid numbers
    ySdr = max(ySdr, 1e-20);
    //Y'SDR
    const float y_Sdr = pow(ySdr, 1.f / (2.4f + GammaIn));

    //Y'c
    //if pSdr == 1 there is a division by zero
    //this happens when LSdr == 0
    const float y_C = log((y_Sdr * (pSdr - 1.f)) + 1.f)
                    / log(pSdr); //log = ln

    // order of branching is so that a float accuracy issue edge case
    // where none of the cases match is covered
    // this happens when:
    //  - y_P0 is slightly above 0.7399 and should be the match
    //  - y_P2 is slightly below 0.9909 and should be the match
    // this is caught by the else case always being y_P1
    // which causes no curve discontinuity issues
    const float y_P0 = y_C
                     / 1.0770f;

    //(4.83307641 - 4.604f * y_C) == (pow(2.7811f, 2) - 4 * (-1.151f) * (-0.6302f - y_C))
    const float y_P1 = (-2.7811f + sqrt(4.83307641f - 4.604f * y_C))
                     / -2.302f;

    const float y_P2 = (y_C - 0.5000f)
                     / 0.5000f;

    const float y_P = y_P0 <= 0.7399f ? y_P0
                    : y_P2 >= 0.9909f ? y_P2
                                      : y_P1;

    //Y'HDR
    //if pHdr == 1 there is a division by zero
    //this happens when LHdr == 0
    const float y_Hdr = (pow(pHdr, y_P) - 1.f)
                      / (pHdr - 1.f);

    float3 hdr;

    // get RGB
    switch(ProcessingMode)
    {
      case BT2446A_PRO_MODE_LUMINANCE:
      {
        hdr = sdr * (y_Hdr / y_Sdr);
      }
      break;
      case BT2446A_PRO_MODE_YCBCR_LIKE:
      {
        const float yHdr = pow(y_Hdr, 2.4f);

        hdr = sdr * (yHdr / ySdr);
      }
      break;
      default:
        hdr = 0.f;
        break;
    }

    //expand to target luminance
    hdr *= (LHdr / 10000.f);

    //scRGB
    hdr = ConditionallyConvertNormalisedBt709ToScRgb(hdr);

    //HDR10
    hdr = ConditionallyConvertBt709ToBt2020(hdr);
    hdr = ConditionallyConvertNormalisedBt2020ToHdr10(hdr);

    return hdr;
  } //Bt2446a


  // HDR reference white in XYZ
  #define HDR_REF_WHITE_XYZ float3(192.93f, 203.f, 221.05f)
  #define DELTA             (6.f / 29.f)
  #define POW_DELTA_3       (DELTA * DELTA * DELTA)
  #define _3_X_POW_DELTA_2  (3.f * (DELTA * DELTA))

  //// outputs normalised values
  //float3 Bt2446c(
  //  float3 Input,
  //  float  SdrBrightness,
  //  float  Alpha,
  ////  float  k1,
  ////  float  inf_point,
  //  bool   UseAchromaticCorrection,
  //  float  Sigma)
  //{
  //
  //  //103.2 =  400 nits
  //  //107.1 =  500 nits
  //  //110.1 =  600 nits
  //  //112.6 =  700 nits
  //  //114.8 =  800 nits
  //  //116.7 =  900 nits
  //  //118.4 = 1000 nits
  //  //153.7 is just under 10000 nits for Alpha=0 and above it starts clipping
  //  float3 sdr = Input * (SdrBrightness > 153.7f
  //                      ? 153.7f
  //                      : SdrBrightness);
  //
  //  //6.1.6 (inverse)
  //  //crosstalk matrix from 6.1.2
  //  //float Alpha = 0.f; //hardcode for now as it gives the best results imo
  //  float xlpha = 1.f - 2.f * Alpha;
  //  float3x3 crosstalkMatrix = float3x3(xlpha, Alpha, Alpha,
  //                                      Alpha, xlpha, Alpha,
  //                                      Alpha, Alpha, xlpha);
  //
  //  sdr = mul(crosstalkMatrix, sdr);
  //
  //  //6.1.5 (inverse)
  //  //conversion to XYZ and then Yxy -> x and y is at the end of the achromatic correction or the else case
  //  sdr = mul(BT2020_To_XYZ, sdr);
  //  float YSdr = sdr.y;
  //  float xyz  = sdr.x + sdr.y + sdr.z;
  //  float xSdr = sdr.x /
  //               xyz;
  //  float ySdr = sdr.y /
  //               xyz;
  //
  //  //6.1.4 (inverse)
  //  //inverse tone mapping
  //  float k1 = 0.83802f;
  //  float k2 = 15.09968f;
  //  float k3 = 0.74204f;
  //  float k4 = 78.99439f;
  //  float YHdrIp = 69.84922394059541817564; // 58.535046646 / k1; // 58.5 = 0.80^2.4
  //  //k1 = 0.83802f;
  //  //k2 = 15.09968f;
  //  //k3 = 0.74204f;
  //  //k4 = 78.99439f;
  //  //float Y_hlg_ref = 203.f;
  //  //float Y_sdr_wp  = pow(0.96, 2.4f) * 100.f
  //  //float YHdrIp = inf_point / k1; // 58.5 = 0.80^2.4
  //  //float k2 = inf_point * (1.f - k3);
  //  //float k4 = inf_point - k2 * log(1.f - k3);
  //  //float k3 = -exp((Y_sdr_wp - k4) / k2) + (203.f / YHdrIp);
  //
  //  float YHdr = YSdr / k1;
  //
  //  if (YHdr >= YHdrIp)
  //    YHdr = (exp((YSdr - k4) / k2) + k3) * YHdrIp;
  //
  //  //6.1.3 (inverse) part 1
  //  //convert to XYZ
  //  float  XHdrUncorrected = (xSdr / ySdr) * YHdr;
  //  float  ZHdrUncorrected = ((1.f - xSdr - ySdr) / ySdr) * YHdr;
  //  float3 hdr             = float3(XHdrUncorrected, YHdr, ZHdrUncorrected);
  //
  //  bool  usefulCorrection = false;
  //  float xCorrected = 0.f;
  //  float yCorrected = 0.f;
  //  // optional chroma correction above HDR Reference White (inverse)
  //  if (UseAchromaticCorrection && Sigma > 0.f)
  //  {
  //    // (3) inverse
  //    // XYZ->L*a*b* from (1) which is actually wrong
  //    // using correct conversion here
  //    //==========================================================================
  //    // it seems the ITU was trying to make a faster version for t <= POW_DELTA_3
  //    // corrected version is here:
  //    // L* = (116 * (t / POW_DELTA_3) - 16) / 10.f
  //    // it's missing the division by 10 in the ITU doc
  //
  //    // get L*
  //    float tY = YHdr / HDR_REF_WHITE_XYZ.y;
  //    float fY = 0.f;
  //
  //    if (tY > POW_DELTA_3) {
  //      fY = (pow(tY, 1.f / 3.f));
  //    }
  //    else {
  //      fY = (tY / (3.f * pow(DELTA, 2.f)) + (16.f / 116.f));
  //    }
  //
  //    float lStar = 116.f * fY - 16.f;
  //
  //    // get a*
  //    float tX = sdr.x / HDR_REF_WHITE_XYZ.x;
  //    float fX = 0.f;
  //
  //    if (tX > POW_DELTA_3) {
  //      fX = (pow(tY, 1.f / 3.f));
  //    }
  //    else {
  //      fX = (tX / (3.f * pow(DELTA, 2.f)) + (16.f / 116.f));
  //    }
  //    fX -= fY;
  //
  //    float a_star = 116.f * fX - 16.f;
  //
  //    // get b*
  //    float tZ = sdr.z / HDR_REF_WHITE_XYZ.z;
  //    float fZ = 0.f;
  //
  //    if (tZ > POW_DELTA_3) {
  //      fZ = (pow(tY, 1.f / 3.f));
  //    }
  //    else {
  //      fZ = (tZ / (3.f * pow(DELTA, 2.f)) + (16.f / 116.f));
  //    }
  //    fZ = fY - fZ;
  //
  //    float bStar = 116.f * fZ - 16.f;
  //
  //    // (2) chroma correction above Reference White
  //    float lStarRef = 100.f;
  //    float lStarMax = 116.f * pow(10000.f / 203.f, 1.f / 3.f) - 16.f; // hardcode to PQ max for now
  //
  //    // convert to CIELCh
  //    float cStarAb = sqrt(pow(a_star, 2.f) + pow(bStar, 2.f));
  //    float hAb     = atan(bStar / a_star);
  //
  //    float fCorrected = 1.f;
  //    if (lStar > lStarRef)
  //    {
  //      fCorrected = 1.f - Sigma * (lStar    - lStarRef) /
  //                            (lStarMax - lStarRef);
  //      if (fCorrected <= 0.f) { // avoid division by zero
  //        fCorrected = 0.0000001f;
  //      }
  //
  //      usefulCorrection = true;
  //
  //      // amazing function inversion!!
  //      float cStarAbCorrected = cStarAb / fCorrected;
  //
  //      // convert back to CIELAB
  //      float aStarCorrected = cStarAbCorrected * cos(hAb);
  //      float bStarCorrected = cStarAbCorrected * sin(hAb);
  //
  //      // (1) inverse
  //      // conversion from L*a*b* to XYZ from (3) and then Yxy
  //      float3 XYZCorrected;
  //      float fYCorrected = (lStar + 16.f) /
  //                          116.f;
  //      float fXCorrected = fYCorrected + aStarCorrected /
  //                                        500.f;
  //      float fZCorrected = fYCorrected - bStarCorrected /
  //                                        200.f;
  //
  //      //X
  //      if (fXCorrected > DELTA) {
  //        XYZCorrected.x = HDR_REF_WHITE_XYZ.x * pow(fXCorrected, 3);
  //      }
  //      else {
  //        XYZCorrected.x = lStar * _3_X_POW_DELTA_2 * HDR_REF_WHITE_XYZ.x;
  //      }
  //
  //      // can you just take the XYZ Y from the input here since it's unchanged?
  //      // probably yes
  //      //Y
  //      if (fYCorrected > DELTA) {
  //        XYZCorrected.y = HDR_REF_WHITE_XYZ.y * pow(fYCorrected, 3.f);
  //      }
  //      else {
  //        XYZCorrected.y = lStar * _3_X_POW_DELTA_2 * HDR_REF_WHITE_XYZ.y;
  //      }
  //
  //      //Z
  //      if (fZCorrected > DELTA) {
  //        XYZCorrected.z = HDR_REF_WHITE_XYZ.z * pow(fZCorrected, 3.f);
  //      }
  //      else {
  //        XYZCorrected.z = lStar * _3_X_POW_DELTA_2 * HDR_REF_WHITE_XYZ.z;
  //      }
  //
  //      //convert to Yxy without the Y as it is unneeded
  //      float xyz = XYZCorrected.x + XYZCorrected.y + XYZCorrected.z;
  //
  //      xCorrected = XYZCorrected.x /
  //                   xyz;
  //      yCorrected = XYZCorrected.y /
  //                   xyz;
  //    }
  //  }
  //
  //  //6.1.3 (inverse) part 2
  //  //convert to XYZ if achromaitc correction is used
  //  //and then to RGB
  //  if (UseAchromaticCorrection && usefulCorrection)
  //  {
  //    float XHdrCorrected = (xCorrected / yCorrected) * YHdr;
  //    float ZHdrCorrected = ((1.f - xCorrected - yCorrected) / yCorrected) * YHdr;
  //
  //    hdr = float3(XHdrCorrected, YHdr, ZHdrCorrected);
  //  }
  //  hdr = mul(XYZ_To_BT2020, hdr);
  //
  //  //6.1.2 (inverse)
  //  //inverse crosstalk matrix from 6.1.6
  //  float mlpha = 1.f - Alpha;
  //  float3x3 inverseCrosstalkMatrix =
  //    mul(1.f / (1.f - 3.f * Alpha), float3x3( mlpha, -Alpha, -Alpha,
  //                                            -Alpha,  mlpha, -Alpha,
  //                                            -Alpha, -Alpha,  mlpha));
  //  hdr = mul(inverseCrosstalkMatrix, hdr);
  //
  //  hdr = clamp(hdr / 10000.f, 0.f, 1.f);
  //
  //  return hdr;
  //}

  // outputs normalised values
  float3 Bt2446C(
    const float3 Input,
    const float  SdrRelativeBrightness,
    const float  Alpha)
  //  float  k1,
  //  float  inf_point,
  //  bool   UseAchromaticCorrection,
  //  float  Sigma)
  {

    //103.2 =  400 nits
    //107.1 =  500 nits
    //110.1 =  600 nits
    //112.6 =  700 nits
    //114.8 =  800 nits
    //116.7 =  900 nits
    //118.4 = 1000 nits
    //153.7 is just under 10000 nits for Alpha == 0 and above it starts clipping
    float3 sdr = Input * SdrRelativeBrightness;

    sdr = Csp::Mat::BT709_To::BT2020(sdr);

    //6.1.6 (inverse)
    //crosstalk matrix from 6.1.2
    //float Alpha = 0.f; //hardcode for now as it gives the best results imo
    static const float xlpha = 1.f - 2.f * Alpha;
    static const float3x3 crosstalkMatrix = float3x3(xlpha, Alpha, Alpha,
                                                     Alpha, xlpha, Alpha,
                                                     Alpha, Alpha, xlpha);

    sdr = mul(crosstalkMatrix, sdr);

    //6.1.5 (inverse)
    //conversion to XYZ and then Yxy -> x and y is at the end of the achromatic correction or the else case
    sdr = Csp::Mat::BT2020_To::XYZ(sdr);
    float YSdr = sdr.y;
    float xyz  = sdr.x + sdr.y + sdr.z;
    float xSdr = sdr.x
               / xyz;
    float ySdr = sdr.y
               / xyz;

    //6.1.4 (inverse)
    //inverse tone mapping
    float k1 = 83.802f; // multiplied by 100
    //float k1 = 0.83802f;
    float k2 = 15.09968f;
    float k3 = 0.74204f;
    float k4 = 78.99439f;
    //float YHdrIp = 69.84922394059541817564; // 58.535046646 / k1; // 58.5 = 0.80^2.4
    float YHdrIp = 0.006984922394059541817564; // divided by 10000
    //k1 = 0.83802f;
    //k2 = 15.09968f;
    //k3 = 0.74204f;
    //k4 = 78.99439f;
    //float Y_hlg_ref = 203.f;
    //float Y_sdr_wp  = pow(0.96, 2.4f) * 100.f
    //float YHdrIp = inf_point / k1; // 58.5 = 0.80^2.4
    //float k2 = inf_point * (1.f - k3);
    //float k4 = inf_point - k2 * log(1.f - k3);
    //float k3 = -exp((Y_sdr_wp - k4) / k2) + (203.f / YHdrIp);

    float YHdr = YSdr / k1;

    [branch]
    if (YHdr >= YHdrIp)
    {
      YHdr = (exp((YSdr * 100.f - k4) / k2) + k3) * YHdrIp;
    }

    //6.1.3 (inverse) part 1
    //convert to XYZ
    float XHdrUncorrected = (xSdr / ySdr) * YHdr;
    float ZHdrUncorrected = ((1.f - xSdr - ySdr) / ySdr) * YHdr;

    float3 hdr = float3(XHdrUncorrected, YHdr, ZHdrUncorrected);

  //  bool  usefulCorrection = false;
  //  float xCorrected = 0.f;
  //  float yCorrected = 0.f;
  //  // optional chroma correction above HDR Reference White (inverse)
  //  if (UseAchromaticCorrection && Sigma > 0.f)
  //  {
  //    // (3) inverse
  //    // XYZ->L*a*b* from (1) which is actually wrong
  //    // using correct conversion here
  //    //==========================================================================
  //    // it seems the ITU was trying to make a faster version for t <= POW_DELTA_3
  //    // corrected version is here:
  //    // L* = (116 * (t / POW_DELTA_3) - 16) / 10.f
  //    // it's missing the division by 10 in the ITU doc
  //
  //    // get L*
  //    float tY = YHdr / HDR_REF_WHITE_XYZ.y;
  //    float fY = 0.f;
  //
  //    if (tY > POW_DELTA_3) {
  //      fY = (pow(tY, 1.f / 3.f));
  //    }
  //    else {
  //      fY = (tY / (3.f * pow(DELTA, 2.f)) + (16.f / 116.f));
  //    }
  //
  //    float lStar = 116.f * fY - 16.f;
  //
  //    // get a*
  //    float tX = sdr.x / HDR_REF_WHITE_XYZ.x;
  //    float fX = 0.f;
  //
  //    if (tX > POW_DELTA_3) {
  //      fX = (pow(tY, 1.f / 3.f));
  //    }
  //    else {
  //      fX = (tX / (3.f * pow(DELTA, 2.f)) + (16.f / 116.f));
  //    }
  //    fX -= fY;
  //
  //    float a_star = 116.f * fX - 16.f;
  //
  //    // get b*
  //    float tZ = sdr.z / HDR_REF_WHITE_XYZ.z;
  //    float fZ = 0.f;
  //
  //    if (tZ > POW_DELTA_3) {
  //      fZ = (pow(tY, 1.f / 3.f));
  //    }
  //    else {
  //      fZ = (tZ / (3.f * pow(DELTA, 2.f)) + (16.f / 116.f));
  //    }
  //    fZ = fY - fZ;
  //
  //    float bStar = 116.f * fZ - 16.f;
  //
  //    // (2) chroma correction above Reference White
  //    float lStarRef = 100.f;
  //    float lStarMax = 116.f * pow(10000.f / 203.f, 1.f / 3.f) - 16.f; // hardcode to PQ max for now
  //
  //    // convert to CIELCh
  //    float cStarAb = sqrt(pow(a_star, 2.f) + pow(bStar, 2.f));
  //    float hAb      = atan(bStar / a_star);
  //
  //    float fCorrected = 1.f;
  //    if (lStar > lStarRef)
  //    {
  //      fCorrected = 1.f - Sigma * (lStar     - lStarRef) /
  //                                 (lStarMax - lStarRef);
  //      if (fCorrected <= 0.f) // avoid division by zero
  //        fCorrected = 0.0000001f;
  //
  //      usefulCorrection = true;
  //
  //      // amazing function inversion!!
  //      float cStarAbCorrected = cStarAb / fCorrected;
  //
  //      // convert back to CIELAB
  //      float aStarCorrected = cStarAbCorrected * cos(hAb);
  //      float bStarCorrected = cStarAbCorrected * sin(hAb);
  //
  //      // (1) inverse
  //      // conversion from L*a*b* to XYZ from (3) and then Yxy
  //      float3 XYZCorrected;
  //      float fYCorrected = (lStar + 16.f) /
  //                          116.f;
  //      float fXCorrected = fYCorrected + aStarCorrected /
  //                                        500.f;
  //      float fZCorrected = fYCorrected - bStarCorrected /
  //                                        200.f;
  //
  //      //X
  //      if (fXCorrected > DELTA) {
  //        XYZCorrected.x = HDR_REF_WHITE_XYZ.x * pow(fXCorrected, 3);
  //      }
  //      else {
  //        XYZCorrected.x = lStar * _3_X_POW_DELTA_2 * HDR_REF_WHITE_XYZ.x;
  //      }
  //
  //      // can you just take the XYZ Y from the input here since it's unchanged?
  //      // probably yes
  //      //Y
  //      if (fYCorrected > DELTA) {
  //        XYZCorrected.y = HDR_REF_WHITE_XYZ.y * pow(fYCorrected, 3.f);
  //      }
  //      else {
  //        XYZCorrected.y = lStar * _3_X_POW_DELTA_2 * HDR_REF_WHITE_XYZ.y;
  //      }
  //
  //      //Z
  //      if (fZCorrected > DELTA) {
  //        XYZCorrected.z = HDR_REF_WHITE_XYZ.z * pow(fZCorrected, 3.f);
  //      }
  //      else {
  //        XYZCorrected.z = lStar * _3_X_POW_DELTA_2 * HDR_REF_WHITE_XYZ.z;
  //      }
  //
  //      //convert to Yxy without the Y as it is unneeded
  //      float xyz = XYZCorrected.x + XYZCorrected.y + XYZCorrected.z;
  //
  //      xCorrected = XYZCorrected.x /
  //                   xyz;
  //      yCorrected = XYZCorrected.y /
  //                   xyz;
  //    }
  //  }

    //6.1.3 (inverse) part 2
    //convert to XYZ if achromaitc correction is used
    //and then to RGB
  //  if (UseAchromaticCorrection && usefulCorrection)
  //  {
  //    float XHdrCorrected = (xCorrected / yCorrected) * YHdr;
  //    float ZHdrCorrected = ((1.f - xCorrected - yCorrected) / yCorrected) * YHdr;
  //
  //    hdr = float3(XHdrCorrected, YHdr, ZHdrCorrected);
  //  }

    hdr = Csp::Mat::XYZ_To::BT2020(hdr);

    //6.1.2 (inverse)
    //inverse crosstalk matrix from 6.1.6
    static const float mlpha = 1.f - Alpha;
    static const float3x3 inverseCrosstalkMatrix =
      mul(1.f / (1.f - 3.f * Alpha), float3x3(mlpha, -Alpha, -Alpha,
                                             -Alpha,  mlpha, -Alpha,
                                             -Alpha, -Alpha,  mlpha));
    hdr = mul(inverseCrosstalkMatrix, hdr);

    hdr = max(hdr, 0.f);

    //scRGB
    hdr = ConditionallyConvertNormalisedBt2020ToScRgb(hdr);

    //HDR10
    hdr = ConditionallyConvertNormalisedBt2020ToHdr10(hdr);

    return hdr;
  } //Bt2446c


  namespace Dice
  {

    float LuminanceExpand(float Colour, float MaxNits, float ShoulderStart)
    {
      float u = log(-((-MaxNits + Colour) / (MaxNits - ShoulderStart)));

      return -MaxNits * u + ShoulderStart * u + ShoulderStart;
    } //LuminanceExpand

    float3 InverseToneMapper(
      float3 Input,
      float  MaxNits,
      float  ShoulderStart)
    {
      float3x3 RgbToLms  = ICtCp::BT2020_To_ICtCp_LMS;
      float3x3 LmsToRgb  = ICtCp::ICtCp_LMS_To_BT2020;

      float3 LMS = mul(RgbToLms, Input);

      LMS = Csp::Trc::Linear_To::PQ(LMS);

      float I1 = 0.5f * LMS.x + 0.5f * LMS.y;

      if (I1 < ShoulderStart)
      {
        return Input;
      }
      else
      {
        float Ct1 = dot(LMS, Csp::ICtCp::PQ_ICtCp_LMS_To_ICtCp[1]);
        float Cp1 = dot(LMS, Csp::ICtCp::PQ_ICtCp_LMS_To_ICtCp[2]);

        float I2 = LuminanceExpand(I1, MaxNits, ShoulderStart);

        float min_I = min(min((I1 / I2), (I2 / I1)) * 1.1f, 1.f);

        //to LMS
        LMS = Csp::ICtCp::ICtCp_To::LMS(float3(I2,
                                              min_I * Ct1,
                                              min_I * Cp1));
        //to RGB
        return max(mul(LmsToRgb, LMS), 0.f);
      }
    } //InverseToneMapper

  }

}

#endif //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))
