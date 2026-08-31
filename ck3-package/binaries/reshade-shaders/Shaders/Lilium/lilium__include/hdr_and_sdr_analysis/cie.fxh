#pragma once


#include "cie_datasets.fxh"


#define CIE_TEXTURE_ENTRY_DIAGRAM_COLOUR   0
#define CIE_TEXTURE_ENTRY_DIAGRAM_BLACK_BG 1
#define CIE_TEXTURE_ENTRY_BT709_OUTLINE    2
#define CIE_TEXTURE_ENTRY_DCI_P3_OUTLINE   3
#define CIE_TEXTURE_ENTRY_BT2020_OUTLINE   4
#define CIE_TEXTURE_ENTRY_AP0_OUTLINE      5

//width and height description are in lilium__hdr_and_sdr_analysis.fx

texture3D TextureCieCounter
<
  pooled = true;
>
{
  Width  = CIE_TEXTURE_WIDTH_UINT;
  Height = CIE_TEXTURE_HEIGHT;
  Depth  = 16;
  Format = R32U;
};

sampler3D<uint> SamplerCieCounter
{
  Texture = TextureCieCounter;
};

storage3D<uint> StorageCieCounter
{
  Texture = TextureCieCounter;
};

texture2D TextureCieOverlay
{
  Width  = CIE_TEXTURE_WIDTH_UINT;
  Height = CIE_TEXTURE_HEIGHT;
  Format = RG8;
};

sampler2D<float4> SamplerCieOverlay
{
  Texture = TextureCieOverlay;
};

storage2D<float4> StorageCieOverlay
{
  Texture = TextureCieOverlay;
};

texture2D TextureCieFinal
<
  pooled = true;
>
{
  Width  = CIE_TEXTURE_WIDTH_UINT;
  Height = CIE_TEXTURE_HEIGHT;
  Format = RGBA8;
};

sampler2D<float4> SamplerCieFinal
{
  Texture = TextureCieFinal;
};

storage2D<float4> StorageCieFinal
{
  Texture = TextureCieFinal;
};


float GetCieDragramSizeMultiplier()
{
  return clamp(_CIE_DIAGRAM_SIZE / 100.f, 0.f, 1.f);
}

int2 GetCieDiagramRenderSize()
{
  const float2 cieSize = _CIE_DIAGRAM_TYPE == CIE_1931 ? CIE_XY_SIZE_FLOAT
                                                       : CIE_UV_SIZE_FLOAT;

  const float cieDiagramSizeMultiplier = GetCieDragramSizeMultiplier();

  return int2(cieSize * cieDiagramSizeMultiplier);
}

float2 GetCieDiagramRenderSizeMinus1()
{
  const float2 cieSize = _CIE_DIAGRAM_TYPE == CIE_1931 ? CIE_XY_SIZE_FLOAT
                                                       : CIE_UV_SIZE_FLOAT;

  const float cieDiagramSizeMultiplier = GetCieDragramSizeMultiplier();

  return floor(cieSize * cieDiagramSizeMultiplier - 0.5f);
}

float2 GetCieDiagramRenderSizeMinus1
(
  const float2 CieSize
)
{
  const float cieDiagramSizeMultiplier = GetCieDragramSizeMultiplier();

  return floor(CieSize * cieDiagramSizeMultiplier - 0.5f);
}

float2 DecodeCieDiagramCoords
(
  const float2 EncodedCoords,
  const float  RenderSizeMinus1Y,
  const float2 OneDivRenderSizeMinus1
)
{
  float decodedCoordsY = RenderSizeMinus1Y - EncodedCoords.y;

  float2 decodedCoords = float2(EncodedCoords.x, decodedCoordsY)
                       * OneDivRenderSizeMinus1;

  return decodedCoords;
}

float2 ConvertDecodedCieDiagramCoordsToxy
(
  const float2 DecodedCoords
)
{
  return DecodedCoords * CIE_XY_NORMALISE + CIE_XY_MIN_EXTRA;
}

float2 ConvertDecodedCieDiagramCoordsTouv
(
  const float2 DecodedCoords
)
{
  return DecodedCoords * CIE_UV_NORMALISE + CIE_UV_MIN_EXTRA;
}


float2 GetVector
(
  const float2 xy0,
  const float2 xy1
)
{
  return xy1 - xy0;
}

float GetM
(
  const float2 xy0,
  const float2 xy1
)
{
  float num = xy1.y - xy0.y;
  float den = xy1.x - xy0.x;

  if (den == 0.f)
  {
    return num;
  }
  else
  {
    return num
         / den;
  }
}

float GetB
(
  const float2 xy0,
  const float  m
)
{
  return xy0.y - m * xy0.x;
}

