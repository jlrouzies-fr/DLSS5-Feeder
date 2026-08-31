
namespace Csp
{

  namespace Jzazbz
  {

    // https://doi.org/10.1364/OE.25.015131

    static const float Jzazbz_p  = 134.034375f; // 1.7 * 2523 / 2^5 (2523 / 2^5 = 2523 / 4096 * 128, which is the PQ constant m2)
    static const float Jzazbz_d  =  -0.56f;
    static const float Jzazbz_d0 =   0.0000000000162954996f; // 1.6295499532821566 * 10^-11

    static const float Jzazbz_d_plus_1 = 0.44f;

    static const float Jzazbz_rcp_p = 0.00746077252f;


    //XYZ -> LMS (Jzazbz variant)
    static const float3x3 XYZ_To_Jzazbz_LMS =
      float3x3
      (
        0.674207866f,  0.382799327f, -0.0475704595f,
        0.149284154f,  0.739628314f,  0.0833273008f,
        0.0709410831f, 0.174768000f,  0.670970022f
      );

    //LMS (Jzazbz variant) -> XYZ
    static const float3x3 Jzazbz_LMS_To_XYZ =
      float3x3
      (
         1.66137301f,   -0.914523065f,  0.231362074f,
        -0.325075864f,   1.57184708f,  -0.218253836f,
        -0.0909828096f, -0.312728285f,  1.52276659f
      );

    //BT.709 -> LMS (Jzazbz variant)
    static const float3x3 BT709_To_Jzazbz_LMS =
      float3x3
      (
        0.358515590f,  0.509182095f, 0.104099482f,
        0.220448032f,  0.592272877f, 0.159543678f,
        0.0793883427f, 0.230332136f, 0.663199007f
      );

    //LMS (Jzazbz variant) -> BT.709
    static const float3x3 Jzazbz_LMS_To_BT709 =
      float3x3
      (
         5.92959117f,   -5.22454357f,   0.326109498f,
        -2.22388792f,    3.82213425f,  -0.570404648f,
         0.0625640675f, -0.702040493f,  1.66691029f
      );

    //scRGB -> LMS (Jzazbz variant)
    static const float3x3 scRGB_To_Jzazbz_LMS =
      float3x3
      (
        0.00286812474f,  0.00407345686f, 0.000832795863f,
        0.00176358432f,  0.00473818322f, 0.00127634941f,
        0.000635106756f, 0.00184265710f, 0.00530559197f
      );

    //LMS (Jzazbz variant) -> scRGB
    static const float3x3 Jzazbz_LMS_To_scRGB =
      float3x3
      (
         741.198913f, -653.067932f,  40.7636909f,
        -277.985992f,  477.766784f, -71.3005828f,
         7.82050800f, -87.7550582f,  208.363784f
      );

    //DCI-P3 -> LMS (Jzazbz variant)
    static const float3x3 DCIP3_To_Jzazbz_LMS =
      float3x3
      (
        0.415701270f,  0.441766232f, 0.114329710f,
        0.241993412f,  0.555048584f, 0.175222620f,
        0.0745352953f, 0.170010238f, 0.728373944f
      );

    //LMS (Jzazbz variant) -> DCI-P3
    static const float3x3 Jzazbz_LMS_To_DCIP3 =
      float3x3
      (
         4.48203849f,    -3.61841392f,   0.166944146f,
        -1.95323967f,     3.52183699f,  -0.540645599f,
        -0.00274494127f, -0.451758056f,  1.48203003f
      );

    //BT.2020 -> LMS (Jzazbz variant)
    static const float3x3 BT2020_To_Jzazbz_LMS =
      float3x3
      (
        0.530003547f,  0.355703622f, 0.0860899910f,
        0.289388269f,  0.525394797f, 0.157481506f,
        0.0910980850f, 0.147587582f, 0.734233796f
      );

    //LMS (Jzazbz variant) -> BT.2020
    static const float3x3 Jzazbz_LMS_To_BT2020 =
      float3x3
      (
         2.99066996f,   -2.04974246f,   0.0889767929f,
        -1.63452517f,    3.14562821f,  -0.483036875f,
        -0.0425051115f, -0.377983212f,  1.44801914f
      );

    //Jzazbz L'M'S' -> Izazbz
    static const float3x3 PQ_Jzazbz_LMS_To_Izazbz =
      float3x3
      (
        0.5f,       0.5f,       0.f,
        3.524f,    -4.066708f,  0.542708f,
        0.199076f,  1.096799f, -1.295875f
      );

    //Izazbz -> Jzazbz L'M'S'
    static const float3x3 Izazbz_To_PQ_Jzazbz_LMS =
      float3x3
      (
        1.f,  0.138605043f,   0.0580473169f,
        1.f, -0.138605043f,  -0.0580473169f,
        1.f, -0.0960192456f, -0.811891913f
      );


    namespace XYZ_To
    {
      //XYZ -> LMS (Jzazbz variant)
      float3 Jzazbz_LMS(const float3 XYZ)
      {
        return mul(XYZ_To_Jzazbz_LMS, XYZ);
      }
    } //XYZ_To


