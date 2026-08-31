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


uniform uint INPUT_TRC
<
  ui_label    = "input gamma";
  ui_tooltip  = "\"linear with SDR black floor emulation (scRGB)\" fixes the sRGB<->gamma 2.2 mismatch";
  ui_type     = "combo";
  ui_items    = "2.2\0"
                "2.4\0"
                "linear (scRGB)\0"
                "linear with SDR black floor emulation (scRGB)\0"
                "sRGB\0"
                "PQ\0";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 0;

#undef CO_TRC_PQ

#define TRC_GAMMA_22                    0
#define TRC_GAMMA_24                    1
#define TRC_LINEAR                      2
#define TRC_LINEAR_WITH_BLACK_FLOOR_EMU 3
#define TRC_SRGB                        4
#define CO_TRC_PQ                          5

uniform uint OVERBRIGHT_HANDLING
<
  ui_label    = "overbright bits handling";
  ui_tooltip  = "- \"roll off to linear\" uses the inverse of the gamma function until the slope is 1 and then it is linear"
           "\n" "- \"filmic roll off\" uses the inverse of the gamma function to create a smooth roll off"
           "\n" "- \"linear\" takes the input value as is without applying any modifications"
           "\n" "- \"apply gamma\" applies the gamma normally, which leads to an exponential increase of the brightness that may be undesireable"
           "\n" "- \"clamp\" clamps the overbright bits away (mostly for testing)";
  ui_type     = "combo";
  ui_items    = "roll off to linear\0"
                "filmic roll off (S-curve)\0"
                "linear\0"
                "apply gamma\0"
                "clamp\0";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 0;

#define OVERBRIGHT_HANDLING_ROLL_OFF_TO_LINEAR 0
#define OVERBRIGHT_HANDLING_S_CURVE            1
#define OVERBRIGHT_HANDLING_LINEAR             2
#define OVERBRIGHT_HANDLING_APPLY_GAMMA        3
#define OVERBRIGHT_HANDLING_CLAMP              4

uniform float SDR_WHITEPOINT_NITS
<
  ui_label   = "SDR whitepoint";
  ui_tooltip = "Only works when input gamma is not PQ!";
  ui_type    = "drag";
  ui_units   = " nits";
  ui_min     = 1.f;
  ui_max     = 300.f;
  ui_step    = 1.f;
  hidden     = HIDDEN_OPTION_HDR_CSP;
> = 80.f;

uniform bool ENABLE_GAMMA_ADJUST
<
  ui_label   = "enable gamma adjust";
  ui_tooltip = "Only works when input gamma is not PQ!";
> = false;

uniform float GAMMA_ADJUST
<
  ui_label   = "gamma adjust";
  ui_tooltip = "Only works when input gamma is not PQ!";
  ui_type    = "drag";
  ui_min     = -1.f;
  ui_max     =  1.f;
  ui_step    =  0.001f;
  hidden     = HIDDEN_OPTION_HDR_CSP;
> = 0.f;

uniform bool ENABLE_CLAMPING
<
  ui_category = "clamping";
  ui_label    = "enable clamping";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = false;

uniform float CLAMP_NEGATIVE_TO
<
  ui_category = "clamping";
  ui_label    = "clamp negative values to";
  ui_type     = "drag";
  ui_min      = -125.f;
  ui_max      = 0.f;
  ui_step     = 0.1f;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = -125.f;

uniform float CLAMP_POSITIVE_TO
<
  ui_category = "clamping";
  ui_label    = "clamp positive values to";
  ui_type     = "drag";
  ui_min      = 1.f;
  ui_max      = 125.f;
  ui_step     = 0.1f;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 125.f;


