
#define WAVE64_THREAD_SIZE_X 8
#define WAVE64_THREAD_SIZE_Y 8

#define WAVE64_THREAD_SIZE (WAVE64_THREAD_SIZE_X * WAVE64_THREAD_SIZE_Y)

#if (BUFFER_WIDTH % WAVE64_THREAD_SIZE_X == 0)
  #define WAVE64_DISPATCH_X (BUFFER_WIDTH / WAVE64_THREAD_SIZE_X)
#else
  #define WAVE64_FETCH_X_NEEDS_CLAMPING
  #define WAVE64_DISPATCH_X (BUFFER_WIDTH / WAVE64_THREAD_SIZE_X + 1)
#endif

#if (BUFFER_HEIGHT % WAVE64_THREAD_SIZE_Y == 0)
  #define WAVE64_DISPATCH_Y (BUFFER_HEIGHT / WAVE64_THREAD_SIZE_Y)
#else
  #define WAVE64_FETCH_Y_NEEDS_CLAMPING
  #define WAVE64_DISPATCH_Y (BUFFER_HEIGHT / WAVE64_THREAD_SIZE_Y + 1)
#endif

#define WAVE_SIZE_6_X (WAVE64_THREAD_SIZE_X * 6)
#define WAVE_SIZE_6_Y (WAVE64_THREAD_SIZE_Y * 6)

#define WAVE_SIZE_5_X (WAVE64_THREAD_SIZE_X * 5)
#define WAVE_SIZE_5_Y (WAVE64_THREAD_SIZE_Y * 5)

#define WAVE_SIZE_4_X (WAVE64_THREAD_SIZE_X * 4)
#define WAVE_SIZE_4_Y (WAVE64_THREAD_SIZE_Y * 4)

#define WAVE_SIZE_3_X (WAVE64_THREAD_SIZE_X * 3)
#define WAVE_SIZE_3_Y (WAVE64_THREAD_SIZE_Y * 3)

#define WAVE_SIZE_2_X (WAVE64_THREAD_SIZE_X * 2)
#define WAVE_SIZE_2_Y (WAVE64_THREAD_SIZE_Y * 2)


// 0.0000000894069671630859375 = ((ieee754_half_decode(0x0002)
//                               - ieee754_half_decode(0x0001))
//                              / 2)
//                             + ieee754_half_decode(0x0001)
#define SMALLEST_FP16   asfloat(0x33C00000)
// 0.0014662756584584712982177734375 = 1.5 / 1023
#define SMALLEST_UINT10 asfloat(0x3AC0300C)


//16:9
//examples:
// - 1920x1080
// - 2560x1440
// - 3200x1800
// - 3840x2160
// - 5120x2880
// - 7680x4320
#if (BUFFER_WIDTH  % 16 == 0 \
  && BUFFER_HEIGHT %  9 == 0 \
  && ((BUFFER_WIDTH / 16) == (BUFFER_HEIGHT / 9)))

  #define AVG_NITS_WIDTH  16
  #define AVG_NITS_HEIGHT  9

//9:16
#elif (BUFFER_WIDTH  %  9 == 0 \
    && BUFFER_HEIGHT % 16 == 0 \
    && ((BUFFER_WIDTH / 9) == (BUFFER_HEIGHT / 16)))

  #define AVG_NITS_WIDTH   9
  #define AVG_NITS_HEIGHT 16

//16:10
//examples:
// - 1920x1200
// - 2560x1600
// - 3840x2400
// - 5120x3200
// - 7680x4800
#elif (BUFFER_WIDTH  % 16 == 0 \
    && BUFFER_HEIGHT % 10 == 0 \
    && ((BUFFER_WIDTH / 16) == (BUFFER_HEIGHT / 10)))

  #define AVG_NITS_WIDTH  16
  #define AVG_NITS_HEIGHT 10

//10:16
#elif (BUFFER_WIDTH  % 10 == 0 \
    && BUFFER_HEIGHT % 16 == 0 \
    && ((BUFFER_WIDTH / 10) == (BUFFER_HEIGHT / 16)))

  #define AVG_NITS_WIDTH  10
  #define AVG_NITS_HEIGHT 16

//32:9
//can be 2x 16:9 stacked horizontally
//examples:
// - 3840x1080
// - 5120x1440
#elif (BUFFER_WIDTH  % 32 == 0 \
    && BUFFER_HEIGHT %  9 == 0 \
    && ((BUFFER_WIDTH / 32) == (BUFFER_HEIGHT / 9)))

  #define AVG_NITS_WIDTH  32
  #define AVG_NITS_HEIGHT  9

//32:10
//can be 2x 16:10 stacked horizontally
//examples:
// - 3840x1200
// - 5120x1600
#elif (BUFFER_WIDTH  % 32 == 0 \
    && BUFFER_HEIGHT % 10 == 0 \
    && ((BUFFER_WIDTH / 32) == (BUFFER_HEIGHT / 10)))

  #define AVG_NITS_WIDTH  32
  #define AVG_NITS_HEIGHT 10

//48:9
//can be 3x 16:9 stacked horizontally
//examples:
// - 5760x1080
// - 7680x1440
#elif (BUFFER_WIDTH  % 48 == 0 \
    && BUFFER_HEIGHT %  9 == 0 \
    && ((BUFFER_WIDTH / 48) == (BUFFER_HEIGHT / 9)))

  #define AVG_NITS_WIDTH  48
  #define AVG_NITS_HEIGHT  9

//48:10
//can be 3x 16:10 stacked horizontally
//examples:
// - 5760x1200
// - 7680x1600
#elif (BUFFER_WIDTH  % 48 == 0 \
    && BUFFER_HEIGHT % 10 == 0 \
    && ((BUFFER_WIDTH / 48) == (BUFFER_HEIGHT / 10)))

  #define AVG_NITS_WIDTH  48
  #define AVG_NITS_HEIGHT 10

//27:16
//3x 16:9 turned by 90° and stacked horizontally
//example:
// - 3240x1920
#elif (BUFFER_WIDTH  % 27 == 0 \
    && BUFFER_HEIGHT % 16 == 0 \
    && ((BUFFER_WIDTH / 27) == (BUFFER_HEIGHT / 16)))

  #define AVG_NITS_WIDTH  27
  #define AVG_NITS_HEIGHT 16

