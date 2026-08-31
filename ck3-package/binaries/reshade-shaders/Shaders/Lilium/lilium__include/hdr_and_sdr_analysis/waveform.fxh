#pragma once


static const float TEXTURE_WAVEFORM_BUFFER_WIDTH_FACTOR = float(TEXTURE_WAVEFORM_WIDTH - 1)
                                                        / BUFFER_WIDTH_MINUS_1_FLOAT;

static const float TEXTURE_WAVEFORM_BUFFER_FACTOR = (BUFFER_WIDTH_FLOAT  / 3840.f
                                                   + BUFFER_HEIGHT_FLOAT / 2160.f)
                                                  / 2.f;

static const uint TEXTURE_WAVEFORM_SCALE_BORDER = TEXTURE_WAVEFORM_BUFFER_FACTOR * 35.f + 0.5f;
static const uint TEXTURE_WAVEFORM_SCALE_FRAME  = TEXTURE_WAVEFORM_BUFFER_FACTOR *  7.f + 0.5f;

//static const uint TEXTURE_WAVEFORM_FONT_SIZE =
//  clamp(uint(round(TEXTURE_WAVEFORM_BUFFER_FACTOR * 27.f + 5.f)), 14, 32);

static const uint TEXTURE_WAVEFORM_SCALE_WIDTH = TEXTURE_WAVEFORM_TOTAL_WIDTH
                                               + (CHAR_DIM_UINT.x * 2u * 8u) //8 chars for 10000.00
                                               + uint(CHAR_DIM_FLOAT.x / 2.f + 0.5f)
                                               + (TEXTURE_WAVEFORM_SCALE_BORDER * 2)
                                               + (TEXTURE_WAVEFORM_SCALE_FRAME  * 3);

#if (!defined(IS_HDR_CSP) \
  && BUFFER_COLOR_BIT_DEPTH != 10)
  #define MAX_WAVEFORM_HEIGHT_FACTOR 4
#else
  #define MAX_WAVEFORM_HEIGHT_FACTOR 1
#endif

static const uint TEXTURE_WAVEFORM_SCALE_HEIGHT = TEXTURE_WAVEFORM_HEIGHT * MAX_WAVEFORM_HEIGHT_FACTOR
                                                + uint(CHAR_DIM_FLOAT.y / 2.f - TEXTURE_WAVEFORM_SCALE_FRAME + 0.5f) * 2u
                                                + (TEXTURE_WAVEFORM_SCALE_BORDER * 2)
                                                + (TEXTURE_WAVEFORM_SCALE_FRAME  * 2);

static const float TEXTURE_WAVEFORM_SCALE_FACTOR_X = (TEXTURE_WAVEFORM_SCALE_WIDTH - 1.f)
                                                   / float(TEXTURE_WAVEFORM_WIDTH  - 1);

static const float TEXTURE_WAVEFORM_SCALE_FACTOR_Y = (TEXTURE_WAVEFORM_SCALE_HEIGHT - 1.f)
                                                   / float(TEXTURE_WAVEFORM_HEIGHT  - 1);

#if (TEXTURE_WAVEFORM_TOTAL_WIDTH % WAVE64_THREAD_SIZE_X == 0)
  #define TEXTURE_WAVEFORM_COUNTER_DISPATCH_X (TEXTURE_WAVEFORM_TOTAL_WIDTH / WAVE64_THREAD_SIZE_X)
#else
  #define TEXTURE_WAVEFORM_COUNTER_DISPATCH_X (TEXTURE_WAVEFORM_TOTAL_WIDTH / WAVE64_THREAD_SIZE_X + 1)
#endif
#if (TEXTURE_WAVEFORM_HEIGHT % WAVE64_THREAD_SIZE_Y == 0)
  #define TEXTURE_WAVEFORM_COUNTER_DISPATCH_Y (TEXTURE_WAVEFORM_HEIGHT / WAVE64_THREAD_SIZE_Y)
#else
  #define TEXTURE_WAVEFORM_COUNTER_DISPATCH_Y (TEXTURE_WAVEFORM_HEIGHT / WAVE64_THREAD_SIZE_Y + 1)
#endif


texture2D TextureWaveformScale
{
  Width  = TEXTURE_WAVEFORM_SCALE_WIDTH;
  Height = TEXTURE_WAVEFORM_SCALE_HEIGHT;
  Format = RG8;
};

sampler2D<float4> SamplerWaveformScale
{
  Texture = TextureWaveformScale;
};

storage2D<float4> StorageWaveformScale
{
  Texture = TextureWaveformScale;
};


#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED
  #define TEXTURE_WAVEFORM_DEPTH 3
#else
  #define TEXTURE_WAVEFORM_DEPTH (3 * TEXTURE_WAVEFORM_DEPTH_MULTIPLIER)
#endif

texture3D TextureWaveform
<
  pooled = true;
>
{
  Width  = TEXTURE_WAVEFORM_WIDTH;
  Height = TEXTURE_WAVEFORM_HEIGHT;
  Depth  = TEXTURE_WAVEFORM_DEPTH;
  Format = R32U;
};

sampler3D<uint> SamplerWaveform
{
  Texture = TextureWaveform;
};

storage3D<uint> StorageWaveform
{
  Texture = TextureWaveform;
};


texture2D Texture_Waveform_Colour
<
  pooled = true;
>
{
  Width  = TEXTURE_WAVEFORM_TOTAL_WIDTH;
  Height = TEXTURE_WAVEFORM_HEIGHT;
  Format = RGB10A2;
};

sampler2D<float4> Sampler_Waveform_Colour
{
  Texture = Texture_Waveform_Colour;

  MagFilter = POINT;
  MinFilter = POINT;
  MipFilter = POINT;

  AddressU = CLAMP;
  AddressV = CLAMP;
  AddressW = CLAMP;
};


texture2D Texture_Waveform_Column_Max_Min
<
  pooled = true;
>
{
  Width  = TEXTURE_WAVEFORM_TOTAL_WIDTH;
  Height = 2;
  Format = R32I;
};

sampler2D<int> Sampler_Waveform_Column_Max_Min
{
  Texture = Texture_Waveform_Column_Max_Min;

  AddressU = CLAMP;
  AddressV = CLAMP;
  AddressW = CLAMP;
};

storage2D<int> Storage_Waveform_Column_Max_Min
{
  Texture = Texture_Waveform_Column_Max_Min;
};


namespace Waveform
{

  struct SWaveformData
  {
    int    borderSize;
    int    frameSize;
    float2 charDimensions;
#ifndef IS_HDR_CSP
    float  charDimensionXForPercent;
#endif
    int2   waveformArea;
#ifdef IS_HDR_CSP
    int    cutoffOffset;
    #define WAVEDAT_CUTOFF_OFFSET waveDat.cutoffOffset
    int    tickPoints[16];
#else
    #define WAVEDAT_CUTOFF_OFFSET 0
    int    tickPoints[14];
#endif
#ifdef IS_HDR_CSP
    int    cutoffOffsetAt100PercentScale;
#endif
    int    fontSpacer;
    int2   offsetToFrame;
    int2   textOffset;
    int    tickXOffset;
    int    lowerFrameStart;
    int2   endXY;
    int    endYminus1;
  };

  SWaveformData GetData()
  {
    SWaveformData waveDat;

#if (!defined(IS_HDR_CSP) \
  && BUFFER_COLOR_BIT_DEPTH != 10)
  #define WAVEFORM_SCALE_FACTOR_CLAMP_MIN 0.5f
  #define WAVEFORM_SCALE_FACTOR_CLAMP_MAX float2(1.f, 4.f)
#else
  #define WAVEFORM_SCALE_FACTOR_CLAMP_MIN 0.5f
  #define WAVEFORM_SCALE_FACTOR_CLAMP_MAX 1.f.xx
#endif

    float2 waveformScaleFactorXY = clamp(_WAVEFORM_SIZE / DIV_100, 0.5f, 1.f);

    const float waveformScaleFactor =
      (waveformScaleFactorXY.x + waveformScaleFactorXY.y) / 2.f;

    const float borderAndFrameSizeFactor = max(waveformScaleFactor, 0.75f);

    const float fontSizeFactor = max(waveformScaleFactor, 0.85f);

    static const int maxBorderSize = TEXTURE_WAVEFORM_SCALE_BORDER;
    static const int maxFrameSize  = TEXTURE_WAVEFORM_SCALE_FRAME;

    waveDat.borderSize = clamp(int(TEXTURE_WAVEFORM_BUFFER_FACTOR * 35.f * borderAndFrameSizeFactor + 0.5f), 10, maxBorderSize);
    waveDat.frameSize  = clamp(int(TEXTURE_WAVEFORM_BUFFER_FACTOR *  7.f * borderAndFrameSizeFactor + 0.5f),  4, maxFrameSize);

    static const float minFontSize = 0.375f;

    static const float maxFontSize =
      max(((TEXTURE_WAVEFORM_BUFFER_FACTOR * 27.f + 5.f) / 2.f) * 2.f / 32.f * FONT_SIZE_MULTIPLIER, minFontSize);

    const float fontSize =
      clamp(((TEXTURE_WAVEFORM_BUFFER_FACTOR * 27.f + 5.f) / 2.f * fontSizeFactor) * 2.f / 32.f * FONT_SIZE_MULTIPLIER * (_WAVEFORM_TEXT_SIZE_ADJUST / DIV_100),
            minFontSize,
            maxFontSize);

#ifndef IS_HDR_CSP
    waveDat.charDimensionXForPercent = CHAR_DIM_FLOAT.x * fontSize.x;
#endif

    waveDat.charDimensions = float2(CHAR_DIM_FLOAT.x - 1.f, CHAR_DIM_FLOAT.y) * fontSize;

#ifdef IS_HDR_CSP
    const int maxChars = WAVEFORM_CUTOFF_POINT == 0 ? 8
                                                    : 7;
#else
    const int maxChars = 7;
#endif

    const int textWidth  = waveDat.charDimensions.x * maxChars;
    const int tickSpacer = int(waveDat.charDimensions.x / 2.f + 0.5f);

    waveDat.fontSpacer = int(waveDat.charDimensions.y / 2.f - float(waveDat.frameSize) + 0.5f);

    waveDat.offsetToFrame = int2(waveDat.borderSize + textWidth + tickSpacer + waveDat.frameSize,
                                 waveDat.borderSize + waveDat.fontSpacer);

#if (!defined(IS_HDR_CSP) \
  && BUFFER_COLOR_BIT_DEPTH != 10)
    waveformScaleFactorXY.y += (3.f - ((8.f / 9.f) - waveformScaleFactorXY.y) * 6.f);
    waveformScaleFactorXY.y *= (3.f / 3.5f);
#endif

  const int   texture_waveform_height_scaled       = float(TEXTURE_WAVEFORM_HEIGHT) * waveformScaleFactorXY.y;
  const float texture_waveform_height_scaled_float = texture_waveform_height_scaled;

