
#include "lilium__include/cas.fxh"


void PS_Cas
(
  in  float4 Position : SV_Position,
  in  float2 TexCoord : TEXCOORD0,
  out float4 Output   : SV_Target0
)
{
  static const float Peak = -SHARPEN_AMOUNT;

  SPixelsToProcess ptp;

  PSGetPixels(int2(Position.xy), ptp);

  Output.rgb = CasSharpenOnly(ptp, Peak);

  Output.a = 0.f;

  return;
}


technique lilium__cas_hdr_ps
<
#if defined(IS_HDR_CSP)
  ui_label = "Lilium's HDR Contrast Adaptive Sharpening (AMD FidelityFX CAS)";
#else
  ui_label = "Lilium's SDR Contrast Adaptive Sharpening (AMD FidelityFX CAS)";
#endif
>
{
  pass PS_Cas
  {
    VertexShader = VS_PostProcess;
     PixelShader = PS_Cas;
  }
}