//45:16
//5x 16:9 turned by 90° and stacked horizontally
//example:
// - 5400x1920
#elif (BUFFER_WIDTH  % 45 == 0 \
    && BUFFER_HEIGHT % 16 == 0 \
    && ((BUFFER_WIDTH / 45) == (BUFFER_HEIGHT / 16)))

  #define AVG_NITS_WIDTH  45
  #define AVG_NITS_HEIGHT 16

//30:16
//3x 16:10 turned by 90° and stacked horizontally
//example:
// - 3600x1920
#elif (BUFFER_WIDTH  % 30 == 0 \
    && BUFFER_HEIGHT % 16 == 0 \
    && ((BUFFER_WIDTH / 30) == (BUFFER_HEIGHT / 16)))

  #define AVG_NITS_WIDTH  30
  #define AVG_NITS_HEIGHT 16

//50:16
//5x 16:10 turned by 90° and stacked horizontally
//example:
// - 6000x1920
#elif (BUFFER_WIDTH  % 50 == 0 \
    && BUFFER_HEIGHT % 16 == 0 \
    && ((BUFFER_WIDTH / 50) == (BUFFER_HEIGHT / 16)))

  #define AVG_NITS_WIDTH  50
  #define AVG_NITS_HEIGHT 16

//24:10
//example:
// - 3840x1600
#elif (BUFFER_WIDTH  % 24 == 0 \
    && BUFFER_HEIGHT % 10 == 0 \
    && ((BUFFER_WIDTH / 24) == (BUFFER_HEIGHT / 10)))

  #define AVG_NITS_WIDTH  24
  #define AVG_NITS_HEIGHT 10

//10:24
#elif (BUFFER_WIDTH  % 10 == 0 \
    && BUFFER_HEIGHT % 24 == 0 \
    && ((BUFFER_WIDTH / 10) == (BUFFER_HEIGHT / 24)))

  #define AVG_NITS_WIDTH  10
  #define AVG_NITS_HEIGHT 24

//48:10
//example:
// - 7680x1600 (2x 3840x1600)
#elif (BUFFER_WIDTH  % 48 == 0 \
    && BUFFER_HEIGHT % 10 == 0 \
    && ((BUFFER_WIDTH / 48) == (BUFFER_HEIGHT / 10)))

  #define AVG_NITS_WIDTH  48
  #define AVG_NITS_HEIGHT 10

//72:10
//example:
// - 11520x1600 (3x 3840x1600)
#elif (BUFFER_WIDTH  % 72 == 0 \
    && BUFFER_HEIGHT % 10 == 0 \
    && ((BUFFER_WIDTH / 72) == (BUFFER_HEIGHT / 10)))

  #define AVG_NITS_WIDTH  72
  #define AVG_NITS_HEIGHT 10

//21:9 (~2.37 variant; it's really 21.34:9)
//examples:
// - 2560x1080
// - 5120x2160
#elif (BUFFER_HEIGHT % 9 == 0 \
    && (((BUFFER_WIDTH * 100000) / 2133333) == (BUFFER_HEIGHT / 9)))

  #define AVG_NITS_WIDTH  22
  #define AVG_NITS_HEIGHT  9

//9:21 (~2.37 variant; it's really 9:21.34)
//examples:
// - 1080x2560
// - 2160x5120
#elif (BUFFER_WIDTH % 9 == 0 \
    && ((BUFFER_WIDTH / 9) == ((BUFFER_HEIGHT * 100000) / 2133333)))

  #define AVG_NITS_WIDTH   9
  #define AVG_NITS_HEIGHT 22

//42:9 (~4.67 variant; it's really 42.67:9)
//examples:
// -  5120x1080 (2x 2560x1080)
// - 10240x2160 (2x 5120x2160)
#elif (BUFFER_HEIGHT % 9 == 0 \
    && (((BUFFER_WIDTH * 100000) / 4266666) == (BUFFER_HEIGHT / 9)))

  #define AVG_NITS_WIDTH  43
  #define AVG_NITS_HEIGHT  9

//63:9 (~7.1 variant; it's really 64:9)
//examples:
// -  7680x1080 (3x 2560x1080)
// - 15360x2160 (3x 5120x2160)
#elif (BUFFER_WIDTH  % 64 == 0 \
    && BUFFER_HEIGHT %  9 == 0 \
    && ((BUFFER_WIDTH / 64) == (BUFFER_HEIGHT / 9)))

  #define AVG_NITS_WIDTH  64
  #define AVG_NITS_HEIGHT  9

//21:9 (~2.39 variant; it's really 21.5:9)
//examples:
// - 3440x1440
#elif (BUFFER_HEIGHT % 9 == 0 \
    && (((BUFFER_WIDTH * 100) / 2150) == (BUFFER_HEIGHT / 9)))

  #define AVG_NITS_WIDTH  22
  #define AVG_NITS_HEIGHT  9

//9:21 (~2.39 variant; it's really 9:21.5)
//examples:
// - 1440x3440
#elif (BUFFER_WIDTH % 9 == 0 \
    && ((BUFFER_WIDTH / 9) == ((BUFFER_HEIGHT * 100) / 2150)))

  #define AVG_NITS_WIDTH   9
  #define AVG_NITS_HEIGHT 22

//42:9 (~4.78 variant; it's really 43:9)
//examples:
// - 6880x1440 (2x 3440x1440)
#elif (BUFFER_WIDTH  % 43 == 0 \
    && BUFFER_HEIGHT %  9 == 0 \
    && ((BUFFER_WIDTH / 43) == (BUFFER_HEIGHT / 9)))

  #define AVG_NITS_WIDTH  43
  #define AVG_NITS_HEIGHT  9

//63:9 (~7.167 variant; it's really 64.5:9)
//examples:
// - 10320x1440 (3x 3440x1440)
#elif (BUFFER_HEIGHT % 9 == 0 \
    && (((BUFFER_WIDTH * 100) / 6450) == (BUFFER_HEIGHT / 9)))

  #define AVG_NITS_WIDTH  65
  #define AVG_NITS_HEIGHT  9