    namespace BT709_To
    {
      //BT.709 -> LMS (Jzazbz variant)
      float3 Jzazbz_LMS(const float3 XYZ)
      {
        return mul(BT709_To_Jzazbz_LMS, XYZ);
      }
    } //BT709_To


    namespace scRGB_To
    {
      //scRGB -> LMS (Jzazbz variant)
      float3 Jzazbz_LMS(const float3 XYZ)
      {
        return mul(scRGB_To_Jzazbz_LMS, XYZ);
      }
    } //scRGB_To


    namespace DCIP3_To
    {
      //DCI-P3 -> LMS (Jzazbz variant)
      float3 Jzazbz_LMS(const float3 XYZ)
      {
        return mul(DCIP3_To_Jzazbz_LMS, XYZ);
      }
    } //BT709_To


    namespace BT2020_To
    {
      //BT.2020 -> LMS (Jzazbz variant)
      float3 Jzazbz_LMS(const float3 XYZ)
      {
        return mul(BT2020_To_Jzazbz_LMS, XYZ);
      }
    } //BT2020_To


    namespace Jzazbz_LMS_To
    {
      //LMS (Jzazbz variant) -> L'M'S' (Jzazbz variant)
      float3 PQ_Jzazbz_LMS(const float3 Jzazbz_LMS)
      {
        float3 lms_pow = pow(Jzazbz_LMS, PQ_m1);

        float3 num = PQ_c1 + PQ_c2 * lms_pow;

        float3 den = 1.f   + PQ_c3 * lms_pow;

        return pow(num / den, Jzazbz_p);
      }

      //LMS (Jzazbz variant) -> XYZ
      float3 XYZ(const float3 Jzazbz_LMS)
      {
        return mul(Jzazbz_LMS_To_XYZ, Jzazbz_LMS);
      }

      //LMS (Jzazbz variant) -> BT.709
      float3 BT709(const float3 Jzazbz_LMS)
      {
        return mul(Jzazbz_LMS_To_BT709, Jzazbz_LMS);
      }

      //LMS (Jzazbz variant) -> scRGB
      float3 scRGB(const float3 Jzazbz_LMS)
      {
        return mul(Jzazbz_LMS_To_scRGB, Jzazbz_LMS);
      }

      //LMS (Jzazbz variant) -> BT.2020
      float3 BT2020(const float3 Jzazbz_LMS)
      {
        return mul(Jzazbz_LMS_To_BT2020, Jzazbz_LMS);
      }
    } //Jzazbz_LMS_To


    namespace PQ_Jzazbz_LMS_To
    {
      //L'M'S' (Jzazbz variant) -> Izazbz
      float3 Izazbz(const float3 PQ_Jzazbz_LMS)
      {
        return mul(PQ_Jzazbz_LMS_To_Izazbz, PQ_Jzazbz_LMS);
      }

      //L'M'S' (Jzazbz variant) -> LMS (Jzazbz variant)
      float3 Jzazbz_LMS(const float3 PQ_Jzazbz_LMS)
      {
        float3 pq_lms_pow = pow(PQ_Jzazbz_LMS, Jzazbz_rcp_p);

        float3 num = PQ_c1 - pq_lms_pow;

        float3 den = PQ_c3 * pq_lms_pow - PQ_c2;

        return pow(num / den, PQ_rcp_m1);
      }

      //L'M'S' (Jzazbz variant) -> XYZ
      float3 XYZ(const float3 PQ_Jzazbz_LMS)
      {
        float3 lms = PQ_Jzazbz_LMS_To::Jzazbz_LMS(PQ_Jzazbz_LMS);

        return Jzazbz_LMS_To::XYZ(lms);
      }

      //L'M'S' (Jzazbz variant) -> BT.709
      float3 BT709(const float3 PQ_Jzazbz_LMS)
      {
        float3 lms = PQ_Jzazbz_LMS_To::Jzazbz_LMS(PQ_Jzazbz_LMS);

        return Jzazbz_LMS_To::BT709(lms);
      }

      //L'M'S' (Jzazbz variant) -> scRGB
      float3 scRGB(const float3 PQ_Jzazbz_LMS)
      {
        float3 lms = PQ_Jzazbz_LMS_To::Jzazbz_LMS(PQ_Jzazbz_LMS);

        return Jzazbz_LMS_To::scRGB(lms);
      }

      //L'M'S' (Jzazbz variant) -> BT.2020
      float3 BT2020(const float3 PQ_Jzazbz_LMS)
      {
        float3 lms = PQ_Jzazbz_LMS_To::Jzazbz_LMS(PQ_Jzazbz_LMS);

        return Jzazbz_LMS_To::BT2020(lms);
      }
    } //PQ_Jzazbz_LMS_To


    namespace Izazbz_To
    {
      //Izazbz -> Jzazbz
      float3 Jzazbz(const float3 Izazbz)
      {
        float num = Jzazbz_d_plus_1 * Izazbz.x;

        float den = 1.f + (Jzazbz_d * Izazbz.x);

        //Jz
        float jz = (num / den) - Jzazbz_d0;

        return float3(jz, Izazbz.yz);
      }