void PS_MapSdrIntoHdr
(
      float4 Position : SV_Position,
  out float4 Output   : SV_Target0
)
{
  float4 inputColour = tex2Dfetch(SamplerBackBuffer, int2(Position.xy));

  float3 colour = inputColour.rgb;

  static const bool inputTrcIsPq = INPUT_TRC == CO_TRC_PQ;

  switch(INPUT_TRC)
  {
    case TRC_GAMMA_22:
    {
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_ROLL_OFF_TO_LINEAR)
      {
        colour = Csp::Trc::Extended_Gamma22_Roll_Off_To_Linear_To::Linear(colour);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_S_CURVE)
      {
        colour = Csp::Trc::Extended_Gamma22_S_Curve_To::Linear(colour);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_LINEAR)
      {
        colour = Csp::Trc::Extended_Gamma22_Linear_To::Linear(colour);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_APPLY_GAMMA)
      {
        colour = sign(colour) * pow(abs(colour), 2.2f);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_CLAMP)
      {
        colour = saturate(colour);
        colour = pow(colour, 2.2f);
      }
      else
      {
        colour = 0.f;
      }
    }
    break;
    case TRC_GAMMA_24:
    {
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_ROLL_OFF_TO_LINEAR)
      {
        colour = Csp::Trc::Extended_Gamma24_Roll_Off_To_Linear_To::Linear(colour);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_S_CURVE)
      {
        colour = Csp::Trc::Extended_Gamma24_S_Curve_To::Linear(colour);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_LINEAR)
      {
        colour = Csp::Trc::Extended_Gamma24_Linear_To::Linear(colour);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_APPLY_GAMMA)
      {
        colour = sign(colour) * pow(abs(colour), 2.4f);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_CLAMP)
      {
        colour = saturate(colour);
        colour = pow(colour, 2.4f);
      }
      else
      {
        colour = 0.f;
      }
    }
    break;
    case TRC_LINEAR:
    {
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_LINEAR)
      { }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_CLAMP)
      {
        colour = saturate(colour);
      }
      else
      {
        colour = 0.f;
      }
    }
    break;
    case TRC_LINEAR_WITH_BLACK_FLOOR_EMU:
    {
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_ROLL_OFF_TO_LINEAR)
      {
        float3 abs_colour  = abs(colour);
        float3 sign_colour = sign(colour);

        colour = sign_colour * Csp::Trc::Extended_Gamma22_Roll_Off_To_Linear_To::Linear(Csp::Trc::Linear_To::sRGB(abs_colour));
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_S_CURVE)
      {
        float3 abs_colour  = abs(colour);
        float3 sign_colour = sign(colour);

        colour = sign_colour * Csp::Trc::Extended_Gamma22_S_Curve_To::Linear(Csp::Trc::Linear_To::sRGB(abs_colour));
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_LINEAR)
      {
        float3 absColour  = abs(colour);
        float3 signColour = sign(colour);

        colour = absColour < 1.f ? signColour * pow(Csp::Trc::Linear_To::sRGB(absColour), 2.2f)
                                 : colour;
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_APPLY_GAMMA)
      {
        float3 abs_colour  = abs(colour);
        float3 sign_colour = sign(colour);

        colour = sign_colour * pow(Csp::Trc::Linear_To::sRGB(abs_colour), 2.2f);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_CLAMP)
      {
        colour = saturate(colour);
        colour = pow(Csp::Trc::Linear_To::sRGB(colour), 2.2f);
      }
      else
      {
        colour = 0.f;
      }
    }
    break;
    case TRC_SRGB:
    {
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_ROLL_OFF_TO_LINEAR)
      {
        colour = Csp::Trc::Extended_sRGB_Roll_Off_To_Linear_To::Linear(colour);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_S_CURVE)
      {
        colour = Csp::Trc::Extended_sRGB_S_Curve_To::Linear(colour);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_LINEAR)
      {
        colour = Csp::Trc::Extended_sRGB_Linear_To::Linear(colour);
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_APPLY_GAMMA)
      {
        colour = sign(colour) * Csp::Trc::sRGB_To::Linear(abs(colour));
      }
      else
      BRANCH()
      if (OVERBRIGHT_HANDLING == OVERBRIGHT_HANDLING_CLAMP)
      {
        colour = saturate(colour);
        colour = Csp::Trc::sRGB_To::Linear(colour);
      }
      else
      {
        colour = 0.f;
      }
    }
    break;
    case CO_TRC_PQ:
    {
      //scRGB
      colour = ConditionallyLineariseHdr10(colour);
      colour = ConditionallyConvertBt2020ToBt709(colour);

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
      colour *= 125.f;
#elif (ACTUAL_COLOUR_SPACE == CSP_BT2020_EXTENDED)
      colour *= 100.f;
#endif
    }
    break;
    default:
    {
      colour = 0.f;
    }
    break;
  }

  BRANCH()
  if (ENABLE_CLAMPING)
  {
    colour = clamp(colour, CLAMP_NEGATIVE_TO, CLAMP_POSITIVE_TO);
  }

  BRANCH()
  if (ENABLE_GAMMA_ADJUST
   && !inputTrcIsPq)
  {
    colour = Csp::Trc::Extended_Gamma_Adjust(colour, 1.f + GAMMA_ADJUST);
  }

//  if (dot(BT709_To_XYZ[1].rgb, colour) < 0.f)
//    colour = float3(0.f, 0.f, 0.f);

  BRANCH()
  if (!inputTrcIsPq)
  {

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

    colour *= (SDR_WHITEPOINT_NITS / 80.f);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

    colour *= (SDR_WHITEPOINT_NITS / 10000.f);

#elif (ACTUAL_COLOUR_SPACE == CSP_BT2020_EXTENDED)

    colour *= (SDR_WHITEPOINT_NITS / 100.f);

#endif

    //HDR10
    colour = ConditionallyConvertBt709ToBt2020(colour);
    colour = ConditionallyConvertNormalisedBt2020ToHdr10(colour);
  }

  //colour = fixNAN(colour);

  Output = float4(colour, inputColour.a);
}


technique lilium__map_SDR_into_HDR
<
  ui_label = "Lilium's map SDR into HDR";
>
{
  pass PS_MapSdrIntoHdr
  {
    VertexShader = VS_PostProcessWithoutTexCoord;
     PixelShader = PS_MapSdrIntoHdr;
  }
}

#else //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))

ERROR_STUFF

technique lilium__map_SDR_into_HDR
<
  ui_label = "Lilium's map SDR into HDR (ERROR)";
>
VS_ERROR

#endif //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))