//19:10 (DCI; it's really 18.962962962:10)
//examples:
// - 4096x2160
// - 2048x1080
#elif (BUFFER_HEIGHT % 10 == 0 \
    && (((BUFFER_WIDTH * 100000) / 1896296) == (BUFFER_HEIGHT / 10)))

  #define AVG_NITS_WIDTH  19
  #define AVG_NITS_HEIGHT 10

//10:19 (DCI; it's really 10:18.962962962)
//examples:
// - 2160x4096
// - 1080x2048
#elif (BUFFER_WIDTH % 10 == 0 \
    && ((BUFFER_WIDTH / 10) == ((BUFFER_HEIGHT * 100000) / 1896296)))

  #define AVG_NITS_WIDTH  19
  #define AVG_NITS_HEIGHT 10

//4:3
#elif (BUFFER_WIDTH  % 4 == 0 \
    && BUFFER_HEIGHT % 3 == 0 \
    && ((BUFFER_WIDTH / 4) == (BUFFER_HEIGHT / 3)))

  #define AVG_NITS_WIDTH  16
  #define AVG_NITS_HEIGHT 12

//3:4
#elif (BUFFER_WIDTH  % 3 == 0 \
    && BUFFER_HEIGHT % 4 == 0 \
    && ((BUFFER_WIDTH / 3) == (BUFFER_HEIGHT / 4)))

  #define AVG_NITS_WIDTH  12
  #define AVG_NITS_HEIGHT 16

//5:4
#elif (BUFFER_WIDTH  % 5 == 0 \
    && BUFFER_HEIGHT % 4 == 0 \
    && ((BUFFER_WIDTH / 5) == (BUFFER_HEIGHT / 4)))

  #define AVG_NITS_WIDTH  20
  #define AVG_NITS_HEIGHT 16

//4:5
#elif (BUFFER_WIDTH  % 4 == 0 \
    && BUFFER_HEIGHT % 5 == 0 \
    && ((BUFFER_WIDTH / 4) == (BUFFER_HEIGHT / 5)))

  #define AVG_NITS_WIDTH  16
  #define AVG_NITS_HEIGHT 20

//fallback
#else

  //width > height
  #if (BUFFER_WIDTH > BUFFER_HEIGHT)

    #define AVG_NITS_WIDTH  (((BUFFER_WIDTH * 10) / BUFFER_HEIGHT) + 1)
    #define AVG_NITS_HEIGHT 10

  //height > width
  #else

    #define AVG_NITS_WIDTH  10
    #define AVG_NITS_HEIGHT (((BUFFER_HEIGHT * 10) / BUFFER_WIDTH) + 1)

  #endif

#endif


#ifdef IS_COMPUTE_CAPABLE_API
  #define NITS_NUMBERS_COLUMNS 4
#else
  #define NITS_NUMBERS_COLUMNS 1
#endif

#define NITS_NUMBERS_ROWS 4


#if defined(IS_HDR_CSP)

  #define NITS_NUMBERS_COUNT 11

  #define GAMUTS_Y_OFFSET NITS_NUMBERS_ROWS

  #define GAMUTS_NUMBERS_COUNT 6

#ifdef IS_FLOAT_HDR_CSP
  #define GAMUTS_NUMBERS_ROWS 5
#else
  #define GAMUTS_NUMBERS_ROWS 3
#endif

#else

  #define NITS_NUMBERS_COUNT 9

  #define GAMUTS_NUMBERS_ROWS 0

#endif


#define NITS_NUMBERS_PER_ROW (NITS_NUMBERS_COUNT \
                            * NITS_NUMBERS_COLUMNS)

#define NITS_NUMBERS_TOTAL (NITS_NUMBERS_COUNT   \
                          * NITS_NUMBERS_COLUMNS \
                          * NITS_NUMBERS_ROWS)


#if defined(IS_FLOAT_HDR_CSP)
  #define NEEDED_HEIGHT 15
#elif defined(IS_HDR_CSP)
  #define NEEDED_HEIGHT 13
#else
  #define NEEDED_HEIGHT 10
#endif

#if (AVG_NITS_WIDTH >= NITS_NUMBERS_COUNT)
  #define NEEDED_WIDTH AVG_NITS_WIDTH
#else
  #define NEEDED_WIDTH NITS_NUMBERS_COUNT
#endif


//lowest is 9 so only one check needed, since there are only a max of 15 values
#ifdef IS_COMPUTE_CAPABLE_API
  #if (AVG_NITS_HEIGHT >= NEEDED_HEIGHT)
    #define TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_WIDTH (NEEDED_WIDTH * NITS_NUMBERS_COLUMNS + 1)
    #define POS_STORE_X (TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_WIDTH - 1)
  #else
    #define TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_WIDTH (NEEDED_WIDTH * NITS_NUMBERS_COLUMNS + 2)
    #define POS_STORE_X (TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_WIDTH - 2)
  #endif

  #define TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_HEIGHT AVG_NITS_HEIGHT
#else
  #if (NITS_NUMBERS_COUNT >= GAMUTS_NUMBERS_COUNT)
    #define TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_WIDTH NITS_NUMBERS_COUNT
  #else
    #define TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_WIDTH GAMUTS_NUMBERS_COUNT
  #endif

  #define TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_HEIGHT (NITS_NUMBERS_ROWS + GAMUTS_NUMBERS_ROWS)
#endif



