
namespace Csp
{

  namespace Trc
  {

    //linear->gamma compressed = inverse EOTF -> ^(1 / 2.2)
    //
    //gamma compressed->display (also linear) = EOTF -> ^(2.2)

    namespace sRGB_To
    {
      // IEC 61966-2-1
      #define SRGB_TO_LINEAR(T)                                      \
        T Linear(T C)                                                \
        {                                                            \
          return C <= 0.04045f ? C / 12.92f                          \
                               : pow(((C + 0.055f) / 1.055f), 2.4f); \
        }

      SRGB_TO_LINEAR(float)
      SRGB_TO_LINEAR(float3)
    } //sRGB_To


    namespace Linear_To
    {
      #define LINEAR_TO_SRGB(T)                                          \
        T sRGB(T C)                                                      \
        {                                                                \
          return C <= 0.0031308f ? C * 12.92f                            \
                                 : 1.055f * pow(C, 1.f / 2.4f) - 0.055f; \
        }

      LINEAR_TO_SRGB(float)
      LINEAR_TO_SRGB(float3)
    } //Linear_To


    namespace Extended_sRGB_Roll_Off_To_Linear_To
    {
      // extended sRGB gamma including above 1 and below -1
      #define EXTENDED_SRGB_ROLL_OFF_TO_LINEAR_TO_LINEAR(T)                                                          \
        T Linear(const T C)                                                                                          \
        {                                                                                                            \
          const T absC  = abs(C);                                                                                    \
          const T signC = sign(C);                                                                                   \
                                                                                                                     \
          return                                                                                                     \
            absC > 1.24438285f ? C + (signC * 0.0720067024f)                                                         \
          : absC > 1.f         ? signC * ((1.055f * pow(absC - 0.940277040f, (1.f / 2.4f)) - 0.055f) + 0.728929638f) \
          : absC > 0.04045f    ? signC * pow((absC + 0.055f) / 1.055f, 2.4f)                                         \
          :                      C / 12.92f;                                                                         \
        }

      EXTENDED_SRGB_ROLL_OFF_TO_LINEAR_TO_LINEAR(float)
      EXTENDED_SRGB_ROLL_OFF_TO_LINEAR_TO_LINEAR(float3)
    } //Extended_sRGB_Roll_Off_To_Linear_To


    namespace Extended_sRGB_S_Curve_To
    {
      // extended sRGB gamma including above 1 and below -1
      #define EXTENDED_SRGB_S_CURVE_TO_LINEAR(T)                                                                  \
        T Linear(T C)                                                                                             \
        {                                                                                                         \
          const T absC  = abs(C);                                                                                 \
          const T signC = sign(C);                                                                                \
                                                                                                                  \
          return                                                                                                  \
            absC > 1.f      ? signC * ((1.055f * pow(absC - 0.940277040f, (1.f / 2.4f)) - 0.055f) + 0.728929638f) \
          : absC > 0.04045f ? signC * pow((absC + 0.055f) / 1.055f, 2.4f)                                         \
          :                   C / 12.92f;                                                                         \
        }

      EXTENDED_SRGB_S_CURVE_TO_LINEAR(float)
      EXTENDED_SRGB_S_CURVE_TO_LINEAR(float3)
    } //Extended_sRGB_S_Curve_To


    namespace Extended_sRGB_Linear_To
    {
      // extended sRGB gamma including above 1 and below -1
      #define EXTENDED_SRGB_LINEAR_TO_LINEAR(T)                           \
        T Linear(const T C)                                               \
        {                                                                 \
          const T absC  = abs(C);                                         \
          const T signC = sign(C);                                        \
                                                                          \
          return                                                          \
            absC > 1.f      ? C                                           \
          : absC > 0.04045f ? signC * pow((absC + 0.055f) / 1.055f, 2.4f) \
          :                   C / 12.92f;                                 \
        }

      EXTENDED_SRGB_LINEAR_TO_LINEAR(float)
      EXTENDED_SRGB_LINEAR_TO_LINEAR(float3)
    } //Extended_sRGB_Linear_To


// DO NOT USE!!!
// it does not match the ExtendedSrgbSCurveToLinear version!
//
//    namespace Linear_To
//    {
//      float ExtendedSrgbSCurve(float C)
//      {
//        static const float absC  = abs(C);
//        static const float signC = sign(C);
//
//        if (absC > 1.f)
//        {
//          return signC * pow((absC - 0.728929579257965087890625f + 0.055f) / 1.055f, 2.4f) + 0.940277040004730224609375f;
//        }
//        else if (absC > 0.0031308f)
//        {
//          return signC * (1.055f * pow(absC, (1.f / 2.4f)) - 0.055f);
//        }
//        else
//        {
//          return C * 12.92f;
//        }
//      }
//
//      float3 ExtendedSrgbSCurve(float3 Colour)
//      {
//        return float3(Csp::Trc::Linear_To::ExtendedSrgbSCurve(Colour.r),
//                      Csp::Trc::Linear_To::ExtendedSrgbSCurve(Colour.g),
//                      Csp::Trc::Linear_To::ExtendedSrgbSCurve(Colour.b));
//      }
//    }


