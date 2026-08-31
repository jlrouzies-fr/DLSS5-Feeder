#include "lilium__include/include_main.fxh"


#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB \
  || ACTUAL_COLOUR_SPACE == CSP_HDR10 \
  || ACTUAL_COLOUR_SPACE == CSP_SRGB)


#ifndef DITHER_TEST_PATTERN_ENABLE
  #define DITHER_TEST_PATTERN_ENABLE NO
#endif


HDR10_TO_LINEAR_LUT()


#if (!defined(IS_HDR_CSP) \
  && !defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))
  #define _DITHER_ENABLE                               SDR_DITHER_ENABLE
  #define _DITHER_STRENGTH_HIGH                        SDR_STRENGTH_HIGH
  #define _DITHER_STRENGTH_LOW                         SDR_STRENGTH_LOW
  #define _DITHER_TARGET_BIT_DEPTH                     SDR_DITHER_TARGET_BIT_DEPTH
  #define _DITHER_ROUNDING_ENABLE                      SDR_DITHER_ROUNDING_ENABLE
  #define _DITHER_TEMPORAL_TIMING_METHOD               SDR_DITHER_TEMPORAL_TIMING_METHOD
  #define _DITHER_TEMPORAL_PERIOD_TIME                 SDR_DITHER_TEMPORAL_PERIOD_TIME
  #define _DITHER_TEMPORAL_PERIOD_FRAME                SDR_DITHER_TEMPORAL_PERIOD_FRAME
  #define _DITHER_TEST_PATTERN_PATTERN_BIT_DEPTH       SDR_DITHER_TEST_PATTERN_SIMULATED_BIT_DEPTH
  #define _DITHER_TEST_PATTERN_COLOUR                  SDR_DITHER_TEST_PATTERN_COLOUR
  #define _DITHER_TEST_PATTERN_MIN_BRIGHTNESS          SDR_DITHER_TEST_PATTERN_MIN_BRIGHTNESS
  #define _DITHER_TEST_PATTERN_MAX_BRIGHTNESS          SDR_DITHER_TEST_PATTERN_MAX_BRIGHTNESS
  #define _DITHER_TEST_PATTERN_DUPLICATE_ENTRY_X_TIMES SDR_DITHER_TEST_PATTERN_DUPLICATE_TIMES
  #define _DITHER_TEST_PATTERN_HEIGHT                  SDR_DITHER_TEST_PATTERN_HEIGHT
#else
  #define _DITHER_ENABLE                               DITHER_ENABLE
  #define _DITHER_STRENGTH_HIGH                        DITHER_STRENGTH_HIGH
  #define _DITHER_STRENGTH_LOW                         DITHER_STRENGTH_LOW
  #define _DITHER_TARGET_BIT_DEPTH                     DITHER_TARGET_BIT_DEPTH
  #define _DITHER_ROUNDING_ENABLE                      DITHER_ROUNDING_ENABLE
  #define _DITHER_TEMPORAL_TIMING_METHOD               DITHER_TEMPORAL_TIMING_METHOD
  #define _DITHER_TEMPORAL_PERIOD_TIME                 DITHER_TEMPORAL_PERIOD_TIME
  #define _DITHER_TEMPORAL_PERIOD_FRAME                DITHER_TEMPORAL_PERIOD_FRAME
  #define _DITHER_TEST_PATTERN_PATTERN_BIT_DEPTH       DITHER_TEST_PATTERN_SIMULATED_BIT_DEPTH
  #define _DITHER_TEST_PATTERN_COLOUR                  DITHER_TEST_PATTERN_COLOUR
  #define _DITHER_TEST_PATTERN_MIN_BRIGHTNESS          DITHER_TEST_PATTERN_MIN_BRIGHTNESS
  #define _DITHER_TEST_PATTERN_MAX_BRIGHTNESS          DITHER_TEST_PATTERN_MAX_BRIGHTNESS
  #define _DITHER_TEST_PATTERN_DUPLICATE_ENTRY_X_TIMES DITHER_TEST_PATTERN_DUPLICATE_TIMES
  #define _DITHER_TEST_PATTERN_HEIGHT                  DITHER_TEST_PATTERN_HEIGHT
