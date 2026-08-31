#pragma once


//#ifndef IGNORE_NEAR_BLACK_VALUES_FOR_GAMUT_DETECTION
  #define IGNORE_NEAR_BLACK_VALUES_FOR_GAMUT_DETECTION NO
//#endif


#ifdef IS_HDR_CSP

texture2D TextureGamuts
<
  pooled = true;
>
{
  Width  = BUFFER_WIDTH;
  Height = BUFFER_HEIGHT;

  Format = R8;
};

sampler2D<float> SamplerGamuts
{
  Texture = TextureGamuts;
};


void FinaliseGamutCounter()
{

#if defined(IS_COMPUTE_CAPABLE_API)

  uint counterBt709   = tex2Dfetch(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_BT709_PERCENTAGE);
  uint counterDciP3   = tex2Dfetch(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_DCIP3_PERCENTAGE);
  uint counterBt2020  = tex2Dfetch(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_BT2020_PERCENTAGE);

#if defined(IS_FLOAT_HDR_CSP)

  uint counterAp0     = tex2Dfetch(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_AP0_PERCENTAGE);
  uint counterInvalid = tex2Dfetch(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_INVALID_PERCENTAGE);

#endif //IS_FLOAT_HDR_CSP

    float percentageBt709   = float(counterBt709)   / PIXEL_COUNT_FLOAT * TIMES_100;
    float percentageDciP3   = float(counterDciP3)   / PIXEL_COUNT_FLOAT * TIMES_100;
    float percentageBt2020  = float(counterBt2020)  / PIXEL_COUNT_FLOAT * TIMES_100;

#if defined(IS_FLOAT_HDR_CSP)

    float percentageAp0     = float(counterAp0)     / PIXEL_COUNT_FLOAT * TIMES_100;
    float percentageInvalid = float(counterInvalid) / PIXEL_COUNT_FLOAT * TIMES_100;

#endif //IS_FLOAT_HDR_CSP

  tex1Dstore(StorageConsolidated, COORDS_PERCENTAGE_BT709,   percentageBt709);
  tex1Dstore(StorageConsolidated, COORDS_PERCENTAGE_DCI_P3,  percentageDciP3);
  tex1Dstore(StorageConsolidated, COORDS_PERCENTAGE_BT2020,  percentageBt2020);

#if defined(IS_FLOAT_HDR_CSP)

  tex1Dstore(StorageConsolidated, COORDS_PERCENTAGE_AP0,     percentageAp0);
  tex1Dstore(StorageConsolidated, COORDS_PERCENTAGE_INVALID, percentageInvalid);

#endif //IS_FLOAT_HDR_CSP
#endif //IS_COMPUTE_CAPABLE_API

  return;
}


bool IsGamut
(
  const float3 Rgb
)
{
  if (all(Rgb >= 0.f))
  {
    return true;
  }
  else
  {
    return false;
  }
}

#define IS_GAMUT_BT709   0
#define IS_GAMUT_DCI_P3  1
#define IS_GAMUT_BT2020  2
#define IS_GAMUT_AP0     3
#define IS_GAMUT_INVALID 4


#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  #define _IS_GAMUT_BT709(Rgb)  Rgb
  #define _IS_GAMUT_DCI_P3(Rgb) Csp::Mat::BT709_To::DCIP3(Rgb)
  #define _IS_GAMUT_BT2020(Rgb) Csp::Mat::BT709_To::BT2020(Rgb)
  #define _IS_GAMUT_AP0(Rgb)    Csp::Mat::BT709_To::AP0D65(Rgb)

#elif (defined(IS_HDR10_LIKE_CSP) \
    || ACTUAL_COLOUR_SPACE == CSP_BT2020_EXTENDED)

  #define _IS_GAMUT_BT709(Rgb)  Csp::Mat::BT2020_To::BT709(Rgb)
  #define _IS_GAMUT_DCI_P3(Rgb) Csp::Mat::BT2020_To::DCIP3(Rgb)
  #define _IS_GAMUT_BT2020(Rgb) Rgb
  #define _IS_GAMUT_AP0(Rgb)    Csp::Mat::BT2020_To::AP0D65(Rgb)

#endif


