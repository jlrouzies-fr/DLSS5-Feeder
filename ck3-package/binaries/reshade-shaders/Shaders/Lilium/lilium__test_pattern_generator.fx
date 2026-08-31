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


#if (BUFFER_WIDTH > BUFFER_HEIGHT)
  #define MAX_TEST_AREA_SIZE BUFFER_WIDTH
#else
  #define MAX_TEST_AREA_SIZE BUFFER_HEIGHT
#endif

uniform float2 TEST_AREA_SIZE
<
  ui_label    = "test area size";
  ui_type     = "drag";
  ui_min      = 0.f;
  ui_max      = MAX_TEST_AREA_SIZE;
  ui_step     = 1.f;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = float2(100.f, 100.f);

precise uniform float3 TEST_THINGY_BG_COLOUR
<
  ui_label    = "background colour (linear BT.709)";
  ui_type     = "drag";
  ui_min      = -10000.f;
  ui_max      =  10000.f;
  ui_step     =      0.000001f;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 0.f;

#define TESTY_MODE_RGB_BT709     0
#define TESTY_MODE_RGB_DCI_P3    1
#define TESTY_MODE_RGB_BT2020    2
#define TESTY_MODE_RGB_AP0D65    3
#define TESTY_MODE_xyY           4
#define TESTY_MODE_AS_FLOAT      5
#define TESTY_MODE_POS_INF_ON_R  6
#define TESTY_MODE_NEG_INF_ON_G  7
#define TESTY_MODE_NAN_ON_B      8

uniform uint TPG_MODE
<
  ui_label    = "mode";
  ui_type     = "combo";
  ui_items    = " RGB BT.709\0"
                " RGB DCI-P3\0"
                " RGB BT.2020\0"
                " ACES AP0 (D65 white point)\0"
                " xyY\0"
                " uint as float\0"
                "+Inf (0x7F800000) on R (else is 0)\0"
                "-Inf (0xFF800000) on G (else is 0)\0"
                " NaN (0xFFFFFFFF) on B (else is 0)\0";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = TESTY_MODE_RGB_BT709;

precise uniform float3 TPG_RGB
<
  ui_label    = "RGB (linear)";
  ui_type     = "drag";
  ui_min      = -10000.f;
  ui_max      =  10000.f;
  ui_step     =      0.000001f;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 80.f;

precise uniform float2 TPG_xy
<
  ui_label    = "xy";
  ui_tooltip  = "D65 white is:"
           "\n" "x: 0.3127"
           "\n" "y: 0.3290";
  ui_type     = "drag";
  ui_min      = 0.f;
  ui_max      = 1.f;
  ui_step     = 0.000001f;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 0.f;

precise uniform float TPG_Y
<
  ui_label    = "Y";
  ui_type     = "drag";
  ui_units    = " nits";
  ui_min      =     0.f;
  ui_max      = 10000.f;
  ui_step     =     0.000001f;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 80.f;

precise uniform uint3 TPG_AS_FLOAT
<
  ui_label    = "uint as float";
  ui_type     = "input";
  ui_min      = 0u;
  ui_max      = UINT_MAX;
  ui_step     = 1u;
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 0u;


void VS_Tpg
(
  in                  uint   VertexID : SV_VertexID,
  out                 float4 Position : SV_Position,
  out nointerpolation float4 TpgStuff : TpgStuff
)
{
  float2 texCoord;
  texCoord.x = (VertexID == 2) ? 2.f
                               : 0.f;
  texCoord.y = (VertexID == 1) ? 2.f
                               : 0.f;
  Position = float4(texCoord * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);

  float2 testAreaSizeDiv2 = TEST_AREA_SIZE / 2.f;

  TpgStuff.x = BUFFER_WIDTH  / 2.f - testAreaSizeDiv2.x - 1.f;
  TpgStuff.y = BUFFER_HEIGHT / 2.f - testAreaSizeDiv2.y - 1.f;
  TpgStuff.z = BUFFER_WIDTH  - TpgStuff.x;
  TpgStuff.w = BUFFER_HEIGHT - TpgStuff.y;
}