#ifdef IS_HDR_CSP

    waveDat.tickPoints =
    {
      int(0),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(4000.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(2000.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(1000.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ( 400.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ( 203.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ( 100.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(  50.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(  25.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(  10.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(   5.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(   2.5f ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(   1.f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(   0.25f) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(Csp::Trc::Nits_To::PQ(   0.05f) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled
    };

    waveDat.cutoffOffset = waveDat.tickPoints[WAVEFORM_CUTOFF_POINT];

    waveDat.tickPoints[ 1] -= waveDat.cutoffOffset;
    waveDat.tickPoints[ 2] -= waveDat.cutoffOffset;
    waveDat.tickPoints[ 3] -= waveDat.cutoffOffset;
    waveDat.tickPoints[ 4] -= waveDat.cutoffOffset;
    waveDat.tickPoints[ 5] -= waveDat.cutoffOffset;
    waveDat.tickPoints[ 6] -= waveDat.cutoffOffset;
    waveDat.tickPoints[ 7] -= waveDat.cutoffOffset;
    waveDat.tickPoints[ 8] -= waveDat.cutoffOffset;
    waveDat.tickPoints[ 9] -= waveDat.cutoffOffset;
    waveDat.tickPoints[10] -= waveDat.cutoffOffset;
    waveDat.tickPoints[11] -= waveDat.cutoffOffset;
    waveDat.tickPoints[12] -= waveDat.cutoffOffset;
    waveDat.tickPoints[13] -= waveDat.cutoffOffset;
    waveDat.tickPoints[14] -= waveDat.cutoffOffset;
    waveDat.tickPoints[15] -= waveDat.cutoffOffset;

#ifdef IS_HDR_CSP
    static const int tick_points_at_100_percent_scale[4] =
    {
      int(0),
      int((float(TEXTURE_WAVEFORM_HEIGHT) - (Csp::Trc::Nits_To::PQ(4000.f) * float(TEXTURE_WAVEFORM_HEIGHT))) + 0.5f),
      int((float(TEXTURE_WAVEFORM_HEIGHT) - (Csp::Trc::Nits_To::PQ(2000.f) * float(TEXTURE_WAVEFORM_HEIGHT))) + 0.5f),
      int((float(TEXTURE_WAVEFORM_HEIGHT) - (Csp::Trc::Nits_To::PQ(1000.f) * float(TEXTURE_WAVEFORM_HEIGHT))) + 0.5f)
    };

    waveDat.cutoffOffsetAt100PercentScale = tick_points_at_100_percent_scale[WAVEFORM_CUTOFF_POINT];
#endif

    if (WAVEFORM_CUTOFF_POINT > 0u)
    {
      waveDat.tickPoints[0] = -100;

      if (WAVEFORM_CUTOFF_POINT > 1u)
      {
        waveDat.tickPoints[1] = -100;

        if (WAVEFORM_CUTOFF_POINT > 2u)
        {
          waveDat.tickPoints[2] = -100;
        }
      }
    }

#else

    waveDat.tickPoints =
    {
      int(0),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.875f ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.75f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.6f   ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.5f   ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.35f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.25f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.18f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.1f   ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.05f  ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.025f ) * texture_waveform_height_scaled_float),
      texture_waveform_height_scaled - int(ENCODE_SDR(0.01f  ) * texture_waveform_height_scaled_float),
#if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
  || OVERWRITE_SDR_GAMMA == GAMMA_22    \
  || OVERWRITE_SDR_GAMMA == GAMMA_24)
      texture_waveform_height_scaled - int(ENCODE_SDR(0.0025f) * texture_waveform_height_scaled_float),
#else
      texture_waveform_height_scaled - int(ENCODE_SDR(0.004f ) * texture_waveform_height_scaled_float),
#endif
      texture_waveform_height_scaled
    };

#endif

    float waveform_area_width_float = float(TEXTURE_WAVEFORM_TOTAL_WIDTH) * waveformScaleFactorXY.x;
    uint  waveform_area_width_uint;

    BRANCH()
    if (_WAVEFORM_MODE == WAVEFORM_MODE_RGB_INDIVIDUALLY)
    {
      BRANCH()
      if (_WAVEFORM_SIZE.x < 66.7f || _WAVEFORM_SIZE.x == 100.f)
      {
        waveform_area_width_uint = uint(waveform_area_width_float / 3.f) * 3u;
      }
      else
      {
        waveform_area_width_uint = uint(waveform_area_width_float / 2.f) * 2u;
      }
    }
    else
    {
      waveform_area_width_uint = waveform_area_width_float;
    }

    waveDat.waveformArea =
      int2(waveform_area_width_uint,
#ifdef IS_HDR_CSP
           waveDat.tickPoints[15]
#else
           waveDat.tickPoints[13]
#endif
           );

    waveDat.textOffset = int2(0, int(float(waveDat.charDimensions.y) / 2.f + 0.5f));

    waveDat.tickXOffset = waveDat.borderSize
                        + textWidth
                        + tickSpacer;

    waveDat.lowerFrameStart = waveDat.frameSize
                            + waveDat.waveformArea.y;

    waveDat.endXY = waveDat.frameSize
                  * 2
                  + waveDat.waveformArea;

    waveDat.endYminus1 = waveDat.endXY.y
                       - 1;

    return waveDat;
  }

  int2 GetActiveArea()
  {
    SWaveformData waveDat = GetData();

    return waveDat.offsetToFrame
         + waveDat.frameSize
         + waveDat.waveformArea
         + waveDat.frameSize
         + int2(0, waveDat.fontSpacer)
         + waveDat.borderSize;
  }

  int2 GetNitsOffset
  (
    const int ActiveBorderSize,
    const int ActiveFrameSize,
    const int ActiveFontSpacer,
    const int YOffset
  )
  {
    return int2(ActiveBorderSize,
                ActiveBorderSize + ActiveFontSpacer + ActiveFrameSize + YOffset);
  } //GetNitsOffset


  void DrawCharToScale
  (
    const int    Unrolling_Be_Gone_Int,
    const uint2  Char,
    const float2 CharDim,
    const int2   Pos,
    const int    CharCount
  )
  {
    const float2 charFetchPos = float2(Char * CHAR_DIM_UINT);

    const int2 currentDrawPos = Pos + int2(CharCount * CharDim.x, 0);

    const float charDimFactor = CharDim.x / CHAR_DIM_FLOAT.x;

    const float screenPixelRange = Msdf::GetScreenPixelRange(charDimFactor);

    const float2 char_dim_factor = min(CHAR_DIM_FLOAT / CharDim, 2.f);

    const int2 floorCharDim = floor(CharDim);

    int2 currentOffset = int2(0, 0);

    [loop]
    while (currentOffset.x < (floorCharDim.x + Unrolling_Be_Gone_Int))
    {
      [loop]
      while (currentOffset.y < (floorCharDim.y + Unrolling_Be_Gone_Int))
      {
        float2 currentSamplePos = charFetchPos
                                + float2(currentOffset)
                                * char_dim_factor
                                + 0.5f;

        float2 fract = frac(currentSamplePos);

        int2 currentFetchPos = floor(currentSamplePos);

        float4 a = tex2Dfetch(SamplerFontAtlasConsolidated, currentFetchPos);
        float4 b = tex2Dfetch(SamplerFontAtlasConsolidated, int2(currentFetchPos.x + 1, currentFetchPos.y));
        float4 c = tex2Dfetch(SamplerFontAtlasConsolidated, int2(currentFetchPos.x,     currentFetchPos.y + 1));
        float4 d = tex2Dfetch(SamplerFontAtlasConsolidated, currentFetchPos + 1);

        float4 mtsdf = lerp(lerp(a, b, fract.x),
                            lerp(c, d, fract.x),
                            fract.y);

        const float2 opacities = Msdf::GetTextOpacities(mtsdf, screenPixelRange);

        const float innerOpacity = opacities[0];

        const float outerOpacity = opacities[1];

        float grey = lerp(0, 0.5f, innerOpacity);

        float alpha = innerOpacity + outerOpacity;

        int2 currentDrawOffset = currentDrawPos + currentOffset;

        tex2Dstore(StorageWaveformScale, currentDrawOffset, float4(sqrt(grey), alpha, 0.f, 0.f));

        currentOffset.y++;
      }
      currentOffset.x++;
      currentOffset.y = 0;
    }

    return;
  } //DrawCharToScale

}


groupshared int4 Group_Column_Max0;
groupshared int4 Group_Column_Max1;
groupshared int4 Group_Column_Max2;
groupshared int4 Group_Column_Min0;
groupshared int4 Group_Column_Min1;
groupshared int4 Group_Column_Min2;
groupshared uint Group_Max_Store_X;
void RenderWaveform
(
  const int2   Fetch_Pos,
  const float3 Pixel,
  const uint   GI,
  const uint3  GTID
)
{
  const bool waveform_size_x_is_one_fourth = _WAVEFORM_SIZE.x <  66.7f;
  const bool waveform_size_x_is_one_third  = _WAVEFORM_SIZE.x < 100.f;

  const bool is_gtid_00 = all(GTID.xy == 0u);

  [branch]
  if (is_gtid_00)
  {
    Group_Column_Max0 = uint(INT_MAX);
    Group_Column_Max1 = uint(INT_MAX);
    Group_Column_Max2 = uint(INT_MAX);
    Group_Column_Min0 = 0u;
    Group_Column_Min1 = 0u;
    Group_Column_Min2 = 0u;
    Group_Max_Store_X = 0u;
  }

  barrier();

  const float column_max_min_coord_x_mul = waveform_size_x_is_one_fourth ? 2.f
                                         : waveform_size_x_is_one_third  ? 2.66666666f
                                         :                                 4.f;

  const float coord_x_multiplier = waveform_size_x_is_one_fourth ? 0.25f
                                 : waveform_size_x_is_one_third  ? 0.333333333f
                                 :                                 0.5f;

#ifdef IS_HDR_CSP
  const int waveform_height_int = _WAVEFORM_SIZE.y < 100.f ? int((TEXTURE_WAVEFORM_HEIGHT + 1) / 2 - 1)
                                                           : int(TEXTURE_WAVEFORM_HEIGHT);
#else
  const int waveform_height_int = uint(TEXTURE_WAVEFORM_HEIGHT);
#endif
  const float waveform_height_float = (float)waveform_height_int;

  BRANCH()
  if (_WAVEFORM_MODE == WAVEFORM_MODE_LUMINANCE)
  {
    float pixel_nits_encoded;

    float pixel_nits = Calc_Nits_Normalised(Pixel);

#ifdef IS_HDR_CSP

    pixel_nits_encoded = Csp::Trc::Linear_To::PQ(pixel_nits);

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

    pixel_nits_encoded = ENCODE_SDR(pixel_nits);

#endif

    const uint coord_x = float(Fetch_Pos.x)
                       * coord_x_multiplier;

    const int coord_y = waveform_height_int
                      - int(pixel_nits_encoded * waveform_height_float + 0.5f);

    BRANCH()
    if (_WAVEFORM_SHOW_MAX_MIN_PER_COLUMN)
    {
      const uint group_base_pos_x = uint(float(uint(Fetch_Pos.x) / 8u) * column_max_min_coord_x_mul);

      const uint index = coord_x - group_base_pos_x;

      atomicMax(Group_Max_Store_X, index);

      atomicMin(Group_Column_Max0[index], coord_y);
      atomicMax(Group_Column_Min0[index], coord_y);

      barrier();

      [branch]
      if (GTID.x <= Group_Max_Store_X
       && GTID.y == 0u)
      {
        const int column_max_min_coord_x = int(group_base_pos_x + GTID.x);

        atomicMin(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 0), Group_Column_Max0[GTID.x]);
        atomicMax(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 1), Group_Column_Min0[GTID.x]);
      }
    }

    [branch]
    if
    (
      coord_y <  waveform_height_int
#ifdef IS_FLOAT_HDR_CSP
   && coord_y >= 0
#endif
    )
    {
      int3 store_pos;

#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

      store_pos = int3(coord_x, coord_y, 0);

#else //TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

      store_pos = int3(coord_x % uint(TEXTURE_WAVEFORM_WIDTH),
                       coord_y,
                       coord_x / uint(TEXTURE_WAVEFORM_WIDTH));

#endif //TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

      atomicAdd(StorageWaveform, store_pos, 1u);
    }
  }
  else
  BRANCH()
  if (_WAVEFORM_MODE == WAVEFORM_MODE_MAX_CLL)
  {
    float pixel_max_cll_encoded;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

    float3 pixel_cll = Calc_Cll_Normalised(Pixel);

    pixel_max_cll_encoded = Csp::Trc::Linear_To::PQ(MAXRGB(pixel_cll));

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10 \
    || ACTUAL_COLOUR_SPACE == CSP_SRGB)

    //this is more performant to do
    pixel_max_cll_encoded = MAXRGB(Pixel);

#endif

    const uint coord_x = float(Fetch_Pos.x)
                       * coord_x_multiplier;

    const int coord_y = waveform_height_int
                      - int(pixel_max_cll_encoded * waveform_height_float + 0.5f);

    BRANCH()
    if (_WAVEFORM_SHOW_MAX_MIN_PER_COLUMN)
    {
      const uint group_base_pos_x = uint(float(uint(Fetch_Pos.x) / 8u) * column_max_min_coord_x_mul);

      const uint index = coord_x - group_base_pos_x;

      atomicMax(Group_Max_Store_X, index);

      atomicMin(Group_Column_Max0[index], coord_y);
      atomicMax(Group_Column_Min0[index], coord_y);

      barrier();

      [branch]
      if (GTID.x <= Group_Max_Store_X
       && GTID.y == 0u)
      {
        const int column_max_min_coord_x = int(group_base_pos_x + GTID.x);

        atomicMin(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 0), Group_Column_Max0[GTID.x]);
        atomicMax(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 1), Group_Column_Min0[GTID.x]);
      }
    }

    [branch]
    if
    (
      coord_y <  waveform_height_int
#ifdef IS_FLOAT_HDR_CSP
   && coord_y >= 0
#endif
    )
    {
      int3 store_pos;

#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED
      store_pos = int3(coord_x, coord_y, 0);
#else
      store_pos = int3(coord_x % uint(TEXTURE_WAVEFORM_WIDTH),
                       coord_y,
                       coord_x / uint(TEXTURE_WAVEFORM_WIDTH));
#endif

      atomicAdd(StorageWaveform, store_pos, 1u);
    }
  }
  else
  BRANCH()
  if (_WAVEFORM_MODE == WAVEFORM_MODE_RGB_COMBINED)
  {
    float3 pixel_cll_encoded;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

    float3 pixel_cll = Calc_Cll_Normalised(Pixel);

    pixel_cll_encoded = Csp::Trc::Linear_To::PQ(pixel_cll);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10 \
    || ACTUAL_COLOUR_SPACE == CSP_SRGB)

    //this is more performant to do
    pixel_cll_encoded = Pixel;

#endif

    const uint coord_x = float(Fetch_Pos.x)
                       * coord_x_multiplier;

    const uint3 coords_y = waveform_height_int
                         - uint3(pixel_cll_encoded * waveform_height_float + 0.5f);

    BRANCH()
    if (_WAVEFORM_SHOW_MAX_MIN_PER_COLUMN)
    {
      const int pixel_max_cll_encoded_for_max = MINRGB(coords_y);
      const int pixel_min_cll_encoded_for_min = MAXRGB(coords_y);

      const uint group_base_pos_x = uint(float(uint(Fetch_Pos.x) / 8u) * column_max_min_coord_x_mul);

      const uint index = coord_x - group_base_pos_x;

      atomicMax(Group_Max_Store_X, index);

      atomicMin(Group_Column_Max0[index], pixel_max_cll_encoded_for_max);
      atomicMax(Group_Column_Min0[index], pixel_min_cll_encoded_for_min);

      barrier();

      [branch]
      if (GTID.x <= Group_Max_Store_X
       && GTID.y == 0u)
      {
        const int column_max_min_coord_x = int(group_base_pos_x + GTID.x);

        atomicMin(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 0), Group_Column_Max0[GTID.x]);
        atomicMax(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 1), Group_Column_Min0[GTID.x]);
      }
    }

#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

    const int  store_pos_x = coord_x;
    const int3 store_pos_z = int3(0, 1, 2);

#else //TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

    const int  store_pos_x = int(coord_x % uint(TEXTURE_WAVEFORM_WIDTH));
    const int3 store_pos_z = int3(0, 1, 2) + int((coord_x / uint(TEXTURE_WAVEFORM_WIDTH)) * 3u);

#endif //TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

    [branch]
    if
    (
      coords_y.r <  waveform_height_int
#ifdef IS_FLOAT_HDR_CSP
   && coords_y.r >= 0
#endif
    )
    {
      atomicAdd(StorageWaveform, int3(store_pos_x, coords_y.r, store_pos_z.r), 1u);
    }
    [branch]
    if
    (
      coords_y.g <  waveform_height_int
#ifdef IS_FLOAT_HDR_CSP
   && coords_y.g >= 0
#endif
    )
    {
      atomicAdd(StorageWaveform, int3(store_pos_x, coords_y.g, store_pos_z.g), 1u);
    }
    [branch]
    if
    (
      coords_y.b <  waveform_height_int
#ifdef IS_FLOAT_HDR_CSP
   && coords_y.b >= 0
#endif
    )
    {
      atomicAdd(StorageWaveform, int3(store_pos_x, coords_y.b, store_pos_z.b), 1u);
    }
  }
  else //if (_WAVEFORM_MODE == WAVEFORM_MODE_RGB_INDIVIDUALLY)
  {
    float3 pixel_cll_encoded;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

    float3 pixel_cll = Calc_Cll_Normalised(Pixel);

    pixel_cll_encoded = Csp::Trc::Linear_To::PQ(pixel_cll);

    //this is more performant to do
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10 \
    || ACTUAL_COLOUR_SPACE == CSP_SRGB)

    pixel_cll_encoded = Pixel;

#endif

    const float x_normalised = float(Fetch_Pos.x)
                             / BUFFER_WIDTH_MINUS_1_FLOAT;

    const uint waveform_width_div3 = _WAVEFORM_SIZE.x <  66.7f ? uint(TEXTURE_WAVEFORM_TOTAL_WIDTH / 6)
                                   : _WAVEFORM_SIZE.x < 100.f  ? uint(TEXTURE_WAVEFORM_TOTAL_WIDTH * 2 / 9)
                                   :                             uint(TEXTURE_WAVEFORM_TOTAL_WIDTH / 3);

    const float waveform_width_div3_float = float(waveform_width_div3);

    uint3 coords_x;

    coords_x.r = uint(x_normalised * waveform_width_div3_float);

    coords_x.g = coords_x.r + waveform_width_div3;
    coords_x.b = coords_x.g + waveform_width_div3;

    const int3 coords_y = waveform_height_int
                        - int3(pixel_cll_encoded * waveform_height_float + 0.5f);

    BRANCH()
    if (_WAVEFORM_SHOW_MAX_MIN_PER_COLUMN)
    {
      const float group_base_x_normalised = float((uint(Fetch_Pos.x) / 8u) * 8u)
                                          / BUFFER_WIDTH_MINUS_1_FLOAT;

      const uint group_base_pos_x = uint(group_base_x_normalised * waveform_width_div3_float);

      const uint index = coords_x.r - group_base_pos_x;

      atomicMax(Group_Max_Store_X, index);

      atomicMin(Group_Column_Max0[index], coords_y.r);
      atomicMin(Group_Column_Max1[index], coords_y.g);
      atomicMin(Group_Column_Max2[index], coords_y.b);

      atomicMax(Group_Column_Min0[index], coords_y.r);
      atomicMax(Group_Column_Min1[index], coords_y.g);
      atomicMax(Group_Column_Min2[index], coords_y.b);

      barrier();

      [branch]
      if (GTID.x <= Group_Max_Store_X
       && GTID.y == 0u)
      {
        int column_max_min_coord_x = int(group_base_pos_x + GTID.x);

        atomicMin(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 0), Group_Column_Max0[GTID.x]);
        atomicMax(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 1), Group_Column_Min0[GTID.x]);

        column_max_min_coord_x += waveform_width_div3;
        atomicMin(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 0), Group_Column_Max1[GTID.x]);
        atomicMax(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 1), Group_Column_Min1[GTID.x]);

        column_max_min_coord_x += waveform_width_div3;
        atomicMin(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 0), Group_Column_Max2[GTID.x]);
        atomicMax(Storage_Waveform_Column_Max_Min, int2(column_max_min_coord_x, 1), Group_Column_Min2[GTID.x]);
      }
    }

