
namespace Csp
{

  namespace Trc
  {

    // Rec. ITU-R BT.2100-2 Table 4
    static const float PQ_m1 =  0.1593017578125f; // = 1305 / 8192;
    static const float PQ_m2 = 78.84375f;         // = 2523 /   32;
    static const float PQ_c1 =  0.8359375f;       // =  107 /  128;
    static const float PQ_c2 = 18.8515625f;       // = 2413 /  128;
    static const float PQ_c3 = 18.6875f;          // = 2392 /  128;

    static const float PQ_rcp_m1 = 6.27739477f;
    static const float PQ_rcp_m2 = 0.0126833133f;


    // Rec. ITU-R BT.2100-2 Table 4
    namespace PQ_To
    {

      #define PQ_TO_LINEAR(T)                       \
        T Linear(T E_)                              \
        {                                           \
          E_ = max(E_, 0.f);                        \
                                                    \
          T E_pow_1_div_m2 = pow(E_, PQ_rcp_m2);    \
                                                    \
          T num = max(E_pow_1_div_m2 - PQ_c1, 0.f); \
                                                    \
          T den = PQ_c2 - PQ_c3 * E_pow_1_div_m2;   \
                                                    \
          /* Y */                                   \
          return pow(num / den, PQ_rcp_m1);         \
        }

      // (EOTF) takes PQ values as input
      // outputs as 1 = 10000 nits
      PQ_TO_LINEAR(float)

      // (EOTF) takes PQ values as input
      // outputs as 1 = 10000 nits
      PQ_TO_LINEAR(float2)

      // (EOTF) takes PQ values as input
      // outputs as 1 = 10000 nits
      PQ_TO_LINEAR(float3)

      // (EOTF) takes PQ values as input
      // outputs as 1 = 10000 nits
      PQ_TO_LINEAR(float4)

      // (EOTF) takes PQ values as input
      // outputs nits
      #define PQ_TO_NITS(T)                  \
        T Nits(T E_)                         \
        {                                    \
          return Csp::Trc::PQ_To::Linear(E_) \
               * 10000.f;                    \
        }

      // (EOTF) takes PQ values as input
      // outputs nits
      PQ_TO_NITS(float)

      // (EOTF) takes PQ values as input
      // outputs nits
      PQ_TO_NITS(float2)

      // (EOTF) takes PQ values as input
      // outputs nits
      PQ_TO_NITS(float3)

      // (EOTF) takes PQ values as input
      // outputs nits
      PQ_TO_NITS(float4)

    } //PQ_To


    // Rec. ITU-R BT.2100-2 Table 4 (end)
    namespace Linear_To
    {
      #define LINEAR_TO_PQ(T)               \
        T PQ(T Y)                           \
        {                                   \
          Y = max(Y, 0.f);                  \
                                            \
          T Y_pow_m1 = pow(Y, PQ_m1);       \
                                            \
          T num = PQ_c1 + PQ_c2 * Y_pow_m1; \
                                            \
          T den =   1.f + PQ_c3 * Y_pow_m1; \
                                            \
          /* E' */                          \
          return pow(num / den, PQ_m2);     \
        }

      // (inverse EOTF) takes normalised to 10000 nits values as input
      LINEAR_TO_PQ(float)

      // (inverse EOTF) takes normalised to 10000 nits values as input
      LINEAR_TO_PQ(float2)

      // (inverse EOTF) takes normalised to 10000 nits values as input
      LINEAR_TO_PQ(float3)

      // (inverse EOTF) takes normalised to 10000 nits values as input
      LINEAR_TO_PQ(float4)

    } //Linear_To


    // Rec. ITU-R BT.2100-2 Table 4 (end)
    namespace Nits_To
    {
      // (OETF) takes nits as input
      #define NITS_TO_PQ(T)                  \
        T PQ(T Fd)                           \
        {                                    \
          T Y = Fd / 10000.f;                \
                                             \
          return Csp::Trc::Linear_To::PQ(Y); \
        }

