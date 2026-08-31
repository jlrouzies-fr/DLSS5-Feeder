// gaussian film grain by haasn (https://github.com/haasn/gentoo-conf/blob/xor/home/nand/.mpv/shaders/filmgrain.glsl)

#include "lilium__include/include_main.fxh"


uniform float INTENSITY
<
  ui_label = "grain intensity";
  ui_type  = "drag";
  ui_min   = 0.f;
  ui_max   = 0.1f;
  ui_step  = 0.0001f;
> = 0.025f;

uniform int RANDOM
<
  source = "random";
  min    = 0;
  max    = 100000;
>;

float Permute(float X)
{
  X = (34.f * X + 1.f) * X;
  return frac(X * 1.f / 289.f) * 289.f;
}

float Rand(inout float State)
{
  State = Permute(State);
  return frac(State * 1.f / 41.f);
}

static const float a0 =  0.151015505647689f;
static const float a1 = -0.5303572634357367f;
static const float a2 =  1.365020122861334f;
static const float b0 =  0.132089632343748f;
static const float b1 = -0.7607324991323768f;

void PS_Filmgrain(
  in  float4 Position : SV_Position,
  in  float2 TexCoord : TEXCOORD0,
  out float4 Output   : SV_Target0)
{
  float4 input = tex2Dfetch(SamplerBackBuffer, int2(Position.xy));

  float3 m     = float3(TexCoord, RANDOM / 100000.f) + 1.f;
  float  state = Permute(Permute(m.x) + m.y) + m.z;

  float p = 0.95f * Rand(state) + 0.025f;
  float q = p - 0.5f;
  float r = q * q;

  float Grain = q * (a2 + (a1 * r + a0) / (r*r + b1*r + b0));
  Grain *= 0.255121822830526f; // normalize to (-1, 1)

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  float3 ycbcr = Csp::YCbCr::RGB_To::YCbCr_BT2020(Csp::Trc::Linear_To::PQ(Csp::Mat::BT709_To::BT2020(input.rgb / 125.f)));

#elif defined(IS_HDR10_LIKE_CSP)

  float3 ycbcr = Csp::YCbCr::RGB_To::YCbCr_BT2020(input.rgb);

#elif (ACTUAL_COLOUR_SPACE == CSP_PS5)

  float3 ycbcr = Csp::YCbCr::RGB_To::YCbCr_BT2020(Csp::Trc::Linear_To::PQ(input.rgb / 100.f));

#else //ACTUAL_COLOUR_SPACE == CSP_SRGB

  float3 ycbcr = Csp::YCbCr::RGB_To::YCbCr_BT709(input.rgb);

#endif //ACTUAL_COLOUR_SPACE ==

//  float maxMul = min(1.25f + INTENSITY, 2.f);
//  float minMul = 2.01f - maxMul;
//
//  float maxY = ycbcr.x * maxMul;
//  float minY = ycbcr.x * minMul;

  ycbcr.x += (INTENSITY * Grain);

  //ycbcr.x = clamp(ycbcr.x, minY, maxY);

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  float3 rgb = Csp::Mat::BT2020_To::BT709(Csp::Trc::PQ_To::Linear(Csp::YCbCr::YCbCr_To::RGB_BT2020(ycbcr))) * 125.f;

#elif defined(IS_HDR10_LIKE_CSP)

  float3 rgb = Csp::YCbCr::YCbCr_To::RGB_BT2020(ycbcr);

#elif (ACTUAL_COLOUR_SPACE == CSP_PS5)

  float3 rgb = Csp::Trc::PQ_To::Linear(Csp::YCbCr::YCbCr_To::RGB_BT2020(ycbcr)) * 100.f;

#else //ACTUAL_COLOUR_SPACE == CSP_SRGB

  float3 rgb = Csp::YCbCr::YCbCr_To::RGB_BT709(ycbcr);

#endif //ACTUAL_COLOUR_SPACE ==

  Output = float4(rgb, input.a);
}

technique lilium__filmgrain
<
  ui_label = "Lilium's luma filmgrain";
>
{
  pass PS_Filmgrain
  {
    VertexShader = VS_PostProcess;
     PixelShader = PS_Filmgrain;
  }
}