#ifdef IS_COMPUTE_CAPABLE_API

  static const int2 POS_MAX_NITS = int2(POS_STORE_X + (0u / AVG_NITS_HEIGHT), (0u % AVG_NITS_HEIGHT));
  static const int2 POS_MAX_R    = int2(POS_STORE_X + (1u / AVG_NITS_HEIGHT), (1u % AVG_NITS_HEIGHT));
  static const int2 POS_MAX_G    = int2(POS_STORE_X + (2u / AVG_NITS_HEIGHT), (2u % AVG_NITS_HEIGHT));
  static const int2 POS_MAX_B    = int2(POS_STORE_X + (3u / AVG_NITS_HEIGHT), (3u % AVG_NITS_HEIGHT));
  static const int2 POS_MIN_NITS = int2(POS_STORE_X + (4u / AVG_NITS_HEIGHT), (4u % AVG_NITS_HEIGHT));
  static const int2 POS_MIN_R    = int2(POS_STORE_X + (5u / AVG_NITS_HEIGHT), (5u % AVG_NITS_HEIGHT));
  static const int2 POS_MIN_G    = int2(POS_STORE_X + (6u / AVG_NITS_HEIGHT), (6u % AVG_NITS_HEIGHT));
  static const int2 POS_MIN_B    = int2(POS_STORE_X + (7u / AVG_NITS_HEIGHT), (7u % AVG_NITS_HEIGHT));

  #ifndef IS_HDR_CSP

    static const int2 POS_CIE_COUNTER_MAX      = int2(POS_STORE_X + (8u / AVG_NITS_HEIGHT), (8u % AVG_NITS_HEIGHT));
    static const int2 POS_WAVEFORM_COUNTER_MAX = int2(POS_STORE_X + (9u / AVG_NITS_HEIGHT), (9u % AVG_NITS_HEIGHT));

  #else //!IS_HDR_CSP

    static const int2 POS_BT709_PERCENTAGE  = int2(POS_STORE_X + ( 8u / AVG_NITS_HEIGHT), ( 8u % AVG_NITS_HEIGHT));
    static const int2 POS_DCIP3_PERCENTAGE  = int2(POS_STORE_X + ( 9u / AVG_NITS_HEIGHT), ( 9u % AVG_NITS_HEIGHT));
    static const int2 POS_BT2020_PERCENTAGE = int2(POS_STORE_X + (10u / AVG_NITS_HEIGHT), (10u % AVG_NITS_HEIGHT));

    #ifndef IS_FLOAT_HDR_CSP

      static const int2 POS_CIE_COUNTER_MAX      = int2(POS_STORE_X + (11u / AVG_NITS_HEIGHT), (11u % AVG_NITS_HEIGHT));
      static const int2 POS_WAVEFORM_COUNTER_MAX = int2(POS_STORE_X + (12u / AVG_NITS_HEIGHT), (12u % AVG_NITS_HEIGHT));

    #else //!IS_FLOAT_HDR_CSP

      static const int2 POS_AP0_PERCENTAGE     = int2(POS_STORE_X + (11u / AVG_NITS_HEIGHT), (11u % AVG_NITS_HEIGHT));
      static const int2 POS_INVALID_PERCENTAGE = int2(POS_STORE_X + (12u / AVG_NITS_HEIGHT), (12u % AVG_NITS_HEIGHT));

      static const int2 POS_CIE_COUNTER_MAX      = int2(POS_STORE_X + (13u / AVG_NITS_HEIGHT), (13u % AVG_NITS_HEIGHT));
      static const int2 POS_WAVEFORM_COUNTER_MAX = int2(POS_STORE_X + (14u / AVG_NITS_HEIGHT), (14u % AVG_NITS_HEIGHT));

    #endif //!IS_FLOAT_HDR_CSP

  #endif //!IS_HDR_CSP

#endif //IS_COMPUTE_CAPABLE_API

#if defined(IS_HDR_CSP)

  #define NUMBERS_COUNT (NITS_NUMBERS_TOTAL + GAMUTS_NUMBERS_ROWS * GAMUTS_NUMBERS_COUNT + 1)

#else

  #define NUMBERS_COUNT NITS_NUMBERS_TOTAL

#endif


#include "../draw_font.fxh"


texture2D TextureMaxAvgMinNitsAndGamutCounterAndShowNumbers
{
  Width  = TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_WIDTH;
  Height = TEXTURE_MAX_AVG_MIN_NITS_AND_GAMUT_COUNTER_AND_SHOW_NUMBERS_HEIGHT;
#ifdef IS_COMPUTE_CAPABLE_API
#ifdef IS_FLOAT_HDR_CSP
  Format = R32I;
#else
  Format = R32U;
#endif //IS_FLOAT_HDR_CSP
#else  //IS_COMPUTE_CAPABLE_API
  Format = R8;
#endif
};

sampler2D
#ifdef IS_COMPUTE_CAPABLE_API
#ifdef IS_FLOAT_HDR_CSP
         <int>
#else
         <uint>
#endif //IS_FLOAT_HDR_CSP
#else  //IS_COMPUTE_CAPABLE_API
         <float>
#endif
                 SamplerMaxAvgMinNitsAndGamutCounterAndShowNumbers
{
  Texture = TextureMaxAvgMinNitsAndGamutCounterAndShowNumbers;
};

#ifdef IS_COMPUTE_CAPABLE_API
storage2D
#ifdef IS_FLOAT_HDR_CSP
         <int>
#else
         <uint>
#endif
                StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers
{
  Texture = TextureMaxAvgMinNitsAndGamutCounterAndShowNumbers;
};
#endif //IS_COMPUTE_CAPABLE_API


// consolidated texture start


// update Nits values and gamut percentages for the overlay
#define UPDATE_OVERLAY_PERCENTAGES_COUNT 1
#define UPDATE_OVERLAY_PERCENTAGES_X_OFFSET 0
#define UPDATE_OVERLAY_PERCENTAGES_Y_OFFSET 0
#define COORDS_UPDATE_OVERLAY_PERCENTAGES int(UPDATE_OVERLAY_PERCENTAGES_X_OFFSET)


// max, avg and min Nits
#ifdef IS_COMPUTE_CAPABLE_API
  #define MAX_AVG_MIN_NITS_VALUES_COUNT 15
#else
  #define MAX_AVG_MIN_NITS_VALUES_COUNT 3
