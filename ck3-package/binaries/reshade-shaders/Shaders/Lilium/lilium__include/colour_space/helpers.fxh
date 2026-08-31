
namespace Csp
{

  namespace Map
  {

    namespace BT709_Into
    {

      float3 scRGB
      (
        const float3 RGB,
        const float  Brightness
      )
      {
        return RGB / 80.f * Brightness;
      }

      float3 HDR10
      (
        const float3 RGB,
        const float  Brightness
      )
      {
        return Csp::Trc::Nits_To::PQ(Csp::Mat::BT709_To::BT2020(RGB) * Brightness);
      }

      float3 BT2020_Extended
      (
        const float3 RGB,
        const float  Brightness
      )
      {
        return Csp::Mat::BT709_To::BT2020(RGB / 100.f) * Brightness;
      }

    } //BT709_Into

  } //Map

} //Csp


#if (OVERWRITE_SDR_GAMMA == GAMMA_24)

  #define ENCODE_SDR(COLOUR) \
            pow(COLOUR, 1.f / 2.4f)

  #define DECODE_SDR(COLOUR) \
            pow(COLOUR, 2.4f)

#elif (OVERWRITE_SDR_GAMMA == GAMMA_SRGB)

  #define ENCODE_SDR(COLOUR) \
            Csp::Trc::Linear_To::sRGB(COLOUR)

  #define DECODE_SDR(COLOUR) \
            Csp::Trc::sRGB_To::Linear(COLOUR)

#else

  #define ENCODE_SDR(COLOUR) \
            pow(COLOUR, 1.f / 2.2f)

  #define DECODE_SDR(COLOUR) \
            pow(COLOUR, 2.2f)

#endif


// convert BT.709 to BT.2020
float3 ConditionallyConvertBt709ToBt2020(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10 \
  || ACTUAL_COLOUR_SPACE == CSP_BT2020_EXTENDED)
  Colour = Csp::Mat::BT709_To::BT2020(Colour);
#endif
  return Colour;
}

// convert DCI-P3 to BT.709
float3 ConditionallyConvertDciP3ToBt709(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  Colour = Csp::Mat::DCIP3_To::BT709(Colour);
#endif
  return Colour;
}

// convert DCI-P3 to BT.2020
float3 ConditionallyConvertDciP3ToBt2020(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
  Colour = Csp::Mat::DCIP3_To::BT2020(Colour);
#endif
  return Colour;
}

// convert BT.2020 to BT.709
float3 ConditionallyConvertBt2020ToBt709(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  Colour = Csp::Mat::BT2020_To::BT709(Colour);
#endif
  return Colour;
}

// normalise so that 10000 = 1
float3 ConditionallyNormaliseScRgb(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  Colour /= 125.f;
#endif
  return Colour;
}

// - normalise so that 10000 = 1
// - convert BT.709 to BT.2020
float3 ConditionallyConvertScRgbToNormalisedBt2020(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  Colour = Csp::Mat::scRGB_To::BT2020_normalised(Colour);
#endif
  return Colour;
}

// - normalise so that 10000 = 1
// - convert BT.709 to BT.2020
// - convert normalised to PQ
float3 ConditionallyConvertScRgbToHdr10(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  Colour = ConditionallyConvertScRgbToNormalisedBt2020(Colour);
  Colour = Csp::Trc::Linear_To::PQ(Colour);
#endif
  return Colour;
}

// convert HDR10 to normalised BT.2020
float3 ConditionallyLineariseHdr10(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
  Colour = Csp::Trc::PQ_To::Linear(Colour);
#endif
  return Colour;
}

// convert normalised BT.2020 to HDR10
float3 ConditionallyConvertNormalisedBt2020ToHdr10(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
  Colour = Csp::Trc::Linear_To::PQ(Colour);
#endif
  return Colour;
}

// convert normalised BT.709 to scRGB
float3 ConditionallyConvertNormalisedBt709ToScRgb(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  Colour *= 125.f;
#endif
  return Colour;
}

// - convert BT.2020 to BT.709
// - convert normalised BT.709 to scRGB
float3 ConditionallyConvertNormalisedBt2020ToScRgb(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  Colour = Csp::Mat::BT2020_normalised_To::scRGB(Colour);
#endif
  return Colour;
}

// - convert HDR10 to normalised BT.2020
// - convert BT.2020 to BT.709
// - convert normalised BT.709 to scRGB
float3 ConditionallyConvertHdr10ToScRgb(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  Colour = Csp::Trc::PQ_To::Linear(Colour);
  Colour = ConditionallyConvertNormalisedBt2020ToScRgb(Colour);
#endif
  return Colour;
}

// get luminance for the current colour space
float GetLuminance(float3 Colour)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
  return dot(Colour, Csp::Mat::BT709_To_XYZ[1]);
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
  return dot(Colour, Csp::Mat::BT2020_To_XYZ[1]);
#else
  return 0;
#endif
}
