#include "lilium__include/include_main.fxh"


#ifndef LINUX_AMD_SHADER_INPUTS_OUTPUTS_WORKAROUND
  #define LINUX_AMD_SHADER_INPUTS_OUTPUTS_WORKAROUND NO
#endif


#if (defined(IS_ANALYSIS_CAPABLE_API)   \
  && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB \
    || ACTUAL_COLOUR_SPACE == CSP_HDR10 \
    || ACTUAL_COLOUR_SPACE == CSP_SRGB) \
   || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))


HDR10_TO_LINEAR_LUT()


#undef TEXT_BRIGHTNESS

#ifndef GAMESCOPE
  //#define _DEBUG
#endif


#ifdef MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL

  #if (ACTUAL_COLOUR_SPACE == CSP_SCRGB \
    || ACTUAL_COLOUR_SPACE == CSP_HDR10)
    #define HIDDEN_OPTION_HDR_CSP false
  #else
    #define HIDDEN_OPTION_HDR_CSP true
  #endif

  #ifdef IS_COMPUTE_CAPABLE_API
    #define HIDDEN_OPTION_COMPUTE_CAPABLE_API false
  #else
    #define HIDDEN_OPTION_COMPUTE_CAPABLE_API true
  #endif

#else //MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL

  #define HIDDEN_OPTION_HDR_CSP false

  #define HIDDEN_OPTION_COMPUTE_CAPABLE_API false

#endif //MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL


#define DEFAULT_BRIGHTNESS 80.f

#define DEFAULT_ALPHA_LEVEL 75.f

uniform int2 MOUSE_POSITION
<
  source = "mousepoint";
>;

uniform float FRAMETIME
<
  source = "frametime";
>;

uniform float2 NIT_PINGPONG0
<
  source    = "pingpong";
  min       = 0.f;
  max       = 2.f;
  step      = 1.f;
  smoothing = 0.f;
>;

uniform float2 NIT_PINGPONG1
<
  source    = "pingpong";
  min       =  0.f;
  max       =  3.f;
  step      =  1.f;
  smoothing =  0.f;
>;

uniform float2 NIT_PINGPONG2
<
  source    = "pingpong";
  min       = 0.f;
  max       = 1.f;
  step      = 3.f;
  smoothing = 0.f;
>;

#if ((defined(GAMESCOPE) || defined(POSSIBLE_DECK_VULKAN_USAGE)) \
  && BUFFER_HEIGHT <= 800)
  static const uint TEXT_SIZE_DEFAULT = 0;
#else
  // 2160 height gives 42
  //  800 height gives 16
  //  720 height gives 14
  static const uint TEXT_SIZE_DEFAULT = (uint((BUFFER_HEIGHT / 2160.f) * 42.f + 0.5f) - 12) / 2;
#endif


#if (!defined(IS_HDR_CSP) \
  && !defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))
  #define _TEXT_SIZE                              SDR_TEXT_SIZE
  #define _TEXT_BRIGHTNESS                        SDR_TEXT_BRIGHTNESS
  #define _TEXT_BG_ALPHA                          SDR_TEXT_BG_ALPHA
  #define _TEXT_POSITION                          SDR_TEXT_POSITION
  #define _VALUES_UPDATE_RATE                     SDR_VALUES_UPDATE_RATE
  #define _ACTIVE_AREA_ENABLE                     SDR_ACTIVE_AREA_ENABLE
  #define _ACTIVE_AREA_CROP_LEFT                  SDR_ACTIVE_AREA_CROP_LEFT
  #define _ACTIVE_AREA_CROP_TOP                   SDR_ACTIVE_AREA_CROP_TOP
  #define _ACTIVE_AREA_CROP_RIGHT                 SDR_ACTIVE_AREA_CROP_RIGHT
  #define _ACTIVE_AREA_CROP_BOTTOM                SDR_ACTIVE_AREA_CROP_BOTTOM
  #define _SHOW_NITS_VALUES                       SDR_SHOW_NITS_VALUES
  #define _SHOW_RGB_OR_CLL                        SDR_SHOW_RGB_OR_CLL
  #define _SHOW_NITS_FROM_CURSOR                  SDR_SHOW_NITS_FROM_CURSOR
  #define _SHOW_CIE                               SDR_SHOW_CIE
  #define _SHOW_CROSSHAIR_ON_CIE_DIAGRAM          SDR_SHOW_CROSSHAIR_ON_CIE_DIAGRAM
  #define _CIE_DIAGRAM_TYPE                       SDR_CIE_DIAGRAM_TYPE
  #define _CIE_DIAGRAM_BRIGHTNESS                 SDR_CIE_DIAGRAM_BRIGHTNESS
  #define _CIE_DIAGRAM_SIZE                       SDR_CIE_DIAGRAM_SIZE
  #define _CIE_SHOW_GAMUT_OUTLINE_BT709           SDR_CIE_SHOW_GAMUT_BT709_OUTLINE
  #define _CIE_SHOW_GAMUT_OUTLINE_POINTERS        SDR_CIE_SHOW_GAMUT_POINTERS_OUTLINE
  #define _SHOW_HEATMAP                           SDR_SHOW_HEATMAP
  #define _HEATMAP_BRIGHTNESS                     SDR_HEATMAP_BRIGHTNESS
  #define _SHOW_WAVEFORM                          SDR_SHOW_WAVEFORM
  #define _WAVEFORM_MODE                          SDR_WAVEFORM_MODE
  #define _WAVEFORM_TEXT_SIZE_ADJUST              SDR_WAVEFORM_TEXT_SIZE_ADJUST
  #define _WAVEFORM_SCALE_BRIGHTNESS              SDR_WAVEFORM_SCALE_BRIGHTNESS
  #define _WAVEFORM_ALPHA                         SDR_WAVEFORM_ALPHA
  #define _WAVEFORM_SIZE                          SDR_WAVEFORM_SIZE
  #define _WAVEFORM_SHOW_MIN_NITS_LINE            SDR_WAVEFORM_SHOW_MIN_NITS_LINE
  #define _WAVEFORM_SHOW_MAX_NITS_LINE            SDR_WAVEFORM_SHOW_MAX_NITS_LINE
  #define _WAVEFORM_SHOW_MAX_MIN_PER_COLUMN       SDR_WAVEFORM_SHOW_MAX_MIN_PER_COLUMN
  #define _HIGHLIGHT_NIT_RANGE                    SDR_HIGHLIGHT_NIT_RANGE
  #define _HIGHLIGHT_NIT_RANGE_BRIGHTNESS         SDR_HIGHLIGHT_NIT_RANGE_BRIGHTNESS
  #define _HIGHLIGHT_NIT_RANGE_START_POINT        SDR_HIGHLIGHT_NIT_RANGE_START_POINT
  #define _HIGHLIGHT_NIT_RANGE_END_POINT          SDR_HIGHLIGHT_NIT_RANGE_END_POINT
  #define _DRAW_ABOVE_NITS_AS_BLACK               SDR_DRAW_ABOVE_NITS_AS_BLACK
  #define _ABOVE_NITS_AS_BLACK                    SDR_ABOVE_NITS_AS_BLACK
  #define _DRAW_BELOW_NITS_AS_BLACK               SDR_DRAW_BELOW_NITS_AS_BLACK
  #define _BELOW_NITS_AS_BLACK                    SDR_BELOW_NITS_AS_BLACK
