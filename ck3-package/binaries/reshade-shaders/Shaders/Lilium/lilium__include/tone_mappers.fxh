#pragma once


#if (defined(IS_ANALYSIS_CAPABLE_API)    \
  && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB  \
    || ACTUAL_COLOUR_SPACE == CSP_HDR10) \
   || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))


void GetUsedMaxNits
(
  out float UsedMaxNits
)
{
  UsedMaxNits = max(Ui::Tm::StaticMode::InputLuminanceMax, Ui::Tm::Global::TargetLuminance);

  return;
}

void GetTmoParamsBt2390
(
  in  float UsedMaxNits,
  out float SrcMinPq,
  out float SrcMaxPq,
  out float SrcMaxPqMinusSrcMinPq,
  out float MinLum,
  out float MaxLum,
  out float KneeStart
)
{
  // source min brightness (Lb) in PQ
  // source max brightness (Lw) in PQ
  // target min brightness (Lmin) in PQ
  // target max brightness (Lmax) in PQ
  float4 SrcMinMaxPqTgtMinMaxPq = Csp::Trc::Nits_To::PQ(float4(Ui::Tm::Bt2390::OldBlackPoint,
                                                              UsedMaxNits,
                                                              Ui::Tm::Bt2390::NewBlackPoint,
                                                              Ui::Tm::Global::TargetLuminance));

  SrcMinPq = SrcMinMaxPqTgtMinMaxPq[0];
  SrcMaxPq = SrcMinMaxPqTgtMinMaxPq[1];

  // this is needed often so precalculate it
  SrcMaxPqMinusSrcMinPq = SrcMaxPq
                        - SrcMinPq;

  float2 minMaxLum = (SrcMinMaxPqTgtMinMaxPq.zw - SrcMinPq)
                   / SrcMaxPqMinusSrcMinPq;

  MinLum = minMaxLum[0];
  MaxLum = minMaxLum[1];

  // knee start (KS)
  KneeStart = 1.5f
            * MaxLum
            - Ui::Tm::Bt2390::KneeOffset;

  return;
}

void GetTmoParamsExpCompress
(
  in  float UsedMaxNits,
  out float ShoulderStartInPq,
  out float TargetLuminanceInPqMinusShoulderStartInPq
)
{
  float shoulderStart = Ui::Tm::ExpCompress::ShoulderStart
                      / 100.f
                      * Ui::Tm::Global::TargetLuminance
                      / 10000.f;

  ShoulderStartInPq = Csp::Trc::Linear_To::PQ(shoulderStart);

  TargetLuminanceInPqMinusShoulderStartInPq =
    Csp::Trc::Nits_To::PQ(Ui::Tm::Global::TargetLuminance)
  - ShoulderStartInPq;

  return;
}

void GetTmoParamsBt2446A
(
  in  float UsedMaxNits,
  out float PreAdjust,
  out float PostAdjust,
  out float PSdr,
  out float PSdrMinus1Inverse,
  out float PHdrMinus1,
  out float LnPHdrInverse
)
{
  const float targetLuminance = Ui::Tm::Global::TargetLuminance;

  PreAdjust  = 10000.f / UsedMaxNits;
  PostAdjust = targetLuminance / 10000.f;

  //pSDR and pHDR
  float2 pSdrHdr = 1.f + 32.f * pow(float2(targetLuminance, UsedMaxNits) / 10000.f, 1.f / 2.4f);

  PSdr              = pSdrHdr[0];
  PSdrMinus1Inverse = 1.f / (pSdrHdr[0] - 1.f);
  PHdrMinus1        = pSdrHdr[1] - 1.f;
  LnPHdrInverse     = 1.f / log(pSdrHdr[1]);

  return;
}


HDR10_TO_LINEAR_LUT()

// convert HDR10 to normalised BT.2020
float3 ConditionallyLineariseHdr10Temp(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
  Colour = FetchFromHdr10ToLinearLUT(Colour);
#endif
  return Colour;
}