float GetGamut
(
  const float3 Rgb
)
{
  [branch]
  if (IsGamut(_IS_GAMUT_BT709(Rgb)))
  {
    return IS_GAMUT_BT709;
  }
  else
  [branch]
  if (IsGamut(_IS_GAMUT_DCI_P3(Rgb)))
  {
    return IS_GAMUT_DCI_P3 / 254.f; // /254 for safety
  }

#if defined(IS_HDR10_LIKE_CSP)

  else
  {
    return IS_GAMUT_BT2020 / 254.f; // /254 for safety
  }

#else

  else
  [branch]
  if (IsGamut(_IS_GAMUT_BT2020(Rgb)))
  {
    return IS_GAMUT_BT2020 / 254.f; // /254 for safety
  }
  else
  [branch]
  if (IsGamut(_IS_GAMUT_AP0(Rgb)))
  {
    return IS_GAMUT_AP0 / 254.f; // /254 for safety
  }
  else
  {
    return IS_GAMUT_INVALID / 254.f; // /254 for safety
  }

#endif //IS_HDR10_LIKE_CSP

  return IS_GAMUT_INVALID / 254.f; // /254 for safety
}


void PS_CalcGamuts
(
      float4 Position : SV_Position,
  out float  CurGamut : SV_Target0
)
{
  CurGamut = 0.f;

  BRANCH()
  if (SHOW_GAMUTS
   || SHOW_GAMUT_FROM_CURSOR
   || SHOW_GAMUT_MAP)
  {
    const float3 pixel = tex2Dfetch(SamplerBackBuffer, int2(Position.xy)).rgb;

#if defined(IS_FLOAT_HDR_CSP)

#if (IGNORE_NEAR_BLACK_VALUES_FOR_GAMUT_DETECTION == YES)

    const float3 absPixel = abs(pixel);
    if (absPixel.r > SMALLEST_FP16
     && absPixel.g > SMALLEST_FP16
     && absPixel.b > SMALLEST_FP16)
    {
      CurGamut = GetGamut(pixel);
    }
    else
    {
      CurGamut = IS_GAMUT_BT709;
    }
    return;

#else

    CurGamut = GetGamut(pixel);

    return;

#endif

#elif defined(IS_HDR10_LIKE_CSP)

#if (IGNORE_NEAR_BLACK_VALUES_FOR_GAMUT_DETECTION == YES)

    if (pixel.r > SMALLEST_UINT10
     && pixel.g > SMALLEST_UINT10
     && pixel.b > SMALLEST_UINT10)
    {
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
    #ifdef IS_COMPUTE_CAPABLE_API
      const float3 curPixel = FetchFromHdr10ToLinearLUT(pixel);
    #else
      const float3 curPixel = Csp::Trc::PQ_To::Linear(pixel);
    #endif
#endif
      CurGamut = GetGamut(curPixel);
    }
    else
    {
      CurGamut = IS_GAMUT_BT709;
    }
    return;

#else

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
  #ifdef IS_COMPUTE_CAPABLE_API
    const float3 curPixel = FetchFromHdr10ToLinearLUT(pixel);
  #else
    const float3 curPixel = Csp::Trc::PQ_To::Linear(pixel);
  #endif
#endif
    CurGamut = GetGamut(curPixel);

    return;

#endif

#else

    CurGamut = IS_GAMUT_INVALID / 254.f; // /254 for safety

    return;

#endif
  }
  discard;
}


#ifdef IS_COMPUTE_CAPABLE_API

#if (BUFFER_WIDTH  % WAVE_SIZE_6_X == 0  \
  && BUFFER_HEIGHT % WAVE_SIZE_6_Y == 0)
  #define GAMUT_COUNTER_THREAD 6
#elif (BUFFER_WIDTH  % WAVE_SIZE_5_X == 0  \
    && BUFFER_HEIGHT % WAVE_SIZE_5_Y == 0)
  #define GAMUT_COUNTER_THREAD 5
#elif (BUFFER_WIDTH  % WAVE_SIZE_4_X == 0  \
    && BUFFER_HEIGHT % WAVE_SIZE_4_Y == 0)
  #define GAMUT_COUNTER_THREAD 4
#elif (BUFFER_WIDTH  % WAVE_SIZE_3_X == 0  \
    && BUFFER_HEIGHT % WAVE_SIZE_3_Y == 0)
  #define GAMUT_COUNTER_THREAD 3
#else
  #define GAMUT_COUNTER_THREAD 2
#endif

#define GAMUT_COUNTER_THREAD_SIZE (GAMUT_COUNTER_THREAD * GAMUT_COUNTER_THREAD)

#define GAMUT_COUNTER_GROUP_PIXELS_X (GAMUT_COUNTER_THREAD * WAVE64_THREAD_SIZE_X)
#define GAMUT_COUNTER_GROUP_PIXELS_Y (GAMUT_COUNTER_THREAD * WAVE64_THREAD_SIZE_Y)