void Draw_Cie_Lines
(
  const float  Unrolling_Be_Gone_Float,
  const float2 Cie_Min_Extra,
  const float2 Cie_Normalise,
  const float2 Render_Size_Minus_1,
  const float2 Start,
  const float2 Stop
)
{
  const float2 xy_diff = abs(Start - Stop);

  const bool x_diff_is_greater = xy_diff.x > xy_diff.y;

  float4 start_stop;

  [flatten]
  if (x_diff_is_greater)
  {
    start_stop = Start.x < Stop.x ? float4(Start, Stop) : float4(Stop, Start);
  }
  else
  {
    start_stop = Start.y < Stop.y ? float4(Start, Stop) : float4(Stop, Start);
  }

  const float2 start_encoded = ((start_stop.xy + (-Cie_Min_Extra)) / Cie_Normalise) * Render_Size_Minus_1;
  const float2  stop_encoded = ((start_stop.zw + (-Cie_Min_Extra)) / Cie_Normalise) * Render_Size_Minus_1;


  const float m = GetM(start_encoded, stop_encoded);
  const float b = GetB(start_encoded, m);


  const float2 start_encoded_floor = floor(start_encoded);
  const float2 stop_encoded_ceil   =  ceil(stop_encoded);


  #define PLUS_MINUS 2.f

  [branch]
  if (x_diff_is_greater)
  {
    [loop]
    for (float x = start_encoded_floor.x - PLUS_MINUS; x <= (stop_encoded_ceil.x + PLUS_MINUS + Unrolling_Be_Gone_Float); x = x + 1.f)
    {
      float y_correct = m * x + b;
      float y_floor   = floor(y_correct);
      float y_ceil    =  ceil(y_correct);

      float2 xy_correct = float2(x, y_correct);

      bool x_is_under = x < start_encoded.x;
      bool x_is_over  = x >  stop_encoded.x;

      [loop]
      for (float y = y_floor - PLUS_MINUS; y <= (y_ceil + PLUS_MINUS + Unrolling_Be_Gone_Float); y = y + 1.f)
      {
        float2 xy = float2(x, y);

        float fract = distance(xy_correct, xy);

        [branch]
        if (x_is_under)
        {
          fract = max(fract, distance(start_encoded, xy));
        }
        else
        [branch]
        if (x_is_over)
        {
          fract = max(fract, distance(stop_encoded, xy));
        }

        //avoid invalid numbers
        fract = saturate(fract * 0.5f);

        float grey = 1.f - fract;

        [branch]
        if (grey > 0.f)
        {
          float grey_encoded = sqrt(grey);

          xy.y = Render_Size_Minus_1.y - xy.y;

          int2 xy_as_int = int2(xy);

          memoryBarrier();

          float current_value = tex2Dfetch(StorageCieOverlay, xy_as_int).x;

          [branch]
          if (grey_encoded > current_value)
          {
            tex2Dstore(StorageCieOverlay, xy_as_int, float4(grey_encoded, grey_encoded, 0.f, 0.f));
          }
        }
      }
    }
  }
  else
  {
    [loop]
    for (float y = start_encoded_floor.y - PLUS_MINUS; y <= (stop_encoded_ceil.y + PLUS_MINUS + Unrolling_Be_Gone_Float); y = y + 1.f)
    {
      float x_correct = (y - b) / m;
      float x_floor   = floor(x_correct);
      float x_ceil    =  ceil(x_correct);

      float2 xy_correct = float2(x_correct, y);

      bool y_is_under = y < start_encoded.y;
      bool y_is_over  = y >  stop_encoded.y;

      [loop]
      for (float x = x_floor - PLUS_MINUS; x <= (x_ceil + PLUS_MINUS + Unrolling_Be_Gone_Float); x = x + 1.f)
      {
        float2 xy = float2(x, y);

        float fract = distance(xy_correct, xy);

        [branch]
        if (y_is_under)
        {
          fract = max(fract, distance(start_encoded, xy));
        }
        else
        [branch]
        if (y_is_over)
        {
          fract = max(fract, distance(stop_encoded, xy));
        }

        //avoid invalid numbers
        fract = saturate(fract * 0.5f);

        float grey = 1.f - fract;

        [branch]
        if (grey > 0.f)
        {
          float grey_encoded = sqrt(grey);

          xy.y = Render_Size_Minus_1.y - xy.y;

          int2 xy_as_int = int2(xy);

          memoryBarrier();

          float current_value = tex2Dfetch(StorageCieOverlay, xy_as_int).x;

          [branch]
          if (grey_encoded > current_value)
          {
            tex2Dstore(StorageCieOverlay, xy_as_int, float4(grey_encoded, grey_encoded, 0.f, 0.f));
          }
        }
      }
    }
  }

  return;
}