#endif


uniform uint FRAMECOUNT
<
  source = "framecount";
>;

uniform float TIMER
<
  source = "timer";
>;


#define BLUE_NOISE_TEXTURE_SIZE_XY 64


texture2D Texture_Blue_Noise
<
  source = "lilium__blue_noise_64x64.png";
>
{
  Width  = BLUE_NOISE_TEXTURE_SIZE_XY;
  Height = BLUE_NOISE_TEXTURE_SIZE_XY;

  Format = R8;
};

sampler2D<float> Sampler_Blue_Noise
{
  Texture = Texture_Blue_Noise;

  AddressU = REPEAT;
  AddressV = REPEAT;
  AddressW = REPEAT;

  MagFilter = POINT;
  MinFilter = POINT;
  MipFilter = POINT;
};


#if (DITHER_TEST_PATTERN_ENABLE == YES)

  #define MAX_FOR__8_BIT     255
  #define MAX_FOR__9_BIT     511
  #define MAX_FOR_10_BIT    1023
  #define MAX_FOR_11_BIT    2047
  #define MAX_FOR_11_5_BIT  2895
  #define MAX_FOR_12_BIT    4095
  #define MAX_FOR_13_BIT    8191
  #define MAX_FOR_14_BIT   16383
  #define MAX_FOR_15_BIT   32767
  #define MAX_FOR_16_BIT   65535

#ifdef IS_HDR_CSP

  static const float max_array[4] = { MAX_FOR_10_BIT,
                                      MAX_FOR_11_BIT,
                                      MAX_FOR_11_5_BIT,
                                      MAX_FOR_12_BIT };

#else //IS_HDR_CSP

  static const float max_array[9] = { MAX_FOR__8_BIT,
                                      MAX_FOR__9_BIT,
                                      MAX_FOR_10_BIT,
                                      MAX_FOR_11_BIT,
                                      MAX_FOR_12_BIT,
                                      MAX_FOR_13_BIT,
                                      MAX_FOR_14_BIT,
                                      MAX_FOR_15_BIT,
                                      MAX_FOR_16_BIT };

#endif

uniform uint _DITHER_TEST_PATTERN_PATTERN_BIT_DEPTH
<
  ui_category = "Dither Test Pattern";
  ui_label    = "pattern bit depth";
  ui_type     = "combo";
  ui_items    =
#ifdef IS_HDR_CSP

                "10.0 bit\0"
                "11.0 bit\0"
                "11.5 bit (theoritical ideal)\0"
                "12.0 bit\0"

#else //IS_HDR_CSP

                " 8 bit\0"
                " 9 bit\0"
                "10 bit\0"
                "11 bit\0"
                "12 bit\0"
                "13 bit\0"
                "14 bit\0"
                "15 bit\0"
                "16 bit\0"

#endif //IS_HDR_CSP
                ;
#ifdef IS_HDR_CSP
> = 3u;
#elif (BUFFER_COLOR_BIT_DEPTH == 16)
> = 4u;
#elif (BUFFER_COLOR_BIT_DEPTH == 10)
> = 2u;
#else
> = 0u;
#endif

uniform uint _DITHER_TEST_PATTERN_COLOUR
<
  ui_category = "Dither Test Pattern";
  ui_label    = "test pattern colour";
  ui_type     = "combo";
  ui_items    = "white\0"
                "red\0"
                "green\0"
                "blue\0"
                "yellow\0"
                "cyan\0"
                "magenta\0";
> = 0u;

#define DITHER_TEST_PATTERN_COLOUR_WHITE    0u
#define DITHER_TEST_PATTERN_COLOUR_RED      1u
#define DITHER_TEST_PATTERN_COLOUR_GREEN    2u
#define DITHER_TEST_PATTERN_COLOUR_BLUE     3u
#define DITHER_TEST_PATTERN_COLOUR_YELLOW   4u
#define DITHER_TEST_PATTERN_COLOUR_CYAN     5u
#define DITHER_TEST_PATTERN_COLOUR_MAGENTA  6u