namespace Tmos
{
  // Rep. ITU-R BT.2446-1 Table 2 & 3
  void Bt2446A
  (
    inout       float3 Colour,
          const float  PreAdjust,
          const float  PostAdjust,
          const float  PSdr,
          const float  PSdrMinus1Inverse,
          const float  PHdrMinus1,
          const float  LnPHdrInverse
  )
  {
    //scRGB
    Colour = ConditionallyNormaliseScRgb(Colour);
    //HDR10
    Colour = ConditionallyLineariseHdr10Temp(Colour);

    // adjust the max of 1 according to max nits
    Colour *= PreAdjust;

    // get luminance (YHDR)
    //clamp to avoid invalid numbers
    const float yHdr = max(GetLuminance(Colour), 1e-20);
    //Y'HDR
    const float y_Hdr = pow(yHdr, 1.f / 2.4f);

    //Y'p
    const float y_P = log(1.f + (PHdrMinus1) * y_Hdr)
                    * LnPHdrInverse;

    //Y'c
    const float y_C0 = 1.0770f * y_P;

    const float y_C1 = -1.1510f * (y_P * y_P)
                     +  2.7811f * y_P
                     -  0.6302f;

    const float y_C2 = 0.5000f * y_P
                     + 0.5000f;

    const float y_C = y_P <= 0.7399f ? y_C0
                    : y_P >= 0.9909f ? y_C2
                    :                  y_C1;

    //Y'SDR
    const float y_Sdr = (pow(PSdr, y_C) - 1.f)
                      * PSdrMinus1Inverse;

    //YSDR
    const float ySdr = pow(y_Sdr, 2.4f);

    Colour *= ySdr / yHdr * PostAdjust;

    //scRGB
    Colour = ConditionallyConvertNormalisedBt709ToScRgb(Colour);
    //HDR10
    Colour = ConditionallyConvertNormalisedBt2020ToHdr10(Colour);

    return;
  }

  float3 Bt2446A_MOD1(
    inout       float3 Colour,
          const float  MaxNits,
          const float  TargetNits,
          const float  LumaPostAdjust,
          const float  GamutCompression,
          const float  TestH,
          const float  TestS)
  {
    float3 Rgb = Colour;

    //scRGB
    Rgb = ConditionallyNormaliseScRgb(Rgb);
    //HDR10
    Rgb = ConditionallyLineariseHdr10Temp(Rgb);

    // adjust the max of 1 according to maxCLL
    Rgb *= (10000.f / MaxNits);

    // non-linear transfer function RGB->R'G'B'
    Rgb = pow(Rgb, 1.f / 2.4f);

    //to Y'C'bC'r
    float3 ycbcr = Csp::YCbCr::RGB_To::YCbCr_BT2020(Rgb);

    // tone mapping step 1
    //pHDR
    float pHdr = 1.f + 32.f * pow(
                                  TestH /
                                  10000.f
                              , 1.f / 2.4f);

    //Y'p
    float yP = (log(1.f + (pHdr - 1.f) * ycbcr.x)) /
                log(pHdr);

    // tone mapping step 2
    //Y'c
    float yC = yP <= 0.7399f ? 1.0770f * yP
             : yP >= 0.9909f ? (0.5000f * yP) + 0.5000f
                             : (-1.1510f * (yP * yP)) + (2.7811f * yP) - 0.6302f;

    // tone mapping step 3
    //pSDR
    float pSdr = 1.f + 32.f * pow(
                                  TestS /
                                  10000.f
                              , 1.f / 2.4f);

    //Y'SDR
    float ySdr = (pow(pSdr, yC) - 1.f) /
                 (pSdr - 1.f);

    //f(Y'SDR)
    float colourScaling = ySdr /
                          (GamutCompression * ycbcr.x);

    //C'b,tmo
    float cbTmo = colourScaling * ycbcr.y;

    //C'r,tmo
    float crTmo = colourScaling * ycbcr.z;

    //Y'tmo
    float yTmo = ySdr - max(LumaPostAdjust * crTmo, 0.f);

    Rgb = Csp::YCbCr::YCbCr_To::RGB_BT2020(float3(yTmo,
                                                cbTmo,
                                                crTmo));

    // avoid invalid colours
    Rgb = max(Rgb, 0.f);

    // gamma decompression and adjust to TargetNits
    Rgb = pow(Rgb, 2.4f) * (TargetNits / 10000.f);

    //scRGB
    Rgb = ConditionallyConvertNormalisedBt2020ToScRgb(Rgb);
    //HDR10
    Rgb = ConditionallyConvertNormalisedBt2020ToHdr10(Rgb);

    Colour = Rgb;
  }