#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

    const int3 store_pos_x = coords_x;
    const int3 store_pos_z = 0;

#else // TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

    const int3 store_pos_x = int3(coords_x % uint(TEXTURE_WAVEFORM_WIDTH));
    const int3 store_pos_z = int3(coords_x / uint(TEXTURE_WAVEFORM_WIDTH));

#endif // TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

    [branch]
    if
    (
      coords_y.r <  waveform_height_int
#ifdef IS_FLOAT_HDR_CSP
   && coords_y.r >= 0
#endif
    )
    {
      atomicAdd(StorageWaveform, int3(store_pos_x.r, coords_y.r, store_pos_z.r), 1u);
    }
    [branch]
    if
    (
      coords_y.g <  waveform_height_int
#ifdef IS_FLOAT_HDR_CSP
   && coords_y.g >= 0
#endif
    )
    {
      atomicAdd(StorageWaveform, int3(store_pos_x.g, coords_y.g, store_pos_z.g), 1u);
    }
    [branch]
    if
    (
      coords_y.b <  waveform_height_int
#ifdef IS_FLOAT_HDR_CSP
   && coords_y.b >= 0
#endif
    )
    {
      atomicAdd(StorageWaveform, int3(store_pos_x.b, coords_y.b, store_pos_z.b), 1u);
    }

  }

  return;
}