#ifdef IS_HDR_CSP

uniform uint DITHER_TEST_PATTERN_COLOUR_PRIMARIES
<
  ui_category = "Dither Test Pattern";
  ui_label    = "test pattern colour primaries";
  ui_type     = "combo";
  ui_items    = "BT.709\0"
                "DCI-P3\0"
                "BT.2020\0";
> = 0u;

#define DITHER_TEST_PATTERN_COLOUR_PRIMARIES_BT709  0u
#define DITHER_TEST_PATTERN_COLOUR_PRIMARIES_DCI_P3 1u
#define DITHER_TEST_PATTERN_COLOUR_PRIMARIES_BT2020 2u

#endif //IS_HDR_CSP

uniform float _DITHER_TEST_PATTERN_MIN_BRIGHTNESS
<
  ui_category = "Dither Test Pattern";
  ui_label    = "test pattern min brightness";
  ui_type     = "slider";
  ui_units    = " CLL";
#ifdef IS_HDR_CSP
  ui_min      =   0.f;
  ui_max      = 100.f;
  ui_step     =   0.01f;
#else //IS_HDR_CSP
  ui_min      =  0.f;
  ui_max      = 20.f;
  ui_step     =  0.1f;
#endif //IS_HDR_CSP
> = 0.f;

uniform float _DITHER_TEST_PATTERN_MAX_BRIGHTNESS
<
  ui_category = "Dither Test Pattern";
  ui_label    = "test pattern max brightness";
  ui_type     = "slider";
  ui_units    = " CLL";
#ifdef IS_HDR_CSP
  ui_min      =    1.f;
  ui_max      = 1000.f;
  ui_step     =    1.f;
#else //IS_HDR_CSP
  ui_min      =  20.f;
  ui_max      = 100.f;
  ui_step     =   1.f;
#endif //IS_HDR_CSP
#ifdef IS_HDR_CSP
> = 20.f;
#else
> = 50.f;
#endif

uniform uint _DITHER_TEST_PATTERN_DUPLICATE_ENTRY_X_TIMES
<
  ui_category = "Dither Test Pattern";
  ui_label    = "duplicate each entry x times";
  ui_type     = "slider";
  ui_units    = "x";
  ui_min      =  1u;
#ifdef IS_HDR_CSP
  ui_max      = 10u;
#else
  ui_max      = 10u;
#endif
  ui_step     =  1u;
> = 1u;

uniform float _DITHER_TEST_PATTERN_HEIGHT
<
  ui_category = "Dither Test Pattern";
  ui_label    = "test pattern height";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      =  5.f;
  ui_max      = 75.f;
  ui_step     =  5.f;
> = 25.f;

#endif //DITHER_TEST_PATTERN_ENABLE == YES

uniform bool _DITHER_ENABLE
<
  ui_category = "dither settings";
  ui_label    = "enable dithering";
> = true;

uniform float _DITHER_STRENGTH_HIGH
<
  ui_category = "dither settings";
  ui_label    = "dither strength high";
  ui_tooltip  = "At a value of 1 a pixel value will be at max changed by +/-1."
           "\n" "For HDR I found a value of \"5\" to be the most pleasing on my LG OLED 42C2."
           "\n" "You might need another value depending on your display.";
  ui_type     = "slider";
  ui_min      =  0.f;
  ui_max      = 10.f;
  ui_step     =  0.1f;
#ifdef IS_HDR_CSP
> = 5.f;
#else
> = 3.f;
#endif

uniform float _DITHER_STRENGTH_LOW
<
  ui_category = "dither settings";
  ui_label    = "dither strength low";
  ui_tooltip  = "At a value of 1 a pixel value will be at max changed by +/-1."
           "\n" "For HDR I found a value of \"3\" to be the most pleasing on my LG OLED 42C2."
           "\n" "You might need another value depending on your display.";
  ui_type     = "slider";
  ui_min      =  0.f;
  ui_max      = 10.f;
  ui_step     =  0.1f;