#endif
#define MAX_AVG_MIN_NITS_VALUES_X_OFFSET (UPDATE_OVERLAY_PERCENTAGES_COUNT + UPDATE_OVERLAY_PERCENTAGES_X_OFFSET)
#define MAX_AVG_MIN_NITS_VALUES_Y_OFFSET 0
#define COORDS_MAX_NITS_VALUE int(     MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_MIN_NITS_VALUE int( 1 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_AVG_NITS_VALUE int( 2 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_MAX_R_VALUE    int( 3 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_MAX_G_VALUE    int( 4 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_MAX_B_VALUE    int( 5 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_AVG_R_VALUE    int( 6 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_AVG_G_VALUE    int( 7 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_AVG_B_VALUE    int( 8 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_MIN_R_VALUE    int( 9 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_MIN_G_VALUE    int(10 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_MIN_B_VALUE    int(11 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_MAX_CLL_VALUE  int(12 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_AVG_CLL_VALUE  int(13 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define COORDS_MIN_CLL_VALUE  int(14 + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)


// gamut percentages
#if defined(IS_FLOAT_HDR_CSP)
  #define GAMUT_PERCENTAGES_COUNT 5
#elif defined(IS_HDR10_LIKE_CSP)
  #define GAMUT_PERCENTAGES_COUNT 3
#else
  #define GAMUT_PERCENTAGES_COUNT 0
#endif
#define GAMUT_PERCENTAGES_X_OFFSET (MAX_AVG_MIN_NITS_VALUES_COUNT + MAX_AVG_MIN_NITS_VALUES_X_OFFSET)
#define GAMUT_PERCENTAGES_Y_OFFSET 0
#define COORDS_PERCENTAGE_BT709   int(    GAMUT_PERCENTAGES_X_OFFSET)
#define COORDS_PERCENTAGE_DCI_P3  int(1 + GAMUT_PERCENTAGES_X_OFFSET)
#define COORDS_PERCENTAGE_BT2020  int(2 + GAMUT_PERCENTAGES_X_OFFSET)
#define COORDS_PERCENTAGE_AP0     int(3 + GAMUT_PERCENTAGES_X_OFFSET)
#define COORDS_PERCENTAGE_INVALID int(4 + GAMUT_PERCENTAGES_X_OFFSET)


// show values for max, avg and min Nits plus gamut % for BT.709, DCI-P3, BT.2020, AP0 and invalid
#if defined(IS_FLOAT_HDR_CSP)
  #define SHOW_VALUES_COUNT 20
#elif defined(IS_HDR10_LIKE_CSP)
  #define SHOW_VALUES_COUNT 18
#else
  #define SHOW_VALUES_COUNT 15
#endif
#define SHOW_VALUES_X_OFFSET (GAMUT_PERCENTAGES_COUNT + GAMUT_PERCENTAGES_X_OFFSET)
#define SHOW_VALUES_Y_OFFSET 0
#define COORDS_SHOW_MAX_NITS           int(     SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_AVG_NITS           int( 1 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_MIN_NITS           int( 2 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_MAX_R_VALUE        int( 3 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_AVG_R_VALUE        int( 4 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_MIN_R_VALUE        int( 5 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_MAX_G_VALUE        int( 6 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_AVG_G_VALUE        int( 7 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_MIN_G_VALUE        int( 8 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_MAX_B_VALUE        int( 9 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_AVG_B_VALUE        int(10 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_MIN_B_VALUE        int(11 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_MAX_CLL_VALUE      int(12 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_AVG_CLL_VALUE      int(13 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_MIN_CLL_VALUE      int(14 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_PERCENTAGE_BT709   int(15 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_PERCENTAGE_DCI_P3  int(16 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_PERCENTAGE_BT2020  int(17 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_PERCENTAGE_AP0     int(18 + SHOW_VALUES_X_OFFSET)
#define COORDS_SHOW_PERCENTAGE_INVALID int(19 + SHOW_VALUES_X_OFFSET)


#define UNROLLING_BE_GONE_VARIABLES_COUNT 1
#define UNROLLING_BE_GONE_VARIABLES_X_OFFSET (SHOW_VALUES_COUNT + SHOW_VALUES_X_OFFSET)
#define UNROLLING_BE_GONE_VARIABLES_Y_OFFSET 0
#define COORDS_UNROLLING_BE_GONE             int(UNROLLING_BE_GONE_VARIABLES_X_OFFSET)


#ifdef IS_COMPUTE_CAPABLE_API

// luminance waveform variables
#define LUMINANCE_WAVEFORM_VARIABLES_COUNT 4
#define LUMINANCE_WAVEFORM_VARIABLES_X_OFFSET (UNROLLING_BE_GONE_VARIABLES_COUNT + UNROLLING_BE_GONE_VARIABLES_X_OFFSET)
#define LUMINANCE_WAVEFORM_VARIABLES_Y_OFFSET 0
#define COORDS_WAVEFORM_LAST_SIZE_X           int(    LUMINANCE_WAVEFORM_VARIABLES_X_OFFSET)
#define COORDS_WAVEFORM_LAST_SIZE_Y           int(1 + LUMINANCE_WAVEFORM_VARIABLES_X_OFFSET)
#define COORDS_WAVEFORM_LAST_CUTOFF_POINT     int(2 + LUMINANCE_WAVEFORM_VARIABLES_X_OFFSET)
#define COORDS_WAVEFORM_LAST_TEXT_SIZE_ADJUST int(3 + LUMINANCE_WAVEFORM_VARIABLES_X_OFFSET)


// CIE diagram variables
#define CIE_DIAGRAM_VARIABLES_COUNT 3
#define CIE_DIAGRAM_VARIABLES_X_OFFSET (LUMINANCE_WAVEFORM_VARIABLES_COUNT + LUMINANCE_WAVEFORM_VARIABLES_X_OFFSET)
#define CIE_DIAGRAM_VARIABLES_Y_OFFSET 0
#define CIE_DIAGRAM_TYPE_ENCODE_OFFSET                20
#define CIE_SHOW_GAMUT_OUTLINE_POINTERS_ENCODE_OFFSET 21
#define CIE_SHOW_GAMUT_OUTLINE_BT709_ENCODE_OFFSET    22
#define CIE_SHOW_GAMUT_OUTLINE_DCI_P3_ENCODE_OFFSET   23
#define CIE_SHOW_GAMUT_OUTLINE_BT2020_ENCODE_OFFSET   24
#define CIE_DIAGRAM_TYPE_BIT                0x00100000
#define CIE_SHOW_GAMUT_OUTLINE_POINTERS_BIT 0x00200000
#define CIE_SHOW_GAMUT_OUTLINE_BT709_BIT    0x00400000
#define CIE_SHOW_GAMUT_OUTLINE_DCI_P3_BIT   0x00800000
#define CIE_SHOW_GAMUT_OUTLINE_BT2020_BIT   0x01000000
#define COORDS_CIE_LAST_SETTINGS int(    CIE_DIAGRAM_VARIABLES_X_OFFSET)
#define COORDS_CIE_LAST_SIZE     int(1 + CIE_DIAGRAM_VARIABLES_X_OFFSET)
#define COORDS_CIE_TIMER         int(2 + CIE_DIAGRAM_VARIABLES_X_OFFSET)