groupshared uint Group_Max;
void CS_Get_Max_Waveform_Value
(
  uint3 GTID : SV_GroupThreadID,
  uint3 DTID : SV_DispatchThreadID
)
{
  BRANCH()
  if (_SHOW_WAVEFORM)
  {
    const bool is_dtid_00 = all(DTID.xy == 0u);

    [branch]
    if (is_dtid_00)
    {
      tex2Dstore(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_WAVEFORM_COUNTER_MAX, 0);
    }

    const bool is_gtid_00 = all(GTID.xy == 0u);

    [branch]
    if (is_gtid_00)
    {
      Group_Max = 0u;
    }

    barrier();

    // worth to do performance wise
    uint2 active_texture_size;

    active_texture_size.x = _WAVEFORM_SIZE.x <  66.7f ? uint(TEXTURE_WAVEFORM_TOTAL_WIDTH / 2)
                          : _WAVEFORM_SIZE.x < 100.f  ? uint(TEXTURE_WAVEFORM_TOTAL_WIDTH * 2 / 3)
                          :                             uint(TEXTURE_WAVEFORM_TOTAL_WIDTH);

#ifdef IS_HDR_CSP
    active_texture_size.y = _WAVEFORM_SIZE.y < 100.f ? uint((TEXTURE_WAVEFORM_HEIGHT + 1) / 2 - 1)
                                                     : uint(TEXTURE_WAVEFORM_HEIGHT);
#else
    active_texture_size.y = uint(TEXTURE_WAVEFORM_HEIGHT);
#endif

    [branch]
    if (all(DTID.xy < active_texture_size))
    {
      BRANCH()
      if (_WAVEFORM_MODE != WAVEFORM_MODE_RGB_COMBINED)
      {
#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

        const int3 fetch_pos = int3(DTID.xy, 0);

#else

        const int3 fetch_pos = int3(DTID.x % uint(TEXTURE_WAVEFORM_WIDTH),
                                    DTID.y,
                                    DTID.x / uint(TEXTURE_WAVEFORM_WIDTH));

#endif

        uint local_value = tex3Dfetch(StorageWaveform, fetch_pos);

        atomicMax(Group_Max, local_value);
      }
      else //if (_WAVEFORM_MODE == WAVEFORM_MODE_RGB_COMBINED)
      {
#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

        const int  fetch_pos_x = DTID.x;
        const int3 fetch_pos_z = int3(0, 1, 2);

#else

        const int  fetch_pos_x = int(DTID.x % uint(TEXTURE_WAVEFORM_WIDTH));
        const int3 fetch_pos_z = int3(0, 1, 2) + int((DTID.x / uint(TEXTURE_WAVEFORM_WIDTH)) * 3u);

#endif

        uint3 local_waveform;

        local_waveform.r = tex3Dfetch(StorageWaveform, int3(fetch_pos_x, DTID.y, fetch_pos_z.r));
        local_waveform.g = tex3Dfetch(StorageWaveform, int3(fetch_pos_x, DTID.y, fetch_pos_z.g));
        local_waveform.b = tex3Dfetch(StorageWaveform, int3(fetch_pos_x, DTID.y, fetch_pos_z.b));

        atomicMax(Group_Max, MAXRGB(local_waveform));
      }

      groupMemoryBarrier();

      [branch]
      if (is_gtid_00)
      {
#ifdef IS_FLOAT_HDR_CSP
        atomicMax(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_WAVEFORM_COUNTER_MAX, int(Group_Max));
#else
        atomicMax(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_WAVEFORM_COUNTER_MAX, Group_Max);
#endif
      }
    }
  }

  return;
}


void RenderWaveformScale
(
  const float Unrolling_Be_Gone_Float,
  const uint  Unrolling_Be_Gone_Uint,
  const int   Unrolling_Be_Gone_Int
)
{
  [branch]
  if (tex1Dfetch(StorageConsolidated, COORDS_WAVEFORM_LAST_SIZE_X)           != _WAVEFORM_SIZE.x
   || tex1Dfetch(StorageConsolidated, COORDS_WAVEFORM_LAST_SIZE_Y)           != _WAVEFORM_SIZE.y
   || tex1Dfetch(StorageConsolidated, COORDS_WAVEFORM_LAST_TEXT_SIZE_ADJUST) != _WAVEFORM_TEXT_SIZE_ADJUST
#ifdef IS_HDR_CSP
   || tex1Dfetch(StorageConsolidated, COORDS_WAVEFORM_LAST_CUTOFF_POINT)     != WAVEFORM_CUTOFF_POINT
#endif
  )
  {
    //make background all black
    [loop]
    for (int x = 0; x < (TEXTURE_WAVEFORM_SCALE_WIDTH + Unrolling_Be_Gone_Int); x++)
    {
      [loop]
      for (int y = 0; y < (TEXTURE_WAVEFORM_SCALE_HEIGHT + Unrolling_Be_Gone_Int); y++)
      {
        tex2Dstore(StorageWaveformScale, int2(x, y), float4(0.f, 0.f, 0.f, 0.f));
      }
    }

    memoryBarrier();

    Waveform::SWaveformData waveDat = Waveform::GetData();

#ifdef IS_HDR_CSP

    #define WAVEFORM_STRINGS_COUNT 16

    int2 nitsOffsets[WAVEFORM_STRINGS_COUNT];

    [loop]
    for (uint i = 0u; i < (uint(WAVEFORM_STRINGS_COUNT) + Unrolling_Be_Gone_Uint); i++)
    {
      nitsOffsets[i] = Waveform::GetNitsOffset(waveDat.borderSize, waveDat.frameSize, waveDat.fontSpacer, waveDat.tickPoints[i]);
    }

    #define nits10000_00Offset nitsOffsets[ 0]
    #define nits_4000_00Offset nitsOffsets[ 1]
    #define nits_2000_00Offset nitsOffsets[ 2]
    #define nits_1000_00Offset nitsOffsets[ 3]
    #define nits__400_00Offset nitsOffsets[ 4]
    #define nits__203_00Offset nitsOffsets[ 5]
    #define nits__100_00Offset nitsOffsets[ 6]
    #define nits___50_00Offset nitsOffsets[ 7]
    #define nits___25_00Offset nitsOffsets[ 8]
    #define nits___10_00Offset nitsOffsets[ 9]
    #define nits____5_00Offset nitsOffsets[10]
    #define nits____2_50Offset nitsOffsets[11]
    #define nits____1_00Offset nitsOffsets[12]
    #define nits____0_25Offset nitsOffsets[13]
    #define nits____0_05Offset nitsOffsets[14]
    #define nits____0_00Offset nitsOffsets[15]


    const int2 text_offset_10000_00 = nits10000_00Offset - waveDat.textOffset;
    const int2 text_offset__4000_00 = nits_4000_00Offset - waveDat.textOffset;
    const int2 text_offset__2000_00 = nits_2000_00Offset - waveDat.textOffset;
    const int2 text_offset__1000_00 = nits_1000_00Offset - waveDat.textOffset;
    const int2 text_offset___400_00 = nits__400_00Offset - waveDat.textOffset;
    const int2 text_offset___203_00 = nits__203_00Offset - waveDat.textOffset;
    const int2 text_offset___100_00 = nits__100_00Offset - waveDat.textOffset;
    const int2 text_offset____50_00 = nits___50_00Offset - waveDat.textOffset;
    const int2 text_offset____25_00 = nits___25_00Offset - waveDat.textOffset;
    const int2 text_offset____10_00 = nits___10_00Offset - waveDat.textOffset;
    const int2 text_offset_____5_00 = nits____5_00Offset - waveDat.textOffset;
    const int2 text_offset_____2_50 = nits____2_50Offset - waveDat.textOffset;
    const int2 text_offset_____1_00 = nits____1_00Offset - waveDat.textOffset;
    const int2 text_offset_____0_25 = nits____0_25Offset - waveDat.textOffset;
    const int2 text_offset_____0_05 = nits____0_05Offset - waveDat.textOffset;
    const int2 text_offset_____0_00 = nits____0_00Offset - waveDat.textOffset;

    const int2 text_offsets[WAVEFORM_STRINGS_COUNT] =
    {
      text_offset_10000_00,
      text_offset__4000_00,
      text_offset__2000_00,
      text_offset__1000_00,
      text_offset___400_00,
      text_offset___203_00,
      text_offset___100_00,
      text_offset____50_00,
      text_offset____25_00,
      text_offset____10_00,
      text_offset_____5_00,
      text_offset_____2_50,
      text_offset_____1_00,
      text_offset_____0_25,
      text_offset_____0_05,
      text_offset_____0_00
    };


    #define CHAR_LIST_10000_00_ARRAY_SIZE 8
    #define CHAR_LIST__4000_00_ARRAY_SIZE 7
    #define CHAR_LIST__2000_00_ARRAY_SIZE 7
    #define CHAR_LIST__1000_00_ARRAY_SIZE 7
    #define CHAR_LIST___400_00_ARRAY_SIZE 6
    #define CHAR_LIST___203_00_ARRAY_SIZE 6
    #define CHAR_LIST___100_00_ARRAY_SIZE 6
    #define CHAR_LIST____50_00_ARRAY_SIZE 5
    #define CHAR_LIST____25_00_ARRAY_SIZE 5
    #define CHAR_LIST____10_00_ARRAY_SIZE 5
    #define CHAR_LIST_____5_00_ARRAY_SIZE 4
    #define CHAR_LIST_____2_50_ARRAY_SIZE 4
    #define CHAR_LIST_____1_00_ARRAY_SIZE 4
    #define CHAR_LIST_____0_25_ARRAY_SIZE 4
    #define CHAR_LIST_____0_05_ARRAY_SIZE 4
    #define CHAR_LIST_____0_00_ARRAY_SIZE 4

    static const uint2 char_list_10000_00[CHAR_LIST_10000_00_ARRAY_SIZE] = { __1, __0, __0, __0, __0, __dot, __0, __0 };
    static const uint2 char_list__4000_00[CHAR_LIST__4000_00_ARRAY_SIZE] = {      __4, __0, __0, __0, __dot, __0, __0 };
    static const uint2 char_list__2000_00[CHAR_LIST__2000_00_ARRAY_SIZE] = {      __2, __0, __0, __0, __dot, __0, __0 };
    static const uint2 char_list__1000_00[CHAR_LIST__1000_00_ARRAY_SIZE] = {      __1, __0, __0, __0, __dot, __0, __0 };
    static const uint2 char_list___400_00[CHAR_LIST___400_00_ARRAY_SIZE] = {           __4, __0, __0, __dot, __0, __0 };
    static const uint2 char_list___203_00[CHAR_LIST___203_00_ARRAY_SIZE] = {           __2, __0, __3, __dot, __0, __0 };
    static const uint2 char_list___100_00[CHAR_LIST___100_00_ARRAY_SIZE] = {           __1, __0, __0, __dot, __0, __0 };
    static const uint2 char_list____50_00[CHAR_LIST____50_00_ARRAY_SIZE] = {                __5, __0, __dot, __0, __0 };
    static const uint2 char_list____25_00[CHAR_LIST____25_00_ARRAY_SIZE] = {                __2, __5, __dot, __0, __0 };
    static const uint2 char_list____10_00[CHAR_LIST____10_00_ARRAY_SIZE] = {                __1, __0, __dot, __0, __0 };
    static const uint2 char_list_____5_00[CHAR_LIST_____5_00_ARRAY_SIZE] = {                     __5, __dot, __0, __0 };
    static const uint2 char_list_____2_50[CHAR_LIST_____2_50_ARRAY_SIZE] = {                     __2, __dot, __5, __0 };
    static const uint2 char_list_____1_00[CHAR_LIST_____1_00_ARRAY_SIZE] = {                     __1, __dot, __0, __0 };
    static const uint2 char_list_____0_25[CHAR_LIST_____0_25_ARRAY_SIZE] = {                     __0, __dot, __2, __5 };
    static const uint2 char_list_____0_05[CHAR_LIST_____0_05_ARRAY_SIZE] = {                     __0, __dot, __0, __5 };
    static const uint2 char_list_____0_00[CHAR_LIST_____0_00_ARRAY_SIZE] = {                     __0, __dot, __0, __0 };

    static const uint char_list_array_sizes[WAVEFORM_STRINGS_COUNT] =
    {
      CHAR_LIST_10000_00_ARRAY_SIZE,
      CHAR_LIST__4000_00_ARRAY_SIZE,
      CHAR_LIST__2000_00_ARRAY_SIZE,
      CHAR_LIST__1000_00_ARRAY_SIZE,
      CHAR_LIST___400_00_ARRAY_SIZE,
      CHAR_LIST___203_00_ARRAY_SIZE,
      CHAR_LIST___100_00_ARRAY_SIZE,
      CHAR_LIST____50_00_ARRAY_SIZE,
      CHAR_LIST____25_00_ARRAY_SIZE,
      CHAR_LIST____10_00_ARRAY_SIZE,
      CHAR_LIST_____5_00_ARRAY_SIZE,
      CHAR_LIST_____2_50_ARRAY_SIZE,
      CHAR_LIST_____1_00_ARRAY_SIZE,
      CHAR_LIST_____0_25_ARRAY_SIZE,
      CHAR_LIST_____0_05_ARRAY_SIZE,
      CHAR_LIST_____0_00_ARRAY_SIZE
    };


    static const int char_offsets[WAVEFORM_STRINGS_COUNT] =
    {
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST_10000_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST__4000_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST__2000_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST__1000_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST___400_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST___203_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST___100_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST____50_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST____25_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST____10_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST_____5_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST_____2_50_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST_____1_00_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST_____0_25_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST_____0_05_ARRAY_SIZE,
      CHAR_LIST_10000_00_ARRAY_SIZE - CHAR_LIST_____0_00_ARRAY_SIZE
    };


    [loop]
    for (uint i = 0u; i < (uint(WAVEFORM_STRINGS_COUNT) + Unrolling_Be_Gone_Uint); i++)
    {
      const int2 text_offset_current = text_offsets[i];

      const int char_offset_current = char_offsets[i];

      bool drawing_needed = true;

      [loop]
      for (int j = 0; j < (int(char_list_array_sizes[i]) + Unrolling_Be_Gone_Int); j++)
      {
        uint2 char_current;

        [forcecase]
        switch(i)
        {
          case 0:
          {
            char_current = char_list_10000_00[j];

            drawing_needed = WAVEFORM_CUTOFF_POINT == 0;
          }
          break;
          case 1:
          {
            char_current = char_list__4000_00[j];

            drawing_needed = WAVEFORM_CUTOFF_POINT <= 1;
          }
          break;
          case 2:
          {
            char_current = char_list__2000_00[j];

            drawing_needed = WAVEFORM_CUTOFF_POINT <= 2;
          }
          break;
          case 3:
          {
            char_current = char_list__1000_00[j];
          }
          break;
          case 4:
          {
            char_current = char_list___400_00[j];
          }
          break;
          case 5:
          {
            char_current = char_list___203_00[j];
          }
          break;
          case 6:
          {
            char_current = char_list___100_00[j];
          }
          break;
          case 7:
          {
            char_current = char_list____50_00[j];
          }
          break;
          case 8:
          {
            char_current = char_list____25_00[j];
          }
          break;
          case 9:
          {
            char_current = char_list____10_00[j];
          }
          break;
          case 10:
          {
            char_current = char_list_____5_00[j];
          }
          break;
          case 11:
          {
            char_current = char_list_____2_50[j];
          }
          break;
          case 12:
          {
            char_current = char_list_____1_00[j];
          }
          break;
          case 13:
          {
            char_current = char_list_____0_25[j];
          }
          break;
          case 14:
          {
            char_current = char_list_____0_05[j];

          }
          break;
          default: //case 15:
          {
            char_current = char_list_____0_00[j];
          }
          break;
        }

        [branch]
        if (drawing_needed)
        {
          int index_current = j + char_offset_current;

          [flatten]
          if (WAVEFORM_CUTOFF_POINT != 0)
          {
            index_current--;
          }

          Waveform::DrawCharToScale(Unrolling_Be_Gone_Int,
                                    char_current,
                                    waveDat.charDimensions,
                                    text_offset_current,
                                    index_current);
        }
      }
    }

#else

    #define WAVEFORM_STRINGS_COUNT 14

    int2 nitsOffsets[WAVEFORM_STRINGS_COUNT];

    [loop]
    for (uint i = 0u; i < (uint(WAVEFORM_STRINGS_COUNT) + Unrolling_Be_Gone_Uint); i++)
    {
      nitsOffsets[i] = Waveform::GetNitsOffset(waveDat.borderSize, waveDat.frameSize, waveDat.fontSpacer, waveDat.tickPoints[i]);
    }

    #define nits100_00Offset nitsOffsets[ 0]
    #define nits_87_50Offset nitsOffsets[ 1]
    #define nits_75_00Offset nitsOffsets[ 2]
    #define nits_60_00Offset nitsOffsets[ 3]
    #define nits_50_00Offset nitsOffsets[ 4]
    #define nits_35_00Offset nitsOffsets[ 5]
    #define nits_25_00Offset nitsOffsets[ 6]
    #define nits_18_00Offset nitsOffsets[ 7]
    #define nits_10_00Offset nitsOffsets[ 8]
    #define nits__5_00Offset nitsOffsets[ 9]
    #define nits__2_50Offset nitsOffsets[10]
    #define nits__1_00Offset nitsOffsets[11]
#if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
  || OVERWRITE_SDR_GAMMA == GAMMA_22    \
  || OVERWRITE_SDR_GAMMA == GAMMA_24)
    #define nits__0_25Offset nitsOffsets[12]
#else
    #define nits__0_40Offset nitsOffsets[12]
#endif
    #define nits__0_00Offset nitsOffsets[13]


    const int2 text_offset_100_00 = nits100_00Offset - waveDat.textOffset;
    const int2 text_offset__87_50 = nits_87_50Offset - waveDat.textOffset;
    const int2 text_offset__75_00 = nits_75_00Offset - waveDat.textOffset;
    const int2 text_offset__60_00 = nits_60_00Offset - waveDat.textOffset;
    const int2 text_offset__50_00 = nits_50_00Offset - waveDat.textOffset;
    const int2 text_offset__35_00 = nits_35_00Offset - waveDat.textOffset;
    const int2 text_offset__25_00 = nits_25_00Offset - waveDat.textOffset;
    const int2 text_offset__18_00 = nits_18_00Offset - waveDat.textOffset;
    const int2 text_offset__10_00 = nits_10_00Offset - waveDat.textOffset;
    const int2 text_offset___5_00 = nits__5_00Offset - waveDat.textOffset;
    const int2 text_offset___2_50 = nits__2_50Offset - waveDat.textOffset;
    const int2 text_offset___1_00 = nits__1_00Offset - waveDat.textOffset;
#if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
  || OVERWRITE_SDR_GAMMA == GAMMA_22    \
  || OVERWRITE_SDR_GAMMA == GAMMA_24)
    const int2 text_offset___0_25 = nits__0_25Offset - waveDat.textOffset;