float MitchellNetravali
(
        float x,
  const float B,
  const float C
)
{
  x = abs(x);

  [branch]
  if (x < 2.f)
  {
    float x2 = x * x;
    float x3 = x * x2;

    [branch]
    if (x < 1.f)
    {
      return (
               ( 12.f -  9.f * B - 6.f * C) * x3
             + (-18.f + 12.f * B + 6.f * C) * x2
             + (  6.f -  2.f * B)
             ) / 6.f;
    }
    else //if (x >= 1.f && x < 2.f)
    {
      return (
               (       -B -  6.f * C) * x3
             + (  6.f * B + 30.f * C) * x2
             + (-12.f * B - 48.f * C) * x
             + (  8.f * B + 24.f * C)
             ) / 6.f;
    }
  }
  else
  {
    return 0.f;
  }
}

//https://www.codeproject.com/Articles/236394/Bi-Cubic-and-Bi-Linear-Interpolation-with-GLSL
float2 Bicubic
(
  const int2  Coords,
  const float B,
  const float C
)
{
  float sum = 0.f;
  float den = 0.f;

  float alpha;

  [loop]
  for (int x = -1; x <= 2; x++)
  {
    [loop]
    for (int y = -1; y <= 2; y++)
    {
      float2 currentPixel = tex2Dfetch(StorageCieOverlay, Coords + int2(x, y)).xy;

      float c = currentPixel[0];

      [branch]
      if (x == 0
       && y == 0)
      {
        alpha = currentPixel[1];
      }

      float2 xy = float2(x, y) - 0.5f;

      float f0 = MitchellNetravali( xy.x, B, C);
      float f1 = MitchellNetravali(-xy.y, B, C);

      float f0xf1 = f0 * f1;

      sum += c * f0xf1;

      den += f0xf1;
    }
  }

  float c = sum / den;

  alpha = max(c, alpha);

  return float2(c, alpha);
}