#if (BUFFER_WIDTH % GAMUT_COUNTER_GROUP_PIXELS_X == 0)
  #define GAMUT_COUNTER_DISPATCH_X (BUFFER_WIDTH / GAMUT_COUNTER_GROUP_PIXELS_X)
#else
  #define GAMUT_COUNTER_FETCH_X_NEEDS_CLAMPING
  #define GAMUT_COUNTER_DISPATCH_X (BUFFER_WIDTH / GAMUT_COUNTER_GROUP_PIXELS_X + 1)
#endif

#if (BUFFER_HEIGHT % GAMUT_COUNTER_GROUP_PIXELS_Y == 0)
  #define GAMUT_COUNTER_DISPATCH_Y (BUFFER_HEIGHT / GAMUT_COUNTER_GROUP_PIXELS_Y)
#else
  #define GAMUT_COUNTER_FETCH_Y_NEEDS_CLAMPING
  #define GAMUT_COUNTER_DISPATCH_Y (BUFFER_HEIGHT / GAMUT_COUNTER_GROUP_PIXELS_Y + 1)
#endif


groupshared uint GroupBt709;
groupshared uint GroupDciP3;
groupshared uint GroupBt2020;
#if defined(IS_FLOAT_HDR_CSP)
groupshared uint GroupAp0;
groupshared uint GroupInvalid;
#endif
void CS_CountGamuts
(
  uint3 GTID : SV_GroupThreadID,
  uint3 DTID : SV_DispatchThreadID
)
{
  BRANCH()
  if (SHOW_GAMUTS)
  {

    if (all(GTID.xy == 0))
    {
      GroupBt709   = 0;
      GroupDciP3   = 0;
      GroupBt2020  = 0;
#if defined(IS_FLOAT_HDR_CSP)
      GroupAp0     = 0;
      GroupInvalid = 0;
#endif
    }
    barrier();

#if defined(IS_FLOAT_HDR_CSP)
    uint counter[5] = {0,0,0,0,0};
#else
    uint counter[3] = {0,0,0};
#endif

    int2 curThreadPos = DTID.xy * GAMUT_COUNTER_THREAD;

    [loop]
    for (int x = 0; x < GAMUT_COUNTER_THREAD; x++)
    {
      [loop]
      for (int y = 0; y < GAMUT_COUNTER_THREAD; y++)
      {
        int2 curFetchPos = curThreadPos + int2(x, y);

        uint curGamut = uint(tex2Dfetch(SamplerGamuts, curFetchPos) * 256.f); // *256 for safety

#if (defined(GAMUT_COUNTER_FETCH_X_NEEDS_CLAMPING)  \
  && defined(GAMUT_COUNTER_FETCH_Y_NEEDS_CLAMPING))

        [branch]
        if (curFetchPos.x < BUFFER_WIDTH_INT
         && curFetchPos.y < BUFFER_HEIGHT_INT)

#elif (defined(GAMUT_COUNTER_FETCH_X_NEEDS_CLAMPING)  \
    || defined(GAMUT_COUNTER_FETCH_Y_NEEDS_CLAMPING))

  #if defined(GAMUT_COUNTER_FETCH_X_NEEDS_CLAMPING)

        [branch]
        if (curFetchPos.x < BUFFER_WIDTH_INT)

  #else //defined(GAMUT_COUNTER_FETCH_Y_NEEDS_CLAMPING)

        [branch]
        if (curFetchPos.y < BUFFER_HEIGHT_INT)

  #endif

#endif
        {
          counter[curGamut]++;
        }
      }
    }

//    const uint groupGamutCounterId = (DTID.x - (GID.x * 8)) | ((DTID.y - (GID.y * 8)) << 3);
    atomicAdd(GroupBt709,   counter[0]);
    atomicAdd(GroupDciP3,   counter[1]);
    atomicAdd(GroupBt2020,  counter[2]);
#if defined(IS_FLOAT_HDR_CSP)
    atomicAdd(GroupAp0,     counter[3]);
    atomicAdd(GroupInvalid, counter[4]);
#endif

    barrier();

    if (all(GTID.xy == 0))
    {
      atomicAdd(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_BT709_PERCENTAGE,   GroupBt709);
      atomicAdd(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_DCIP3_PERCENTAGE,   GroupDciP3);
      atomicAdd(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_BT2020_PERCENTAGE,  GroupBt2020);
#if defined(IS_FLOAT_HDR_CSP)
      atomicAdd(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_AP0_PERCENTAGE,     GroupAp0);
      atomicAdd(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_INVALID_PERCENTAGE, GroupInvalid);
#endif
    }
  }
}