#else
  #define _TEXT_SIZE                              TEXT_SIZE
  #define _TEXT_BRIGHTNESS                        TEXT_BRIGHTNESS
  #define _TEXT_BG_ALPHA                          TEXT_BG_ALPHA
  #define _TEXT_POSITION                          TEXT_POSITION
  #define _VALUES_UPDATE_RATE                     VALUES_UPDATE_RATE
  #define _ACTIVE_AREA_ENABLE                     ACTIVE_AREA_ENABLE
  #define _ACTIVE_AREA_CROP_LEFT                  ACTIVE_AREA_CROP_LEFT
  #define _ACTIVE_AREA_CROP_TOP                   ACTIVE_AREA_CROP_TOP
  #define _ACTIVE_AREA_CROP_RIGHT                 ACTIVE_AREA_CROP_RIGHT
  #define _ACTIVE_AREA_CROP_BOTTOM                ACTIVE_AREA_CROP_BOTTOM
  #define _SHOW_NITS_VALUES                       SHOW_NITS_VALUES
  #define _SHOW_RGB_OR_CLL                        SHOW_RGB_OR_CLL
  #define _SHOW_NITS_FROM_CURSOR                  SHOW_NITS_FROM_CURSOR
  #define _SHOW_CIE                               SHOW_CIE
  #define _SHOW_CROSSHAIR_ON_CIE_DIAGRAM          SHOW_CROSSHAIR_ON_CIE_DIAGRAM
  #define _CIE_DIAGRAM_TYPE                       CIE_DIAGRAM_TYPE
  #define _CIE_DIAGRAM_BRIGHTNESS                 CIE_DIAGRAM_BRIGHTNESS
  #define _CIE_DIAGRAM_SIZE                       CIE_DIAGRAM_SIZE
  #define _CIE_SHOW_GAMUT_OUTLINE_BT709           CIE_SHOW_GAMUT_BT709_OUTLINE
  #define _CIE_SHOW_GAMUT_OUTLINE_POINTERS        CIE_SHOW_GAMUT_POINTERS_OUTLINE
  #define _SHOW_HEATMAP                           SHOW_HEATMAP
  #define _HEATMAP_BRIGHTNESS                     HEATMAP_BRIGHTNESS
  #define _SHOW_WAVEFORM                          SHOW_WAVEFORM
  #define _WAVEFORM_MODE                          WAVEFORM_MODE
  #define _WAVEFORM_TEXT_SIZE_ADJUST              WAVEFORM_TEXT_SIZE_ADJUST
  #define _WAVEFORM_BRIGHTNESS                    WAVEFORM_BRIGHTNESS
  #define _WAVEFORM_SCALE_BRIGHTNESS              WAVEFORM_SCALE_BRIGHTNESS
  #define _WAVEFORM_ALPHA                         WAVEFORM_ALPHA
  #define _WAVEFORM_SIZE                          WAVEFORM_SIZE
  #define _WAVEFORM_SHOW_MIN_NITS_LINE            WAVEFORM_SHOW_MIN_NITS_LINE
  #define _WAVEFORM_SHOW_MAX_NITS_LINE            WAVEFORM_SHOW_MAX_NITS_LINE
  #define _WAVEFORM_SHOW_MAX_MIN_PER_COLUMN       WAVEFORM_SHOW_MAX_MIN_PER_COLUMN
  #define _HIGHLIGHT_NIT_RANGE                    HIGHLIGHT_NIT_RANGE
  #define _HIGHLIGHT_NIT_RANGE_BRIGHTNESS         HIGHLIGHT_NIT_RANGE_BRIGHTNESS
  #define _HIGHLIGHT_NIT_RANGE_START_POINT        HIGHLIGHT_NIT_RANGE_START_POINT
  #define _HIGHLIGHT_NIT_RANGE_END_POINT          HIGHLIGHT_NIT_RANGE_END_POINT
  #define _DRAW_ABOVE_NITS_AS_BLACK               DRAW_ABOVE_NITS_AS_BLACK
  #define _ABOVE_NITS_AS_BLACK                    ABOVE_NITS_AS_BLACK
  #define _DRAW_BELOW_NITS_AS_BLACK               DRAW_BELOW_NITS_AS_BLACK
  #define _BELOW_NITS_AS_BLACK                    BELOW_NITS_AS_BLACK
#endif


uniform float _TEXT_SIZE
<
  ui_category = "Global";
  ui_label    = "text size";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      =  35.f;
  ui_max      = 200.f;
  ui_step     =   0.1f;
> = 100.f;

uniform float _TEXT_BRIGHTNESS
<
  ui_category = "Global";
  ui_label    = "text brightness";
  ui_type     = "slider";
#ifdef IS_HDR_CSP
  ui_units    = " nits";
  ui_min      = 10.f;
  ui_max      = 250.f;
#else
  ui_units    = "%%";
  ui_min      = 10.f;
  ui_max      = 100.f;
#endif
  ui_step     = 0.5f;
#if defined(IS_HDR_CSP)
> = 140.f;
#elif (defined(GAMESCOPE) \
    && defined(IS_HDR_CSP))
> = GAMESCOPE_SDR_ON_HDR_NITS;
#else
> = DEFAULT_BRIGHTNESS;
#endif

uniform float _TEXT_BG_ALPHA
<
  ui_category = "Global";
  ui_label    = "text background opacity";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.5f;
> = DEFAULT_ALPHA_LEVEL;

#define TEXT_POSITION_TOP_LEFT  0
#define TEXT_POSITION_TOP_RIGHT 1

uniform uint _TEXT_POSITION
<
  ui_category = "Global";
  ui_label    = "text position";
  ui_type     = "combo";
  ui_items    = "top left\0"
                "top right\0";
> = 0;

uniform float _VALUES_UPDATE_RATE
<
  ui_category = "Global";
  ui_label    = "values update rate";
  ui_type     = "slider";
  ui_units    = " ms";
  ui_min      = 0.f;
  ui_max      = 1000.f;
  ui_step     = 1.f;
> = 500.f;

// Active Area
uniform bool _ACTIVE_AREA_ENABLE
<
  ui_category = "Set Active Area for analysis";
  ui_label    = "enable setting the active area";
> = false;

uniform float _ACTIVE_AREA_CROP_LEFT
<
  ui_category = "Set Active Area for analysis";
  ui_label    = "% to crop from the left side";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.1f;
> = 0.f;

uniform float _ACTIVE_AREA_CROP_TOP
<
  ui_category = "Set Active Area for analysis";
  ui_label    = "% to crop from the top side";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.1f;
> = 0.f;

uniform float _ACTIVE_AREA_CROP_RIGHT
<
  ui_category = "Set Active Area for analysis";
  ui_label    = "% to crop from the right side";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.1f;
> = 0.f;

uniform float _ACTIVE_AREA_CROP_BOTTOM
<
  ui_category = "Set Active Area for analysis";
  ui_label    = "% to crop from the bottom side";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.1f;
> = 0.f;


// Nit and RGB Values
uniform bool _SHOW_NITS_VALUES
<
  ui_category = "Luminance and Content Light Level analysis";
  ui_label    = "show max/avg/min luminance and RGB or CLL values";
  ui_tooltip  = "The individual R, G and B values are not connected to the luminance value."
           "\n" "But are all individually calculated with no connection to the other values."
           "\n" "As in they are with high certainty not from the same pixel!"
           "\n"
           "\n"
#ifdef IS_HDR_CSP
                "In HDR the RGB/CLL values represent \"optical\" RGB values with BT.2020 primaries."
#else
                "In SDR the RGB/CLL values represent relative \"optical\" RGB values in % with BT.709 primaries."
#endif
           "\n" "If you do not understand what that is:"
           "\n" "if a pixel has R, G and B set to the same value, let's say 50,"
#ifdef IS_HDR_CSP
           "\n" "the nits of that pixel will be 50."
#else
           "\n" "the relative output value of that pixel will be 50%."
#endif
           "\n" "But do not forget that the max and min values are not from the same pixel!"
           "\n"
           "\n" "But what are those RGB/CLL values for then?"
#ifdef IS_HDR_CSP
           "\n" "In HDR this easily tells you if your display clips those values."
           "\n" "Let's say your display can display up to 800 nits."
           "\n" "If any R, G or B value is above 800 it will be clipped to 800."
           "\n" "Because your display is not able to process and display that value."
           "\n" "This always leads to hue shifts."
#else
           "\n" "In SDR it is purely more data points."
#endif
           "\n"
           "\n" "This description is different depending on if you are in HDR or SDR!";
> = true;

uniform bool _SHOW_NITS_FROM_CURSOR
<
  ui_category = "Luminance and Content Light Level analysis";
  ui_label    = "show luminance value and RGB or CLL values from cursor position";
  ui_tooltip  = "See tooltip from \"show max/avg/min luminance and RGB values\" for more Information.";
> = true;

#define SHOW_RGB_VALUES 0
#define SHOW_CLL_VALUES 1

uniform uint _SHOW_RGB_OR_CLL
<
  ui_category = "Luminance and Content Light Level analysis";
  ui_label    = "show RGB or CLL values";
  ui_type     = "combo";
  ui_items    = "RGB values\0"
                "CLL values\0";
> = 0;


// gamuts
#if (defined(IS_HDR_CSP) \
  || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))

uniform bool SHOW_GAMUTS
<
  ui_category = "Gamut analysis";
  ui_label    = "show gamuts used";
  ui_tooltip  = "in %";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = true;

uniform bool SHOW_GAMUT_FROM_CURSOR
<
  ui_category = "Gamut analysis";
  ui_label    = "show gamut from cursor position";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = true;

uniform bool SHOW_GAMUT_MAP
<
  ui_category = "Gamut analysis";
  ui_label    = "show gamut map";
  ui_tooltip  = "        colours:"
           "\n" "black and white: BT.709"
           "\n" "         yellow: DCI-P3"
           "\n" "           blue: BT.2020"
           "\n" "            red: AP0"
           "\n" "           pink: invalid";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = false;

#endif //defined(IS_HDR_CSP) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)


#if (defined(IS_COMPUTE_CAPABLE_API) \
  || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))

// CIE
uniform bool _SHOW_CIE
<
  ui_category = "CIE diagram visualisation";
  ui_label    = "show CIE diagram";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = true;

uniform bool _SHOW_CROSSHAIR_ON_CIE_DIAGRAM
<
  ui_category = "CIE diagram visualisation";
  ui_label    = "show crosshair of cursor gamut on CIE diagram";
  ui_tooltip  = "it disappears when the colour is 100% black!";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = true;

#define CIE_1931 0
#define CIE_1976 1