void DrawCieOutlines
(
  const float Unrolling_Be_Gone_Float,
  const uint  Unrolling_Be_Gone_Uint,
  const int   Unrolling_Be_Gone_Int
)
{
  BRANCH()
  if (_SHOW_CIE)
  {
    const uint cieSettingsOld = asuint(tex1Dfetch(StorageConsolidated, COORDS_CIE_LAST_SETTINGS));

                                //safety so it's a big enough float number that does not get flushed
    const uint cieSettingsNew = uint(0x40000000)
                              |      (_CIE_DIAGRAM_TYPE                 << CIE_DIAGRAM_TYPE_ENCODE_OFFSET)
                              | (uint(_CIE_SHOW_GAMUT_OUTLINE_POINTERS) << CIE_SHOW_GAMUT_OUTLINE_POINTERS_ENCODE_OFFSET)
                              | (uint(_CIE_SHOW_GAMUT_OUTLINE_BT709)    << CIE_SHOW_GAMUT_OUTLINE_BT709_ENCODE_OFFSET)
#ifdef IS_HDR_CSP
                              | (uint( CIE_SHOW_GAMUT_OUTLINE_DCI_P3)   << CIE_SHOW_GAMUT_OUTLINE_DCI_P3_ENCODE_OFFSET)
                              | (uint( CIE_SHOW_GAMUT_OUTLINE_BT2020)   << CIE_SHOW_GAMUT_OUTLINE_BT2020_ENCODE_OFFSET)
#endif
                              ;

    [branch]
    if (cieSettingsOld                                        != cieSettingsNew
     || tex1Dfetch(StorageConsolidated, COORDS_CIE_LAST_SIZE) != _CIE_DIAGRAM_SIZE)
    {
      [loop]
      for (int x = 0; x < (CIE_TEXTURE_WIDTH_INT + Unrolling_Be_Gone_Int); x++)
      {
        [loop]
        for (int y = 0; y < (CIE_TEXTURE_HEIGHT_INT + Unrolling_Be_Gone_Int); y++)
        {
          tex2Dstore(StorageCieOverlay, int2(x, y), (float4)0.f);
        }
      }

      float2 cieMinExtra;
      float2 cieNormalise;
      float2 cieSize;

      float2 primBt709R;
      float2 primBt709G;
      float2 primBt709B;

      float2 coordsPointersGamut[POINTERS_GAMUT_ARRAY_LENGTH];

#ifdef IS_HDR_CSP
      float2 primDciP3R;
      float2 primDciP3G;
      float2 primDciP3B;

      float2 primBt2020R;
      float2 primBt2020G;
      float2 primBt2020B;
#endif

      FLATTEN()
      if (_CIE_DIAGRAM_TYPE == CIE_1931)
      {
        cieMinExtra  = CIE_XY_MIN_EXTRA;
        cieNormalise = CIE_XY_NORMALISE;
        cieSize      = CIE_XY_SIZE_FLOAT;

        coordsPointersGamut = Pointers_Gamut_xy;

        primBt709R = CIE_xy_Primary_BT709_Red;
        primBt709G = CIE_xy_Primary_BT709_Green;
        primBt709B = CIE_xy_Primary_BT709_Blue;

#ifdef IS_HDR_CSP
        primDciP3R = CIE_xy_Primary_DCIP3_Red;
        primDciP3G = CIE_xy_Primary_DCIP3_Green;
        primDciP3B = CIE_xy_Primary_DCIP3_Blue;

        primBt2020R = CIE_xy_Primary_BT2020_Red;
        primBt2020G = CIE_xy_Primary_BT2020_Green;
        primBt2020B = CIE_xy_Primary_BT2020_Blue;
#endif
      }
      else //if (_CIE_DIAGRAM_TYPE == CIE_1976)
      {
        cieMinExtra  = CIE_UV_MIN_EXTRA;
        cieNormalise = CIE_UV_NORMALISE;
        cieSize      = CIE_UV_SIZE_FLOAT;

        coordsPointersGamut = Pointers_Gamut_uv1976;

        primBt709R = CIE_uv1976_Primary_BT709_Red;
        primBt709G = CIE_uv1976_Primary_BT709_Green;
        primBt709B = CIE_uv1976_Primary_BT709_Blue;

#ifdef IS_HDR_CSP
        primDciP3R = CIE_uv1976_Primary_DCIP3_Red;
        primDciP3G = CIE_uv1976_Primary_DCIP3_Green;
        primDciP3B = CIE_uv1976_Primary_DCIP3_Blue;

        primBt2020R = CIE_uv1976_Primary_BT2020_Red;
        primBt2020G = CIE_uv1976_Primary_BT2020_Green;
        primBt2020B = CIE_uv1976_Primary_BT2020_Blue;
#endif
      }

      const float2 renderSizeMinus1 = GetCieDiagramRenderSizeMinus1(cieSize);


#ifdef IS_HDR_CSP
                                  //BT.709 + DCI-P3 + BT.2020
      static const uint drawCount = 1 + 1 + 1;
//      static const uint drawCount = 3 + 3 + 3;
#endif


#define DRAW_COORDS_FROM_ARRAY(ARRAY_NAME, ARRAY_LENGTH)                      \
          [loop]                                                              \
          for (uint i = 0u; i < (ARRAY_LENGTH + Unrolling_Be_Gone_Uint); i++) \
          {                                                                   \
            float2 coords0 = ARRAY_NAME[i];                                   \
            float2 coords1 = ARRAY_NAME[(i + 1u) % ARRAY_LENGTH];             \
                                                                              \
            Draw_Cie_Lines(Unrolling_Be_Gone_Float,                           \
                           cieMinExtra,                                       \
                           cieNormalise,                                      \
                           renderSizeMinus1,                                  \
                           coords0,                                           \
                           coords1);                                          \
          }

      BRANCH()
      if (_CIE_SHOW_GAMUT_OUTLINE_POINTERS)
      {
        DRAW_COORDS_FROM_ARRAY(coordsPointersGamut, uint(POINTERS_GAMUT_ARRAY_LENGTH))
      }


#ifdef IS_HDR_CSP
      [loop]
      for (uint i = 0u; i < (drawCount + Unrolling_Be_Gone_Uint); i++)
      {
        float2 coordsArray[3];

        bool needsDrawing;

        [forcecase]
        switch(i)
        {
          case 0u:
          {
            coordsArray[0] = primBt709R;
            coordsArray[1] = primBt709G;
            coordsArray[2] = primBt709B;

            needsDrawing = _CIE_SHOW_GAMUT_OUTLINE_BT709;
          }
          break;
          case 1u:
          {
            coordsArray[0] = primDciP3R;
            coordsArray[1] = primDciP3G;
            coordsArray[2] = primDciP3B;

            needsDrawing = CIE_SHOW_GAMUT_OUTLINE_DCI_P3;
          }
          break;
          default: //case 2u:
          {
            coordsArray[0] = primBt2020R;
            coordsArray[1] = primBt2020G;
            coordsArray[2] = primBt2020B;

            needsDrawing = CIE_SHOW_GAMUT_OUTLINE_BT2020;
          }
          break;
        }

        [branch]
        if (needsDrawing)
        {
          DRAW_COORDS_FROM_ARRAY(coordsArray, 3u)
        }
      }
#else
      static const float2 coordsArray[3] =
      {
        primBt709R,
        primBt709G,
        primBt709B
      };

      BRANCH()
      if (_CIE_SHOW_GAMUT_OUTLINE_BT709)
      {
        DRAW_COORDS_FROM_ARRAY(coordsArray, 3u)
      }
#endif

      tex1Dstore(StorageConsolidated, COORDS_CIE_LAST_SETTINGS, asfloat(cieSettingsNew));
      tex1Dstore(StorageConsolidated, COORDS_CIE_LAST_SIZE,     _CIE_DIAGRAM_SIZE);
      tex1Dstore(StorageConsolidated, COORDS_CIE_TIMER,         FRAMETIME);
    }


    static const float cieTimer = tex1Dfetch(StorageConsolidated, COORDS_CIE_TIMER);

    [branch]
    if (cieTimer >= 1000.f)
    {
      float2 cieMinExtra;
      float2 cieNormalise;
      float2 cieSize;

      float2 coordsSpectralLocus[CIE_1931_2_DEGREE_STANDARD_OBSERVER_ARRAY_LENGTH];

      FLATTEN()
      if (_CIE_DIAGRAM_TYPE == CIE_1931)
      {
        cieMinExtra  = CIE_XY_MIN_EXTRA;
        cieNormalise = CIE_XY_NORMALISE;
        cieSize      = CIE_XY_SIZE_FLOAT;

        coordsSpectralLocus = CIE_1931_2_Degree_Standard_Observer_xy;
      }
      else //if (_CIE_DIAGRAM_TYPE == CIE_1976)
      {
        cieMinExtra  = CIE_UV_MIN_EXTRA;
        cieNormalise = CIE_UV_NORMALISE;
        cieSize      = CIE_UV_SIZE_FLOAT;

        coordsSpectralLocus = CIE_1931_2_Degree_Standard_Observer_uv1976;
      }

      const float2 renderSizeMinus1 = GetCieDiagramRenderSizeMinus1(cieSize);

      DRAW_COORDS_FROM_ARRAY(coordsSpectralLocus, uint(CIE_1931_2_DEGREE_STANDARD_OBSERVER_ARRAY_LENGTH))

      memoryBarrier();

      static const int2 render_size_minus_1_as_int = int2(renderSizeMinus1);

      [loop]
      for (int y = 0; y <= (render_size_minus_1_as_int.y + Unrolling_Be_Gone_Int); y++)
      {
        int coord_x_left;
        int coord_x_right;

        int x_left = 0;

        float value_last = -1.f;

        //search from the left for the first pixel that is not 0 and the highest one
        [loop]
        while (x_left <= (render_size_minus_1_as_int.x + Unrolling_Be_Gone_Int))
        {
          int2 xy_left = int2(x_left, y);

          float value_current = tex2Dfetch(StorageCieOverlay, xy_left).x;

          //if the pixel is found end loop
          if (value_current >  0.f
           && value_current <= value_last)
          {
            coord_x_left = x_left - 1;

            break;
          }

          x_left++;

          value_last = value_current;
        }


        int x_right = render_size_minus_1_as_int.x;

        //search from the right for the first pixel that is not 0 and the highest one
        [loop]
        while (x_right >= (0 + Unrolling_Be_Gone_Int))
        {
          int2 xy_right = int2(x_right, y);

          float value_current = tex2Dfetch(StorageCieOverlay, xy_right).x;

          //if the pixel is found end loop
          if (value_current >  0.f
           && value_current <= value_last)
          {
            coord_x_right = x_right + 1;

            break;
          }

          x_right--;

          value_last = value_current;
        }

        value_last = tex2Dfetch(StorageCieOverlay, int2(coord_x_left, y)).x;

        float local_max = value_last;

        bool row_has_value_at_zero      = false;
        bool row_has_been_smaller_once  = false;
        bool row_has_been_greater_twice = false;
        [loop]
        for (int x_local = coord_x_left + 1; x_local <= (coord_x_right + Unrolling_Be_Gone_Int); x_local++)
        {
          int2 xy_local = int2(x_local, y);

          float value_current = tex2Dfetch(StorageCieOverlay, xy_local).x;

          local_max = max(value_current, local_max);

          if (!row_has_value_at_zero)
          {
            row_has_value_at_zero = value_current == 0.f;
          }

          // up + down
          if (!row_has_been_smaller_once)
          {
            // make sure the difference is high enough to matter
            row_has_been_smaller_once = (local_max - value_current) > 0.1f
                                     && value_current < value_last;
          }

          // up + down + up
          if (!row_has_been_greater_twice)
          {
            row_has_been_greater_twice = row_has_been_smaller_once
                                      && value_current > value_last;
          }

          value_last = value_current;
        }

        [branch]
        if (row_has_been_greater_twice
         || row_has_value_at_zero)
        {
          //set alpha to 1 so that the background also gets drawn
          [loop]
          for (int x_alpha = coord_x_left; x_alpha <= (coord_x_right + Unrolling_Be_Gone_Int); x_alpha++)
          {
            int2 xy_local = int2(x_alpha, y);

            float value_current = tex2Dfetch(StorageCieOverlay, xy_local).x;

            tex2Dstore(StorageCieOverlay, xy_local, float4(value_current, 1.f, 0.f, 0.f));
          }
        }

//        memoryBarrier();
//
//        //bicubic interpolation with Mitchell-Netravali
//        [loop]
//        for (int x = 0; x <= render_size_minus_1_as_int.x; x++)
//        {
//          int2 xy = int2(x, y);
//
//          float2 interpolated = Bicubic(xy, 1.f, 0.f);
//
//          tex2Dstore(StorageCieOverlay, xy, float4(interpolated, 0.f, 0.f));
//        }
      }


      tex1Dstore(StorageConsolidated, COORDS_CIE_TIMER, -1.f);
    }
    else
    [branch]
    if (cieTimer >= 0.f)
    {
      tex1Dstore(StorageConsolidated, COORDS_CIE_TIMER, cieTimer + FRAMETIME);
    }

  }

  return;
}


