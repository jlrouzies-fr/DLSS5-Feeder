#pragma once


// Vertex shader generating a triangle covering the entire screen.
// Calculate values only "once" (3 times because it's 3 vertices)
// for the pixel shader.
void VS_PrepareSetActiveArea
(
  in                  uint   VertexID          : SV_VertexID,
  out                 float4 Position          : SV_Position,
  out nointerpolation float4 PercentagesToCrop : PercentagesToCrop
)
{
  float2 TexCoord;
  TexCoord.x = (VertexID == 2) ? 2.f
                               : 0.f;
  TexCoord.y = (VertexID == 1) ? 2.f
                               : 0.f;
  Position = float4(TexCoord * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);


#define percentageToCropFromLeft   PercentagesToCrop.x
#define percentageToCropFromTop    PercentagesToCrop.y
#define percentageToCropFromRight  PercentagesToCrop.z
#define percentageToCropFromBottom PercentagesToCrop.w

  float fractionCropLeft   = _ACTIVE_AREA_CROP_LEFT   / 100.f;
  float fractionCropTop    = _ACTIVE_AREA_CROP_TOP    / 100.f;
  float fractionCropRight  = _ACTIVE_AREA_CROP_RIGHT  / 100.f;
  float fractionCropBottom = _ACTIVE_AREA_CROP_BOTTOM / 100.f;

  percentageToCropFromLeft   =                 fractionCropLeft   * BUFFER_WIDTH;
  percentageToCropFromTop    =                 fractionCropTop    * BUFFER_HEIGHT;
  percentageToCropFromRight  = BUFFER_WIDTH  - fractionCropRight  * BUFFER_WIDTH;
  percentageToCropFromBottom = BUFFER_HEIGHT - fractionCropBottom * BUFFER_HEIGHT;

}

void PS_SetActiveArea
(
  in                  float4 Position          : SV_Position,
  in  nointerpolation float4 PercentagesToCrop : PercentagesToCrop,
  out                 float4 Output            : SV_Target0
)
{
  Output = 0.f;

  if (_ACTIVE_AREA_ENABLE)
  {
    if (Position.x > percentageToCropFromLeft
     && Position.y > percentageToCropFromTop
     && Position.x < percentageToCropFromRight
     && Position.y < percentageToCropFromBottom)
    {
      discard;
    }
    else
    {
      Output = float4(0.f, 0.f, 0.f, 1.f);
      return;
    }
  }

  discard;
}