    namespace sRGB_Accurate_To
    {
      // accurate sRGB with no slope discontinuity
      #define SrgbX       asfloat(0x3D20EA0B) //  0.0392857
      #define SrgbPhi     asfloat(0x414EC578) // 12.92321
      #define SrgbXDivPhi asfloat(0x3B4739A5) //  0.003039935

      #define SRGB_ACCURATE_TO_LINEAR(T)                          \
        T Linear(const T C)                                       \
        {                                                         \
          return C <= SrgbX ? C / SrgbPhi                         \
                            : pow(((C + 0.055f) / 1.055f), 2.4f); \
        }

      SRGB_ACCURATE_TO_LINEAR(float)
      SRGB_ACCURATE_TO_LINEAR(float3)
    } //sRGB_Accurate_To


    namespace Linear_To
    {
      #define LINEAR_TO_SRGB_ACCURATE(T)                                    \
        T sRGB_Accurate(const T C)                                          \
        {                                                                   \
          return C <= SrgbXDivPhi ? C * SrgbPhi                             \
                                  : 1.055f * pow(C, (1.f / 2.4f)) - 0.055f; \
        }

      LINEAR_TO_SRGB_ACCURATE(float)
      LINEAR_TO_SRGB_ACCURATE(float3)
    } //Linear_To


//    namespace Extended_sRGB_S_Curve_Accurate_To
//    {
//      float Linear(float C)
//      {
//        static const float absC  = abs(C);
//        static const float signC = sign(C);
//
//        if (absC > 1.f)
//        {
//          return signC * (1.055f * pow(absC, (1.f / 2.4f)) - 0.055f);
//        }
//        else if (absC > SrgbX)
//        {
//          return signC * pow((absC + 0.055f) / 1.055f, 2.4f);
//        }
//        else
//        {
//          return C / SrgbPhi;
//        }
//      }
//
//      float3 Linear(float3 Colour)
//      {
//        return float3(Csp::Trc::Extended_sRGB_S_Curve_Accurate_To::Linear(Colour.r),
//                      Csp::Trc::Extended_sRGB_S_Curve_Accurate_To::Linear(Colour.g),
//                      Csp::Trc::Extended_sRGB_S_Curve_Accurate_To::Linear(Colour.b));
//      }
//    } //Extended_sRGB_S_Curve_Accurate_To
//
//
//    namespace Linear_To
//    {
//      float Extended_sRGB_S_Curve_Accurate(float C)
//      {
//        static const float absC  = abs(C);
//        static const float signC = sign(C);
//
//        if (absC > 1.f)
//        {
//          return signC * pow((absC + 0.055f) / 1.055f, 2.4f);
//        }
//        else if (absC > SrgbXDivPhi)
//        {
//          return signC * (1.055f * pow(absC, (1.f / 2.4f)) - 0.055f);
//        }
//        else
//        {
//          return C * SrgbPhi;
//        }
//      }
//
//      float3 Extended_sRGB_S_Curve_Accurate(float3 Colour)
//      {
//        return float3(Csp::Trc::Linear_To::Extended_sRGB_S_Curve_Accurate(Colour.r),
//                      Csp::Trc::Linear_To::Extended_sRGB_S_Curve_Accurate(Colour.g),
//                      Csp::Trc::Linear_To::Extended_sRGB_S_Curve_Accurate(Colour.b));
//      }
//    } //Linear_To


    namespace Extended_Gamma22_Roll_Off_To_Linear_To
    {
      // extended gamma 2.2 including above 1 and below -1
      #define EXTENDED_GAMMA_22_ROLL_OFF_TO_LINEAR_TO_LINEAR(T)                                \
        T Linear(const T C)                                                                    \
        {                                                                                      \
          const T absC  = abs(C);                                                              \
          const T signC = sign(C);                                                             \
                                                                                               \
          return                                                                               \
            absC > 1.23562705f ? C + (signC * 0.0663648843)                                    \
          : absC > 1.f         ? signC * (pow(absC - 0.944479882f, 1.f / 2.2f) + 0.731282770f) \
          :                      signC * pow(absC, 2.2f);                                      \
        }

      EXTENDED_GAMMA_22_ROLL_OFF_TO_LINEAR_TO_LINEAR(float)
      EXTENDED_GAMMA_22_ROLL_OFF_TO_LINEAR_TO_LINEAR(float3)
    } //Extended_Gamma22_Roll_Off_To_Linear_To


    namespace Extended_Gamma22_S_Curve_To
    {
      // extended gamma 2.2 including above 1 and below 0
      #define EXTENDED_GAMMA_22_S_CURVE_TO_LINEAR(T)                                     \
        T Linear(const T C)                                                              \
        {                                                                                \
          const T absC  = abs(C);                                                        \
          const T signC = sign(C);                                                       \
                                                                                         \
          return                                                                         \
            absC <= 1.f ? signC * pow(absC, 2.2f)                                        \
                        : signC * (pow(absC - 0.944479882f, 1.f / 2.2f) + 0.731282770f); \
        }