#else
    const int2 text_offset___0_40 = nits__0_40Offset - waveDat.textOffset;
#endif
    const int2 text_offset___0_00 = nits__0_00Offset - waveDat.textOffset;

    const int2 text_offsets[WAVEFORM_STRINGS_COUNT] =
    {
      text_offset_100_00,
      text_offset__87_50,
      text_offset__75_00,
      text_offset__60_00,
      text_offset__50_00,
      text_offset__35_00,
      text_offset__25_00,
      text_offset__18_00,
      text_offset__10_00,
      text_offset___5_00,
      text_offset___2_50,
      text_offset___1_00,
#if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
  || OVERWRITE_SDR_GAMMA == GAMMA_22    \
  || OVERWRITE_SDR_GAMMA == GAMMA_24)
      text_offset___0_25,
#else
      text_offset___0_40,
#endif
      text_offset___0_00
    };


    #define CHAR_LIST_100_00_ARRAY_SIZE 7
    #define CHAR_LIST__87_50_ARRAY_SIZE 6
    #define CHAR_LIST__75_00_ARRAY_SIZE 6
    #define CHAR_LIST__60_00_ARRAY_SIZE 6
    #define CHAR_LIST__50_00_ARRAY_SIZE 6
    #define CHAR_LIST__35_00_ARRAY_SIZE 6
    #define CHAR_LIST__25_00_ARRAY_SIZE 6
    #define CHAR_LIST__18_00_ARRAY_SIZE 6
    #define CHAR_LIST__10_00_ARRAY_SIZE 6
    #define CHAR_LIST___5_00_ARRAY_SIZE 5
    #define CHAR_LIST___2_50_ARRAY_SIZE 5
    #define CHAR_LIST___1_00_ARRAY_SIZE 5
#if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
  || OVERWRITE_SDR_GAMMA == GAMMA_22    \
  || OVERWRITE_SDR_GAMMA == GAMMA_24)
    #define CHAR_LIST___0_25_ARRAY_SIZE 5
#else
    #define CHAR_LIST___0_40_ARRAY_SIZE 5
#endif
    #define CHAR_LIST___0_00_ARRAY_SIZE 5

    static const uint2 char_list_100_00[CHAR_LIST_100_00_ARRAY_SIZE] = { __1, __0, __0, __dot, __0, __0, __percent };
    static const uint2 char_list__87_50[CHAR_LIST__87_50_ARRAY_SIZE] = {      __8, __7, __dot, __5, __0, __percent };
    static const uint2 char_list__75_00[CHAR_LIST__75_00_ARRAY_SIZE] = {      __7, __5, __dot, __0, __0, __percent };
    static const uint2 char_list__60_00[CHAR_LIST__60_00_ARRAY_SIZE] = {      __6, __0, __dot, __0, __0, __percent };
    static const uint2 char_list__50_00[CHAR_LIST__50_00_ARRAY_SIZE] = {      __5, __0, __dot, __0, __0, __percent };
    static const uint2 char_list__35_00[CHAR_LIST__35_00_ARRAY_SIZE] = {      __3, __5, __dot, __0, __0, __percent };
    static const uint2 char_list__25_00[CHAR_LIST__25_00_ARRAY_SIZE] = {      __2, __5, __dot, __0, __0, __percent };
    static const uint2 char_list__18_00[CHAR_LIST__18_00_ARRAY_SIZE] = {      __1, __8, __dot, __0, __0, __percent };
    static const uint2 char_list__10_00[CHAR_LIST__10_00_ARRAY_SIZE] = {      __1, __0, __dot, __0, __0, __percent };
    static const uint2 char_list___5_00[CHAR_LIST___5_00_ARRAY_SIZE] = {           __5, __dot, __0, __0, __percent };
    static const uint2 char_list___2_50[CHAR_LIST___2_50_ARRAY_SIZE] = {           __2, __dot, __5, __0, __percent };
    static const uint2 char_list___1_00[CHAR_LIST___1_00_ARRAY_SIZE] = {           __1, __dot, __0, __0, __percent };
#if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
  || OVERWRITE_SDR_GAMMA == GAMMA_22    \
  || OVERWRITE_SDR_GAMMA == GAMMA_24)
    static const uint2 char_list___0_25[CHAR_LIST___0_25_ARRAY_SIZE] = {           __0, __dot, __2, __5, __percent };
#else
    static const uint2 char_list___0_40[CHAR_LIST___0_40_ARRAY_SIZE] = {           __0, __dot, __4, __0, __percent };
#endif
    static const uint2 char_list___0_00[CHAR_LIST___0_00_ARRAY_SIZE] = {           __0, __dot, __0, __0, __percent };

    static const uint char_list_array_sizes[WAVEFORM_STRINGS_COUNT] =
    {
      CHAR_LIST_100_00_ARRAY_SIZE,
      CHAR_LIST__87_50_ARRAY_SIZE,
      CHAR_LIST__75_00_ARRAY_SIZE,
      CHAR_LIST__60_00_ARRAY_SIZE,
      CHAR_LIST__50_00_ARRAY_SIZE,
      CHAR_LIST__35_00_ARRAY_SIZE,
      CHAR_LIST__25_00_ARRAY_SIZE,
      CHAR_LIST__18_00_ARRAY_SIZE,
      CHAR_LIST__10_00_ARRAY_SIZE,
      CHAR_LIST___5_00_ARRAY_SIZE,
      CHAR_LIST___2_50_ARRAY_SIZE,
      CHAR_LIST___1_00_ARRAY_SIZE,
#if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
  || OVERWRITE_SDR_GAMMA == GAMMA_22    \
  || OVERWRITE_SDR_GAMMA == GAMMA_24)
      CHAR_LIST___0_25_ARRAY_SIZE,
