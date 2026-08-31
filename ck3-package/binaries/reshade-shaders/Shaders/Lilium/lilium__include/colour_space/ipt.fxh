
namespace Csp
{

  namespace IPT
  {

    //L'M'S' -> IPT
    static const float3x3 G_IPT_LMS_To_IPT =
      float3x3
      (
        0.4f,     0.4f,     0.2f,
        4.4550f, -4.8510f,  0.3960f,
        0.8056f,  0.3572f, -1.1628f
      );

    //IPT -> L'M'S'
    static const float3x3 IPT_To_G_IPT_LMS =
      float3x3
      (
        1.f,  0.0975689291f,  0.205226436f,
        1.f, -0.113876484f,   0.133217155f,
        1.f,  0.0326151102f, -0.676887154f
      );


    //XYZ -> LMS
    static const float3x3 XYZ_To_IPT_LMS =
      float3x3
      (
         0.4002f, 0.7075f, -0.0807f,
        -0.2280f, 1.1500f,  0.0612f,
         0.f    , 0.f,      0.9184f
      );

    //RGB BT.709 -> LMS
    static const float3x3 BT709_To_IPT_LMS =
      float3x3
      (
        0.313920885f,  0.639468073f, 0.0465965308f,
        0.151692807f,  0.748209476f, 0.100044108f,
        0.0177534241f, 0.109468482f, 0.872968733f
      );


    //LMS -> XYZ
    static const float3x3 IPT_LMS_To_XYZ =
      float3x3
      (
        1.85024297f,  -1.13830161f,   0.238434955f,
        0.366830766f,  0.643884539f, -0.0106734437f,
        0.f,           0.f,           1.08885014f
      );

    //LMS -> RGB BT.709
    static const float3x3 IPT_LMS_To_BT709 =
      float3x3
      (
         5.43262243f,   -4.67909860f,   0.246257290f,
        -1.10517358f,    2.31119799f,  -0.205877363f,
         0.0281041357f, -0.194661423f,  1.16632485f
      );


    namespace IPT_To
    {
      //IPT -> L'M'S'
      float3 G_IPT_LMS(const float3 IPT)
      {
        return mul(IPT_To_G_IPT_LMS, IPT);
      }
    } //IPT_To

    namespace G_IPT_LMS_To
    {
      //L'M'S' -> IPT
      float3 IPT(const float3 G_IPT_LMS)
      {
        return mul(G_IPT_LMS_To_IPT, G_IPT_LMS);
      }

      //L'M'S' -> LMS
      float3 IPT_LMS(const float3 G_IPT_LMS)
      {
        return pow(G_IPT_LMS, 1.f / 0.43f);
      }
    } //G_IPT_LMS_To

    namespace IPT_To
    {
      //IPT -> LMS
      float3 IPT_LMS(const float3 IPT)
      {
        float3 g_lms = IPT_To::G_IPT_LMS(IPT);

        //LMS
        return G_IPT_LMS_To::IPT_LMS(g_lms);
      }
    } //IPT_To

    namespace IPT_LMS_To
    {
      //LMS -> L'M'S'
      float3 G_IPT_LMS(const float3 IPT_LMS)
      {
        return pow(IPT_LMS, 0.43f);
      }

      //LMS -> IPT
      float3 IPT(const float3 IPT_LMS)
      {
        float3 g_lms = IPT_LMS_To::G_IPT_LMS(IPT_LMS);

        return G_IPT_LMS_To::IPT(g_lms);
      }

      //LMS -> XYZ
      float3 XYZ(const float3 IPT_LMS)
      {
        return mul(IPT_LMS_To_XYZ, IPT_LMS);
      }

      //LMS -> RGB BT.709
      float3 BT709(const float3 IPT_LMS)
      {
        return mul(IPT_LMS_To_BT709, IPT_LMS);
      }
    } //IPT_LMS_To

    namespace G_IPT_LMS_To
    {
      //L'M'S' -> XYZ
      float3 XYZ(const float3 G_IPT_LMS)
      {
        float3 lms = G_IPT_LMS_To::IPT_LMS(G_IPT_LMS);

        return IPT_LMS_To::XYZ(lms);
      }

      //L'M'S' -> RGB BT.709
      float3 BT709(const float3 G_IPT_LMS)
      {
        float3 lms = G_IPT_LMS_To::IPT_LMS(G_IPT_LMS);

        return IPT_LMS_To::BT709(lms);
      }
    } //G_IPT_LMS_To

    namespace IPT_To
    {
      //IPT -> XYZ
      float3 XYZ(const float3 IPT)
      {
        float3 lms = IPT_To::IPT_LMS(IPT);

        return IPT_LMS_To::XYZ(lms);
      }

      //IPT -> RGB BT.709
      float3 BT709(const float3 IPT)
      {
        float3 lms = IPT_To::IPT_LMS(IPT);

        return IPT_LMS_To::BT709(lms);
      }
    } //IPT_To

    namespace XYZ_To
    {
      //XYZ -> LMS
      float3 IPT_LMS(const float3 XYZ)
      {
        return mul(XYZ_To_IPT_LMS, XYZ);
      }

      //XYZ -> L'M'S'
      float3 G_IPT_LMS(const float3 XYZ)
      {
        float3 lms = XYZ_To::IPT_LMS(XYZ);

        return IPT_LMS_To::G_IPT_LMS(lms);
      }

      //XYZ -> IPT
      float3 IPT(const float3 XYZ)
      {
        float3 g_lms = XYZ_To::G_IPT_LMS(XYZ);

        return G_IPT_LMS_To::IPT(g_lms);
      }
    } //XYZ_To

    namespace BT709_To
    {
      //RGB BT.709 -> LMS
      float3 IPT_LMS(const float3 RGB)
      {
        return mul(BT709_To_IPT_LMS, RGB);
      }

      //RGB BT.709 -> L'M'S'
      float3 G_IPT_LMS(const float3 RGB)
      {
        float3 lms = BT709_To::IPT_LMS(RGB);

        return IPT_LMS_To::G_IPT_LMS(lms);
      }

      //RGB BT.709 -> IPT
      float3 IPT(const float3 RGB)
      {
        float3 g_lms = BT709_To::G_IPT_LMS(RGB);

        return G_IPT_LMS_To::IPT(g_lms);
      }
    } //BT709_To

  } //IPT

} //Csp
