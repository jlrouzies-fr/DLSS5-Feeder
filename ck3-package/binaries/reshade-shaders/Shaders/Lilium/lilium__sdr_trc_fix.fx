#include "lilium__include/include_main.fxh"

//#define _DEBUG

// TODO:
// - implement vertex shader for optimisation ?
// - add namespace for UI

#if (ACTUAL_COLOUR_SPACE == CSP_SRGB \
  || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))


#ifdef MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL

  #if (ACTUAL_COLOUR_SPACE == CSP_SRGB)
    #define HIDDEN_OPTION_SDR_CSP false
  #else
    #define HIDDEN_OPTION_SDR_CSP true
  #endif

#else

  #define HIDDEN_OPTION_SDR_CSP false

#endif


uniform uint INPUT_TRC
<
  ui_label = "input Gamma";
  ui_type  = "combo";
  ui_items = "power Gamma\0"
             "sRGB\0";
  hidden   = HIDDEN_OPTION_SDR_CSP;
> = 0;

uniform uint TARGET_TRC
<
  ui_label = "target Gamma";
  ui_type  = "combo";
  ui_items = "sRGB\0"
             "power Gamma\0";
  hidden   = HIDDEN_OPTION_SDR_CSP;
> = 0;

uniform float INPUT_POWER_GAMMA
<
  ui_label = "input power Gamma";
  ui_type  = "drag";
  ui_min   = 1.f;
  ui_max   = 3.f;
  ui_step  = 0.01f;
  hidden   = HIDDEN_OPTION_SDR_CSP;
> = 2.2f;

uniform float TARGET_POWER_GAMMA
<
  ui_label = "target power Gamma";
  ui_type  = "drag";
  ui_min   = 1.f;
  ui_max   = 3.f;
  ui_step  = 0.01f;
  hidden   = HIDDEN_OPTION_SDR_CSP;
> = 2.2f;

uniform bool USE_BT1886
<
  ui_category = "BT.1886";
  ui_label    = "use BT.1886 for blackpoint correction";
  hidden      = HIDDEN_OPTION_SDR_CSP;
> = false;

uniform float BT1886_TARGET_WHITEPOINT
<
  ui_category = "BT.1886";
  ui_label    = "target whitepoint";
  ui_type     = "drag";
  ui_min      = 1.f;
  ui_max      = 1000.f;
  ui_step     = 0.5f;
  hidden      = HIDDEN_OPTION_SDR_CSP;
> = 100.f;

uniform float BT1886_TARGET_BLACKPOINT
<
  ui_category = "BT.1886";
  ui_label    = "target blackpoint";
  ui_type     = "drag";
  ui_min      = 0.f;
  ui_max      = 1.f;
  ui_step     = 0.0001f;
  hidden      = HIDDEN_OPTION_SDR_CSP;
> = 0.f;


float3 Bt1886
(
  const float3 LinearColour
)
{
  float3 V = pow(LinearColour, 1.f / 2.4f);

  static const float powLw = pow(BT1886_TARGET_WHITEPOINT, 1.f / 2.4f);
  static const float powLb = pow(BT1886_TARGET_BLACKPOINT, 1.f / 2.4f);

  static const float powLw_minus_powLb = powLw - powLb;

  static const float a = pow(powLw_minus_powLb, 2.4f);
  static const float b = powLb
                       / powLw_minus_powLb;

  float3 L = a * pow(max(V + b, 0.f), 2.4f);

  //normalise so that 0 is the black point and 1 the white point of the display
  return (L - BT1886_TARGET_BLACKPOINT) / (BT1886_TARGET_WHITEPOINT - BT1886_TARGET_BLACKPOINT);
}


void PS_SdrTrcFix
(
  in  float4 Position : SV_Position,
  out float4 Output   : SV_Target0
)
{
  const float4 input = tex2Dfetch(SamplerBackBuffer, int2(Position.xy));

  float3 fixedGamma = input.rgb;

  BRANCH()
  if (INPUT_TRC == 0)
  {
    fixedGamma = pow(fixedGamma, INPUT_POWER_GAMMA);
  }
  else
  {
    fixedGamma = Csp::Trc::sRGB_To::Linear(fixedGamma);
  }

  BRANCH()
  if (USE_BT1886)
  {
    fixedGamma = Bt1886(fixedGamma);
  }

  BRANCH()
  if (TARGET_TRC == 0)
  {
    fixedGamma = Csp::Trc::Linear_To::sRGB(fixedGamma);
  }
  else
  {
    fixedGamma = pow(fixedGamma, 1.f / TARGET_POWER_GAMMA);
  }

  Output = float4(fixedGamma, input.a);
}


technique lilium__sdr_trc_fix
<
  ui_label = "Lilium's SDR TRC fix";
>
{
  pass PS_SdrTrcFix
  {
    VertexShader = VS_PostProcessWithoutTexCoord;
     PixelShader = PS_SdrTrcFix;
  }
}

#endif //(ACTUAL_COLOUR_SPACE == CSP_SRGB || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))