//pointer's gamut https://www.desmos.com/calculator/3cmjepebtb

void VS_PrepareComposeCieDiagram
(
  in                  uint   VertexID : SV_VertexID,
  out                 float4 Position : SV_Position,
  out nointerpolation float4 CieData0 : CieData0
)
{
  float2 texCoord;
  texCoord.x = (VertexID == 2) ? 2.f : 0.f;
  texCoord.y = (VertexID == 1) ? 2.f : 0.f;

  Position = float4(texCoord * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);

  CieData0 = 0.f;

#define renderSizeMinus1Y      CieData0.x
#define oneDivRenderSizeMinus1 CieData0.yz
#define cieTimerCurrent        CieData0.w

  const float2 renderSizeMinus1 = GetCieDiagramRenderSizeMinus1();

  renderSizeMinus1Y = renderSizeMinus1.y;

  oneDivRenderSizeMinus1 = 1.f / renderSizeMinus1;

  cieTimerCurrent = tex1Dfetch(SamplerConsolidated, COORDS_CIE_TIMER);

  return;
}

// draw the gamut outlines on the CIE diagram
void PS_ComposeCieDiagram
(
  in                  float4 Position : SV_Position,
  in  nointerpolation float4 CieData0 : CieData0,
  out                 float4 Out      : SV_Target0
)
{
  Out = 0.f;

  BRANCH()
  if (_SHOW_CIE)
  {
    const int2 positionAsInt2 = int2(Position.xy);

    float2 Outline = tex2Dfetch(SamplerCieOverlay, positionAsInt2).xy;

    Outline[0] = (Outline[0] * Outline[0]) * 0.18f;

    if (cieTimerCurrent != -1.f)
    {
      Outline[1] = 1.f;
    }

    uint cieCurrent = tex3Dfetch(SamplerCieCounter, int3(positionAsInt2,  0));

    static const float cieCurrentMax =
      float(tex2Dfetch(SamplerMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_CIE_COUNTER_MAX));

    float cieCurrentIntensity = float(cieCurrent) / cieCurrentMax;

    [branch]
    if (cieCurrentIntensity > 0.f)
    {
      const float2 decodedCoords = DecodeCieDiagramCoords(floor(Position.xy),
                                                          renderSizeMinus1Y,
                                                          oneDivRenderSizeMinus1);

      float Y = min(pow(cieCurrentIntensity, 1.f / 3.f) + (1.f / 255.f), 1.f);

      float3 XYZ;

      BRANCH()
      if (_CIE_DIAGRAM_TYPE == CIE_1931)
      {
        float2 xy = ConvertDecodedCieDiagramCoordsToxy(decodedCoords);

        XYZ = Csp::CIE::xy_To::XYZ(xy);
      }
      else //if (_CIE_DIAGRAM_TYPE == CIE_1976)
      {
        float2 uv = ConvertDecodedCieDiagramCoordsTouv(decodedCoords);

        XYZ = Csp::CIE::uv_To::XYZ(uv);
      }

#ifdef IS_HDR_CSP
      float3 rgb = Csp::Mat::XYZ_To::BT2020(XYZ);
#else
      float3 rgb = Csp::Mat::XYZ_To::BT709(XYZ);
#endif

      rgb = max(rgb, 0.f);

      rgb /= MAXRGB(rgb);

      rgb = rgb * Y + Outline[0];

#ifdef IS_HDR_CSP
      #define K_WEIGHTS Csp::YCbCr::K_BT2020
      #define PB_NB_ENC Csp::YCbCr::PB_NB_BT2020_g2_enc
      #define PR_NR_ENC Csp::YCbCr::PR_NR_BT2020_g2_enc
#else
      #define K_WEIGHTS Csp::YCbCr::K_BT709
      #define PB_NB_ENC Csp::YCbCr::PB_NB_BT709_g2_enc
      #define PR_NR_ENC Csp::YCbCr::PR_NR_BT709_g2_enc
#endif
      float3 yccrccbc;

      // RGB->Y->Y'c
      yccrccbc[0] = sqrt(dot(rgb.rgb, K_WEIGHTS));

      // RB->R'B'->C'rcC'bc
      yccrccbc.yz  = sqrt(rgb.rb) - yccrccbc[0];
      yccrccbc.yz *= yccrccbc.yz <= 0.f ? float2(PR_NR_ENC[1], PB_NB_ENC[1])
                                        : float2(PR_NR_ENC[0], PB_NB_ENC[0]);

#undef K_WEIGHTS
#undef PB_NB_ENC
#undef PR_NR_ENC

      yccrccbc.yz += (127.f / 255.f);

      Out = float4(yccrccbc, 1.f);
    }
    else
    [branch] //flatten?
    if (Outline[1] != 0.f)
    {
      Out = float4(sqrt(Outline[0]), (127.f / 255.f).xx, Outline[1]);
    }
  }

  return;
}