#endif


#ifdef IS_COMPUTE_CAPABLE_API
  #define CONSOLIDATED_TEXTURE_WIDTH (CIE_DIAGRAM_VARIABLES_COUNT + CIE_DIAGRAM_VARIABLES_X_OFFSET)
#else
  #define CONSOLIDATED_TEXTURE_WIDTH (GAMUT_PERCENTAGES_COUNT + GAMUT_PERCENTAGES_X_OFFSET)
#endif

#define CONSOLIDATED_TEXTURE_HEIGHT 1


#ifdef IS_COMPUTE_CAPABLE_API
texture1D
#else
texture2D
#endif
          TextureConsolidated
{
  Width  = CONSOLIDATED_TEXTURE_WIDTH;
#ifndef IS_COMPUTE_CAPABLE_API
  Height = CONSOLIDATED_TEXTURE_HEIGHT;
#endif
  Format = R32F;
};

#ifdef IS_COMPUTE_CAPABLE_API
sampler1D
#else
sampler2D
#endif
         <float> SamplerConsolidated
{
  Texture = TextureConsolidated;
};

#ifdef IS_COMPUTE_CAPABLE_API
storage1D<float> StorageConsolidated
{
  Texture = TextureConsolidated;
};
#endif


// consolidated texture end


#ifndef IS_COMPUTE_CAPABLE_API

texture2D TextureTransfer
<
  pooled = true;
>
{
  Width  = CONSOLIDATED_TEXTURE_WIDTH;
  Height = 1;
  Format = R32F;
};

sampler2D<float> SamplerTransfer
{
  Texture = TextureTransfer;
};

#define TEXTURE_INTERMEDIATE_WIDTH  8
#define TEXTURE_INTERMEDIATE_HEIGHT 8

texture2D TextureIntermediate
<
  pooled = true;
>
{
  Width  = TEXTURE_INTERMEDIATE_WIDTH;
  Height = TEXTURE_INTERMEDIATE_HEIGHT;
  Format = RGBA32F;
};

sampler2D<float4> SamplerIntermediate
{
  Texture = TextureIntermediate;
};


float GetPositonXCoordFromRegularXCoord
(
  const float RegularXCoord
)
{
  float positionXCoord = RegularXCoord / CONSOLIDATED_TEXTURE_WIDTH * 2;

  return positionXCoord - 1.f;
}


#define INTERMEDIATE_X_0 (BUFFER_WIDTH / TEXTURE_INTERMEDIATE_WIDTH)
#define INTERMEDIATE_X_1 (BUFFER_WIDTH - INTERMEDIATE_X_0 * (TEXTURE_INTERMEDIATE_WIDTH - 1))

static const int INTERMEDIATE_X[2] =
{
  INTERMEDIATE_X_0,
  INTERMEDIATE_X_1
};

#define INTERMEDIATE_Y_0 (BUFFER_HEIGHT / TEXTURE_INTERMEDIATE_HEIGHT)
#define INTERMEDIATE_Y_1 (BUFFER_HEIGHT - INTERMEDIATE_Y_0 * (TEXTURE_INTERMEDIATE_HEIGHT - 1))

static const int INTERMEDIATE_Y[2] =
{
  INTERMEDIATE_Y_0,
  INTERMEDIATE_Y_1
};
#endif //!IS_COMPUTE_CAPABLE_API


void VS_Clear
(
  out float4 Position : SV_Position
)
{
  Position = float4(-2.f, -2.f, 0.f, 1.f);
}

void PS_Clear()
{
  discard;
}


float2 GetPositonCoordsFromRegularCoords
(
  const float2 RegularCoords,
  const float2 TextureSize
)
{
  float2 positionCoords = RegularCoords / TextureSize * 2.f;

  return float2(positionCoords.x - 1.f,
                1.f              - positionCoords.y);
}


void ExtendedReinhardTmo
(
  inout float3 Colour,
  in    float  WhitePoint
)
{
#ifdef IS_HDR_CSP
  float maxWhite = 10000.f / WhitePoint;
#else
  float maxWhite =   100.f / WhitePoint;
#endif

  Colour = (Colour * (1.f + (Colour / (maxWhite * maxWhite))))
         / (1.f + Colour);
}

float3 MergeOverlay
(
  float3 Output,
  float3 Overlay,
  float  OverlayBrightness,
  float  Alpha
)
{
  // tone map pixels below the overlay area
  [branch]
  if (Alpha > 0.f)
  {
    // first set 1.0 to be equal to OverlayBrightness
    float adjustFactor;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

    adjustFactor = OverlayBrightness / 80.f;

    Output = Csp::Mat::BT709_To::BT2020(Output);

    // safety clamp colours outside of BT.2020
    Output = max(Output, 0.f);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

    adjustFactor = OverlayBrightness / 10000.f;

  #ifdef IS_COMPUTE_CAPABLE_API
    Output = FetchFromHdr10ToLinearLUT(Output);
  #else
    Output = Csp::Trc::PQ_To::Linear(Output);
  #endif

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

    adjustFactor = OverlayBrightness / 100.f;

    Output = DECODE_SDR(Output);

#endif

    Output /= adjustFactor;

    // then tone map to 1.0 at max
    ExtendedReinhardTmo(Output, OverlayBrightness);

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

    // safety clamp for the case that there are values that represent above 10000 nits
    Output = min(Output, 1.f);

#endif

#ifdef IS_HDR_CSP

    Overlay = Csp::Mat::BT709_To::BT2020(Overlay);

#endif

    // apply the overlay
    Output = lerp(Output, Overlay, Alpha);

    // map everything back to the used colour space
    Output *= adjustFactor;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

    Output = Csp::Mat::BT2020_To::BT709(Output);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

    Output = Csp::Trc::Linear_To::PQ(Output);

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

    Output = ENCODE_SDR(Output);

#endif
  }

  return Output;
}