uniform uint _CIE_DIAGRAM_TYPE
<
  ui_category = "CIE diagram visualisation";
  ui_label    = "CIE diagram type";
  ui_type     = "combo";
  ui_items    = "CIE 1931 xy\0"
                "CIE 1976 UCS u'v'\0";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = CIE_1931;

uniform float _CIE_DIAGRAM_BRIGHTNESS
<
  ui_category = "CIE diagram visualisation";
  ui_label    = "CIE diagram brightness";
  ui_type     = "slider";
#ifdef IS_HDR_CSP
  ui_units    = " nits";
  ui_min      = 10.f;
  ui_max      = 250.f;
#else
  ui_units    = "%%";
  ui_min      = 10.f;
  ui_max      = 100.f;
#endif
  ui_step     = 0.5f;
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
#if (defined(GAMESCOPE) \
  && defined(IS_HDR_CSP))
> = GAMESCOPE_SDR_ON_HDR_NITS;
#else
> = DEFAULT_BRIGHTNESS;
#endif


#define CIE_XY_MAX float2(0.734690189f,   0.834090292f)
#define CIE_XY_MIN float2(0.00363638415f, 0.00477403076f)

#define CIE_UV_MAX float2(0.623366653f,   0.586759090f)
#define CIE_UV_MIN float2(0.00137366366f, 0.0158483609f)

#ifdef IS_HIGHER_THAN_QHD_RES
  #define CIE_BG_BORDER        50

  //#define CIE_TEXTURE_HEIGHT (1024 - CIE_BG_BORDER - CIE_BG_BORDER)
  #define CIE_TEXTURE_HEIGHT 1024
#elif defined(IS_QHD_OR_HIGHER_RES)
  #define CIE_BG_BORDER        38

  //#define CIE_TEXTURE_HEIGHT (768 - CIE_BG_BORDER - CIE_BG_BORDER)
  #define CIE_TEXTURE_HEIGHT  768
#else
  #define CIE_BG_BORDER        25

  //#define CIE_TEXTURE_HEIGHT (512 - CIE_BG_BORDER - CIE_BG_BORDER)
  #define CIE_TEXTURE_HEIGHT  512
#endif

#ifdef IS_FLOAT_HDR_CSP

  static const float2 CIE_XY_EXTRA     = (CIE_XY_MAX - CIE_XY_MIN) / 10.f;
  static const float2 CIE_XY_MAX_EXTRA = CIE_XY_MAX + CIE_XY_EXTRA;
  static const float2 CIE_XY_MIN_EXTRA = CIE_XY_MIN - CIE_XY_EXTRA;

  static const float2 CIE_UV_EXTRA     = (CIE_UV_MAX - CIE_UV_MIN) / 10.f;
  static const float2 CIE_UV_MAX_EXTRA = CIE_UV_MAX + CIE_UV_EXTRA;
  static const float2 CIE_UV_MIN_EXTRA = CIE_UV_MIN - CIE_UV_EXTRA;

  static const float2 CIE_XY_NORMALISE = CIE_XY_MAX_EXTRA - CIE_XY_MIN_EXTRA;
  static const float2 CIE_UV_NORMALISE = CIE_UV_MAX_EXTRA - CIE_UV_MIN_EXTRA;

#else

  static const float2 CIE_XY_EXTRA     = 0.025f;
  static const float2 CIE_XY_MAX_EXTRA = CIE_XY_MAX + CIE_XY_EXTRA;
  static const float2 CIE_XY_MIN_EXTRA = CIE_XY_MIN - CIE_XY_EXTRA;

  static const float2 CIE_UV_EXTRA     = 0.025f;
  static const float2 CIE_UV_MAX_EXTRA = CIE_UV_MAX + CIE_UV_EXTRA;
  static const float2 CIE_UV_MIN_EXTRA = CIE_UV_MIN - CIE_UV_EXTRA;

  static const float2 CIE_XY_NORMALISE = CIE_XY_MAX_EXTRA - CIE_XY_MIN_EXTRA;
  static const float2 CIE_UV_NORMALISE = CIE_UV_MAX_EXTRA - CIE_UV_MIN_EXTRA;

#endif

static const uint CIE_XY_WIDTH_UINT = uint(CIE_XY_NORMALISE.x
                                         / CIE_XY_NORMALISE.y
                                         * float(CIE_TEXTURE_HEIGHT)
                                         + 0.5f);

static const uint CIE_UV_WIDTH_UINT = uint(CIE_UV_NORMALISE.x
                                         / CIE_UV_NORMALISE.y
                                         * float(CIE_TEXTURE_HEIGHT)
                                         + 0.5f);

static const uint2 CIE_XY_SIZE_UINT = uint2(CIE_XY_WIDTH_UINT, CIE_TEXTURE_HEIGHT);
static const uint2 CIE_UV_SIZE_UINT = uint2(CIE_UV_WIDTH_UINT, CIE_TEXTURE_HEIGHT);

static const float2 CIE_XY_SIZE_FLOAT = float2(CIE_XY_SIZE_UINT);
static const float2 CIE_UV_SIZE_FLOAT = float2(CIE_UV_SIZE_UINT);

//u'v' is wider than xy
#define CIE_TEXTURE_WIDTH_UINT CIE_UV_WIDTH_UINT

static const int CIE_TEXTURE_WIDTH_INT  = int(CIE_TEXTURE_WIDTH_UINT);
static const int CIE_TEXTURE_HEIGHT_INT = int(CIE_TEXTURE_HEIGHT);

#define CIE_TEXTURE_WIDTH_MINUS_1_UINT (CIE_TEXTURE_WIDTH_UINT - 1u)

#define CIE_TEXTURE_HEIGHT_MINUS_1 (CIE_TEXTURE_HEIGHT - 1)


static const uint2 CIE_XY_TOTAL_SIZE_UINT = uint2(CIE_XY_WIDTH_UINT, CIE_TEXTURE_HEIGHT) + CIE_BG_BORDER + CIE_BG_BORDER;
static const uint2 CIE_UV_TOTAL_SIZE_UINT = uint2(CIE_UV_WIDTH_UINT, CIE_TEXTURE_HEIGHT) + CIE_BG_BORDER + CIE_BG_BORDER;

//u'v' is wider than xy
#define CIE_BG_TEXTURE_SIZE CIE_UV_TOTAL_SIZE_UINT


static const float CIE_DIAGRAM_DEFAULT_SIZE = (BUFFER_HEIGHT_FLOAT * 0.375f)
                                            / CIE_TEXTURE_HEIGHT
                                            * 100.f;

uniform float _CIE_DIAGRAM_SIZE
<
  ui_category = "CIE diagram visualisation";
  ui_label    = "CIE diagram size";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      = 50.f;
  ui_max      = 100.f;
  ui_step     = 0.1f;
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = CIE_DIAGRAM_DEFAULT_SIZE;

uniform bool _CIE_SHOW_GAMUT_OUTLINE_BT709
<
  ui_category = "CIE diagram visualisation";
  ui_label    = "show BT.709 gamut outline";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = true;

#if (defined(IS_HDR_CSP) \
  || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))

uniform bool CIE_SHOW_GAMUT_OUTLINE_DCI_P3
<
  ui_category = "CIE diagram visualisation";
  ui_label    = "show DCI-P3 gamut outline";
  hidden      = (HIDDEN_OPTION_COMPUTE_CAPABLE_API || HIDDEN_OPTION_HDR_CSP);
> = true;

uniform bool CIE_SHOW_GAMUT_OUTLINE_BT2020
<
  ui_category = "CIE diagram visualisation";
  ui_label    = "show BT.2020 gamut outline";
  hidden      = (HIDDEN_OPTION_COMPUTE_CAPABLE_API || HIDDEN_OPTION_HDR_CSP);
> = true;

#endif //defined(IS_HDR_CSP) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)

uniform bool _CIE_SHOW_GAMUT_OUTLINE_POINTERS
<
  ui_category = "CIE diagram visualisation";
  ui_label    = "show Pointer's gamut outline";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = false;

#endif //defined(IS_COMPUTE_CAPABLE_API) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)


// heatmap
uniform bool _SHOW_HEATMAP
<
  ui_category = "Heatmap visualisation";
  ui_label    = "show heatmap";
#ifndef IS_HDR_CSP
  ui_tooltip  = "         colours: |     % nits:"
           "\n" "------------------|------------"
           "\n" " black to white:  |   0.0-  1.0"
           "\n" "  teal to green:  |   1.0- 18.0"
           "\n" " green to yellow: |  18.0- 50.0"
           "\n" "yellow to red:    |  50.0- 75.0"
           "\n" "   red to pink:   |  75.0- 87.5"
           "\n" "  pink to blue:   |  87.5-100.0"
           "\n" "-------------------------------"
           "\n"
           "\n" "extra cases:"
           "\n" "highly saturated red:  above the cutoff point"
           "\n" "highly saturated blue: below 0 nits";
#endif
> = false;

uniform float _HEATMAP_BRIGHTNESS
<
  ui_category = "Heatmap visualisation";
  ui_label    = "heatmap brightness";
  ui_type     = "slider";