#undef oneDivRenderSizeMinus1
#undef renderSizeMinus1Y


float3 GetXYZFromRgb
(
  const float3 Rgb
)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  const float3 XYZ = Csp::Mat::BT709_To::XYZ(Rgb);

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

  const float3 XYZ = Csp::Mat::BT2020_To::XYZ(FetchFromHdr10ToLinearLUT(Rgb));

#elif (ACTUAL_COLOUR_SPACE == CSP_BT2020_EXTENDED)

  const float3 XYZ = Csp::Mat::BT2020_To::XYZ(Rgb);

#elif (ACTUAL_COLOUR_SPACE == CSP_SRGB)

  const float3 XYZ  = Csp::Mat::BT709_To::XYZ(DECODE_SDR(Rgb));

#else

  const float3 XYZ = float3(0.f, 0.f, 0.f);

#endif

  return XYZ;
}

void CS_ClearTextureCieCounter
(
  uint3 DTID : SV_DispatchThreadID
)
{
  BRANCH()
  if (_SHOW_CIE)
  {
    tex3Dstore(StorageCieCounter, DTID, 0u);
  }

  return;
}

float2 GetxyFromXYZForDiagram
(
  const float3 XYZ
)
{
  float2 xy = Csp::CIE::XYZ_To::xy(XYZ);

  //adjust for negative values
  xy -= CIE_XY_MIN_EXTRA;

  //normalise and clamp
  xy = saturate(xy / CIE_XY_NORMALISE);

  return xy;
}