#include "luminance.fxh"
#ifdef IS_HDR_CSP
  #include "gamut.fxh"
#endif
#ifdef IS_COMPUTE_CAPABLE_API
  #include "cie.fxh"
  #include "waveform.fxh"
#endif
#include "draw_text.fxh"
#include "active_area.fxh"


float3 MapBt709IntoCurrentCsp
(
  float3 Colour,
  float  Brightness
)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  return Csp::Map::BT709_Into::scRGB(Colour, Brightness);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

  return Csp::Map::BT709_Into::HDR10(Colour, Brightness);

#elif (ACTUAL_COLOUR_SPACE == CSP_BT2020_EXTENDED)

  return Csp::Map::BT709_Into::BT2020_Extended(Colour, Brightness);

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

  return ENCODE_SDR(Colour * (Brightness / 100.f));

#else

  return 0.f;

#endif
}


#ifdef IS_COMPUTE_CAPABLE_API

void CS_RenderWaveformAndGenerateCieDiagram
(
  uint  GI   : SV_GroupIndex,
  uint3 GID  : SV_GroupID,
  uint3 GTID : SV_GroupThreadID,
  uint3 DTID : SV_DispatchThreadID
)
{

  BRANCH()
  if (_SHOW_WAVEFORM
   || _SHOW_CIE)
  {

#ifndef WAVE64_FETCH_X_NEEDS_CLAMPING
    const int fetchPosX = DTID.x;
#else
    const int fetchPosX = min(DTID.x, BUFFER_WIDTH_MINUS_1_UINT);
#endif

#ifndef WAVE64_FETCH_Y_NEEDS_CLAMPING
    const int fetchPosY = DTID.y;
#else
    const int fetchPosY = min(DTID.y, BUFFER_HEIGHT_MINUS_1_UINT);
#endif

    const int2 fetchPos = int2(fetchPosX, fetchPosY);

    const float3 pixel = tex2Dfetch(SamplerBackBuffer, fetchPos).rgb;

    // get XYZ
    const float3 XYZ = GetXYZFromRgb(pixel);

    BRANCH()
    if (_SHOW_CIE
     && XYZ.y != 0.f)
    {
      GenerateCieDiagram(XYZ, GID.x);
    }

    //ignore negative luminance and luminance being 0
    BRANCH()
    if (_SHOW_WAVEFORM)
    {
      RenderWaveform(fetchPos, pixel, GI, GTID);
    }
  }
}


void CopyShowValues()
{
  float frametimeCounter = tex1Dfetch(StorageConsolidated, COORDS_UPDATE_OVERLAY_PERCENTAGES);
  frametimeCounter += FRAMETIME;

  // only update every 1/2 of a second
  [branch]
  if (frametimeCounter >= _VALUES_UPDATE_RATE)
  {
    tex1Dstore(StorageConsolidated, COORDS_UPDATE_OVERLAY_PERCENTAGES, 0.f);

    float maxNits = tex1Dfetch(StorageConsolidated, COORDS_MAX_NITS_VALUE);
    float maxR    = tex1Dfetch(StorageConsolidated, COORDS_MAX_R_VALUE);
    float maxG    = tex1Dfetch(StorageConsolidated, COORDS_MAX_G_VALUE);
    float maxB    = tex1Dfetch(StorageConsolidated, COORDS_MAX_B_VALUE);
    float avgNits = tex1Dfetch(StorageConsolidated, COORDS_AVG_NITS_VALUE);
    float avgR    = tex1Dfetch(StorageConsolidated, COORDS_AVG_R_VALUE);
    float avgG    = tex1Dfetch(StorageConsolidated, COORDS_AVG_G_VALUE);
    float avgB    = tex1Dfetch(StorageConsolidated, COORDS_AVG_B_VALUE);
    float minNits = tex1Dfetch(StorageConsolidated, COORDS_MIN_NITS_VALUE);
    float minR    = tex1Dfetch(StorageConsolidated, COORDS_MIN_R_VALUE);
    float minG    = tex1Dfetch(StorageConsolidated, COORDS_MIN_G_VALUE);
    float minB    = tex1Dfetch(StorageConsolidated, COORDS_MIN_B_VALUE);
    float maxCll  = tex1Dfetch(StorageConsolidated, COORDS_MAX_CLL_VALUE);
    float avgCll  = tex1Dfetch(StorageConsolidated, COORDS_AVG_CLL_VALUE);
    float minCll  = tex1Dfetch(StorageConsolidated, COORDS_MIN_CLL_VALUE);

    // avoid average nits being higher than max nits and lower than min in extreme edge cases
    avgNits = clamp(avgNits, minNits, maxNits);
    avgR    = clamp(avgR,    minR,    maxR);
    avgG    = clamp(avgG,    minG,    maxG);
    avgB    = clamp(avgB,    minB,    maxB);
    avgCll  = clamp(avgCll,  minCll,  maxCll);

#ifdef IS_HDR_CSP
    float percentageBt709   = tex1Dfetch(StorageConsolidated, COORDS_PERCENTAGE_BT709);
    float percentageDciP3   = tex1Dfetch(StorageConsolidated, COORDS_PERCENTAGE_DCI_P3);
    float percentageBt2020  = tex1Dfetch(StorageConsolidated, COORDS_PERCENTAGE_BT2020);
#if defined(IS_FLOAT_HDR_CSP)
    float percentageAp0     = tex1Dfetch(StorageConsolidated, COORDS_PERCENTAGE_AP0);
    float percentageInvalid = tex1Dfetch(StorageConsolidated, COORDS_PERCENTAGE_INVALID);
#endif //IS_FLOAT_HDR_CSP
#endif //IS_HDR_CSP

    tex1Dstore(StorageConsolidated, COORDS_SHOW_MAX_NITS,      maxNits);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_MAX_R_VALUE,   maxR);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_MAX_G_VALUE,   maxG);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_MAX_B_VALUE,   maxB);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_AVG_NITS,      avgNits);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_AVG_R_VALUE,   avgR);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_AVG_G_VALUE,   avgG);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_AVG_B_VALUE,   avgB);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_MIN_NITS,      minNits);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_MIN_R_VALUE,   minR);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_MIN_G_VALUE,   minG);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_MIN_B_VALUE,   minB);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_MAX_CLL_VALUE, maxCll);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_AVG_CLL_VALUE, avgCll);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_MIN_CLL_VALUE, minCll);