#ifdef IS_HDR_CSP
> = 3.f;
#else
> = 2.5f;
#endif

uniform float _DITHER_STRENGTH_LOW_STARTING_POINT
<
  ui_category = "dither settings";
  ui_label    = "dither strength low starting point";
  ui_tooltip  = "code value in display gamma space where below this"
           "\n" "\"dither strength low\" will be applied instead of \"dither strength high\"";
  ui_type     = "slider";
  ui_min      =   1.f;
  ui_max      = 200.f;
  ui_step     =   1.f;
#ifdef IS_HDR_CSP
> = 25.f;
#else
> = 35.f;
#endif

uniform float _DITHER_TARGET_BIT_DEPTH
<
  ui_category = "dither settings";
  ui_label    = "dither target bit depth (display bit depth)";
  ui_tooltip  = "this should match the bit depth of your display."
           "\n" "influences the \"dither strength\" parameter";
  ui_type     = "slider";
  ui_units    = " bit";
  ui_min      =  6.f;
#if (BUFFER_COLOR_BIT_DEPTH > 10)
  ui_max      = 12.f;
#elif (BUFFER_COLOR_BIT_DEPTH > 8)
  ui_max      = 10.f;
#else
  ui_max      =  8.f;
#endif
  ui_step     =  0.1f;
#if (BUFFER_COLOR_BIT_DEPTH > 8)
> = 10.f;
#else
> = 8.f;
#endif

#ifdef IS_HDR_CSP

uniform float DITHER_TARGET_GAMMA
<
  ui_category = "dither settings";
  ui_label    = "dither target gamma";
  ui_tooltip  = "this should match the native gamma of your display!";
  ui_type     = "slider";
  ui_min      = 2.0f;
  ui_max      = 2.4f;
  ui_step     = 0.01f;
> = 2.2f;

#endif //IS_HDR_CSP

uniform bool _DITHER_ROUNDING_ENABLE
<
  ui_category = "dither settings";
  ui_label    = "enable rounding";
  ui_tooltip  = "round the values to exact colour space values";
> = false;

#ifdef IS_HDR_CSP

uniform uint DITHER_DISPLAY_MAX_BRIGHTNESS
<
  ui_label   = "dither display max brightness";
  ui_tooltip = "this should match the max brightness of your display!";
  ui_type    = "drag";
  ui_units   = " nits";
  ui_min     =   400.f;
  ui_max     = 10000.f;
  ui_step    =    10.f;
> = 800.f;

uniform uint DITHER_DISPLAY_PRIMARIES
<
  ui_label   = "dither display primaries";
  ui_tooltip = "this should match the primaries of your display"
          "\n" "use:"
          "\n" " - DCI-P3 for W-OLED displays"
          "\n" " - BT.2020 for QD-OLED displays"
          "\n"
          "\n" "colours outside of the target primaries will be clipped!!"
          "\n"
          "\n" "custom primaries and white point will come in the future";
  ui_type    = "combo";
  ui_items   = "BT.2020\0"
               "DCI-P3\0";
> = 0u;

#define DITHER_DISPLAY_PRIMARIES_BT2020 0u
#define DITHER_DISPLAY_PRIMARIES_DCI_P3 1u

#endif //IS_HDR_CSP

uniform uint _DITHER_TEMPORAL_TIMING_METHOD
<
  ui_label   = "temporal timing method";
  ui_tooltip = "use:"
          "\n" " - \"timer based\" for variable frame rates"
          "\n" " - \"frame based\" for constant frame rates";
  ui_type    = "combo";
  ui_items   = "frame based\0"
               "timer based\0";
> = 0u;

#define DITHER_TEMPORAL_TIMING_METHOD_FRAME_BASED 0u
#define DITHER_TEMPORAL_TIMING_METHOD_TIMER_BASED 1u