#ifdef IS_HDR_CSP
  ui_units    = " nits";
  ui_min      = 10.f;
  ui_max      = 250.f;
#else
  ui_units    = "%%";
  ui_min      = 10.f;
  ui_max      = 100.f;
#endif
  ui_step     = 0.5f;
> = DEFAULT_BRIGHTNESS;

#if (defined(IS_HDR_CSP) \
  || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))

uniform uint HEATMAP_CUTOFF_POINT
<
  ui_category = "Heatmap visualisation";
  ui_label    = "heatmap cutoff point";
  ui_type     = "combo";
  ui_items    = "10000 nits\0"
                " 4000 nits\0"
                " 2000 nits\0"
                " 1000 nits\0";
  ui_tooltip  = "         colours: | 10000 nits: | 4000 nits: | 2000 nits: | 1000 nits:"
           "\n" "------------------|-------------|------------|------------|-----------"
           "\n" " black to white:  |     0-  100 |     0- 100 |     0- 100 |     0- 100"
           "\n" "  teal to green:  |   100-  203 |   100- 203 |   100- 203 |   100- 203"
           "\n" " green to yellow: |   203-  400 |   203- 400 |   203- 400 |   203- 400"
           "\n" "yellow to red:    |   400- 1000 |   400-1000 |   400-1000 |   400- 600"
           "\n" "   red to pink:   |  1000- 4000 |  1000-2000 |  1000-1500 |   600- 800"
           "\n" "  pink to blue:   |  4000-10000 |  2000-4000 |  1500-2000 |   800-1000"
           "\n" "----------------------------------------------------------------------"
           "\n"
           "\n" "extra cases:"
           "\n" "highly saturated red:  above the cutoff point"
           "\n" "highly saturated blue: below 0 nits";
  hidden      = HIDDEN_OPTION_HDR_CSP;
> = 0;

#endif //(defined(IS_HDR_CSP) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))


#if (defined(IS_COMPUTE_CAPABLE_API) \
  || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))

uniform bool _SHOW_WAVEFORM
<
  ui_category = "Waveform";
  ui_label    = "show waveform";
  ui_tooltip  = "Luminance waveform paid for by Aemony.";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = true;

uniform uint _WAVEFORM_MODE
<
  ui_category = "Waveform";
  ui_label    = "waveform mode";
  ui_tooltip  = "In the RGB mode the values follow the same encoding as for the shown RGB values."
           "\n" "See tooltip from \"show max/avg/min luminance and RGB values\" for more Information.";
  ui_type     = "combo";
  ui_items    = "luminance\0"
                "max CLL\0"
                "RGB combined\0"
                "RGB individiually\0";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = 0;

#define WAVEFORM_MODE_LUMINANCE        0
#define WAVEFORM_MODE_MAX_CLL          1
#define WAVEFORM_MODE_RGB_COMBINED     2
#define WAVEFORM_MODE_RGB_INDIVIDUALLY 3

#if (defined(IS_HDR_CSP) \
  || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))

uniform uint WAVEFORM_CUTOFF_POINT
<
  ui_category = "Waveform";
  ui_label    = "waveform cutoff point";
  ui_type     = "combo";
  ui_items    = "10000\0"
                " 4000\0"
                " 2000\0"
                " 1000\0";
  hidden      = (HIDDEN_OPTION_COMPUTE_CAPABLE_API || HIDDEN_OPTION_HDR_CSP);
> = 0;

#endif //defined(IS_HDR_CSP) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))

uniform float _WAVEFORM_TEXT_SIZE_ADJUST
<
  ui_category = "Waveform";
  ui_label    = "waveform text size adjust";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      =   1.f;
  ui_max      = 200.f;
  ui_step     =   1.f;
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = 100.f;


#if (defined(IS_HDR_CSP) \
  || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))

uniform float _WAVEFORM_BRIGHTNESS
<
  ui_category = "Waveform";
  ui_label    = "waveform brightness";
  ui_type     = "drag";
  ui_units    = " nits";
  ui_min      =  80.f;
  ui_max      = 500.f;
  ui_step     =  10.f;
  hidden      = (HIDDEN_OPTION_COMPUTE_CAPABLE_API || HIDDEN_OPTION_HDR_CSP);
#ifdef GAMESCOPE
> = (GAMESCOPE_SDR_ON_HDR_NITS * 2.f);
#else
> = 200.f;
#endif

#endif //defined(IS_HDR_CSP) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))

uniform float WAVEFORM_MAX_MIN_PER_ROW_BRIGHTNESS_PERCENTAGE
<
  ui_category = "Waveform";
  ui_label    = "waveform per column max/min brightness percentage";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      =  20.f;
  ui_max      = 100.f;
  ui_step     =   1.f;
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = 50.f;

uniform float _WAVEFORM_SCALE_BRIGHTNESS
<
  ui_category = "Waveform";
  ui_label    = "waveform scale brightness";
  ui_type     = "drag";
#ifdef IS_HDR_CSP
  ui_units    = " nits";
  ui_min      =  10.f;
  ui_max      = 500.f;
  ui_step     =   1.f;
#else
  ui_units    = "%%";
  ui_min      =  10.f;
  ui_max      = 100.f;
  ui_step     =   0.5f;
#endif
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
#if (defined(GAMESCOPE) \
  && defined(IS_HDR_CSP))
> = GAMESCOPE_SDR_ON_HDR_NITS;
#else
> = DEFAULT_BRIGHTNESS;
#endif

uniform float _WAVEFORM_ALPHA
<
  ui_category = "Waveform";
  ui_label    = "waveform opacity";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.5f;
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = DEFAULT_ALPHA_LEVEL;


#if (BUFFER_WIDTH <= 4096)

  #if ((BUFFER_WIDTH % 2) == 0)

    #define TEXTURE_WAVEFORM_WIDTH (BUFFER_WIDTH / 2)

  #else

    #define TEXTURE_WAVEFORM_WIDTH (BUFFER_WIDTH / 2 + 1)

  #endif

  #define TEXTURE_WAVEFORM_TOTAL_WIDTH TEXTURE_WAVEFORM_WIDTH

#else

  #define TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

  #if ((BUFFER_WIDTH % 2) == 0)

    #if (((BUFFER_WIDTH / 2) % 2048) == 0)

      #define TEXTURE_WAVEFORM_WIDTH 2048

      #define TEXTURE_WAVEFORM_DEPTH_MULTIPLIER ((BUFFER_WIDTH / 2) / 2048)

    #else

      #define TEXTURE_WAVEFORM_WIDTH_TEMP ((BUFFER_WIDTH / 2) / ((BUFFER_WIDTH / 2) / 2048 + 1))

      #define TEXTURE_WAVEFORM_DEPTH_MULTIPLIER ((BUFFER_WIDTH / 2) / TEXTURE_WAVEFORM_WIDTH_TEMP)

      #if ((TEXTURE_WAVEFORM_WIDTH_TEMP * TEXTURE_WAVEFORM_DEPTH_MULTIPLIER) < (BUFFER_WIDTH / 2))

        #define TEXTURE_WAVEFORM_WIDTH (TEXTURE_WAVEFORM_WIDTH_TEMP + 1)

      #else

        #define TEXTURE_WAVEFORM_WIDTH TEXTURE_WAVEFORM_WIDTH_TEMP

      #endif

    #endif

  #else

    #define TEXTURE_WAVEFORM_WIDTH ((BUFFER_WIDTH / 2) / ((BUFFER_WIDTH / 2) / 2048 + 1) + 1)

    #define TEXTURE_WAVEFORM_DEPTH_MULTIPLIER (((BUFFER_WIDTH / 2) / TEXTURE_WAVEFORM_WIDTH) + 1)

  #endif

  #if (((BUFFER_WIDTH / 4) * 4) == BUFFER_WIDTH)

    #define TEXTURE_WAVEFORM_TOTAL_WIDTH (TEXTURE_WAVEFORM_WIDTH * TEXTURE_WAVEFORM_DEPTH_MULTIPLIER)

  #else

    #define TEXTURE_WAVEFORM_TOTAL_WIDTH (((BUFFER_WIDTH / 4) + 1) * 2)

  #endif

  #if ((TEXTURE_WAVEFORM_WIDTH * TEXTURE_WAVEFORM_DEPTH_MULTIPLIER) < (BUFFER_WIDTH / 2))
    #error "Could not calculate proper waveform texture width!! Please report this issue (ERROR 1)!"
  #endif

  #if (((BUFFER_WIDTH / 2) * 2) == BUFFER_WIDTH)
    #if ((TEXTURE_WAVEFORM_TOTAL_WIDTH * 2) < (BUFFER_WIDTH / 2))
      #error "Could not calculate proper total waveform texture width!! Please report this issue (ERROR 2)!"
    #endif
  #else
    #if ((TEXTURE_WAVEFORM_TOTAL_WIDTH * 2) < (BUFFER_WIDTH / 2 + 1))
      #error "Could not calculate proper total waveform texture width!! Please report this issue (ERROR 3)!"
    #endif
  #endif