#ifdef IS_HDR_CSP

    tex1Dstore(StorageConsolidated, COORDS_SHOW_PERCENTAGE_BT709,  percentageBt709);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_PERCENTAGE_DCI_P3, percentageDciP3);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_PERCENTAGE_BT2020, percentageBt2020);

#if defined(IS_FLOAT_HDR_CSP)

    tex1Dstore(StorageConsolidated, COORDS_SHOW_PERCENTAGE_AP0,     percentageAp0);
    tex1Dstore(StorageConsolidated, COORDS_SHOW_PERCENTAGE_INVALID, percentageInvalid);

#endif //IS_FLOAT_HDR_CSP
#endif //IS_HDR_CSP

  }
  else
  {
    tex1Dstore(StorageConsolidated, COORDS_UPDATE_OVERLAY_PERCENTAGES, frametimeCounter);
  }

  return;
}


void CS_Finalise()
{
  const float unrolling_be_gone_float = tex1Dfetch(StorageConsolidated, COORDS_UNROLLING_BE_GONE);
  const uint  unrolling_be_gone_uint  = uint(unrolling_be_gone_float);
  const int   unrolling_be_gone_int   = int(unrolling_be_gone_float);

  FinaliseMaxAvgMinNits();

#ifdef IS_HDR_CSP
  FinaliseGamutCounter();
#endif

  RenderWaveformScale(unrolling_be_gone_float,
                      unrolling_be_gone_uint,
                      unrolling_be_gone_int);

  DrawCieOutlines(unrolling_be_gone_float,
                  unrolling_be_gone_uint,
                  unrolling_be_gone_int);

  groupMemoryBarrier();

  CopyShowValues();

  return;
}

#else //IS_COMPUTE_CAPABLE_API

void VS_Transfer
(
  in  uint   VertexID : SV_VertexID,
  out float4 Position : SV_Position
)
{
  Position = float4(-0.99f, 0.f, 0.f, 1.f);
}

void PS_Transfer
(
  in  float4 Position : SV_Position,
  out float  Transfer : SV_Target0
)
{
  Transfer = tex2Dfetch(SamplerConsolidated, int2(COORDS_UPDATE_OVERLAY_PERCENTAGES, 0));
}

void VS_PrepareFinalise
(
  in  uint   VertexID : SV_VertexID,
  out float4 Position : SV_Position
)
{
  static const float positions[2] =
  {
    GetPositonXCoordFromRegularXCoord(COORDS_MAX_NITS_VALUE),
#if defined(IS_FLOAT_HDR_CSP)
    GetPositonXCoordFromRegularXCoord(COORDS_PERCENTAGE_INVALID + 1)
#elif defined(IS_HDR10_LIKE_CSP)
    GetPositonXCoordFromRegularXCoord(COORDS_PERCENTAGE_BT2020 + 1)
#else
    GetPositonXCoordFromRegularXCoord(COORDS_MIN_NITS_VALUE + 1)
#endif
  };

  Position = float4(positions[VertexID], 0.f, 0.f, 1.f);

  return;
}

void PS_Finalise
(
  in  float4 Position : SV_Position,
  out float4 Output   : SV_Target0
)
{
  Output = 0.f;

  float frametimeCounter = tex2Dfetch(SamplerConsolidated, int2(COORDS_UPDATE_OVERLAY_PERCENTAGES, 0));

  // only update every 1/2 of a second
  [branch]
  if (frametimeCounter >= _VALUES_UPDATE_RATE)
  {
    const uint id = uint(Position.x);

#ifdef IS_FLOAT_HDR_CSP
    [branch]
    if (id != COORDS_PERCENTAGE_INVALID)
#endif
    {
      Output = float4(tex2Dfetch(SamplerConsolidated, int2(id, 0)), 0.f, 0.f, 0.f);
    }
#ifdef IS_FLOAT_HDR_CSP
    else
    {
      const float percentageBt709  = tex2Dfetch(SamplerConsolidated, int2(COORDS_PERCENTAGE_BT709,  0));
      const float percentageDciP3  = tex2Dfetch(SamplerConsolidated, int2(COORDS_PERCENTAGE_DCI_P3, 0));
      const float percentageBt2020 = tex2Dfetch(SamplerConsolidated, int2(COORDS_PERCENTAGE_BT2020, 0));
      const float percentageAp0    = tex2Dfetch(SamplerConsolidated, int2(COORDS_PERCENTAGE_AP0,    0));

      const float percentageInvalid = TIMES_100 - (percentageBt709
                                                 + percentageDciP3
                                                 + percentageBt2020
                                                 + percentageAp0);

      Output = float4(percentageInvalid, 0.f, 0.f, 0.f);
    }
#endif

    return;
  }
  else
  {
    discard;
  }
}

void VS_Transfer2
(
  in  uint   VertexID : SV_VertexID,
  out float4 Position : SV_Position
)
{
  Position = float4(-0.99f, 0.f, 0.f, 1.f);
}

void PS_Transfer2
(
  in  float4 Position : SV_Position,
  out float  Transfer : SV_Target0
)
{
  float frametimeCounter = tex2Dfetch(SamplerTransfer, int2(0, 0));

  [branch]
  if (frametimeCounter >= _VALUES_UPDATE_RATE)
  {
    Transfer = 0.f;
  }
  else
  {
    Transfer = frametimeCounter + FRAMETIME;
  }
}

#endif //IS_COMPUTE_CAPABLE_API