#else //IS_COMPUTE_CAPABLE_API

void PS_CountGamuts
(
  in  float4 Position : SV_Position,
  out float4 Output   : SV_Target0
)
{
  const uint2 id = uint2(Position.xy);

  const uint2 arrayId = id - 2;

#ifdef IS_FLOAT_HDR_CSP
  uint gamutCounter[5] = {0, 0, 0, 0, 0};
#else
  uint gamutCounter[3] = {0, 0, 0};
#endif

  [loop]
  for (int x = 0; x < INTERMEDIATE_X[arrayId.x]; x++)
  {
    [loop]
    for (int y = 0; y < INTERMEDIATE_Y[arrayId.y]; y++)
    {
      int2 xy = int2(x + INTERMEDIATE_X_0 * id.x,
                     y + INTERMEDIATE_Y_0 * id.y);

      uint curGamut = uint(tex2Dfetch(SamplerGamuts, xy) * 256.f); // *256 for safety

      gamutCounter[curGamut]++;
    }
  }

#ifdef IS_FLOAT_HDR_CSP

  Output = float4(float(gamutCounter[0]),
                  float(gamutCounter[1]),
                  float(gamutCounter[2]),
                  float(gamutCounter[3]));

#else

  Output = float4(float(gamutCounter[0]),
                  float(gamutCounter[1]),
                  float(gamutCounter[2]),
                  1.f);

#endif
}


void VS_PrepareFinaliseCountGamuts
(
  in  uint   VertexID : SV_VertexID,
  out float4 Position : SV_Position
)
{
  static const float positions[2] =
  {
    GetPositonXCoordFromRegularXCoord(COORDS_PERCENTAGE_BT709),
#ifdef IS_FLOAT_HDR_CSP
    GetPositonXCoordFromRegularXCoord(COORDS_PERCENTAGE_AP0 + 1)
#else
    GetPositonXCoordFromRegularXCoord(COORDS_PERCENTAGE_BT2020 + 1)
#endif
  };

  Position = float4(positions[VertexID], 0.f, 0.f, 1.f);

  return;
}

void PS_FinaliseCountGamuts
(
  in  float4 Position : SV_Position,
  out float  Output   : SV_Target0
)
{
  const uint id = uint(Position.x - COORDS_PERCENTAGE_BT709);

  uint gamutCounter = 0;

  [loop]
  for (int x = 0; x < TEXTURE_INTERMEDIATE_WIDTH; x++)
  {
    [loop]
    for (int y = 0; y < TEXTURE_INTERMEDIATE_HEIGHT; y++)
    {
      uint4 curGamuts = tex2Dfetch(SamplerIntermediate, int2(x, y));

      gamutCounter += curGamuts[id];
    }
  }

  Output = float(gamutCounter) / PIXEL_COUNT_FLOAT * TIMES_100;
}

#endif //IS_COMPUTE_CAPABLE_API

float3 CreateGamutMap
(
  const uint  Gamut,
        float Y
)
//  float WhitePoint)
{
  if (SHOW_GAMUT_MAP)
  {
    float3 output;

    if (Gamut != IS_GAMUT_BT709)
    {
      Y += 20.f;
    }

    switch(Gamut)
    {
      case IS_GAMUT_BT709:
      {
        // shades of grey
        float clamped = Y * 0.25f;
        output = float3(clamped,
                        clamped,
                        clamped);
      } break;
      case IS_GAMUT_DCI_P3:
      {
        // yellow
        output = float3(Y,
                        Y,
                        0.f);
      } break;
#if defined(IS_HDR10_LIKE_CSP)
      default:
#elif defined(IS_FLOAT_HDR_CSP)
      case IS_GAMUT_BT2020:
#endif
      {
        // blue
        output = float3(0.f,
                        0.f,
                        Y);
      } break;
#if defined(IS_FLOAT_HDR_CSP)
      case IS_GAMUT_AP0:
      {
        // red
        output = float3(Y,
                        0.f,
                        0.f);
      } break;
      default: // invalid
      {
        // pink
        output = float3(Y,
                        0.f,
                        Y);
      } break;
#endif
    }

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

    output /= 80.f;

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

    output = Csp::Trc::Nits_To::PQ(Csp::Mat::BT709_To::BT2020(output));

#elif (ACTUAL_COLOUR_SPACE == CSP_BT2020_EXTENDED)

    output = Csp::Mat::BT709_To::BT2020(output / 100.f);

#endif

    return output;
  }
}

#endif //IS_HDR_CSP
