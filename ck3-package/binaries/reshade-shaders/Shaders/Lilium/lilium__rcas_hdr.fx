
#include "lilium__include/rcas.fxh"


// Vertex shader generating a triangle covering the entire screen.
// Calculate values only "once" (3 times because it's 3 vertices)
// for the pixel shader.
void VS_PrepareRCas
(
  in                  uint   VertexID  : SV_VertexID,
  out                 float4 Position  : SV_Position
#if (__RESHADE_PERFORMANCE_MODE__ == 0)
                                                    ,
  out nointerpolation float  Sharpness : Sharpness
#endif
)
{
  float2 texCoord;
  texCoord.x = (VertexID == 2) ? 2.f
                               : 0.f;
  texCoord.y = (VertexID == 1) ? 2.f
                               : 0.f;
  Position = float4(texCoord * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);

#if (__RESHADE_PERFORMANCE_MODE__ == 0)
  Sharpness = exp2(-(1.f - SHARPEN_AMOUNT));
#endif
}

void PS_RCas
(
  in                  float4 Position  : SV_Position,
#if (__RESHADE_PERFORMANCE_MODE__ == 0)
  in  nointerpolation float  Sharpness : Sharpness,
#endif
  out                 float4 Output    : SV_Target0
)
{
#if (__RESHADE_PERFORMANCE_MODE__ != 0)
  static const float Sharpness = exp2(-(1.f - SHARPEN_AMOUNT));
#endif

  Output = float4(RCas(int2(Position.xy), Sharpness), 0.f);

  return;
}


technique lilium__rcas_hdr
<
#if defined(IS_HDR_CSP)
  ui_label = "Lilium's HDR Robust Contrast Adaptive Sharpening (AMD FidelityFX RCAS)";
#else
  ui_label = "Lilium's SDR Robust Contrast Adaptive Sharpening (AMD FidelityFX RCAS)";
#endif
>
{
  pass RCas
  {
    VertexShader = VS_PrepareRCas;
     PixelShader = PS_RCas;
  }
}