#else
      CHAR_LIST___0_40_ARRAY_SIZE,
#endif
      CHAR_LIST___0_00_ARRAY_SIZE
    };

    static const int char_offsets[WAVEFORM_STRINGS_COUNT] =
    {
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST_100_00_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST__87_50_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST__75_00_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST__60_00_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST__50_00_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST__35_00_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST__25_00_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST__18_00_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST__10_00_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST___5_00_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST___2_50_ARRAY_SIZE,
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST___1_00_ARRAY_SIZE,
#if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
  || OVERWRITE_SDR_GAMMA == GAMMA_22    \
  || OVERWRITE_SDR_GAMMA == GAMMA_24)
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST___0_25_ARRAY_SIZE,
#else
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST___0_40_ARRAY_SIZE,
#endif
      CHAR_LIST_100_00_ARRAY_SIZE - CHAR_LIST___0_00_ARRAY_SIZE
    };


    float2 char_dims;

    char_dims.y = waveDat.charDimensions.y;


    [loop]
    for (uint i = 0u; i < (uint(WAVEFORM_STRINGS_COUNT) + Unrolling_Be_Gone_Uint); i++)
    {
      const int2 text_offset_current = text_offsets[i];

      const int char_offset_current = char_offsets[i];

      [loop]
      for (int j = 0; j < (int(char_list_array_sizes[i]) + Unrolling_Be_Gone_Int); j++)
      {
        uint2 char_current;

        [forcecase]
        switch(i)
        {
          case 0:
          {
            char_current = char_list_100_00[j];
          }
          break;
          case 1:
          {
            char_current = char_list__87_50[j];
          }
          break;
          case 2:
          {
            char_current = char_list__75_00[j];
          }
          break;
          case 3:
          {
            char_current = char_list__60_00[j];
          }
          break;
          case 4:
          {
            char_current = char_list__50_00[j];
          }
          break;
          case 5:
          {
            char_current = char_list__35_00[j];
          }
          break;
          case 6:
          {
            char_current = char_list__25_00[j];
          }
          break;
          case 7:
          {
            char_current = char_list__18_00[j];
          }
          break;
          case 8:
          {
            char_current = char_list__10_00[j];
          }
          break;
          case 9:
          {
            char_current = char_list___5_00[j];
          }
          break;
          case 10:
          {
            char_current = char_list___2_50[j];
          }
          break;
          case 11:
          {
            char_current = char_list___1_00[j];
          }
          break;
          case 12:
          {
#if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
  || OVERWRITE_SDR_GAMMA == GAMMA_22    \
  || OVERWRITE_SDR_GAMMA == GAMMA_24)
            char_current = char_list___0_25[j];
#else
            char_current = char_list___0_40[j];
#endif
          }
          break;
          default: //case 13:
          {
            char_current = char_list___0_00[j];
          }
          break;
        }

        char_dims.x = char_current != __percent ? waveDat.charDimensions.x
                                                : waveDat.charDimensionXForPercent;

        Waveform::DrawCharToScale(Unrolling_Be_Gone_Int,
                                  char_current,
                                  char_dims,
                                  text_offset_current,
                                  j + char_offset_current);
      }
    }

#endif

    // draw the frame, ticks and horizontal lines
    [loop]
    for (int y = 0; y < (waveDat.endXY.y + Unrolling_Be_Gone_Int); y++)
    {
      int2 curPos = waveDat.offsetToFrame
                  + int2(0, y);

      float curGrey = lerp(0.23f, 0.13f, (float(y + WAVEDAT_CUTOFF_OFFSET) / float(waveDat.endYminus1 + WAVEDAT_CUTOFF_OFFSET)));
      // using gamma 2 as intermediate gamma space
      curGrey = sqrt(curGrey);

      float4 curColour = float4(curGrey, 1.f.xxx);

      // draw top and bottom part of the frame
      [branch]
      if (y <  waveDat.frameSize
       || y >= waveDat.lowerFrameStart)
      {
        [loop]
        for (int x = 0; x < (waveDat.endXY.x + Unrolling_Be_Gone_Int); x++)
        {
          int2 curXPos = int2(curPos.x + x,
                              curPos.y);
          tex2Dstore(StorageWaveformScale, curXPos, curColour);
        }
      }
      // draw left and right part of the frame
      else
      {
        [loop]
        for (int x = 0; x < (waveDat.frameSize + Unrolling_Be_Gone_Int); x++)
        {
          int2 curLeftPos  = int2(curPos.x + x,
                                  curPos.y);
          int2 curRightPos = int2(curLeftPos.x + waveDat.waveformArea.x + waveDat.frameSize, curLeftPos.y);
          tex2Dstore(StorageWaveformScale, curLeftPos,  curColour);
          tex2Dstore(StorageWaveformScale, curRightPos, curColour);
        }
      }

      // draw top tick and bottom tick
      [branch]
#ifdef IS_HDR_CSP
  #ifdef IS_QHD_OR_HIGHER_RES
      if ((WAVEFORM_CUTOFF_POINT == 0 && ((nits10000_00Offset.y - 1) == curPos.y || nits10000_00Offset.y == curPos.y || (nits10000_00Offset.y + 1) == curPos.y))
       || (WAVEFORM_CUTOFF_POINT == 1 && ((nits_4000_00Offset.y - 1) == curPos.y || nits_4000_00Offset.y == curPos.y || (nits_4000_00Offset.y + 1) == curPos.y))
       || (WAVEFORM_CUTOFF_POINT == 2 && ((nits_2000_00Offset.y - 1) == curPos.y || nits_2000_00Offset.y == curPos.y || (nits_2000_00Offset.y + 1) == curPos.y))
       || (WAVEFORM_CUTOFF_POINT == 3 && ((nits_1000_00Offset.y - 1) == curPos.y || nits_1000_00Offset.y == curPos.y || (nits_1000_00Offset.y + 1) == curPos.y))
       || (nits____0_00Offset.y - 1) == curPos.y || nits____0_00Offset.y == curPos.y || (nits____0_00Offset.y + 1) == curPos.y)
  #else
      if ((WAVEFORM_CUTOFF_POINT == 0 && nits10000_00Offset.y == curPos.y)
       || (WAVEFORM_CUTOFF_POINT == 1 && nits_4000_00Offset.y == curPos.y)
       || (WAVEFORM_CUTOFF_POINT == 2 && nits_2000_00Offset.y == curPos.y)
       || (WAVEFORM_CUTOFF_POINT == 3 && nits_1000_00Offset.y == curPos.y)
       || nits____0_00Offset.y == curPos.y)
  #endif
#else
  #ifdef IS_QHD_OR_HIGHER_RES
      if ((nits100_00Offset.y - 1) == curPos.y || nits100_00Offset.y == curPos.y || (nits100_00Offset.y + 1) == curPos.y
       || (nits__0_00Offset.y - 1) == curPos.y || nits__0_00Offset.y == curPos.y || (nits__0_00Offset.y + 1) == curPos.y)
  #else
      if (nits100_00Offset.y == curPos.y
       || nits__0_00Offset.y == curPos.y)
  #endif
#endif
      {
        [loop]
        for (int x = waveDat.tickXOffset; x < (waveDat.tickXOffset + waveDat.frameSize + Unrolling_Be_Gone_Int); x++)
        {
          int2 curTickPos = int2(x,
                                 curPos.y);
          tex2Dstore(StorageWaveformScale, curTickPos, curColour);
        }
      }

      // draw ticks + draw horizontal lines
      [branch]
#ifdef IS_HDR_CSP
  #ifdef IS_QHD_OR_HIGHER_RES
      if ((WAVEFORM_CUTOFF_POINT < 1 && ((nits_4000_00Offset.y - 1) == curPos.y || nits_4000_00Offset.y == curPos.y || (nits_4000_00Offset.y + 1) == curPos.y))
       || (WAVEFORM_CUTOFF_POINT < 2 && ((nits_2000_00Offset.y - 1) == curPos.y || nits_2000_00Offset.y == curPos.y || (nits_2000_00Offset.y + 1) == curPos.y))
       || (WAVEFORM_CUTOFF_POINT < 3 && ((nits_1000_00Offset.y - 1) == curPos.y || nits_1000_00Offset.y == curPos.y || (nits_1000_00Offset.y + 1) == curPos.y))
       || (nits__400_00Offset.y - 1) == curPos.y || nits__400_00Offset.y == curPos.y || (nits__400_00Offset.y + 1) == curPos.y
       || (nits__203_00Offset.y - 1) == curPos.y || nits__203_00Offset.y == curPos.y || (nits__203_00Offset.y + 1) == curPos.y
       || (nits__100_00Offset.y - 1) == curPos.y || nits__100_00Offset.y == curPos.y || (nits__100_00Offset.y + 1) == curPos.y
       || (nits___50_00Offset.y - 1) == curPos.y || nits___50_00Offset.y == curPos.y || (nits___50_00Offset.y + 1) == curPos.y
       || (nits___25_00Offset.y - 1) == curPos.y || nits___25_00Offset.y == curPos.y || (nits___25_00Offset.y + 1) == curPos.y
       || (nits___10_00Offset.y - 1) == curPos.y || nits___10_00Offset.y == curPos.y || (nits___10_00Offset.y + 1) == curPos.y
       || (nits____5_00Offset.y - 1) == curPos.y || nits____5_00Offset.y == curPos.y || (nits____5_00Offset.y + 1) == curPos.y
       || (nits____2_50Offset.y - 1) == curPos.y || nits____2_50Offset.y == curPos.y || (nits____2_50Offset.y + 1) == curPos.y
       || (nits____1_00Offset.y - 1) == curPos.y || nits____1_00Offset.y == curPos.y || (nits____1_00Offset.y + 1) == curPos.y
       || (nits____0_25Offset.y - 1) == curPos.y || nits____0_25Offset.y == curPos.y || (nits____0_25Offset.y + 1) == curPos.y
       || (nits____0_05Offset.y - 1) == curPos.y || nits____0_05Offset.y == curPos.y || (nits____0_05Offset.y + 1) == curPos.y)
  #else
      if ((WAVEFORM_CUTOFF_POINT < 1 && nits_4000_00Offset.y == curPos.y)
       || (WAVEFORM_CUTOFF_POINT < 2 && nits_2000_00Offset.y == curPos.y)
       || (WAVEFORM_CUTOFF_POINT < 3 && nits_1000_00Offset.y == curPos.y)
       || nits__400_00Offset.y == curPos.y
       || nits__203_00Offset.y == curPos.y
       || nits__100_00Offset.y == curPos.y
       || nits___50_00Offset.y == curPos.y
       || nits___25_00Offset.y == curPos.y
       || nits___10_00Offset.y == curPos.y
       || nits____5_00Offset.y == curPos.y
       || nits____2_50Offset.y == curPos.y
       || nits____1_00Offset.y == curPos.y
       || nits____0_25Offset.y == curPos.y
       || nits____0_05Offset.y == curPos.y)
  #endif
#else
  #ifdef IS_QHD_OR_HIGHER_RES
      if ((nits_87_50Offset.y - 1) == curPos.y || nits_87_50Offset.y == curPos.y || (nits_87_50Offset.y + 1) == curPos.y
       || (nits_75_00Offset.y - 1) == curPos.y || nits_75_00Offset.y == curPos.y || (nits_75_00Offset.y + 1) == curPos.y
       || (nits_60_00Offset.y - 1) == curPos.y || nits_60_00Offset.y == curPos.y || (nits_60_00Offset.y + 1) == curPos.y
       || (nits_50_00Offset.y - 1) == curPos.y || nits_50_00Offset.y == curPos.y || (nits_50_00Offset.y + 1) == curPos.y
       || (nits_35_00Offset.y - 1) == curPos.y || nits_35_00Offset.y == curPos.y || (nits_35_00Offset.y + 1) == curPos.y
       || (nits_25_00Offset.y - 1) == curPos.y || nits_25_00Offset.y == curPos.y || (nits_25_00Offset.y + 1) == curPos.y
       || (nits_18_00Offset.y - 1) == curPos.y || nits_18_00Offset.y == curPos.y || (nits_18_00Offset.y + 1) == curPos.y
       || (nits_10_00Offset.y - 1) == curPos.y || nits_10_00Offset.y == curPos.y || (nits_10_00Offset.y + 1) == curPos.y
       || (nits__5_00Offset.y - 1) == curPos.y || nits__5_00Offset.y == curPos.y || (nits__5_00Offset.y + 1) == curPos.y
       || (nits__2_50Offset.y - 1) == curPos.y || nits__2_50Offset.y == curPos.y || (nits__2_50Offset.y + 1) == curPos.y
       || (nits__1_00Offset.y - 1) == curPos.y || nits__1_00Offset.y == curPos.y || (nits__1_00Offset.y + 1) == curPos.y
    #if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
      || OVERWRITE_SDR_GAMMA == GAMMA_22    \
      || OVERWRITE_SDR_GAMMA == GAMMA_24)
       || (nits__0_25Offset.y - 1) == curPos.y || nits__0_25Offset.y == curPos.y || (nits__0_25Offset.y + 1) == curPos.y
    #else
       || (nits__0_40Offset.y - 1) == curPos.y || nits__0_40Offset.y == curPos.y || (nits__0_40Offset.y + 1) == curPos.y
    #endif
      )
  #else
      if (nits_87_50Offset.y == curPos.y
       || nits_75_00Offset.y == curPos.y
       || nits_60_00Offset.y == curPos.y
       || nits_50_00Offset.y == curPos.y
       || nits_35_00Offset.y == curPos.y
       || nits_25_00Offset.y == curPos.y
       || nits_18_00Offset.y == curPos.y
       || nits_10_00Offset.y == curPos.y
       || nits__5_00Offset.y == curPos.y
       || nits__2_50Offset.y == curPos.y
       || nits__1_00Offset.y == curPos.y
    #if (OVERWRITE_SDR_GAMMA == GAMMA_UNSET \
      || OVERWRITE_SDR_GAMMA == GAMMA_22    \
      || OVERWRITE_SDR_GAMMA == GAMMA_24)
       || nits__0_25Offset.y == curPos.y
    #else
       || nits__0_40Offset.y == curPos.y
    #endif
      )
  #endif