      // (OETF) takes nits as input
      NITS_TO_PQ(float)

      // (OETF) takes nits as input
      NITS_TO_PQ(float2)

      // (OETF) takes nits as input
      NITS_TO_PQ(float3)

      // (OETF) takes nits as input
      NITS_TO_PQ(float4)
    } //Nits_To


    static const float HLG_a = 0.17883277;
    static const float HLG_b = 0.28466892; // = 1 - 4 * HLG_a
    static const float HLG_c = 0.55991072952956202016; //0.55991072952956202016 = 0.5 - HLG_a * ln(4 * HLG_a)

    // Rec. ITU-R BT.2100-3 Table 5
    namespace HLG_To
    {
      // Rec. ITU-R BT.2100-3 Table 5 (end)
      // (inverse OETF) takes HLG values as input
      #define HLG_TO_LINEAR(T)                           \
        T Linear(T X)                                    \
        {                                                \
          T e_lower = (X * X)                            \
                    / 3.f;                               \
                                                         \
          T e_upper = (exp((X - HLG_c) / HLG_a) + HLG_b) \
                    / 12.f;                              \
                                                         \
          /* E */                                        \
          return X <= 0.5f ? e_lower                     \
                           : e_upper;                    \
        }

      // (inverse OETF) takes HLG values as input
      HLG_TO_LINEAR(float)

      // (inverse OETF) takes HLG values as input
      HLG_TO_LINEAR(float2)

      // (inverse OETF) takes HLG values as input
      HLG_TO_LINEAR(float3)

      // (inverse OETF) takes HLG values as input
      HLG_TO_LINEAR(float4)
    } //HLG_To

    float HLG_Gamma
    (
      const float LW
    )
    {
      return 1.2f + 0.42f * log10(LW / 1000.f);
    }

    // Rec. ITU-R BT.2100-3 Table 5 (end)
    // (OOTF) takes linear light, the white luminance (LW), the luminance and the HLG Gamma as input
    #define HLG_OOTF_TEMPLATE(T)             \
      T HLG_OOTF                             \
      (                                      \
        const T     E,                       \
        const float Y,                       \
        const float LW,                      \
        const float Gamma                    \
      )                                      \
      {                                      \
        return LW * pow(Y, Gamma - 1.f) * E; \
      }

    // (OOTF) takes linear light, the white luminance (LW), the luminance and the HLG Gamma as input
    HLG_OOTF_TEMPLATE(float)

    // (OOTF) takes linear light, the white luminance (LW), the luminance and the HLG Gamma as input
    HLG_OOTF_TEMPLATE(float2)

    // (OOTF) takes linear light, the white luminance (LW), the luminance and the HLG Gamma as input
    HLG_OOTF_TEMPLATE(float3)

    // (OOTF) takes linear light, the white luminance (LW), the luminance and the HLG Gamma as input
    HLG_OOTF_TEMPLATE(float4)

    namespace Linear_To
    {
      // Rec. ITU-R BT.2100-2 Table 5
      // (OETF) takes normalised to 1000 nits values as input
      #define LINEAR_TO_HLG(T)                 \
        T HLG(T E)                             \
        {                                      \
          /* E' */                             \
          T e__lower = sqrt(3.f * E);          \
                                               \
          T e__upper = HLG_a                   \
                     * log(12.f * E - HLG_b)   \
                     + HLG_c;                  \
                                               \
          return E <= (1.f / 12.f) ? e__lower  \
                                   : e__upper; \
        }

      // (OETF) takes normalised to 1000 nits values as input
      LINEAR_TO_HLG(float)

      // (OETF) takes normalised to 1000 nits values as input
      LINEAR_TO_HLG(float2)

      // (OETF) takes normalised to 1000 nits values as input
      LINEAR_TO_HLG(float3)

      // (OETF) takes normalised to 1000 nits values as input
      LINEAR_TO_HLG(float4)
    } //Linear_To

  } //Trc

} //Csp
