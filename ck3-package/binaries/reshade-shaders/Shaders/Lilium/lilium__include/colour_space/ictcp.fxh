
namespace Csp
{

  namespace ICtCp
  {

    // The matrices use higher precision rather than being rounded to 12 bit values like the Dolby spec describes.
    // This is because I only use these for internal processing.

    //L'M'S' -> ICtCp
    static const float3x3 PQ_ICtCp_LMS_To_ICtCp =
      float3x3
      (
        0.5f,         0.5f,         0.f,
        1.61370003f, -3.32339620f,  1.70969617f,
        4.37806224f, -4.24553966f, -0.132522642f
      );

    //ICtCp -> L'M'S'
    static const float3x3 ICtCp_To_PQ_ICtCp_LMS =
      float3x3
      (
        1.f,  0.00860647484f,  0.111033529f,
        1.f, -0.00860647484f, -0.111033529f,
        1.f,  0.560046315f,   -0.320631951f
      );


    //RGB BT.709 -> LMS
    static const float3x3 BT709_To_ICtCp_LMS =
      float3x3
      (
        0.295764088f,  0.623072445f, 0.0811667516f,
        0.156191974f,  0.727251648f, 0.116557933f,
        0.0351022854f, 0.156589955f, 0.808302998f
      );

    //scRGB -> LMS
    static const float3x3 scRGB_To_ICtCp_LMS =
      float3x3
      (
        0.00236611254f,  0.00498457951f, 0.000649333989f,
        0.00124953582f,  0.00581801310f, 0.000932463502f,
        0.000280818290f, 0.00125271955f, 0.00646642409f
      );

    //RGB DCI-P3 -> LMS
    static const float3x3 DCIP3_To_ICtCp_LMS =
      float3x3
      (
        0.334494858f,  0.576365113f,  0.0891432985f,
        0.158450931f,  0.713538110f,  0.128012508f,
        0.0205394085f, 0.0917179808f, 0.887737870f
      );

    //RGB BT.2020 -> LMS
    static const float3x3 BT2020_To_ICtCp_LMS =
      float3x3
      (
        0.412036389f,  0.523911893f,  0.0640549808f,
        0.166660219f,  0.720395207f,  0.112946122f,
        0.0241123586f, 0.0754749625f, 0.900407910f
      );


    //LMS -> RGB BT.709
    static const float3x3 ICtCp_LMS_To_BT709 =
      float3x3
      (
         6.17353248f,   -5.32089900f,   0.147354885f,
        -1.32403194f,    2.56026983f,  -0.236238613f,
        -0.0115983877f, -0.264921456f,  1.27652633f
      );

    //LMS -> scRGB
    static const float3x3 ICtCp_LMS_To_scRGB =
      float3x3
      (
         771.691589f,   -665.112365f,    18.4193611f,
        -165.503982f,    320.033721f,   -29.5298271f,
          -1.44979846f,  -33.1151809f,  159.565795f
      );

    //LMS -> RGB DCI-P3
    static const float3x3 ICtCp_LMS_To_DCIP3 =
      float3x3
      (
         4.84242963f,     -3.92169165f,   0.0792524516f,
        -1.07515621f,      2.29866075f,  -0.223505541f,
        -0.000956906354f, -0.146754235f,  1.14771676f
      );

    //LMS -> RGB BT.2020
    static const float3x3 ICtCp_LMS_To_BT2020 =
      float3x3
      (
         3.43681478f,   -2.50677371f,    0.0699519291f,
        -0.791058242f,   1.98360168f,   -0.192544832f,
        -0.0257268063f, -0.0991417691f,  1.12487411f
      );

    namespace ICtCp_To
    {
      //ICtCp -> L'M'S'
      float3 PQ_ICtCp_LMS(const float3 ICtCp)
      {
        return mul(ICtCp_To_PQ_ICtCp_LMS, ICtCp);
      }
    } //ICtCp_To

    namespace PQ_ICtCp_LMS_To
    {
      //L'M'S' -> ICtCp
      float3 ICtCp(const float3 PQ_ICtCp_LMS)
      {
        return mul(PQ_ICtCp_LMS_To_ICtCp, PQ_ICtCp_LMS);
      }

      //L'M'S' -> LMS
      float3 LMS(const float3 PQ_ICtCp_LMS)
      {
        return Csp::Trc::PQ_To::Linear(PQ_ICtCp_LMS);
      }
    } //PQ_ICtCp_LMS_To

    namespace ICtCp_To
    {
      //ICtCp -> LMS
      float3 LMS(const float3 ICtCp)
      {
        float3 pq_lms = ICtCp_To::PQ_ICtCp_LMS(ICtCp);

        //LMS
        return PQ_ICtCp_LMS_To::LMS(pq_lms);
      }
    } //ICtCp_To

    namespace LMS_To
    {
      //LMS -> L'M'S'
      float3 PQ_ICtCp_LMS(const float3 LMS)
      {
        return Csp::Trc::Linear_To::PQ(LMS);
      }

      //LMS -> ICtCp
      float3 ICtCp(const float3 LMS)
      {
        float3 pq_lms = LMS_To::PQ_ICtCp_LMS(LMS);

        return PQ_ICtCp_LMS_To::ICtCp(pq_lms);
      }

      //LMS -> RGB BT.709
      float3 BT709(const float3 RGB)
      {
        return mul(ICtCp_LMS_To_BT709, RGB);
      }