#endif
      {
        [loop]
        for (int x = waveDat.tickXOffset; x < (waveDat.tickXOffset + waveDat.endXY.x + Unrolling_Be_Gone_Int); x++)
        {
          int2 curTickPos = int2(x,
                                 curPos.y);
          tex2Dstore(StorageWaveformScale, curTickPos, curColour);
        }
      }
    }

    tex1Dstore(StorageConsolidated, COORDS_WAVEFORM_LAST_SIZE_X,           _WAVEFORM_SIZE.x);
    tex1Dstore(StorageConsolidated, COORDS_WAVEFORM_LAST_SIZE_Y,           _WAVEFORM_SIZE.y);
    tex1Dstore(StorageConsolidated, COORDS_WAVEFORM_LAST_TEXT_SIZE_ADJUST, _WAVEFORM_TEXT_SIZE_ADJUST);
#ifdef IS_HDR_CSP
    tex1Dstore(StorageConsolidated, COORDS_WAVEFORM_LAST_CUTOFF_POINT,     WAVEFORM_CUTOFF_POINT);
#endif
  }

  return;
}


void VS_Prepare_Waveform_Render_Colour
(
  in                  uint   VertexID         : SV_VertexID,
  out                 float4 Position         : SV_Position,
  out nointerpolation float  Waveform_Max_Inv : Waveform_Max_Inv
)
{
  Position.zw = float2(0.f, 1.f);

  const float target_width = _WAVEFORM_SIZE.x <  66.7f ? float(TEXTURE_WAVEFORM_TOTAL_WIDTH / 2)
                           : _WAVEFORM_SIZE.x < 100.f  ? float(TEXTURE_WAVEFORM_TOTAL_WIDTH * 2 / 3)
                           :                             float(TEXTURE_WAVEFORM_TOTAL_WIDTH);

#ifdef IS_HDR_CSP
  const float target_height = _WAVEFORM_SIZE.y < 100.f ? float((TEXTURE_WAVEFORM_HEIGHT + 1) / 2 - 1)
                                                       : float(TEXTURE_WAVEFORM_HEIGHT);
#else
  const float target_height = float(TEXTURE_WAVEFORM_HEIGHT);
#endif

  float2 position_xy = float2(target_width, target_height);

  [flatten]
  if (VertexID == 1u)
  {
    position_xy.x = -position_xy.x;
  }
  else
  [flatten]
  if (VertexID == 2u)
  {
    position_xy.y = -position_xy.y;
  }

  Position.xy = GetPositonCoordsFromRegularCoords(position_xy, float2(TEXTURE_WAVEFORM_TOTAL_WIDTH, TEXTURE_WAVEFORM_HEIGHT));

  Waveform_Max_Inv = 1.f
                   / (float)tex2Dfetch(SamplerMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_WAVEFORM_COUNTER_MAX);

  return;
}