uniform uint _DITHER_TEMPORAL_PERIOD_FRAME
<
  ui_category = "temporal timing method frame based";
  ui_label    = "temporal dither frame period";
  ui_tooltip  = "change dither pattern every x frames";
  ui_type     = "slider";
  ui_units    = " frames";
  ui_min      = 1u;
  ui_max      = 5u;
  ui_step     = 1u;
> = 1u;

uniform float _DITHER_TEMPORAL_PERIOD_TIME
<
  ui_category = "temporal timing method timer based";
  ui_label    = "temporal dither timer period";
  ui_tooltip  = "change dither pattern every x milliseconds";
  ui_type     = "drag";
  ui_units    = " ms";
  ui_min      =   1.f;
  ui_max      = 100.f;
  ui_step     =   0.01f;
> = 16.66f;


void VS_Prepare_Dither
(
  in                  uint   VertexID    : SV_VertexID,
  out                 float4 Position    : SV_Position,
#if (__RESHADE_PERFORMANCE_MODE__ == 0)
  out nointerpolation int3   Fetch_Stuff : Fetch_Stuff
#else
  out nointerpolation int2   Fetch_Stuff : Fetch_Stuff
#endif
)
{
  float2 tex_coord;
  tex_coord.x = (VertexID == 2) ? 2.f : 0.f;
  tex_coord.y = (VertexID == 1) ? 2.f : 0.f;

  Position = float4(tex_coord * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);

  Fetch_Stuff = 0;

#if (__RESHADE_PERFORMANCE_MODE__ == 0)
  Fetch_Stuff[2] = asint(pow(2.f, _DITHER_TARGET_BIT_DEPTH) - 1.f);
#endif

  uint fetch_iterator = 0u;

  BRANCH()
  if (_DITHER_TEMPORAL_TIMING_METHOD == DITHER_TEMPORAL_TIMING_METHOD_FRAME_BASED)
  {
    fetch_iterator = uint(FRAMECOUNT) / _DITHER_TEMPORAL_PERIOD_FRAME;
  }
  else
  {
    // can't do +0.5 otherwise it overflows
    fetch_iterator = uint(TIMER / _DITHER_TEMPORAL_PERIOD_TIME);
  }

  [flatten]
  if (fetch_iterator & 1u)
  {
    Fetch_Stuff[0] = BLUE_NOISE_TEXTURE_SIZE_XY - 1;
  }
  [flatten]
  if (fetch_iterator & 2u)
  {
    Fetch_Stuff[1] = BLUE_NOISE_TEXTURE_SIZE_XY - 1;
  }

  return;
}