#endif

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  #if (BUFFER_HEIGHT <= (512 * 5 / 2))
    #define TEXTURE_WAVEFORM_HEIGHT  511
  #elif (BUFFER_HEIGHT <= (1024 * 5 / 2))
    #define TEXTURE_WAVEFORM_HEIGHT 1023
  #elif (BUFFER_HEIGHT <= (2048 * 5 / 2))
    #define TEXTURE_WAVEFORM_HEIGHT 2047
  #else //(BUFFER_HEIGHT <= (4096 * 5 / 2))
    #define TEXTURE_WAVEFORM_HEIGHT 4095
  #endif
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
  #if (BUFFER_HEIGHT <= (512 * 5 / 2))
    #define TEXTURE_WAVEFORM_HEIGHT  511
  #else //(BUFFER_HEIGHT <= (1024 * 5 / 2))
    #define TEXTURE_WAVEFORM_HEIGHT 1023
  #endif
#else
  #if (BUFFER_COLOR_BIT_DEPTH == 10)
    #if (BUFFER_HEIGHT <= (512 * 5 / 2))
      #define TEXTURE_WAVEFORM_HEIGHT  511
    #else //(BUFFER_HEIGHT <= (1024 * 5 / 2))
      #define TEXTURE_WAVEFORM_HEIGHT 1023
    #endif
  #else
    #define TEXTURE_WAVEFORM_HEIGHT 255
  #endif
#endif


static const int UGH = uint(BUFFER_HEIGHT_FLOAT * 0.35f
                          / float(TEXTURE_WAVEFORM_HEIGHT)
                          * 10000.f);

// "minimum of 2 variables" without using functions...
// https://guru.multimedia.cx/category/optimization/
#if (!defined(IS_HDR_CSP) \
  && BUFFER_COLOR_BIT_DEPTH != 10)

  #define UGH_MAX 40000
  #define UGH_MIN 10000

#else

  #define UGH_MAX 10000
  #define UGH_MIN  5000

#endif

static const int UGH2 = uint(int(UGH_MAX) + ((UGH - int(UGH_MAX)) & ((UGH - int(UGH_MAX)) >> 31)));

static const uint UGH3 = UGH2 - ((UGH2 - int(UGH_MIN)) & ((UGH2 - int(UGH_MIN)) >> 31));

#if (!defined(IS_HDR_CSP) \
  && BUFFER_COLOR_BIT_DEPTH != 10)

  //inverse of "WaveformSizeYFactor" in waveform.fxh
  static const float LUMINANCE_WAVEFORM_DEFAULT_HEIGHT =
    (((float(UGH3) / 10000.f) / (3.f / 350.f)) * 3.f + 700.f) / 21.f;

#else

  static const float LUMINANCE_WAVEFORM_DEFAULT_HEIGHT = float(UGH3) / 100.f;

#endif

uniform float2 _WAVEFORM_SIZE
<
  ui_category = "Waveform";
  ui_label    = "waveform size";
  ui_type     = "slider";
  ui_units    = "%%";
  ui_min      =  50.f;
  ui_max      = 100.f;
  ui_step     =   0.1f;
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = float2(70.f, LUMINANCE_WAVEFORM_DEFAULT_HEIGHT);

uniform bool _WAVEFORM_SHOW_MIN_NITS_LINE
<
  ui_category = "Waveform";
  ui_label    = "show the minimum nits line";
  ui_tooltip  = "Show a horizontal line where the minimum nits is on the waveform."
           "\n" "The line is invisible when the minimum nits hits 0 nits.";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = true;

uniform bool _WAVEFORM_SHOW_MAX_NITS_LINE
<
  ui_category = "Waveform";
  ui_label    = "show the maximum nits line";
  ui_tooltip  = "Show a horizontal line where the maximum nits is on the waveform."
           "\n" "The line is invisible when the maximum nits hits above 10000 nits.";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = true;

uniform bool _WAVEFORM_SHOW_MAX_MIN_PER_COLUMN
<
  ui_category = "Waveform";
  ui_label    = "show min/max outline per column";
  ui_tooltip  = "Draw a solid outline connecting the per-column minimum and maximum values on the waveform.";
  hidden      = HIDDEN_OPTION_COMPUTE_CAPABLE_API;
> = true;

#endif //defined(IS_COMPUTE_CAPABLE_API) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL))


// highlight a certain nit range
uniform bool _HIGHLIGHT_NIT_RANGE
<
  ui_category = "Highlight brightness range visualisation";
  ui_label    = "enable highlighting brightness levels in a certain range";
  ui_tooltip  = "in nits";
> = false;

uniform float _HIGHLIGHT_NIT_RANGE_BRIGHTNESS
<
  ui_category = "Highlight brightness range visualisation";
  ui_label    = "highlighted range brightness";
  ui_type     = "slider";
#ifdef IS_HDR_CSP
  ui_units    = " nits";
  ui_min      = 10.f;
  ui_max      = 250.f;
#else
  ui_units    = "%%";
  ui_min      = 10.f;
  ui_max      = 100.f;
#endif
  ui_step     = 0.5f;
> = DEFAULT_BRIGHTNESS;

uniform float _HIGHLIGHT_NIT_RANGE_START_POINT
<
  ui_category = "Highlight brightness range visualisation";
  ui_label    = "range starting point";
  ui_tooltip  = "CTRL + LEFT CLICK on the value to input an exact value.";
  ui_type     = "drag";
#ifdef IS_HDR_CSP
  ui_units    = " nits";
  ui_min      = 0.f;
  ui_max      = 10000.f;
  ui_step     = 0.0000001f;
#else
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.00001f;
#endif
> = 0.f;

uniform float _HIGHLIGHT_NIT_RANGE_END_POINT
<
  ui_category = "Highlight brightness range visualisation";
  ui_label    = "range end point";
  ui_tooltip  = "CTRL + LEFT CLICK on the value to input an exact value.";
  ui_type     = "drag";
#ifdef IS_HDR_CSP
  ui_units    = " nits";
  ui_min      = 0.f;
  ui_max      = 10000.f;
  ui_step     = 0.0000001f;
#else
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.00001f;
#endif
> = 0.f;

// draw pixels as black depending on their nits
uniform bool _DRAW_ABOVE_NITS_AS_BLACK
<
  ui_category = "Draw certain brightness levels as black";
  ui_label    = "enable drawing above this brightness as black";
> = false;

uniform float _ABOVE_NITS_AS_BLACK
<
  ui_category = "Draw certain brightness levels as black";
  ui_label    = "draw above this brightness as black";
  ui_type     = "slider";
#ifdef IS_HDR_CSP
  ui_units    = " nits";
  ui_min      = 0.f;
  ui_max      = 10000.f;
  ui_step     = 0.0000001f;
#else
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.00001f;
#endif
#ifdef IS_HDR_CSP
> = 10000.f;
#else
> = 100.f;
#endif

uniform bool _DRAW_BELOW_NITS_AS_BLACK
<
  ui_category = "Draw certain brightness levels as black";
  ui_label    = "enable drawing below this brightness as black";
> = false;

uniform float _BELOW_NITS_AS_BLACK
<
  ui_category = "Draw certain brightness levels as black";
  ui_label    = "draw below this brightness as black";
  ui_type     = "drag";
#ifdef IS_HDR_CSP
  ui_units    = " nits";
  ui_min      = 0.f;
  ui_max      = 10000.f;
  ui_step     = 1.f;
#else
  ui_units    = "%%";
  ui_min      = 0.f;
  ui_max      = 100.f;
  ui_step     = 0.5f;
#endif
#ifdef IS_HDR_CSP
> = 10000.f;
#else
> = 100.f;
#endif


#include "lilium__include/hdr_and_sdr_analysis/main.fxh"