void PS_Waveform_Render_Colour
(
  in                  float4 Position         : SV_Position,
  in  nointerpolation float  Waveform_Max_Inv : Waveform_Max_Inv,
  out                 float4 Out              : SV_Target0
)
{
  Out = (float4)0.f;

  BRANCH()
  if (_SHOW_WAVEFORM)
  {
    const int2 position_as_int = int2(Position.xy);

#ifdef IS_HDR_CSP
    const int waveform_height_int = _WAVEFORM_SIZE.y < 100.f ? int((TEXTURE_WAVEFORM_HEIGHT + 1) / 2 - 1)
                                                             : int(TEXTURE_WAVEFORM_HEIGHT);
#else
    const int waveform_height_int = int(TEXTURE_WAVEFORM_HEIGHT);
#endif

    const float waveform_height_float = float(waveform_height_int);

    const int clamp_fetch_x = _WAVEFORM_SIZE.x <  66.7f ? int(TEXTURE_WAVEFORM_TOTAL_WIDTH / 2 - 1)
                            : _WAVEFORM_SIZE.x < 100.f  ? int(TEXTURE_WAVEFORM_TOTAL_WIDTH * 2 / 3 - 1)
                            :                             int(TEXTURE_WAVEFORM_TOTAL_WIDTH - 1);


    int column_max = 0;
    int column_min = 0;

    int pos_x_left = 0;

    int column_max_left = 0;
    int column_min_left = 0;

    int pos_x_right = 0;

    int column_max_right = 0;
    int column_min_right = 0;

    int column_extra = 0;
    int column_left_right_extra = 0;

    int column_max_extra = 0;
    int column_min_extra = 0;

    int column_max_extra_left = 0;
    int column_min_extra_left = 0;

    int column_max_extra_right = 0;
    int column_min_extra_right = 0;

    BRANCH()
    if (_WAVEFORM_SHOW_MAX_MIN_PER_COLUMN)
    {
      column_max = tex2Dfetch(Sampler_Waveform_Column_Max_Min, int2(position_as_int.x, 0));
      column_min = tex2Dfetch(Sampler_Waveform_Column_Max_Min, int2(position_as_int.x, 1));

      column_min = max(column_min, column_max);

      pos_x_left = clamp(position_as_int.x - 1, 0, clamp_fetch_x);

      column_max_left = tex2Dfetch(Sampler_Waveform_Column_Max_Min, int2(pos_x_left, 0));
      column_min_left = tex2Dfetch(Sampler_Waveform_Column_Max_Min, int2(pos_x_left, 1));

      column_min_left = max(column_min_left, column_max_left);

      pos_x_right = clamp(position_as_int.x + 1, 0, clamp_fetch_x);

      column_max_right = tex2Dfetch(Sampler_Waveform_Column_Max_Min, int2(pos_x_right, 0));
      column_min_right = tex2Dfetch(Sampler_Waveform_Column_Max_Min, int2(pos_x_right, 1));

      column_min_right = max(column_min_right, column_max_right);

#ifdef IS_HDR_CSP
      column_extra            = _WAVEFORM_SIZE.y < 100.f ? 2 : 4;
      column_left_right_extra = _WAVEFORM_SIZE.y < 100.f ? 1 : 2;
#else
      column_extra            = 2;
      column_left_right_extra = 1;
#endif

      column_max_extra = column_max - column_extra;
      column_min_extra = column_min + column_extra;

      column_max_extra_left = column_max_left - column_left_right_extra;
      column_min_extra_left = column_min_left + column_left_right_extra;

      column_max_extra_right = column_max_right - column_left_right_extra;
      column_min_extra_right = column_min_right + column_left_right_extra;
    }


    int  waveform_div_3 = -100;
    bool is_red_part    = false;
    bool is_green_part  = false;

    int2 max_min_value_pos = -100;

    BRANCH()
    if (_WAVEFORM_MODE != WAVEFORM_MODE_RGB_INDIVIDUALLY)
    {
      // for max CLL it needs to be max of min R/G/B CLL...
      max_min_value_pos = _WAVEFORM_MODE == WAVEFORM_MODE_LUMINANCE
                        ? int2(COORDS_MAX_NITS_VALUE, COORDS_MIN_NITS_VALUE)
                        : int2(COORDS_MAX_CLL_VALUE,  COORDS_MIN_CLL_VALUE);
    }
    else
    {
      waveform_div_3 = _WAVEFORM_SIZE.x <  66.7f ? int(TEXTURE_WAVEFORM_TOTAL_WIDTH / 6)
                     : _WAVEFORM_SIZE.x < 100.f  ? int(TEXTURE_WAVEFORM_TOTAL_WIDTH * 2 / 9)
                     :                             int(TEXTURE_WAVEFORM_TOTAL_WIDTH / 3);

      is_red_part   = position_as_int.x < waveform_div_3;
      is_green_part = position_as_int.x < waveform_div_3 * 2;

      [flatten]
      if (is_red_part)
      {
        max_min_value_pos = int2(COORDS_MAX_R_VALUE, COORDS_MIN_R_VALUE);
      }
      else
      [flatten]
      if (is_green_part)
      {
        max_min_value_pos = int2(COORDS_MAX_G_VALUE, COORDS_MIN_G_VALUE);
      }
      else
      {
        max_min_value_pos = int2(COORDS_MAX_B_VALUE, COORDS_MIN_B_VALUE);
      }
    }

    const float max_value = tex1Dfetch(SamplerConsolidated, max_min_value_pos[0]);
    const float min_value = tex1Dfetch(SamplerConsolidated, max_min_value_pos[1]);

#ifdef IS_HDR_CSP
    const float max_value_encoded = Csp::Trc::Nits_To::PQ(max_value);
    const float min_value_encoded = Csp::Trc::Nits_To::PQ(min_value);
#else
    const float max_value_encoded = ENCODE_SDR(max_value / DIV_100);
    const float min_value_encoded = ENCODE_SDR(min_value / DIV_100);
#endif

#ifdef IS_HDR_CSP
    const int max_min_extra = _WAVEFORM_SIZE.y < 100.f ? 1 : 2;
#else
    const int max_min_extra = 1;
#endif

    const int max_value_coord_y = waveform_height_int
                                - int(max_value_encoded * waveform_height_float + 0.5f);

    const int min_value_coord_y = waveform_height_int
                                - int(min_value_encoded * waveform_height_float + 0.5f);

    const int max_value_extra = max_value_coord_y - max_min_extra;
    const int min_value_extra = min_value_coord_y + max_min_extra;

    BRANCH()
    if ((_WAVEFORM_SHOW_MAX_NITS_LINE && position_as_int.y < max_value_coord_y && position_as_int.y >= max_value_extra)
     || (_WAVEFORM_SHOW_MIN_NITS_LINE && position_as_int.y > min_value_coord_y && position_as_int.y <= min_value_extra))
    {
      Out = (float4)1.f;

#ifdef IS_HDR_CSP

#define WAVEFORM_HDR_ENCODING(x) \
          (log2(x * 10000.f + 1.f) / log2(10001.f))

#define WAVEFORM_HDR_DECODING(x) \
          ((exp2(x * log2(10001.f)) - 1.f) / 10000.f)

      FLATTEN()
      if (_WAVEFORM_MODE == WAVEFORM_MODE_RGB_COMBINED
       || _WAVEFORM_MODE == WAVEFORM_MODE_RGB_INDIVIDUALLY)
      {
        Out.rgb = WAVEFORM_HDR_ENCODING(Csp::Mat::BT709_To_XYZ[1].g).xxx;
      }

#endif //IS_HDR_CSP
    }
    else
    BRANCH()
    if (_WAVEFORM_SHOW_MAX_MIN_PER_COLUMN
     && (((column_max_left < waveform_height_int || column_max_right < waveform_height_int)
       && (( _WAVEFORM_SHOW_MAX_NITS_LINE && position_as_int.y >= max_value_coord_y)
        || (!_WAVEFORM_SHOW_MAX_NITS_LINE && position_as_int.y >= (max_value_coord_y - column_extra)))
       && position_as_int.y < column_max && (position_as_int.y >= column_max_extra
                                          || position_as_int.y >= column_max_extra_left
                                          || position_as_int.y >= column_max_extra_right))
      || ((( _WAVEFORM_SHOW_MIN_NITS_LINE && position_as_int.y <= min_value_coord_y)
        || (!_WAVEFORM_SHOW_MIN_NITS_LINE && position_as_int.y <= (min_value_coord_y + column_extra)))
       && position_as_int.y > column_min && (position_as_int.y <= column_min_extra
                                          || position_as_int.y <= column_min_extra_left
                                          || position_as_int.y <= column_min_extra_right))))
    {
      float grey_out = WAVEFORM_MAX_MIN_PER_ROW_BRIGHTNESS_PERCENTAGE / DIV_100;

      BRANCH()
      if (_WAVEFORM_MODE == WAVEFORM_MODE_RGB_COMBINED
       || _WAVEFORM_MODE == WAVEFORM_MODE_RGB_INDIVIDUALLY)
      {
        grey_out *= Csp::Mat::BT709_To_XYZ[1].g;
      }

#ifdef IS_HDR_CSP
      grey_out = WAVEFORM_HDR_ENCODING(grey_out);
#else
      grey_out = ENCODE_SDR(grey_out);
#endif

      Out = float4(grey_out.xxx, 1.f);
    }
    else
    {
      BRANCH()
      if (_WAVEFORM_MODE == WAVEFORM_MODE_LUMINANCE
       || _WAVEFORM_MODE == WAVEFORM_MODE_MAX_CLL)
      {
#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED
        const int3 fetch_pos = int3(position_as_int, 0);
#else
        const uint position_x_uint = uint(position_as_int.x);

        const int3 fetch_pos = int3(position_x_uint % uint(TEXTURE_WAVEFORM_WIDTH),
                                    position_as_int.y,
                                    position_x_uint / uint(TEXTURE_WAVEFORM_WIDTH));
#endif

        uint waveform_current = tex3Dfetch(SamplerWaveform, fetch_pos);

        [branch]
        if (waveform_current > 0u)
        {
          float waveform_luminance = float(waveform_current)
                                   * Waveform_Max_Inv;

          waveform_luminance = pow(waveform_luminance, 1.f / 1.5f);

          float waveform_colour_luminance_encoded = float(waveform_height_int - position_as_int.y)
                                                  / waveform_height_float;

#ifdef IS_HDR_CSP
          float waveform_colour_luminance = Csp::Trc::PQ_To::Linear(waveform_colour_luminance_encoded)
                                          * 10000.f;
#else
          float waveform_colour_luminance = DECODE_SDR(waveform_colour_luminance_encoded)
                                          * 100.f;
#endif

          float3 waveform_colour = WaveformRgbValues(waveform_colour_luminance);

          waveform_colour *= waveform_luminance
                           / dot(waveform_colour, Csp::Mat::BT709_To_XYZ[1]);

          static const float plus_value = (1.f / float(255u * 255u)) * 1.05f;

          waveform_colour += plus_value;

          float3 yrb;

          yrb = float3(dot(waveform_colour, Csp::Mat::BT709_To_XYZ[1]), waveform_colour.rb);

#ifdef IS_HDR_CSP
          yrb = WAVEFORM_HDR_ENCODING(yrb);
#else
          yrb = ENCODE_SDR(yrb);
#endif

          Out = float4(yrb, 1.f);
        }
      }
      else
      BRANCH()
      if (_WAVEFORM_MODE == WAVEFORM_MODE_RGB_COMBINED)
      {
#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED
        const int  fetch_pos_x = position_as_int.x;
        const int  fetch_pos_y = position_as_int.y;
        const int3 fetch_pos_z = int3(0, 1, 2);
#else
        const uint position_x_uint = uint(position_as_int.x);

        const int  fetch_pos_x = int(position_x_uint % uint(TEXTURE_WAVEFORM_WIDTH));
        const int  fetch_pos_y = position_as_int.y;
        const int3 fetch_pos_z = int3(0, 1, 2)
                               + int((position_x_uint / uint(TEXTURE_WAVEFORM_WIDTH)) * 3u);
#endif

        uint3 waveform_current;

        waveform_current.r = tex3Dfetch(SamplerWaveform, int3(fetch_pos_x, fetch_pos_y, fetch_pos_z.r));
        waveform_current.g = tex3Dfetch(SamplerWaveform, int3(fetch_pos_x, fetch_pos_y, fetch_pos_z.g));
        waveform_current.b = tex3Dfetch(SamplerWaveform, int3(fetch_pos_x, fetch_pos_y, fetch_pos_z.b));

        const bool3 waveform_current_above_0 = waveform_current.rgb > 0u;

        [branch]
        if (any(waveform_current_above_0))
        {
          float3 waveform_colour = float3(waveform_current.rgb)
                                 * Waveform_Max_Inv;

          float waveform_luminance = dot(Csp::Mat::BT709_To_XYZ[1], waveform_colour);

          waveform_colour *= pow(waveform_luminance, 1.f / 1.5f)
                           / waveform_luminance;

          static const float3 plus_values = (1.f / float(255u * 255u)) * 1.05f;

          waveform_colour += waveform_current_above_0 ? plus_values
                                                      : 0.f;

          waveform_colour = saturate(waveform_colour);

          float3 yrb;

          yrb = float3(dot(waveform_colour, Csp::Mat::BT709_To_XYZ[1]), waveform_colour.rb);

#ifdef IS_HDR_CSP
          yrb = WAVEFORM_HDR_ENCODING(yrb);
#else
          yrb = ENCODE_SDR(yrb);
#endif

          Out = float4(yrb, 1.f);
        }
      }
      else //if (_WAVEFORM_MODE == WAVEFORM_MODE_RGB_INDIVIDUALLY)
      {
#ifndef TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

        const int3 fetch_pos = int3(position_as_int, 0);

#else //TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

        const uint position_x_uint = uint(position_as_int.x);

        const int3 fetch_pos = int3(position_x_uint % uint(TEXTURE_WAVEFORM_WIDTH),
                                    position_as_int.y,
                                    position_x_uint / uint(TEXTURE_WAVEFORM_WIDTH));

#endif //TEXTURE_WAVEFORM_DEPTH_STACKING_NEEDED

        uint waveform_current = tex3Dfetch(SamplerWaveform, fetch_pos);

        [branch]
        if (waveform_current > 0u)
        {
          float waveform_colour_channel = float(waveform_current)
                                        * Waveform_Max_Inv;

          waveform_colour_channel = pow(waveform_colour_channel, 1.f / 1.5f);

          static const float plus_value = (1.f / float(255u * 255u)) * 1.05f;

          waveform_colour_channel += plus_value;

          float3 waveform_colour = (float3)0.f;
          [branch]
          if (is_red_part)
          {
            waveform_colour.r  = waveform_colour_channel;
            waveform_colour.r *= Csp::Mat::BT709_To_XYZ[1][1] / Csp::Mat::BT709_To_XYZ[1][0];
            waveform_colour.g  = (waveform_colour.r - 1.f) * (Csp::Mat::BT709_To_XYZ[1][0] / Csp::Mat::BT709_To_XYZ[1][1]);
            waveform_colour    = saturate(waveform_colour);
          }
          else
          [branch]
          if (is_green_part)
          {
            waveform_colour.g = waveform_colour_channel;
          }
          else
          {
            waveform_colour.b  = waveform_colour_channel;
            waveform_colour.b *= Csp::Mat::BT709_To_XYZ[1][1] / Csp::Mat::BT709_To_XYZ[1][2];
            waveform_colour.g  = (waveform_colour.b - 1.f) * (Csp::Mat::BT709_To_XYZ[1][2] / Csp::Mat::BT709_To_XYZ[1][1]);
            waveform_colour    = saturate(waveform_colour);
          }

          float3 yrb;

          yrb = float3(dot(Csp::Mat::BT709_To_XYZ[1], waveform_colour), waveform_colour.rb);

#ifdef IS_HDR_CSP
          yrb = WAVEFORM_HDR_ENCODING(yrb);
#else
          yrb = ENCODE_SDR(yrb);
#endif

          Out = float4(yrb, 1.f);
        }
      }
    }

    return;
  }
  else
  {
    discard;
  }
}


void CS_Clear_Texture_Waveform
(
  uint3 DTID : SV_DispatchThreadID
)
{
  BRANCH()
  if (_SHOW_WAVEFORM)
  {
    tex3Dstore(StorageWaveform, DTID, 0u);
  }

  return;
}


void PS_Reset_Texture_Waveform_Column_Max
(
  in  float4 Position : SV_Position,
  out uint   Out      : SV_Target0
)
{
  Out = Position.y < 1.f ? uint(INT_MAX)
                         : 0u;
}