void PS_Dither
(
  in                  float4 Position    : SV_Position,
#if (__RESHADE_PERFORMANCE_MODE__ == 0)
  in  nointerpolation int3   Fetch_Stuff : Fetch_Stuff,
#else
  in  nointerpolation int2   Fetch_Stuff : Fetch_Stuff,
#endif
  out                 float4 Output      : SV_Target0
)
{
  const int2 position_as_int = int2(Position.xy);

#if (__RESHADE_PERFORMANCE_MODE__ == 0)
  const float dither_depth = asfloat(Fetch_Stuff[2]);
#else
  const float dither_depth = pow(2.f, _DITHER_TARGET_BIT_DEPTH) - 1.f;
#endif

  float4 colour;

#if (DITHER_TEST_PATTERN_ENABLE == NO)

  colour = tex2Dfetch(SamplerBackBuffer, position_as_int);

#else //DITHER_TEST_PATTERN_ENABLE == NO

  colour = (float4)0.f;

#ifdef IS_HDR_CSP
  const float min_brightness_encoded = Csp::Trc::Nits_To::PQ(_DITHER_TEST_PATTERN_MIN_BRIGHTNESS);
  const float max_brightness_encoded = Csp::Trc::Nits_To::PQ(_DITHER_TEST_PATTERN_MAX_BRIGHTNESS);
#else
  const float min_brightness_encoded = ENCODE_SDR(_DITHER_TEST_PATTERN_MIN_BRIGHTNESS / DIV_100);
  const float max_brightness_encoded = ENCODE_SDR(_DITHER_TEST_PATTERN_MAX_BRIGHTNESS / DIV_100);
#endif

  const uint min_brightness_pixels = uint(min_brightness_encoded * max_array[_DITHER_TEST_PATTERN_PATTERN_BIT_DEPTH]);
  const uint max_brightness_pixels = uint(max_brightness_encoded * max_array[_DITHER_TEST_PATTERN_PATTERN_BIT_DEPTH] + 0.5f);

  const uint test_pattern_width = clamp(int(max_brightness_pixels - min_brightness_pixels), 0, int(max_brightness_pixels))
                                * _DITHER_TEST_PATTERN_DUPLICATE_ENTRY_X_TIMES;

  const uint x_start = test_pattern_width < BUFFER_WIDTH_UINT ? ((BUFFER_WIDTH_UINT - test_pattern_width) / 2u)
                                                              : 0u;
  const uint x_end   = BUFFER_WIDTH_UINT - x_start;

  const uint test_pattern_height = (uint)(BUFFER_HEIGHT_FLOAT
                                        * (_DITHER_TEST_PATTERN_HEIGHT / DIV_100));

  const uint y_start = (BUFFER_HEIGHT_UINT - test_pattern_height) / 2u;
  const uint y_end   = BUFFER_HEIGHT_UINT - y_start;

  [branch]
  if (all(position_as_int >= int2(x_start, y_start))
   && all(position_as_int <  int2(x_end,   y_end)))
  {
    float grey = float(uint(position_as_int.x - x_start + min_brightness_pixels * _DITHER_TEST_PATTERN_DUPLICATE_ENTRY_X_TIMES) / _DITHER_TEST_PATTERN_DUPLICATE_ENTRY_X_TIMES)
               / max_array[_DITHER_TEST_PATTERN_PATTERN_BIT_DEPTH];

    BRANCH()
    if (_DITHER_TEST_PATTERN_COLOUR != DITHER_TEST_PATTERN_COLOUR_WHITE)
    {
      float3 test_pattern_colour = (float3)0.f;

      BRANCH()
      if (_DITHER_TEST_PATTERN_COLOUR == DITHER_TEST_PATTERN_COLOUR_RED)
      {
        test_pattern_colour.r = grey;
      }
      else
      BRANCH()
      if (_DITHER_TEST_PATTERN_COLOUR == DITHER_TEST_PATTERN_COLOUR_GREEN)
      {
        test_pattern_colour.g = grey;
      }
      else
      BRANCH()
      if (_DITHER_TEST_PATTERN_COLOUR == DITHER_TEST_PATTERN_COLOUR_BLUE)
      {
        test_pattern_colour.b = grey;
      }
      else
      BRANCH()
      if (_DITHER_TEST_PATTERN_COLOUR == DITHER_TEST_PATTERN_COLOUR_YELLOW)
      {
        test_pattern_colour.rg = grey;
      }
      else
      BRANCH()
      if (_DITHER_TEST_PATTERN_COLOUR == DITHER_TEST_PATTERN_COLOUR_CYAN)
      {
        test_pattern_colour.gb = grey;
      }
      else
      BRANCH()
      if (_DITHER_TEST_PATTERN_COLOUR == DITHER_TEST_PATTERN_COLOUR_MAGENTA)
      {
        test_pattern_colour.rb = grey;
      }

#ifdef IS_HDR_CSP

      test_pattern_colour = Csp::Trc::PQ_To::Linear(test_pattern_colour);

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
      BRANCH()
      if (DITHER_TEST_PATTERN_COLOUR_PRIMARIES == DITHER_TEST_PATTERN_COLOUR_PRIMARIES_BT2020)
      {
        test_pattern_colour = Csp::Mat::BT2020_To::BT709(test_pattern_colour);
      }
      else
#endif //ACTUAL_COLOUR_SPACE == CSP_SCRGB
      BRANCH()
      if (DITHER_TEST_PATTERN_COLOUR_PRIMARIES == DITHER_TEST_PATTERN_COLOUR_PRIMARIES_DCI_P3)
      {
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

        test_pattern_colour = Csp::Mat::DCIP3_To::BT709(test_pattern_colour);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

        test_pattern_colour = Csp::Mat::DCIP3_To::BT2020(test_pattern_colour);

#endif //ACTUAL_COLOUR_SPACE == CSP_SCRGB
      }
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
      else
      BRANCH()
      if (DITHER_TEST_PATTERN_COLOUR_PRIMARIES == DITHER_TEST_PATTERN_COLOUR_PRIMARIES_BT709)
      {
        test_pattern_colour = Csp::Mat::BT709_To::BT2020(test_pattern_colour);
      }
#endif //ACTUAL_COLOUR_SPACE == CSP_HDR10

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

      test_pattern_colour *= 125.f;

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

      test_pattern_colour = Csp::Trc::Linear_To::PQ(test_pattern_colour);

#endif

#endif //IS_HDR_CSP

      colour.rgb = test_pattern_colour;
    }
    else
    {
      colour.rgb = grey;
    }
  }

#endif //DITHER_TEST_PATTERN_ENABLE == NO

  BRANCH()
  if (_DITHER_ENABLE)
  {
    int2 fetch_pos_xy = int2(uint2(position_as_int) % uint(BLUE_NOISE_TEXTURE_SIZE_XY));

    float blue_noise = tex2Dfetch(Sampler_Blue_Noise, abs(-int2(fetch_pos_xy) + Fetch_Stuff.xy));

    float brightness_factor;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

    brightness_factor = 80.f / DITHER_DISPLAY_MAX_BRIGHTNESS;

#if (__RESHADE_PERFORMANCE_MODE__ == 0)

    if (DITHER_DISPLAY_PRIMARIES == DITHER_DISPLAY_PRIMARIES_BT2020)
    {
      colour.rgb = Csp::Mat::BT709_To::BT2020(colour.rgb);
    }
    else
    {
      colour.rgb = Csp::Mat::BT709_To::DCIP3(colour.rgb);
    }

    colour.rgb *= brightness_factor;

#else //__RESHADE_PERFORMANCE_MODE__

    float3x3 fwd_mat;
    float3x3 bwd_mat;

    if (DITHER_DISPLAY_PRIMARIES == DITHER_DISPLAY_PRIMARIES_BT2020)
    {
      fwd_mat = Csp::Mat::BT709_To_BT2020 * brightness_factor;
      bwd_mat = Csp::Mat::BT2020_To_BT709 / brightness_factor;
    }
    else
    {
      fwd_mat = Csp::Mat::BT709_To_DCIP3 * brightness_factor;
      bwd_mat = Csp::Mat::DCIP3_To_BT709 / brightness_factor;
    }

    colour.rgb = mul(fwd_mat, colour.rgb);

#endif //__RESHADE_PERFORMANCE_MODE__

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

    colour.rgb = FetchFromHdr10ToLinearLUT(colour.rgb);

    brightness_factor = 10000.f / DITHER_DISPLAY_MAX_BRIGHTNESS;

#if (__RESHADE_PERFORMANCE_MODE__ == 0)

    if (DITHER_DISPLAY_PRIMARIES == DITHER_DISPLAY_PRIMARIES_DCI_P3)
    {
      colour.rgb = Csp::Mat::BT2020_To::DCIP3(colour.rgb);
    }

    colour.rgb *= brightness_factor;

#else //__RESHADE_PERFORMANCE_MODE__

    float3x3 fwd_mat;
    float3x3 bwd_mat;

    if (DITHER_DISPLAY_PRIMARIES == DITHER_DISPLAY_PRIMARIES_DCI_P3)
    {
      fwd_mat = Csp::Mat::BT2020_To_DCIP3 * brightness_factor;
      bwd_mat = Csp::Mat::DCIP3_To_BT2020 / brightness_factor;

      colour.rgb = mul(fwd_mat, colour.rgb);
    }
    else
    {
      colour.rgb *= brightness_factor;
    }

#endif //__RESHADE_PERFORMANCE_MODE__

#endif //ACTUAL_COLOUR_SPACE

#ifdef IS_HDR_CSP

    colour.rgb = max(colour.rgb, 0.f);

    colour.rgb = pow(colour.rgb, 1.f / DITHER_TARGET_GAMMA);

#endif //IS_HDR_CSP

    colour.rgb *= dither_depth;

    const bool colour_is_black = all(floor(colour.rgb + 0.5f) == 0.f);

    float3 dither_strength_combined = (float3)0.f;

#if (DITHER_TEST_PATTERN_ENABLE == NO)

    // keeps pure black at black; does not cause issues from what I can tell
    [branch]
    if (colour_is_black)
    {
      Output = 0.f;

      discard;
    }
    else //if (!colour_is_black)

#endif //DITHER_TEST_PATTERN_ENABLE == NO

    {

#if (DITHER_TEST_PATTERN_ENABLE == YES)
      [branch]
      if (!colour_is_black)
#endif
      {
        //movc is always faster, with optimisations on or off
        dither_strength_combined =
          colour.rgb <= (float3)_DITHER_STRENGTH_LOW_STARTING_POINT ? (float3)_DITHER_STRENGTH_LOW
                                                                    : (float3)_DITHER_STRENGTH_HIGH;
      }
    }

    float3 colour_dithered;

    colour_dithered = (2.f * blue_noise - 1.f) * dither_strength_combined + colour.rgb;
    colour_dithered = max(colour_dithered, 0.f);

    colour.rgb = colour_dithered;

    // rounding
    BRANCH()
    if (_DITHER_ROUNDING_ENABLE)
    {
      colour.rgb = floor(colour.rgb + 0.5f);
    }

    colour.rgb /= dither_depth;

#ifdef IS_HDR_CSP

    colour.rgb = pow(colour.rgb, DITHER_TARGET_GAMMA);

#endif //IS_HDR_CSP

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

#if (__RESHADE_PERFORMANCE_MODE__ == 0)

    colour.rgb /= brightness_factor;

    if (DITHER_DISPLAY_PRIMARIES == DITHER_DISPLAY_PRIMARIES_BT2020)
    {
      colour.rgb = Csp::Mat::BT2020_To::BT709(colour.rgb);
    }
    else
    {
      colour.rgb = Csp::Mat::DCIP3_To::BT709(colour.rgb);
    }

#else //__RESHADE_PERFORMANCE_MODE__

    colour.rgb = mul(bwd_mat, colour.rgb);

#endif //__RESHADE_PERFORMANCE_MODE__

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

#if (__RESHADE_PERFORMANCE_MODE__ == 0)

    colour.rgb /= brightness_factor;

    if (DITHER_DISPLAY_PRIMARIES == DITHER_DISPLAY_PRIMARIES_DCI_P3)
    {
      colour.rgb = Csp::Mat::DCIP3_To::BT2020(colour.rgb);
    }

#else //__RESHADE_PERFORMANCE_MODE__

    if (DITHER_DISPLAY_PRIMARIES == DITHER_DISPLAY_PRIMARIES_DCI_P3)
    {
      colour.rgb = mul(bwd_mat, colour.rgb);
    }
    else
    {
      colour.rgb /= brightness_factor;
    }

#endif //__RESHADE_PERFORMANCE_MODE__

    colour.rgb = Csp::Trc::Linear_To::PQ(colour.rgb);

#endif //ACTUAL_COLOUR_SPACE
  }

  Output = colour;

  return;
}


technique lilium__blue_noise_dithering
<
  ui_label = "Lilium's blue noise dithering";
>
{
  pass Dither
  {
    VertexShader = VS_Prepare_Dither;
     PixelShader = PS_Dither;
  }
}

#else //is hdr API and hdr colour space

ERROR_STUFF

technique lilium__blue_noise_dithering
<
  ui_label = "Lilium's blue noise dithering (ERROR)";
>
VS_ERROR

#endif //is hdr API and hdr colour space