// Vertex shader generating a triangle covering the entire screen.
// Calculate values only "once" (3 times because it's 3 vertices)
// for the pixel shader.
void VS_PrepareHdrAnalysis
(
  in                  uint   VertexID          : SV_VertexID,
  out                 float4 Position          : SV_Position,
  out nointerpolation float4 HighlightNitRange : HighlightNitRange,
#ifdef IS_COMPUTE_CAPABLE_API
#if (LINUX_AMD_SHADER_INPUTS_OUTPUTS_WORKAROUND == NO)
  out nointerpolation int4   DisplaySizes      : DisplaySizes,
  out nointerpolation int4   Waveform_Data0    : Waveform_Data0,
#else
  out nointerpolation float4 DisplaySizes      : DisplaySizes,
  out nointerpolation float4 Waveform_Data0    : Waveform_Data0,
#endif
#ifdef IS_HDR_CSP
#if (LINUX_AMD_SHADER_INPUTS_OUTPUTS_WORKAROUND == NO)
  out nointerpolation int    Waveform_Data1    : Waveform_Data1,
#else
  out nointerpolation float  Waveform_Data1    : Waveform_Data1,
#endif
#endif
#endif
  out nointerpolation bool2  PingPongChecks    : PingPongChecks
)
{
  float2 texCoord;
  texCoord.x = (VertexID == 2) ? 2.f
                               : 0.f;
  texCoord.y = (VertexID == 1) ? 2.f
                               : 0.f;
  Position = float4(texCoord * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);

#define pingpong0Above1   PingPongChecks[0]
#define breathingIsActive PingPongChecks[1]

#define highlightNitRangeOut HighlightNitRange.rgb
#define breathing            HighlightNitRange.w

  pingpong0Above1   = false;
  breathingIsActive = false;
  HighlightNitRange = 0.f;
#ifdef IS_COMPUTE_CAPABLE_API
  DisplaySizes      = 0;
  Waveform_Data0    = 0;
#ifdef IS_HDR_CSP
  Waveform_Data1    = 0;
#endif

#define WaveformTextureDisplayAreaBegin DisplaySizes.xy
#define CieDiagramSize                  DisplaySizes.zw

#define Waveform_Size           Waveform_Data0.xy
#define Offset_To_Waveform_Area Waveform_Data0.zw
#ifdef IS_HDR_CSP
#define Waveform_Cutoff_Offset  Waveform_Data1
#endif

#endif //IS_COMPUTE_CAPABLE_API

  BRANCH()
  if (_HIGHLIGHT_NIT_RANGE)
  {
    float pingpong0 = NIT_PINGPONG0.x + 0.5f;

    pingpong0Above1 = pingpong0 >= 1.f;

    if (pingpong0Above1)
    {
      breathing = saturate(pingpong0 - 1.f);
      //breathing = 1.f;

      breathingIsActive = breathing > 0.f;

      float pingpong1 = NIT_PINGPONG1.y == 1 ?       NIT_PINGPONG1.x
                                             : 6.f - NIT_PINGPONG1.x;

      [flatten]
      if (pingpong1 <= 1.f)
      {
        highlightNitRangeOut = float3(1.f, NIT_PINGPONG2.x, 0.f);
      }
      else
      [flatten]
      if (pingpong1 <= 2.f)
      {
        highlightNitRangeOut = float3(NIT_PINGPONG2.x, 1.f, 0.f);
      }
      else
      [flatten]
      if (pingpong1 <= 3.f)
      {
        highlightNitRangeOut = float3(0.f, 1.f, NIT_PINGPONG2.x);
      }
      else
      [flatten]
      if (pingpong1 <= 4.f)
      {
        highlightNitRangeOut = float3(0.f, NIT_PINGPONG2.x, 1.f);
      }
      else
      [flatten]
      if (pingpong1 <= 5.f)
      {
        highlightNitRangeOut = float3(NIT_PINGPONG2.x, 0.f, 1.f);
      }
      else //if (pingpong1 <= 6.f)
      {
        highlightNitRangeOut = float3(1.f, 0.f, NIT_PINGPONG2.x);
      }

      highlightNitRangeOut *= breathing;

      highlightNitRangeOut = MapBt709IntoCurrentCsp(highlightNitRangeOut, _HIGHLIGHT_NIT_RANGE_BRIGHTNESS);
    }
  }

#ifdef IS_COMPUTE_CAPABLE_API
  BRANCH()
  if (_SHOW_WAVEFORM)
  {
    WaveformTextureDisplayAreaBegin = BUFFER_SIZE_INT - Waveform::GetActiveArea();

    Waveform::SWaveformData waveform_data = Waveform::GetData();

    Waveform_Size = waveform_data.waveformArea;

    Offset_To_Waveform_Area = waveform_data.offsetToFrame
                            + waveform_data.frameSize;

#ifdef IS_HDR_CSP
    Waveform_Cutoff_Offset = waveform_data.cutoffOffset;
#endif
  }

  BRANCH()
  if (_SHOW_CIE)
  {
    int2 cieDiagramRenderSize = GetCieDiagramRenderSize();

    cieDiagramRenderSize.y = BUFFER_HEIGHT_INT - cieDiagramRenderSize.y;

    CieDiagramSize = cieDiagramRenderSize;
  }
#endif //IS_COMPUTE_CAPABLE_API
}


void PS_HdrAnalysis
(
  in                  float4 Position          : SV_Position,
  in  nointerpolation float4 HighlightNitRange : HighlightNitRange,
#ifdef IS_COMPUTE_CAPABLE_API
#if (LINUX_AMD_SHADER_INPUTS_OUTPUTS_WORKAROUND == NO)
  in  nointerpolation int4   DisplaySizes      : DisplaySizes,
  in  nointerpolation int4   Waveform_Data0    : Waveform_Data0,
#else
  in  nointerpolation float4 DisplaySizes      : DisplaySizes,
  in  nointerpolation float4 Waveform_Data0    : Waveform_Data0,
#endif
#ifdef IS_HDR_CSP
#if (LINUX_AMD_SHADER_INPUTS_OUTPUTS_WORKAROUND == NO)
  in  nointerpolation int    Waveform_Data1    : Waveform_Data1,
#else
  in  nointerpolation float  Waveform_Data1    : Waveform_Data1,
#endif
#endif
#endif
  in  nointerpolation bool2  PingPongChecks    : PingPongChecks,
  out                 float4 Output            : SV_Target0
)
{
  const int2 pureCoordAsInt = int2(Position.xy);

  Output = tex2Dfetch(SamplerBackBuffer, pureCoordAsInt);

  BRANCH()
  if (_SHOW_HEATMAP
#ifdef IS_HDR_CSP
   || SHOW_GAMUT_MAP
#endif
   || _HIGHLIGHT_NIT_RANGE
   || _DRAW_ABOVE_NITS_AS_BLACK
   || _DRAW_BELOW_NITS_AS_BLACK)
  {
    //static const float4 pixelRgbNits = CalcNitsAndCll(Output.rgb);
    static const float pixelNits = max(CalcNits(Output.rgb), 0.f);

    BRANCH()
    if (_SHOW_HEATMAP)
    {
      Output.rgb = HeatmapRgbValues(pixelNits,
#ifdef IS_HDR_CSP
                                    HEATMAP_CUTOFF_POINT,
#endif
                                    false);

      Output.rgb = MapBt709IntoCurrentCsp(Output.rgb, _HEATMAP_BRIGHTNESS);
    }

#ifdef IS_HDR_CSP
    if (SHOW_GAMUT_MAP)
    {
      Output.rgb = CreateGamutMap(tex2Dfetch(SamplerGamuts, pureCoordAsInt) * 256.f, pixelNits); // *256 for safety
    }
#endif

    BRANCH()
    if (_HIGHLIGHT_NIT_RANGE)
    {
      if (pixelNits >= _HIGHLIGHT_NIT_RANGE_START_POINT
       && pixelNits <= _HIGHLIGHT_NIT_RANGE_END_POINT
       && pingpong0Above1
       && breathingIsActive)
      {
        //Output.rgb = HighlightNitRangeOut;
        Output.rgb = lerp(Output.rgb, highlightNitRangeOut, breathing);
      }
    }

    BRANCH()
    if (_DRAW_ABOVE_NITS_AS_BLACK)
    {
      if (pixelNits > _ABOVE_NITS_AS_BLACK)
      {
        Output.rgba = 0.f;
      }
    }

    BRANCH()
    if (_DRAW_BELOW_NITS_AS_BLACK)
    {
      if (pixelNits < _BELOW_NITS_AS_BLACK)
      {
        Output.rgba = 0.f;
      }
    }
  }

#ifdef IS_COMPUTE_CAPABLE_API
  BRANCH()
  if (_SHOW_CIE)
  {
    // draw the diagram in the bottom left corner
    if (pureCoordAsInt.x <  CieDiagramSize.x
     && pureCoordAsInt.y >= CieDiagramSize.y)
    {
      int2 cieFetchCoords = int2(pureCoordAsInt.x,
                                 pureCoordAsInt.y - CieDiagramSize.y);

      float4 cie_yccrccbc = tex2Dfetch(SamplerCieFinal, cieFetchCoords);

      [branch]
      if (cie_yccrccbc.a > 0.f)
      {
        cie_yccrccbc.yz -= (127.f / 255.f);

#ifdef IS_HDR_CSP
        #define PB_NB_DEC    Csp::YCbCr::PB_NB_BT2020_g2_dec
        #define PR_NR_DEC    Csp::YCbCr::PR_NR_BT2020_g2_dec
        #define K_WEIGHTS_RB Csp::YCbCr::K_BT2020.rb
        #define K_WEIGHT_GI  Csp::YCbCr::K_BT2020G_inverse
#else
        #define PB_NB_DEC    Csp::YCbCr::PB_NB_BT709_g2_dec
        #define PR_NR_DEC    Csp::YCbCr::PR_NR_BT709_g2_dec
        #define K_WEIGHTS_RB Csp::YCbCr::K_BT709.rb
        #define K_WEIGHT_GI  Csp::YCbCr::K_BT709G_inverse
#endif

        float3 cie_colour;

        float2 mul = cie_yccrccbc.yz <= 0.f ? float2(PR_NR_DEC[1], PB_NB_DEC[1])
                                            : float2(PR_NR_DEC[0], PB_NB_DEC[0]);

        // C'rcC'bc->R'B'
        cie_colour.rb = mad(cie_yccrccbc.yz, mul, cie_yccrccbc[0]);

        // R'B'->RB
        cie_colour.rb *= cie_colour.rb;

        // Y'c->Y
        cie_yccrccbc[0] *= cie_yccrccbc[0];

        // G
        cie_colour.g = (cie_yccrccbc[0]
                      - dot(cie_colour.rb, K_WEIGHTS_RB))
                     * K_WEIGHT_GI;

#undef PB_NB_DEC
#undef PR_NR_DEC
#undef K_WEIGHTS_RB
#undef K_WEIGHT_GI

      //FIX THIS
#ifdef IS_HDR_CSP
        cie_colour.rgb = Csp::Mat::BT2020_To::BT709(cie_colour.rgb);
#endif

        Output.rgb = MergeOverlay(Output.rgb,
                                  cie_colour.rgb,
                                  _CIE_DIAGRAM_BRIGHTNESS,
                                  cie_yccrccbc.a * cie_yccrccbc.a);
      }
    }
  }

  BRANCH()
  if (_SHOW_WAVEFORM)
  {
    // draw the waveform in the bottom right corner
    [branch]
    if (all(pureCoordAsInt.xy >= WaveformTextureDisplayAreaBegin))
    {
      // get fetch coords
      const int2 fetch_coords = pureCoordAsInt.xy - WaveformTextureDisplayAreaBegin;

      const int2 scale_coords = fetch_coords;

      float2 scale_colour = tex2Dfetch(SamplerWaveformScale, scale_coords).xy;
      scale_colour.x *= scale_colour.x;

      float4 waveform_final_colour = scale_colour.xxxy;

      int2 waveform_coords = fetch_coords - Offset_To_Waveform_Area;

      int2 waveform_size = Waveform_Size;

      [branch]
      if (all(waveform_coords >= 0)
       && all(waveform_coords < waveform_size))
      {
#ifdef IS_HDR_CSP
        FLATTEN()
        if (WAVEFORM_CUTOFF_POINT > 0u)
        {
          waveform_coords.y += Waveform_Cutoff_Offset;
          waveform_size.y   += Waveform_Cutoff_Offset;
        }
#endif

        float2 waveform_sample_coords;

        int div_x = _WAVEFORM_SIZE.x <  66.7f ? waveform_size.x * 2
                  : _WAVEFORM_SIZE.x < 100.f  ? (int)(uint(waveform_size.x) * 3u / 2u)
                                              : waveform_size.x;

#if (!defined(IS_HDR_CSP) \
  && BUFFER_COLOR_BIT_DEPTH != 10)

          waveform_sample_coords = (float2(waveform_coords) + 0.5f)
                                 / float2(div_x, waveform_size.y);

#else

          int div_y = _WAVEFORM_SIZE.y < 100.f ? waveform_size.y * 2
                                               : waveform_size.y;

          waveform_sample_coords = (float2(waveform_coords) + 0.5f)
                                 / float2(div_x, div_y);

#endif

#ifdef IS_HDR_CSP
        [branch]
        if (_WAVEFORM_SIZE.y < 100.f)
        {
          waveform_sample_coords.y = min(waveform_sample_coords.y,
                                         (float((TEXTURE_WAVEFORM_HEIGHT + 1) / 2 - 1) - 0.5f) / float(TEXTURE_WAVEFORM_HEIGHT));
        }
#endif

        float4 waveform_yrba = tex2D(Sampler_Waveform_Colour, waveform_sample_coords);

        float3 waveform_yrb = waveform_yrba.xyz;

#ifdef IS_HDR_CSP
        waveform_yrb = WAVEFORM_HDR_DECODING(waveform_yrb);
#else
        waveform_yrb = DECODE_SDR(waveform_yrb);
#endif

        float green_luminance = waveform_yrb[0]
                              - dot(Csp::Mat::BT709_To_XYZ[1].rb, waveform_yrb.yz);

        float green = green_luminance
                    / Csp::Mat::BT709_To_XYZ[1].g;

        float3 waveform_colour = float3(waveform_yrb[1], green, waveform_yrb[2]);

#ifdef IS_HDR_CSP
        BRANCH()
        if (_WAVEFORM_MODE == WAVEFORM_MODE_RGB_COMBINED
         || _WAVEFORM_MODE == WAVEFORM_MODE_RGB_INDIVIDUALLY)
        {
          waveform_colour /= Csp::Mat::BT709_To_XYZ[1].g;
        }
#endif

#ifdef IS_HDR_CSP
        waveform_colour *= _WAVEFORM_BRIGHTNESS / _WAVEFORM_SCALE_BRIGHTNESS;
#endif

        waveform_final_colour.rgb += waveform_colour;
        waveform_final_colour.a    = saturate(waveform_final_colour.a + waveform_yrba.a);
      }

      float alpha = max(waveform_final_colour.a, _WAVEFORM_ALPHA / DIV_100);

      Output.rgb = MergeOverlay(Output.rgb,
                                waveform_final_colour.rgb,
                                _WAVEFORM_SCALE_BRIGHTNESS,
                                alpha);
    }
  }