      //LMS -> scRGB
      float3 scRGB(const float3 RGB)
      {
        return mul(ICtCp_LMS_To_scRGB, RGB);
      }

      //LMS -> RGB DCI-P3
      float3 DCIP3(const float3 RGB)
      {
        return mul(ICtCp_LMS_To_DCIP3, RGB);
      }

      //LMS -> RGB BT.2020
      float3 BT2020(const float3 RGB)
      {
        return mul(ICtCp_LMS_To_BT2020, RGB);
      }
    } //LMS_To

    namespace PQ_ICtCp_LMS_To
    {
      //L'M'S' -> RGB BT.709
      float3 BT709(const float3 PQ_ICtCp_LMS)
      {
        float3 lms = PQ_ICtCp_LMS_To::LMS(PQ_ICtCp_LMS);

        return LMS_To::BT709(lms);
      }

      //L'M'S' -> scRGB
      float3 scRGB(const float3 PQ_ICtCp_LMS)
      {
        float3 lms = PQ_ICtCp_LMS_To::LMS(PQ_ICtCp_LMS);

        return LMS_To::scRGB(lms);
      }

      //L'M'S' -> RGB DCI-P3
      float3 DCIP3(const float3 PQ_ICtCp_LMS)
      {
        float3 lms = PQ_ICtCp_LMS_To::LMS(PQ_ICtCp_LMS);

        return LMS_To::DCIP3(lms);
      }

      //L'M'S' -> RGB BT.2020
      float3 BT2020(const float3 PQ_ICtCp_LMS)
      {
        float3 lms = PQ_ICtCp_LMS_To::LMS(PQ_ICtCp_LMS);

        return LMS_To::BT2020(lms);
      }
    } //PQ_ICtCp_LMS_To

    namespace ICtCp_To
    {
      //ICtCp -> RGB BT.709
      float3 BT709(const float3 ICtCp)
      {
        float3 lms = ICtCp_To::LMS(ICtCp);

        return LMS_To::BT709(lms);
      }

      //ICtCp -> scRGB
      float3 scRGB(const float3 ICtCp)
      {
        float3 lms = ICtCp_To::LMS(ICtCp);

        return LMS_To::scRGB(lms);
      }

      //ICtCp -> RGB DCI-P3
      float3 DCIP3(const float3 ICtCp)
      {
        float3 lms = ICtCp_To::LMS(ICtCp);

        return LMS_To::DCIP3(lms);
      }

      //ICtCp -> RGB BT.2020
      float3 BT2020(const float3 ICtCp)
      {
        float3 lms = ICtCp_To::LMS(ICtCp);

        return LMS_To::BT2020(lms);
      }
    } //ICtCp_To

    namespace BT709_To
    {
      //RGB BT.709 -> LMS
      float3 LMS(const float3 RGB)
      {
        return mul(BT709_To_ICtCp_LMS, RGB);
      }

      //RGB BT.709 -> L'M'S'
      float3 PQ_ICtCp_LMS(const float3 Rgb)
      {
        float3 lms = BT709_To::LMS(Rgb);

        return LMS_To::PQ_ICtCp_LMS(lms);
      }

      //RGB BT.709 -> ICtCp
      float3 ICtCp(const float3 Rgb)
      {
        float3 lms = BT709_To::LMS(Rgb);

        return LMS_To::ICtCp(lms);
      }
    } //BT709_To

    namespace scRGB_To
    {
      //scRGB -> LMS
      float3 LMS(const float3 RGB)
      {
        return mul(scRGB_To_ICtCp_LMS, RGB);
      }

      //scRGB -> L'M'S'
      float3 PQ_ICtCp_LMS(const float3 Rgb)
      {
        float3 lms = scRGB_To::LMS(Rgb);

        return LMS_To::PQ_ICtCp_LMS(lms);
      }

      //scRGB -> ICtCp
      float3 ICtCp(const float3 Rgb)
      {
        float3 lms = scRGB_To::LMS(Rgb);

        return LMS_To::ICtCp(lms);
      }
    } //scRGB_To

    namespace DCIP3_To
    {
      //RGB DCI-P3 -> LMS
      float3 LMS(const float3 RGB)
      {
        return mul(DCIP3_To_ICtCp_LMS, RGB);
      }

      //RGB DCI-P3 -> L'M'S'
      float3 PQ_ICtCp_LMS(const float3 Rgb)
      {
        float3 lms = DCIP3_To::LMS(Rgb);

        return LMS_To::PQ_ICtCp_LMS(lms);
      }

      //RGB DCI-P3 -> ICtCp
      float3 ICtCp(const float3 Rgb)
      {
        float3 lms = DCIP3_To::LMS(Rgb);

        return LMS_To::ICtCp(lms);
      }
    } //DCIP3_To

    namespace BT2020_To
    {
      //RGB BT.2020 -> LMS
      float3 LMS(const float3 RGB)
      {
        return mul(BT2020_To_ICtCp_LMS, RGB);
      }

      //RGB BT.2020 -> L'M'S'
      float3 PQ_ICtCp_LMS(const float3 Rgb)
      {
        float3 lms = BT2020_To::LMS(Rgb);

        return LMS_To::PQ_ICtCp_LMS(lms);
      }

      //RGB BT.2020 -> ICtCp
      float3 ICtCp(const float3 Rgb)
      {
        float3 lms = BT2020_To::LMS(Rgb);

        return LMS_To::ICtCp(lms);
      }
    } //BT2020_To

  } //ICtCp

} //Csp
