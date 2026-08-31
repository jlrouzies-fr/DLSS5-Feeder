#include "lilium__include/include_main.fxh"

#if (defined(IS_ANALYSIS_CAPABLE_API)    \
  && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB  \
    || ACTUAL_COLOUR_SPACE == CSP_HDR10) \
   || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))


#ifdef MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL

  #if (ACTUAL_COLOUR_SPACE == CSP_SCRGB \
    || ACTUAL_COLOUR_SPACE == CSP_HDR10)
    #define HIDDEN_OPTION_HDR_CSP false
  #else
    #define HIDDEN_OPTION_HDR_CSP true
  #endif

#else

  #define HIDDEN_OPTION_HDR_CSP false

#endif


HDR10_TO_LINEAR_LUT()


#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
static const float Normalisation_Factor_Inverse       =   0.0239486191f; //     1 / (10^((1 - 1.2) / 0.42) * 1000 /    80 * 10)
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
static const float Normalisation_Factor_Inverse       =   2.99357724f;   //     1 / (10^((1 - 1.2) / 0.42) * 1000 / 10000 * 10)
#else
static const float Normalisation_Factor_Inverse       =   0.f;
#endif
static const float Luminance_At_Neutral_Gamma         = 334.048492f;     //          10^((1 - 1.2) / 0.42) * 1000
static const float Max_Gain_Factor                    =  29.9357738f;    // 10000 / (10^((1 - 1.2) / 0.42) * 1000)

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  static const float3 luminance_weights = Csp::Mat::BT709_To_XYZ[1] * Normalisation_Factor_Inverse;
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
  static const float3 luminance_weights = Csp::Mat::BT2020_To_XYZ[1] * Normalisation_Factor_Inverse;
#else
  static const float3 luminance_weights = 0.f;
#endif


uniform float LINEAR_GAIN_FACTOR
<
  ui_category = "Linear gain";
  ui_label    = "linear gain factor";
  ui_tooltip  = "Linear increase or decrease of brightness.";
  ui_type     = "drag";
  ui_min      =  0.005f;
  ui_max      = 10.f;
  ui_step     =  0.005f;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 1.f;

uniform bool APPLY_LINEAR_GAIN_FIRST
<
  ui_category = "Linear gain";
  ui_label    = "apply linear gain first";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = true;

uniform float HLG_GAIN_FACTOR
<
  ui_category = "HLG gain";
  ui_label    = "HLG gain factor";
  ui_tooltip  = "Uses HLG OOTF math to increase the brightness of the brighter parts of the image.";
  ui_type     = "drag";
  ui_min      = 1.f;
  ui_max      = Max_Gain_Factor;
  ui_step     = 0.005f;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 1.f;

uniform bool ONLY_INCREASE_BRIGHTNESS
<
  ui_category = "HLG gain";
  ui_label    = "only increase brightness";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = true;


void Get_HLG_Params
(
  out float HLG_Gamma,
  out float Adjustment_Factor
)
{
  //LW
  const float lw = HLG_GAIN_FACTOR
                 * Luminance_At_Neutral_Gamma;

  HLG_Gamma = Csp::Trc::HLG_Gamma(lw) - 1.f;

  Adjustment_Factor = lw
                    / 10000.f
                    * Max_Gain_Factor;

  return;
}


void VS_Prepare_HDR_Brightness_Adjustment
(
  in                  uint   VertexID   : SV_VertexID,
  out                 float4 Position   : SV_Position
#if (__RESHADE_PERFORMANCE_MODE__ == 0)
                                                     ,
  out nointerpolation float2 HLG_Params : HLG_Params
#endif
)
{
  float2 tex_coord;
  tex_coord.x = (VertexID == 2) ? 2.f : 0.f;
  tex_coord.y = (VertexID == 1) ? 2.f : 0.f;

  Position = float4(tex_coord * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);

#if (__RESHADE_PERFORMANCE_MODE__ == 0)

  static float2 hlg_params;

  Get_HLG_Params(hlg_params[0], hlg_params[1]);

  HLG_Params = hlg_params;

#endif
}

void PS_HDR_Brightness_Adjustment
(
  in                  float4 Position   : SV_Position,
#if (__RESHADE_PERFORMANCE_MODE__ == 0)
  in  nointerpolation float2 HLG_Params : HLG_Params,
#endif
  out                 float4 Output     : SV_Target0
)
{
#if (__RESHADE_PERFORMANCE_MODE__ == 1)

  static float HLG_Params[2];

  Get_HLG_Params(HLG_Params[0], HLG_Params[1]);

#endif

  static const float hlg_gamma         = HLG_Params[0];
  static const float adjustment_factor = HLG_Params[1];


  const float3 rgb_in = tex2Dfetch(SamplerBackBuffer, int2(Position.xy)).rgb;

  float3 rgb_linear;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  rgb_linear = rgb_in;

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

  rgb_linear = FetchFromHdr10ToLinearLUT(rgb_in);

#else

  rgb_linear = 0.f;

#endif

  BRANCH()
  if (APPLY_LINEAR_GAIN_FIRST)
  {
    rgb_linear *= LINEAR_GAIN_FACTOR;
  }

  const float luminance = dot(luminance_weights, rgb_linear);

  //HLG OOTF
  const float adjust = adjustment_factor * pow(luminance, hlg_gamma);

  float3 rgb_new = rgb_linear * adjust;

  BRANCH()
  if (ONLY_INCREASE_BRIGHTNESS)
  {
    rgb_new = adjust > 1.f ? rgb_new
                           : rgb_linear;
  }

  BRANCH()
  if (!APPLY_LINEAR_GAIN_FIRST)
  {
    rgb_new *= LINEAR_GAIN_FACTOR;
  }

  float3 rgb_out;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  rgb_out = rgb_new;

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

  rgb_out = Csp::Trc::Linear_To::PQ(rgb_new);

#else

  rgb_out = 0.f;

#endif

  Output = float4(rgb_out, 1.f);

  return;
}


technique lilium__hdr_brightness_adjustment
<
  ui_label = "Lilium's HDR Brightness adjustment";
>
{
  pass HDR_Brightness_Adjustment
  {
    VertexShader = VS_Prepare_HDR_Brightness_Adjustment;
     PixelShader = PS_HDR_Brightness_Adjustment;
  }
}

#else //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))

ERROR_STUFF

technique lilium__hdr_brightness_adjustment
<
  ui_label = "Lilium's HDR Brightness adjustment (ERROR)";
>
VS_ERROR

#endif //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))