#endif //IS_COMPUTE_CAPABLE_API
}


#ifdef IS_COMPUTE_CAPABLE_API
void CS_MakeOverlayBgAndWaveformScaleRedraw()
{
  tex1Dstore(StorageConsolidated, COORDS_WAVEFORM_LAST_SIZE_X, 0.f);
  tex1Dstore(StorageConsolidated, COORDS_CIE_LAST_SETTINGS,    0.f);
  tex1Dstore(StorageConsolidated, COORDS_UNROLLING_BE_GONE,    0.f);

  memoryBarrier();

  const float unrolling_be_gone_float = tex1Dfetch(StorageConsolidated, COORDS_UNROLLING_BE_GONE);
  const uint  unrolling_be_gone_uint  = uint(unrolling_be_gone_float);
  const int   unrolling_be_gone_int   = int(unrolling_be_gone_float);

  DrawText(unrolling_be_gone_uint,
           unrolling_be_gone_int);

  return;
}

technique lilium__make_overlay_bg_redraw
<
  enabled = true;
  hidden  = true;
  timeout = 1;
>
{
  pass CS_MakeOverlayBgAndWaveformScaleRedraw
  {
    ComputeShader = CS_MakeOverlayBgAndWaveformScaleRedraw <1, 1>;
    DispatchSizeX = 1;
    DispatchSizeY = 1;
  }
}
#endif //IS_COMPUTE_CAPABLE_API

technique lilium__hdr_and_sdr_analysis
<
#ifdef IS_HDR_CSP
  ui_label = "Lilium's HDR analysis";
#else
  ui_label = "Lilium's SDR analysis";