  namespace Bt2390
  {

    float HermiteSpline
    (
      const float E1,
      const float KneeStart,
      const float OneMinusKneeStart,
      const float OneDivOneMinusKneeStart,
      const float KneeStartDivOneMinusKneeStart,
      const float MaxLum
    )
    {
      float t = E1 * OneDivOneMinusKneeStart - KneeStartDivOneMinusKneeStart;
      float tPow2 = t * t;
      float tPow3 = tPow2 * t;
      //float tPow2 = t >= 0.f ?  pow( t, 2.f)
      //                       : -pow(-t, 2.f);
      //float tPow3 = t >= 0.f ?  pow( t, 3.f)
      //                       : -pow(-t, 3.f);

      return ( 2.f * tPow3 - 3.f * tPow2 + 1.f) * KneeStart
           + (       tPow3 - 2.f * tPow2 + t)   * OneMinusKneeStart
           + (-2.f * tPow3 + 3.f * tPow2)       * MaxLum;
    }

    #define BT2390_EETF_E1(T)                 \
      T EetfE1                                \
      (                                       \
        const T     Input,                    \
        const float SrcMaxPq,                 \
        const float SrcMinPq,                 \
        const float SrcMaxPqMinusSrcMinPq,    \
        const bool  DisableBlackFloorAdaption \
      )                                       \
      {                                       \
        /* E1 */                              \
        BRANCH()                              \
        if (DisableBlackFloorAdaption)        \
        {                                     \
          return Input / SrcMaxPq;            \
        }                                     \
        else                                  \
        {                                     \
          return (Input - SrcMinPq)           \
               / SrcMaxPqMinusSrcMinPq;       \
        }                                     \
      }

    BT2390_EETF_E1(float)
    BT2390_EETF_E1(float3)

    #define BT2390_EETF_E3_E4(T)              \
      T EetfE3E4                              \
      (                                       \
        const T     Input,                    \
        const float SrcMaxPq,                 \
        const float SrcMinPq,                 \
        const float SrcMaxPqMinusSrcMinPq,    \
        const float MinLum,                   \
        const bool  DisableBlackFloorAdaption \
      )                                       \
      {                                       \
        BRANCH()                              \
        if (DisableBlackFloorAdaption)        \
        {                                     \
          /* E4 */                            \
          return Input * SrcMaxPq;            \
        }                                     \
        else                                  \
        {                                     \
          /* E3 */                            \
          T e3;                               \
          e3 = 1.f - Input;                   \
          e3 = e3*e3;                         \
          e3 = e3*e3;                         \
          e3 = MinLum * e3 + Input;           \
                                              \
          /* E4 */                            \
          return e3                           \
               * SrcMaxPqMinusSrcMinPq        \
               + SrcMinPq;                    \
        }                                     \
      }

    BT2390_EETF_E3_E4(float)
    BT2390_EETF_E3_E4(float3)

#define BT2390_PRO_MODE_YRGB   0
#define BT2390_PRO_MODE_MAXCLL 1
#define BT2390_PRO_MODE_RGB    2