void PS_Tpg
(
  in                          float4 Position : SV_Position,
  in  nointerpolation         float4 TpgStuff : TpgStuff,
  out                 precise float4 Output   : SV_Target0
)
{
  const float2 pureCoord = floor(Position.xy);

  Output.a = 1.f;

  [branch]
  if (all(pureCoord.xy > TpgStuff.xy)
   && all(pureCoord.xy < TpgStuff.zw))
  {
    switch(TPG_MODE)
    {
      case TESTY_MODE_RGB_BT709:
      {
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

        Output.rgb = TPG_RGB / 80.f;

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

        Output.rgb = Csp::Trc::Linear_To::PQ(Csp::Mat::BT709_To::BT2020(TPG_RGB / 10000.f));

#endif
      }
      break;
      case TESTY_MODE_RGB_DCI_P3:
      {
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

        Output.rgb = Csp::Mat::DCIP3_To::BT709(TPG_RGB / 80.f);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

        Output.rgb = Csp::Trc::Linear_To::PQ(Csp::Mat::DCIP3_To::BT2020(TPG_RGB / 10000.f));

#endif
      }
      break;
      case TESTY_MODE_RGB_BT2020:
      {
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

        Output.rgb = Csp::Mat::BT2020_To::BT709(TPG_RGB / 80.f);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

        Output.rgb = Csp::Trc::Linear_To::PQ(TPG_RGB / 10000.f);

#endif
      }
      break;
      case TESTY_MODE_RGB_AP0D65:
      {
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

        Output.rgb = Csp::Mat::AP0D65_To::BT709(TPG_RGB / 80.f);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

        Output.rgb = Csp::Trc::Linear_To::PQ(Csp::Mat::AP0D65_To::BT2020(TPG_RGB / 10000.f));

#endif
      }
      break;
      case TESTY_MODE_xyY:
      {
        s_xyY xyY;

        xyY.xy = TPG_xy;
        xyY.Y  = TPG_Y;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

        xyY.Y /= 80.f;

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

        xyY.Y /= 10000.f;

#endif

        precise float3 XYZ = Csp::CIE::xyY_To::XYZ(xyY);

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

        Output.rgb = Csp::Mat::XYZ_To::BT709(XYZ);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

        Output.rgb = Csp::Trc::Linear_To::PQ(Csp::Mat::XYZ_To::BT2020(XYZ));

#endif
      }
      break;
      case TESTY_MODE_AS_FLOAT:
      {
        static const precise float3 asFloat = asfloat(TPG_AS_FLOAT);

        Output.rgb = asFloat;
      }
      break;
      case TESTY_MODE_POS_INF_ON_R:
      {
        static const precise float PositiveInfinity = asfloat(0x7F800000);

        Output.rgb = float3(PositiveInfinity, 0.f, 0.f);
      }
      break;
      case TESTY_MODE_NEG_INF_ON_G:
      {
        static const precise float NegativeInfinity = asfloat(0xFF800000);

        Output.rgb = float3(0.f, NegativeInfinity, 0.f);
      }
      break;
      case TESTY_MODE_NAN_ON_B:
      {
        static const precise float NaN = asfloat(0xFFFFFFFF);

        Output.rgb = float3(0.f, 0.f, NaN);
      }
      break;
      default:
      {
        Output.rgb = 0.f;
      }
      break;
    }
  }
  else
  {
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
    Output.rgb = TEST_THINGY_BG_COLOUR / 80.f;
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
    Output.rgb = Csp::Trc::Linear_To::PQ(Csp::Mat::BT709_To::BT2020(TEST_THINGY_BG_COLOUR / 10000.f));
#endif
  }

  return;
}


technique lilium__test_pattern_generator
<
  ui_label = "Lilium's TPG (Test Pattern Generator)";
>
{
  pass Tpg
  {
    VertexShader       = VS_Tpg;
     PixelShader       = PS_Tpg;
    ClearRenderTargets = true;
  }
}


#else //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))

ERROR_STUFF

technique lilium__test_pattern_generator
<
  ui_label = "Lilium's TPG (Test Pattern Generator) [ERROR]";
>
VS_ERROR

#endif //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))