#endif
>
{

#ifndef IS_COMPUTE_CAPABLE_API
  pass PS_Transfer
  {
    VertexShader      = VS_Transfer;
     PixelShader      = PS_Transfer;
    RenderTarget      = TextureTransfer;
    PrimitiveTopology = POINTLIST;
    VertexCount       = 1;
  }
#endif

#ifdef IS_COMPUTE_CAPABLE_API
//Waveform
  pass CS_Clear_Texture_Waveform
  {
    ComputeShader = CS_Clear_Texture_Waveform<WAVE64_THREAD_SIZE_X, WAVE64_THREAD_SIZE_Y, 1>;
    DispatchSizeX = TEXTURE_WAVEFORM_COUNTER_DISPATCH_X;
    DispatchSizeY = TEXTURE_WAVEFORM_COUNTER_DISPATCH_Y;
#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED
    DispatchSizeZ = 3;
#else
    DispatchSizeZ = 3 * TEXTURE_WAVEFORM_DEPTH_MULTIPLIER;
#endif
  }

  pass PS_Reset_Texture_Waveform_Column_Max
  {
    VertexShader = VS_PostProcessWithoutTexCoord;
     PixelShader = PS_Reset_Texture_Waveform_Column_Max;
    RenderTarget = Texture_Waveform_Column_Max_Min;
  }

//Luminance Values
  pass PS_ClearMaxAvgMinNitsAndCspCounterAndShowNumbersTexture
  {
    VertexShader       = VS_Clear;
     PixelShader       = PS_Clear;
    RenderTarget       = TextureMaxAvgMinNitsAndGamutCounterAndShowNumbers;
    ClearRenderTargets = true;
    VertexCount        = 1;
  }

  pass CS_ResetMinNits
  {
    ComputeShader = CS_ResetMinNits<1, 4>;
    DispatchSizeX = 1;
    DispatchSizeY = 1;
  }

#if (CIE_TEXTURE_HEIGHT % WAVE64_THREAD_SIZE_X == 0)
  #define CLEAR_TEXTURE_CIE_COUNTER_DISPATCH (CIE_TEXTURE_HEIGHT / WAVE64_THREAD_SIZE_X)
#else
  #define CLEAR_TEXTURE_CIE_COUNTER_DISPATCH (CIE_TEXTURE_HEIGHT / WAVE64_THREAD_SIZE_X + 1)
#endif
  pass ClearCieCurrentTexture
  {
    ComputeShader = CS_ClearTextureCieCounter<WAVE64_THREAD_SIZE_X, WAVE64_THREAD_SIZE_Y, 1>;
    DispatchSizeX = CLEAR_TEXTURE_CIE_COUNTER_DISPATCH;
    DispatchSizeY = CLEAR_TEXTURE_CIE_COUNTER_DISPATCH;
    DispatchSizeZ = 16;
  }
#endif

//Active Area
  pass PS_SetActiveArea
  {
    VertexShader = VS_PrepareSetActiveArea;
     PixelShader = PS_SetActiveArea;
  }


//CSP
#if (defined(IS_HDR_CSP))
  pass PS_CalcGamuts
  {
    VertexShader = VS_PostProcessWithoutTexCoord;
     PixelShader = PS_CalcGamuts;
    RenderTarget = TextureGamuts;
  }
#endif

#if defined(IS_HDR_CSP)
#if defined(IS_COMPUTE_CAPABLE_API)
  pass CS_CountGamuts
  {
    ComputeShader = CS_CountGamuts <WAVE64_THREAD_SIZE_X, WAVE64_THREAD_SIZE_Y>;
    DispatchSizeX = GAMUT_COUNTER_DISPATCH_X;
    DispatchSizeY = GAMUT_COUNTER_DISPATCH_Y;
  }
#else
  pass PS_CountGamuts
  {
    VertexShader = VS_PostProcessWithoutTexCoord;
     PixelShader = PS_CountGamuts;
    RenderTarget = TextureIntermediate;
  }

  pass PS_FinaliseCountGamuts
  {
    VertexShader      = VS_PrepareFinaliseCountGamuts;
     PixelShader      = PS_FinaliseCountGamuts;
    RenderTarget      = TextureConsolidated;
    PrimitiveTopology = LINELIST;
    VertexCount       = 2;
  }
#endif //defined(IS_COMPUTE_CAPABLE_API)
#endif //defined(IS_HDR_CSP)


//Luminance Values
#ifdef IS_COMPUTE_CAPABLE_API
  pass CS_GetMaxAvgMinNits
  {
    ComputeShader = CS_GetMaxAvgMinNits <WAVE64_THREAD_SIZE_X, WAVE64_THREAD_SIZE_Y>;
    DispatchSizeX = GET_MAX_AVG_MIN_NITS_DISPATCH_X;
    DispatchSizeY = GET_MAX_AVG_MIN_NITS_DISPATCH_Y;
  }
#else
  pass PS_GetMaxAvgMinNits
  {
    VertexShader = VS_PostProcessWithoutTexCoord;
     PixelShader = PS_GetMaxAvgMinNits;
    RenderTarget = TextureIntermediate;
  }

  pass PS_FinaliseGetMaxAvgMinNits
  {
    VertexShader      = VS_PrepareFinaliseGetMaxAvgMinNits;
     PixelShader      = PS_FinaliseGetMaxAvgMinNits;
    RenderTarget      = TextureConsolidated;
    PrimitiveTopology = LINELIST;
    VertexCount       = 2;
  }
#endif //IS_COMPUTE_CAPABLE_API


#ifdef IS_COMPUTE_CAPABLE_API
//Waveform and CIE
  pass CS_RenderWaveformAndGenerateCieDiagram
  {
    ComputeShader = CS_RenderWaveformAndGenerateCieDiagram <WAVE64_THREAD_SIZE_X, WAVE64_THREAD_SIZE_Y>;
    DispatchSizeX = WAVE64_DISPATCH_X;
    DispatchSizeY = WAVE64_DISPATCH_Y;
  }

  pass Get_Max_Waveform_Value
  {
    ComputeShader = CS_Get_Max_Waveform_Value <WAVE64_THREAD_SIZE_X, WAVE64_THREAD_SIZE_Y>;
    DispatchSizeX = TEXTURE_WAVEFORM_COUNTER_DISPATCH_X;
    DispatchSizeY = TEXTURE_WAVEFORM_COUNTER_DISPATCH_Y;
  }

  pass CS_GetMaxCieCounter
  {
    ComputeShader = CS_GetMaxCieCounter <WAVE64_THREAD_SIZE_X, WAVE64_THREAD_SIZE_Y>;
    DispatchSizeX = CIE_TEXTURE_HEIGHT / WAVE64_THREAD_SIZE_X;
    DispatchSizeY = CIE_TEXTURE_HEIGHT / WAVE64_THREAD_SIZE_Y;
  }
#endif


//finalise things
#ifdef IS_COMPUTE_CAPABLE_API
  pass CS_Finalise
  {
    ComputeShader = CS_Finalise <1, 1>;
    DispatchSizeX = 1;
    DispatchSizeY = 1;
  }
#else
  pass PS_Finalise
  {
    VertexShader      = VS_PrepareFinalise;
     PixelShader      = PS_Finalise;
    RenderTarget      = TextureTransfer;
    PrimitiveTopology = LINELIST;
    VertexCount       = 2;
  }
#endif


#ifdef IS_COMPUTE_CAPABLE_API
  pass PS_ComposeCieDiagram
  {
    VertexShader = VS_PrepareComposeCieDiagram;
     PixelShader = PS_ComposeCieDiagram;
    RenderTarget = TextureCieFinal;
  }

  pass CS_RenderCrosshairToCieDiagram
  {
    ComputeShader = CS_RenderCrosshairToCieDiagram <14, 11>;
    DispatchSizeX = 1;
    DispatchSizeY = 1;
  }


//Waveform
  pass PS_Waveform_Render_Colour
  {
    VertexShader       = VS_Prepare_Waveform_Render_Colour;
     PixelShader       = PS_Waveform_Render_Colour;
    RenderTarget       = Texture_Waveform_Colour;
    ClearRenderTargets = true;
  }

#endif //IS_COMPUTE_CAPABLE_API


//get numbers for text drawing
#ifdef IS_COMPUTE_CAPABLE_API
  pass CS_GetNitNumbers
  {
    ComputeShader = CS_GetNitNumbers<NITS_NUMBERS_COUNT, 1>;
    DispatchSizeX = NITS_NUMBERS_COLUMNS;
    DispatchSizeY = NITS_NUMBERS_ROWS;
  }

#ifdef IS_HDR_CSP
  pass CS_GetGamutNumbers
  {
    ComputeShader = CS_GetGamutNumbers<1, 1>;
    DispatchSizeX = GAMUTS_NUMBERS_COUNT;
    DispatchSizeY = GAMUTS_NUMBERS_ROWS;
  }
#endif

#else //IS_COMPUTE_CAPABLE_API

  pass PS_GetNumbersNits
  {
#ifdef IS_HDR_CSP
    VertexShader = VS_PrepareGetNumbersNits;
#else
    VertexShader = VS_PostProcessWithoutTexCoord;
#endif
     PixelShader = PS_GetNumbersNits;
    RenderTarget = TextureMaxAvgMinNitsAndGamutCounterAndShowNumbers;
  }

#ifdef IS_HDR_CSP
  pass PS_GetGamutNumbers
  {
    VertexShader = VS_PrepareGetGamutNumbers;
     PixelShader = PS_GetGamutNumbers;
    RenderTarget = TextureMaxAvgMinNitsAndGamutCounterAndShowNumbers;
  }
#endif
#endif //IS_COMPUTE_CAPABLE_API


  pass PS_HdrAnalysis
  {
    VertexShader = VS_PrepareHdrAnalysis;
     PixelShader = PS_HdrAnalysis;
  }

#ifdef IS_HDR_CSP
  #define FONT_TEXT_VERTEX_COUNT (6 * 6)
#else
  #define FONT_TEXT_VERTEX_COUNT (4 * 6)
#endif

  pass PS_RenderText
  {
    VertexShader = VS_RenderText;
    PixelShader  = PS_RenderText;
#ifdef IS_HDR_CSP
    VertexCount  = (3 + 3 + FONT_TEXT_VERTEX_COUNT);
#else
    VertexCount  = (3 + FONT_TEXT_VERTEX_COUNT);
#endif
  }

  pass PS_RenderNumbers
  {
    VertexShader = VS_RenderNumbers;
    PixelShader  = PS_RenderNumbers;
    VertexCount  = (NUMBERS_COUNT * 6);
  }

#ifndef IS_COMPUTE_CAPABLE_API
  pass PS_Transfer2
  {
    VertexShader      = VS_Transfer2;
     PixelShader      = PS_Transfer2;
    RenderTarget      = TextureConsolidated;
    PrimitiveTopology = POINTLIST;
    VertexCount       = 1;
  }
#endif
}


#else //is hdr API and hdr colour space

ERROR_STUFF

technique lilium__hdr_and_sdr_analysis
<
#if (defined(IS_HDR_CSP) \
  || defined(IS_POSSIBLE_SCRGB_BIT_DEPTH))
  ui_label = "Lilium's HDR analysis (ERROR)";
#else
  ui_label = "Lilium's SDR analysis (ERROR)";
#endif
>
VS_ERROR

#endif //is hdr API and hdr colour space