float2 GetuvFromXYZForDiagram
(
  const float3 XYZ
)
{
  float2 uv = Csp::CIE::XYZ_To::uv(XYZ);

  //adjust for negative values
  uv -= CIE_UV_MIN_EXTRA;

  //normalise and clamp
  uv = saturate(uv / CIE_UV_NORMALISE);

  return uv;
}


void GenerateCieDiagram
(
  const float3 XYZ,
  const uint   GID
)
{
  float2 coords;
  float2 renderSizeMinus1;

  const float cieDragramSizeMultiplier = GetCieDragramSizeMultiplier();

  BRANCH()
  if (_CIE_DIAGRAM_TYPE == CIE_1931)
  {
    // get xy
    coords = GetxyFromXYZForDiagram(XYZ);

    renderSizeMinus1 = CIE_XY_SIZE_FLOAT * cieDragramSizeMultiplier - 0.5f;
  }
  else //if (_CIE_DIAGRAM_TYPE == CIE_1976)
  {
    // get u'v'
    coords = GetuvFromXYZForDiagram(XYZ);

    renderSizeMinus1 = CIE_UV_SIZE_FLOAT * cieDragramSizeMultiplier - 0.5f;
  }

  renderSizeMinus1 = floor(renderSizeMinus1);

  int2 encodedCoords = int2(coords * renderSizeMinus1 + 0.5f);

  encodedCoords.y = int(renderSizeMinus1.y) - encodedCoords.y;

  // faster, probably due to atomic add being group local
  // instead of group thread id "local" which is not local
  // so better cache hits?
  const uint u2 = (GID % 15u) + 1u;

  atomicAdd(StorageCieCounter, int3(encodedCoords, u2), 1u);

  return;
}