      //Izazbz -> L'M'S' (Jzazbz variant)
      float3 PQ_Jzazbz_LMS(const float3 Izazbz)
      {
        return mul(Izazbz_To_PQ_Jzazbz_LMS, Izazbz);
      }

      //Izazbz -> XYZ
      float3 XYZ(const float3 Izazbz)
      {
        float3 pq_lms = Izazbz_To::PQ_Jzazbz_LMS(Izazbz);

        return PQ_Jzazbz_LMS_To::XYZ(pq_lms);
      }

      //Izazbz -> BT.709
      float3 BT709(const float3 Izazbz)
      {
        float3 pq_lms = Izazbz_To::PQ_Jzazbz_LMS(Izazbz);

        return PQ_Jzazbz_LMS_To::BT709(pq_lms);
      }

      //Izazbz -> scRGB
      float3 scRGB(const float3 Izazbz)
      {
        float3 pq_lms = Izazbz_To::PQ_Jzazbz_LMS(Izazbz);

        return PQ_Jzazbz_LMS_To::scRGB(pq_lms);
      }

      //Izazbz -> BT.2020
      float3 BT2020(const float3 Izazbz)
      {
        float3 pq_lms = Izazbz_To::PQ_Jzazbz_LMS(Izazbz);

        return PQ_Jzazbz_LMS_To::BT2020(pq_lms);
      }
    } //Izazbz_To


    namespace Jzazbz_To
    {
      //Jzazbz -> Izazbz
      float3 Izazbz(const float3 Jzazbz)
      {
        float num = Jzazbz.x + Jzazbz_d0;

        float den = Jzazbz_d_plus_1 - (Jzazbz_d * (Jzazbz.x + Jzazbz_d0));

        //Iz
        float iz = num / den;

        return float3(iz, Jzazbz.yz);
      }

      //Jzazbz -> XYZ
      float3 XYZ(const float3 Jzazbz)
      {
        float3 izazbz = Jzazbz_To::Izazbz(Jzazbz);

        return Izazbz_To::XYZ(izazbz);
      }

      //Jzazbz -> BT.709
      float3 BT709(const float3 Jzazbz)
      {
        float3 izazbz = Jzazbz_To::Izazbz(Jzazbz);

        return Izazbz_To::BT709(izazbz);
      }

      //Jzazbz -> scRGB
      float3 scRGB(const float3 Jzazbz)
      {
        float3 izazbz = Jzazbz_To::Izazbz(Jzazbz);

        return Izazbz_To::scRGB(izazbz);
      }

      //Jzazbz -> BT.2020
      float3 BT2020(const float3 Jzazbz)
      {
        float3 izazbz = Jzazbz_To::Izazbz(Jzazbz);

        return Izazbz_To::BT2020(izazbz);
      }
    } //Jzazbz_To


    namespace Jzazbz_LMS_To
    {
      //LMS (Jzazbz variant) -> Izazbz
      float3 Izazbz(const float3 Jzazbz_LMS)
      {
        float3 pq_lms = Jzazbz_LMS_To::PQ_Jzazbz_LMS(Jzazbz_LMS);

        return PQ_Jzazbz_LMS_To::Izazbz(pq_lms);
      }

      //LMS (Jzazbz variant) -> Jzazbz
      float3 Jzazbz(const float3 Jzazbz_LMS)
      {
        float3 izazbz = Jzazbz_LMS_To::Izazbz(Jzazbz_LMS);

        return Izazbz_To::Jzazbz(izazbz);
      }
    } //Jzazbz_LMS_To


    namespace XYZ_To
    {
      //XYZ -> Jzazbz
      float3 Jzazbz(const float3 XYZ)
      {
        float3 lms = XYZ_To::Jzazbz_LMS(XYZ);

        return Jzazbz_LMS_To::Jzazbz(lms);
      }
    } //XYZ_To


    namespace BT709_To
    {
      //RGB BT.709 -> Jzazbz
      float3 Jzazbz(const float3 RGB)
      {
        float3 lms = BT709_To::Jzazbz_LMS(RGB);

        return Jzazbz_LMS_To::Jzazbz(lms);
      }
    } //BT709_To


    namespace scRGB_To
    {
      //RGB scRGB -> Jzazbz
      float3 Jzazbz(const float3 RGB)
      {
        float3 lms = scRGB_To::Jzazbz_LMS(RGB);

        return Jzazbz_LMS_To::Jzazbz(lms);
      }
    } //BT709_To


    namespace DCIP3_To
    {
      //RGB DCI-P3 -> Jzazbz
      float3 Jzazbz(const float3 RGB)
      {
        float3 lms = DCIP3_To::Jzazbz_LMS(RGB);

        return Jzazbz_LMS_To::Jzazbz(lms);
      }
    } //DCIP3_To


    namespace BT2020_To
    {
      //RGB BT.2020 -> Jzazbz
      float3 Jzazbz(const float3 RGB)
      {
        float3 lms = BT2020_To::Jzazbz_LMS(RGB);

        return Jzazbz_LMS_To::Jzazbz(lms);
      }
    } //BT2020_To

  } //Jzazbz

} //Csp