        EXTENDED_GAMMA_22_S_CURVE_TO_LINEAR(float)
        EXTENDED_GAMMA_22_S_CURVE_TO_LINEAR(float3)
    } //Extended_Gamma22_S_Curve_To


    namespace Extended_Gamma22_Linear_To
    {
      // extended gamma 2.2 including above 1 and below 0
      #define EXTENDED_GAMMA_22_LINEAR_TO_LINEAR(T)   \
        T Linear(const T C)                           \
        {                                             \
          const T absC  = abs(C);                     \
          const T signC = sign(C);                    \
                                                      \
          return absC < 1.f ? signC * pow(absC, 2.2f) \
                            : C;                      \
        }

      EXTENDED_GAMMA_22_LINEAR_TO_LINEAR(float)
      EXTENDED_GAMMA_22_LINEAR_TO_LINEAR(float3)
    } //Extended_Gamma22_Linear_To


    namespace Extended_Gamma24_Roll_Off_To_Linear_To
    {
      // extended sRGB gamma including above 1 and below -1
      #define EXTENDED_GAMMA_24_ROLL_OFF_TO_LINEAR_TO_LINEAR(T)                                \
        T Linear(const T C)                                                                    \
        {                                                                                      \
          const T absC  = abs(C);                                                              \
          const T signC = sign(C);                                                             \
                                                                                               \
          return                                                                               \
            absC > 1.22295093f ? C + (signC * 0.0726261138)                                    \
          : absC > 1.f         ? signC * (pow(absC - 0.950292885f, 1.f / 2.4f) + 0.713687002f) \
          :                      signC * pow(absC, 2.4f);                                      \
        }

      EXTENDED_GAMMA_24_ROLL_OFF_TO_LINEAR_TO_LINEAR(float)
      EXTENDED_GAMMA_24_ROLL_OFF_TO_LINEAR_TO_LINEAR(float3)
    } //Extended_Gamma24_Roll_Off_To_Linear_To


    namespace Extended_Gamma24_S_Curve_To
    {
      // extended gamma 2.4 including above 1 and below 0
      #define EXTENDED_GAMMA_24_S_CURVE_TO_LINEAR(T)                                     \
        T Linear(const T C)                                                              \
        {                                                                                \
          const T absC  = abs(C);                                                        \
          const T signC = sign(C);                                                       \
                                                                                         \
          return                                                                         \
            absC <= 1.f ? signC * pow(absC, 2.4f)                                        \
                        : signC * (pow(absC - 0.950292885f, 1.f / 2.4f) + 0.713687002f); \
        }

      EXTENDED_GAMMA_24_S_CURVE_TO_LINEAR(float)
      EXTENDED_GAMMA_24_S_CURVE_TO_LINEAR(float3)
    } //Extended_Gamma24_S_Curve_To


    namespace Extended_Gamma24_Linear_To
    {
      // extended gamma 2.4 including above 1 and below 0
      #define EXTENDED_GAMMA_24_LINEAR_TO_LINEAR(T)   \
        T Linear(const T C)                           \
        {                                             \
          const T absC  = abs(C);                     \
          const T signC = sign(C);                    \
                                                      \
          return absC < 1.f ? signC * pow(absC, 2.4f) \
                            : C;                      \
        }

      EXTENDED_GAMMA_24_LINEAR_TO_LINEAR(float)
      EXTENDED_GAMMA_24_LINEAR_TO_LINEAR(float3)
    } //Extended_Gamma24_Linear_To


    //float X_power_TRC(float C, float pow_gamma)
    //{
    //  float pow_Inverse_gamma = 1.f / pow_gamma;
    //
    //  if (C < -1)
    //    return
    //      -pow(-C, pow_Inverse_gamma);
    //  else if (C < 0)
    //    return
    //      -pow(-C, pow_gamma);
    //  else if (C <= 1)
    //    return
    //      pow(C, pow_gamma);
    //  else
    //    return
    //      pow(C, pow_Inverse_gamma);
    //}
    //
    //float3 X_power_TRC(float3 Colour, float pow_gamma)
    //{
    //  return float3(
    //    X_power_TRC(Colour.r, pow_gamma),
    //    X_power_TRC(Colour.g, pow_gamma),
    //    X_power_TRC(Colour.b, pow_gamma));
    //}


    // gamma adjust including values above 1 and below 0
    float Extended_Gamma_Adjust(float C, float Adjust)
    {
      float inverseAdjust = 1.f / Adjust;

      static const float absC = abs(C);
      static const float signC = sign(C);

      [branch]
      if (absC > 1.f)
      {
        return signC * pow(absC, inverseAdjust);
      }
      else
      {
        return signC * pow(absC, Adjust);
      }
    }

    float3 Extended_Gamma_Adjust(float3 Colour, float Adjust)
    {
      return float3(Extended_Gamma_Adjust(Colour.r, Adjust),
                    Extended_Gamma_Adjust(Colour.g, Adjust),
                    Extended_Gamma_Adjust(Colour.b, Adjust));
    }

  } //Trc

} //Csp