groupshared uint GroupMaxCie;
void CS_GetMaxCieCounter
(
  uint3 GTID : SV_GroupThreadID,
  uint3 DTID : SV_DispatchThreadID
)
{
  BRANCH()
  if (_SHOW_CIE)
  {
    [branch]
    if (all(DTID.xy == 0))
    {
      tex2Dstore(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_CIE_COUNTER_MAX, 0);
    }

    const bool is_gtid_00 = all(GTID.xy == 0);

    [branch]
    if (is_gtid_00)
    {
      GroupMaxCie = 0u;
    }

    barrier();

    //worth to do performance wise
    [branch]
    if (DTID.x < (_CIE_DIAGRAM_TYPE == CIE_1931 ? CIE_XY_WIDTH_UINT : CIE_UV_WIDTH_UINT))
    {
      static const int2 DTIDAsInt = int2(DTID.xy);

                                       //needs to be storage for d3d11
      const uint cieCurrent = tex3Dfetch(StorageCieCounter, int3(DTIDAsInt,  1))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt,  2))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt,  3))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt,  4))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt,  5))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt,  6))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt,  7))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt,  8))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt,  9))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt, 10))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt, 11))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt, 12))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt, 13))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt, 14))
                            + tex3Dfetch(StorageCieCounter, int3(DTIDAsInt, 15));

      atomicMax(GroupMaxCie, cieCurrent);

      groupMemoryBarrier();

      tex3Dstore(StorageCieCounter, int3(DTIDAsInt, 0), cieCurrent);

      [branch]
      if (is_gtid_00)
      {
#ifdef IS_FLOAT_HDR_CSP
        atomicMax(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_CIE_COUNTER_MAX, int(GroupMaxCie));
#else
        atomicMax(StorageMaxAvgMinNitsAndGamutCounterAndShowNumbers, POS_CIE_COUNTER_MAX, GroupMaxCie);
#endif
      }
    }
  }

  return;
}

groupshared int2 StorePos;
void CS_RenderCrosshairToCieDiagram
(
  uint3 DTID : SV_DispatchThreadID
)
{
  static const float3 ycbcrBlack = float3(0.f, (127.f / 255.f), (127.f / 255.f));
  static const float3 ycbcrWhite = float3(1.f, (127.f / 255.f), (127.f / 255.f));

  static const float4 storeColourBlack = float4(ycbcrBlack, 1.f);
  static const float4 storeColourWhite = float4(ycbcrWhite, 1.f);

  BRANCH()
  if (_SHOW_CIE
   && _SHOW_CROSSHAIR_ON_CIE_DIAGRAM)
  {

    if (all(DTID.xy == 0))
    {
      const float3 cursorRgb = tex2Dfetch(SamplerBackBuffer, MOUSE_POSITION).rgb;

      const float3 cursorXYZ = GetXYZFromRgb(cursorRgb);

      [branch]
      if (cursorXYZ.y != 0.f)
      {
        float2 coords;
        float2 renderSizeMinus1;

        const float cieDragramSizeMultiplier = GetCieDragramSizeMultiplier();

        BRANCH()
        if (_CIE_DIAGRAM_TYPE == CIE_1931)
        {
          // get xy
          coords = GetxyFromXYZForDiagram(cursorXYZ);

          renderSizeMinus1 = CIE_XY_SIZE_FLOAT * cieDragramSizeMultiplier - 0.5f;
        }
        else //if (_CIE_DIAGRAM_TYPE == CIE_1976)
        {
          // get u'v'
          coords = GetuvFromXYZForDiagram(cursorXYZ);

          renderSizeMinus1 = CIE_UV_SIZE_FLOAT * cieDragramSizeMultiplier - 0.5f;
        }

        renderSizeMinus1 = floor(renderSizeMinus1);

        StorePos = int2(coords * renderSizeMinus1 + 0.5f);

        StorePos.y = int(renderSizeMinus1.y) - StorePos.y;
      }
      else
      {
        StorePos = int2(0, 0);
      }
    }
    barrier();

    [branch]
    if (all(StorePos != 0))
    {
      static const int2 DTIDInt2 = int2(DTID.xy);

      int2 posX;

      posX.x = DTIDInt2.x + 4;
      posX.y = DTIDInt2.y - 5;

      const uint absPosXy = abs(posX.y);

      [branch]
      if ((absPosXy == 5 && posX.x == 16)
       || (absPosXy == 3 && posX.x ==  4)
       || ((absPosXy == 4 || absPosXy == 5) && (posX.x == 4 || posX.x == 5 || posX.x == 17))
       || (absPosXy == 5 && posX.x ==  6))
      {
        return;
      }
      else
      {
        float4 storeColour;

        [flatten]
        if (absPosXy >   3
         || posX.x   ==  4
         || posX.x   ==  5
         || posX.x   >  15
         || (absPosXy ==  2 && posX.x == 6)
         || (absPosXy ==  3 && (posX.x ==  6
                             || posX.x ==  7
                             || posX.x == 15)))
        {
          storeColour = storeColourWhite;
        }
        else
        {
          storeColour = storeColourBlack;
        }

        int2 negX = int2(-posX.x,
                          posX.y);

        int2 posY = int2(posX.y,
                         posX.x);

        int2 negY = int2(posX.y,
                         negX.x);

        tex2Dstore(StorageCieFinal, StorePos + posX, storeColour);
        tex2Dstore(StorageCieFinal, StorePos + negX, storeColour);
        tex2Dstore(StorageCieFinal, StorePos + posY, storeColour);
        tex2Dstore(StorageCieFinal, StorePos + negY, storeColour);
      }
    }
    else
    {
      return;
    }
  }
  else
  {
    return;
  }
}