    // works in PQ
    void Eetf
    (
      inout       float3 Colour,
            const uint   ProcessingMode,
            const float  SrcMinPq,  // Lb in PQ
            const float  SrcMaxPq,  // Lw in PQ
            const float  SrcMaxPqMinusSrcMinPq, // (Lw in PQ) minus (Lb in PQ)
            const float  MinLum,    // minLum
            const float  MaxLum,    // maxLum
            const float  KneeStart  // KS
    )
    {
      bool DisableBlackFloorAdaption = MinLum == 0.f;
      float OneMinusKneeStart = 1.f - KneeStart;
      float OneDivOneMinusKneeStart = 1.f / OneMinusKneeStart;
      float KneeStartDivOneMinusKneeStart = KneeStart / OneMinusKneeStart;

      BRANCH()
      if (ProcessingMode == BT2390_PRO_MODE_YRGB)
      {
        //HDR10
        float3 Rgb = ConditionallyLineariseHdr10Temp(Colour);

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
        float y1 = dot(Rgb, Csp::Mat::scRGB_To_XYZ_normalised[1]);
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        float y1 = dot(Rgb, Csp::Mat::BT2020_To_XYZ[1]);
#else // fallback for shader permutations
        float y1 = 0.f;
#endif
        //E1
        float y2 = EetfE1(Csp::Trc::Linear_To::PQ(y1),
                          SrcMaxPq,
                          SrcMinPq,
                          SrcMaxPqMinusSrcMinPq,
                          DisableBlackFloorAdaption);

        //E2
        [branch]
        if (y2 >= KneeStart)
        {
          y2 = HermiteSpline(y2,
                             KneeStart,
                             OneMinusKneeStart,
                             OneDivOneMinusKneeStart,
                             KneeStartDivOneMinusKneeStart,
                             MaxLum);
        }
#if (SHOW_ADAPTIVE_MAX_NITS == NO)
        else
        [branch]
        if (MinLum == 0.f)
        {
          discard;
        }
#endif
        //E3+E4
        y2 = EetfE3E4(y2,
                      SrcMaxPq,
                      SrcMinPq,
                      SrcMaxPqMinusSrcMinPq,
                      MinLum,
                      DisableBlackFloorAdaption);

        y2 = Csp::Trc::PQ_To::Linear(y2);

        Rgb *= y2 / y1;

        //HDR10
        Rgb = ConditionallyConvertNormalisedBt2020ToHdr10(Rgb);

        Colour = Rgb;
        return;
      }
      else
      BRANCH()
      if (ProcessingMode == BT2390_PRO_MODE_MAXCLL)
      {
        //scRGB
        float3 Rgb = ConditionallyConvertScRgbToNormalisedBt2020(Colour);

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
        float m1   = MAXRGB(Rgb);
        float m1Pq = Csp::Trc::Linear_To::PQ(m1);
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        float m1Pq = MAXRGB(Rgb);
#else // fallback for shader permutations
        float m1   = 0.f;
        float m1Pq = 0.f;
#endif

        //E1
        float m2 = EetfE1(m1Pq,
                          SrcMaxPq,
                          SrcMinPq,
                          SrcMaxPqMinusSrcMinPq,
                          DisableBlackFloorAdaption);

        //E2
        [branch]
        if (m2 >= KneeStart)
        {
          m2 = HermiteSpline(m2,
                             KneeStart,
                             OneMinusKneeStart,
                             OneDivOneMinusKneeStart,
                             KneeStartDivOneMinusKneeStart,
                             MaxLum);
        }
#if (SHOW_ADAPTIVE_MAX_NITS == NO)
        else
        [branch]
        if (MinLum == 0.f)
        {
          discard;
        }
#endif
        //E3+E4
        m2 = EetfE3E4(m2,
                      SrcMaxPq,
                      SrcMinPq,
                      SrcMaxPqMinusSrcMinPq,
                      MinLum,
                      DisableBlackFloorAdaption);

        m2 = Csp::Trc::PQ_To::Linear(m2);

        //HDR10
        Rgb = ConditionallyLineariseHdr10Temp(Rgb);

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        //more performant than to linearise maxCll1Pq
        float m1 = MAXRGB(Rgb);
#endif

        Rgb *= m2 / m1;

        //scRGB
        Rgb = ConditionallyConvertNormalisedBt2020ToScRgb(Rgb);
        //HDR10
        Rgb = ConditionallyConvertNormalisedBt2020ToHdr10(Rgb);

        Colour = Rgb;
        return;
      }
      else // if (ProcessingMode == BT2390_PRO_MODE_RGB)
      {
        float3 Rgb = ConditionallyConvertScRgbToHdr10(Colour);

        //E1
        Rgb = EetfE1(Rgb,
                     SrcMaxPq,
                     SrcMinPq,
                     SrcMaxPqMinusSrcMinPq,
                     DisableBlackFloorAdaption);

        const bool3 NeedsProcessing = Rgb >= KneeStart;

        //E2

        [branch]
        if (any(NeedsProcessing))
        {
          [branch]
          if (NeedsProcessing.r)
          {
            Rgb.r = HermiteSpline(Rgb.r,
                                  KneeStart,
                                  OneMinusKneeStart,
                                  OneDivOneMinusKneeStart,
                                  KneeStartDivOneMinusKneeStart,
                                  MaxLum);
          }
          [branch]
          if (NeedsProcessing.g)
          {
            Rgb.g = HermiteSpline(Rgb.g,
                                  KneeStart,
                                  OneMinusKneeStart,
                                  OneDivOneMinusKneeStart,
                                  KneeStartDivOneMinusKneeStart,
                                  MaxLum);
          }
          [branch]
          if (NeedsProcessing.b)
          {
            Rgb.b = HermiteSpline(Rgb.b,
                                  KneeStart,
                                  OneMinusKneeStart,
                                  OneDivOneMinusKneeStart,
                                  KneeStartDivOneMinusKneeStart,
                                  MaxLum);
          }
        }
#if (SHOW_ADAPTIVE_MAX_NITS == NO)
        else
        [branch]
        if (MinLum == 0.f)
        {
          discard;
        }
#endif

        //E3+E4
        Rgb = EetfE3E4(Rgb,
                       SrcMaxPq,
                       SrcMinPq,
                       SrcMaxPqMinusSrcMinPq,
                       MinLum,
                       DisableBlackFloorAdaption);

        Rgb = ConditionallyConvertHdr10ToScRgb(Rgb);

        Colour = Rgb;
        return;
      }
    }
  }


  namespace ExpCompress
  {
#define EXP_COMPRESS_PRO_MODE_YRGB   0
#define EXP_COMPRESS_PRO_MODE_MAXCLL 1
#define EXP_COMPRESS_PRO_MODE_RGB    2

    // Applies exponential "Photographic" luminance compression
    float ExponentialCompression
    (
      const float X
    )
    {
      return 1.f - exp(-X);
    }

    float LuminanceCompress
    (
      const float Channel,
      const float ShoulderStartInPq,
      const float TargetLuminanceInPqMinusShoulderStartInPq
    )
    {
      return ExponentialCompression((Channel - ShoulderStartInPq)
                                  / TargetLuminanceInPqMinusShoulderStartInPq)
           * TargetLuminanceInPqMinusShoulderStartInPq
           + ShoulderStartInPq;

//      return Channel < ShoulderStartInPq
//           ? Channel
//           : (TargetNits - ShoulderStart)
//           * RangeCompress((Channel       - ShoulderStartInPq) /
//                           (TargetCllInPq - ShoulderStartInPq))
//           + ShoulderStartInPq;
    }

    // remap from infinite
    // ShoulderStart denotes the point where we change from linear to shoulder
    void ToneMapper
    (
      inout       float3 Colour,
            const uint   ProcessingMode,
            const float  ShoulderStartInPq,
            const float  TargetLuminanceInPqMinusShoulderStartInPq
    )
    {
      // YRGB method copied from BT.2390
      BRANCH()
      if (ProcessingMode == EXP_COMPRESS_PRO_MODE_YRGB)
      {
        //HDR10
        float3 Rgb = ConditionallyLineariseHdr10Temp(Colour);

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
        float y1 = dot(Rgb, Csp::Mat::scRGB_To_XYZ_normalised[1]);
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        float y1 = dot(Rgb, Csp::Mat::BT2020_To_XYZ[1]);
#else // fallback for shader permutations
        float y1 = 0.f;
#endif

        float y2 = Csp::Trc::Linear_To::PQ(y1);

        [branch]
        if (y2 < ShoulderStartInPq)
        {
#if (SHOW_ADAPTIVE_MAX_NITS == NO)
          discard;
#endif
        }
        else
        {
          y2 = LuminanceCompress(y2,
                                 ShoulderStartInPq,
                                 TargetLuminanceInPqMinusShoulderStartInPq);

          y2 = Csp::Trc::PQ_To::Linear(y2);

          Rgb *= y2 / y1;


          //HDR10
          Rgb = ConditionallyConvertNormalisedBt2020ToHdr10(Rgb);

          Colour = Rgb;
        }
      }
      else
      BRANCH()
      if (ProcessingMode == EXP_COMPRESS_PRO_MODE_MAXCLL)
      {
        //scRGB
        float3 Rgb = ConditionallyConvertScRgbToNormalisedBt2020(Colour);

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
        float m1 = MAXRGB(Rgb);
        float m2 = Csp::Trc::Linear_To::PQ(m1);
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        float m2 = MAXRGB(Rgb);
#else // fallback for shader permutations
        float m1 = 0.f;
        float m2 = 0.f;
#endif

        [branch]
        if (m2 < ShoulderStartInPq)
        {
          discard;
        }
        else
        {
          //E3+E4
          m2 = LuminanceCompress(m2,
                                 ShoulderStartInPq,
                                 TargetLuminanceInPqMinusShoulderStartInPq);

          m2 = Csp::Trc::PQ_To::Linear(m2);

          //HDR10
          Rgb = ConditionallyLineariseHdr10Temp(Rgb);

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
          //more performant than to linearise maxCll1Pq
          float m1 = MAXRGB(Rgb);
#endif

          Rgb *= m2 / m1;

          //scRGB
          Rgb = ConditionallyConvertNormalisedBt2020ToScRgb(Rgb);
          //HDR10
          Rgb = ConditionallyConvertNormalisedBt2020ToHdr10(Rgb);

          Colour = Rgb;
          return;
        }
      }
      else //if (ProcessingMode == EXP_COMPRESS_PRO_MODE_RGB)
      {
        //scRGB
        float3 Rgb = ConditionallyConvertScRgbToHdr10(Colour);

        const bool3 NeedsProcessing = Rgb >= ShoulderStartInPq;

        [branch]
        if (!any(NeedsProcessing))
        {
          discard;
        }
        else
        {
          [branch]
          if (NeedsProcessing.r)
          {
            Rgb.r = LuminanceCompress(Rgb.r,
                                      ShoulderStartInPq,
                                      TargetLuminanceInPqMinusShoulderStartInPq);
          }
          [branch]
          if (NeedsProcessing.g)
          {
            Rgb.g = LuminanceCompress(Rgb.g,
                                      ShoulderStartInPq,
                                      TargetLuminanceInPqMinusShoulderStartInPq);
          }
          [branch]
          if (NeedsProcessing.b)
          {
            Rgb.b = LuminanceCompress(Rgb.b,
                                      ShoulderStartInPq,
                                      TargetLuminanceInPqMinusShoulderStartInPq);
          }
        }

        //scRGB
        Rgb = ConditionallyConvertHdr10ToScRgb(Rgb);

        Colour = Rgb;
        return;
      }

//      return float3(LuminanceCompress(Colour.r, TargetCllInPq, ShoulderStartInPq),
//                    LuminanceCompress(Colour.g, TargetCllInPq, ShoulderStartInPq),
//                    LuminanceCompress(Colour.b, TargetCllInPq, ShoulderStartInPq));
    }
  }

}

#endif //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))
