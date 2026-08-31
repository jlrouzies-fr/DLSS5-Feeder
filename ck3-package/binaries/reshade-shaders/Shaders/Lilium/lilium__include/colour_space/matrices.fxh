
namespace Csp
{

  namespace Mat
  {

    //BT.709 To
    //BT.709 -> XYZ
    static const float3x3 BT709_To_XYZ =
      float3x3
      (
        0.412390798f,  0.357584327f, 0.180480793f,
        0.212639003f,  0.715168654f, 0.0721923187f,
        0.0193308182f, 0.119194783f, 0.950532138f
      );

    //BT.709 -> DCI-P3
    static const float3x3 BT709_To_DCIP3 =
      float3x3
      (
        0.822461962f,  0.177538037f,  0.f,
        0.0331941992f, 0.966805815f,  0.f,
        0.0170826315f, 0.0723974406f, 0.910519957f
      );

    //BT.709 -> BT.2020
    static const float3x3 BT709_To_BT2020 =
      float3x3
      (
        0.627403914f,  0.329283028f,  0.0433130674f,
        0.0690972879f, 0.919540405f,  0.0113623151f,
        0.0163914393f, 0.0880133062f, 0.895595252f
      );

    //BT.709 -> AP1 D65
    static const float3x3 BT709_To_AP1D65 =
      float3x3
      (
        0.617028832f,  0.333867609f, 0.0491035431f,
        0.0699223205f, 0.917349696f, 0.0127279674f,
        0.0205497872f, 0.107552029f, 0.871898174f
      );

    //BT.709 -> AP0 D65
    static const float3x3 BT709_To_AP0D65 =
      float3x3
      (
        0.433931618f,  0.376252382f, 0.189815968f,
        0.0886183902f, 0.809275329f, 0.102106288f,
        0.0177500396f, 0.109447620f, 0.872802317f
      );

    //BT.709 -> AP1 D60 (Bradford CAT)
    static const float3x3 BT709_To_AP1D60 =
      float3x3
      (
        0.613097429f,  0.339523136f, 0.0473794527f,
        0.0701937228f, 0.916353881f, 0.0134523985f,
        0.0206155925f, 0.109569773f, 0.869814634f
      );

    //BT.709 -> AP0 D60 (Bradford CAT)
    static const float3x3 BT709_To_AP0D60 =
      float3x3
      (
        0.439632982f,  0.382988691f, 0.177378326f,
        0.0897764414f, 0.813439428f, 0.0967841297f,
        0.0175411701f, 0.111546553f, 0.870912253f
      );


    //scRGB To
    //scRGB -> XYZ normalised
    static const float3x3 scRGB_To_XYZ_normalised =
      float3x3
      (
        0.00329912640f,  0.00286067463f,  0.00144384626f,
        0.00170111202f,  0.00572134926f,  0.000577538507f,
        0.000154646550f, 0.000953558250f, 0.00760425720f
      );

    //scRGB -> BT.2020 normalised
    static const float3x3 scRGB_To_BT2020_normalised =
      float3x3
      (
        0.00501923123f,  0.00263426429f,  0.000346504530f,
        0.000552778306f, 0.00735632330f,  0.0000908985239f,
        0.000131131513f, 0.000704106467f, 0.00716476188f
      );


    //DCI-P3 To
    //DCI-P3 -> XYZ
    static const float3x3 DCIP3_To_XYZ =
      float3x3
      (
        0.486570954f, 0.265667706f,  0.198217287f,
        0.228974565f, 0.691738545f,  0.0792869105f,
        0.f,          0.0451133809f, 1.04394435f
      );

    //DCI-P3 -> BT.709
    static const float3x3 DCIP3_To_BT709 =
      float3x3
      (
         1.22494018f,   -0.224940180f,  0.f,
        -0.0420569553f,  1.04205691f,   0.f,
        -0.0196375548f, -0.0786360427f, 1.09827363f
      );

    //DCI-P3 -> BT.2020
    static const float3x3 DCIP3_To_BT2020 =
      float3x3
      (
         0.753833055f,   0.198597371f,  0.0475695952f,
         0.0457438491f,  0.941777229f,  0.0124789308f,
        -0.00121034029f, 0.0176017172f, 0.983608603f
      );


    //DCI-P3 80 To
    //DCI-P3 80 -> XYZ normalised
    static const float3x3 DCIP3_80_To_XYZ_normalised =
      float3x3
      (
        0.00389256747f, 0.00212534144f,  0.00158573826f,
        0.00183179648f, 0.00553390802f,  0.000634295283f,
        0.f,            0.000360907055f, 0.00835155509f
      );

    //DCI-P3 80 -> BT.2020 normalised
    static const float3x3 DCIP3_80_To_BT2020_normalised =
      float3x3
      (
         0.00603066431f,    0.00158877891f,  0.000380556768f,
         0.000365950778f,   0.00753421755f,  0.0000998314499f,
        -0.00000968272252f, 0.000140813732f, 0.00786886923f
      );


    //BT.2020 To
    //BT.2020 -> XYZ
    static const float3x3 BT2020_To_XYZ =
      float3x3
      (
        0.636958062f, 0.144616901f,  0.168880969f,
        0.262700200f, 0.677998065f,  0.0593017153f,
        0.f,          0.0280726924f, 1.06098508f
      );

    //BT.2020 -> BT.709
    static const float3x3 BT2020_To_BT709 =
      float3x3
      (
         1.66049098f,   -0.587641119f, -0.0728498622f,
        -0.124550476f,   1.13289988f,  -0.00834942236f,
        -0.0181507635f, -0.100578896f,  1.11872971f
      );

    //BT.2020 -> DCI-P3
    static const float3x3 BT2020_To_DCIP3 =
      float3x3
      (
         1.34357821f,    -0.282179683f,  -0.0613985806f,
        -0.0652974545f,   1.07578790f,   -0.0104904631f,
         0.00282178726f, -0.0195984952f,  1.01677668f
      );

    //BT.2020 -> AP1 D65
    static const float3x3 BT2020_To_AP1D65 =
      float3x3
      (
        0.982096254f,   0.0107082454f, 0.00719551974f,
        0.00161802524f, 0.996895968f,  0.00148598209f,
        0.00490146316f, 0.0220752228f, 0.973023295f
      );

    //BT.2020 -> AP0 D65
    static const float3x3 BT2020_To_AP0D65 =
      float3x3
      (
        0.670231819f,  0.152168750f,  0.177599415f,
        0.0445011146f, 0.854482352f,  0.101016514f,
        0.f,           0.0257770475f, 0.974222958f
      );

    //BT.2020 -> AP1 D60 (Bradford CAT)
    static const float3x3 BT2020_To_AP1D60 =
      float3x3
      (
        0.974895000f,   0.0195991080f, 0.00550591340f,
        0.00217956281f, 0.995535492f,  0.00228496827f,
        0.00479723978f, 0.0245320163f, 0.970670759f
      );

    //BT.2020 -> AP0 D60 (Bradford CAT)
    static const float3x3 BT2020_To_AP0D60 =
      float3x3
      (
         0.679085612f,    0.157700911f,  0.163213446f,
         0.0460020042f,   0.859054684f,  0.0949433222f,
        -0.000573943194f, 0.0284677688f, 0.972106158f
      );


    //BT.2020 normalised To
    //BT.2020 normalised -> scRGB
    static const float3x3 BT2020_normalised_To_scRGB =
      float3x3
      (
         207.561370f, -73.4551391f, -9.10623264f,
        -15.5688095f,  141.612487f, -1.04367780f,
        -2.26884531f, -12.5723619f,  139.841201f
      );


    //BT.2020 80 To
    //BT.2020 80 -> XYZ normalised
    static const float3x3 BT2020_80_To_XYZ_normalised =
      float3x3
      (
        0.00509566441f, 0.00115693523f,  0.00135104777f,
        0.00210160179f, 0.00542398449f,  0.000474413740f,
        0.f,            0.000224581541f, 0.00848788022f
      );


    //AP1 D65 To
    //AP1 D65 -> XYZ
    static const float3x3 AP1D65_To_XYZ =
      float3x3
      (
         0.647507190f,   0.134379133f,   0.168569594f,
         0.266086399f,   0.675967812f,   0.0579457953f,
        -0.00544886849f, 0.00407209526f, 1.09043455f
      );


    //AP0 D65 To
    //AP0 D65 -> XYZ
    static const float3x3 AP0D65_To_XYZ =
      float3x3
      (
        0.950354814f, 0.f,           0.000101128956f,
        0.343172907f, 0.734696388f, -0.0778692960f,
        0.f,          0.f,           1.08905780f
      );

    //AP0 D65 -> BT.709
    static const float3x3 AP0D65_To_BT709 =
      float3x3
      (
         2.55248308f,   -1.12950992f,  -0.422973215f,
        -0.277344137f,   1.37826657f,  -0.100922435f,
        -0.0171310510f, -0.149861142f,  1.16699218f
      );

    //AP0 D65 -> BT.2020
    static const float3x3 AP0D65_To_BT2020 =
      float3x3
      (
         1.50937116f,    -0.261310040f,  -0.248061075f,
        -0.0788541212f,   1.18762290f,   -0.108768820f,
         0.00208640797f, -0.0314234159f,  1.02933704f
      );


    //AP1 D60 To
    //AP1 D60 -> XYZ
    static const float3x3 AP1D60_To_XYZ =
      float3x3
      (
         0.662454187f,   0.134004205f,   0.156187683f,
         0.272228717f,   0.674081742f,   0.0536895170f,
        -0.00557464966f, 0.00406073359f, 1.01033914f
      );

    //AP1 D60 -> BT.709 (Bradford CAT)
    static const float3x3 AP1D60_To_BT709 =
      float3x3
      (
         1.70505094f,   -0.621792137f, -0.0832588747f,
        -0.130256414f,   1.14080476f,  -0.0105483187f,
        -0.0240033566f, -0.128968968f,  1.15297234f
      );

    //AP1 D60 -> BT.2020 (Bradford CAT)
    static const float3x3 AP1D60_To_BT2020 =
      float3x3
      (
         1.02582478f,    -0.0200531911f, -0.00577155686f,
        -0.00223436951f,  1.00458645f,   -0.00235213246f,
        -0.00501335132f, -0.0252900719f,  1.03030347f
      );

    //AP1 D60 -> AP0 D60
    static const float3x3 AP1D60_To_AP0D60 =
      float3x3
      (
         0.695452213f,   0.140678703f,   0.163869068f,
         0.0447945632f,  0.859671115f,   0.0955343171f,
        -0.00552588235f, 0.00402521016f, 1.00150072f
      );


    //AP0 D60 To
    //AP0 D60 -> XYZ
    static const float3x3 AP0D60_To_XYZ =
      float3x3
      (
        0.952552378f, 0.f,           0.0000936786309f,
        0.343966454f, 0.728166103f, -0.0721325427f,
        0.f,          0.f,           1.00882518f
      );

    //AP0 D60 -> BT.709 (Bradford CAT)
    static const float3x3 AP0D60_To_BT709 =
      float3x3
      (
         2.52168607f,   -1.13413095f,  -0.387555211f,
        -0.276479899f,   1.37271904f,  -0.0962391719f,
        -0.0153780654f, -0.152975335f,  1.16835343f
      );

    //AP0 D60 -> BT.2020 (Bradford CAT)
    static const float3x3 AP0D60_To_BT2020 =
      float3x3
      (
         1.49040949f,    -0.266170918f,  -0.224238604f,
        -0.0801675021f,   1.18216717f,   -0.101999618f,
         0.00322763109f, -0.0347764752f,  1.03154885f
      );

    //AP0 D60 -> AP1 D60
    static const float3x3 AP0D60_To_AP1D60 =
      float3x3
      (
         1.45143926f,    -0.236510753f,   -0.214928567f,
        -0.0765537768f,   1.17622971f,    -0.0996759235f,
         0.00831614807f, -0.00603244965f,  0.997716307f
      );


    //XYZ To
    //XYZ -> BT.709
    static const float3x3 XYZ_To_BT709 =
      float3x3
      (
         3.24096989f,   -1.53738319f,  -0.498610764f,
        -0.969243645f,   1.87596750f,   0.0415550582f,
         0.0556300804f, -0.203976958f,  1.05697154f
      );

    //XYZ normalised -> scRGB
    static const float3x3 XYZ_normalised_To_scRGB =
      float3x3
      (
         0.0259277597f,   -0.0122990654f,  -0.00398888625f,
        -0.00775394914f,   0.0150077398f,   0.000332440453f,
         0.000445040641f, -0.00163181568f,  0.00845577195f
      );

    //XYZ -> DCI-P3
    static const float3x3 XYZ_To_DCIP3 =
      float3x3
      (
         2.49349689f,   -0.931383609f,  -0.402710795f,
        -0.829488992f,   1.76266407f,    0.0236246865f,
         0.0358458310f, -0.0761723890f,  0.956884503f
      );

    //XYZ -> BT.2020
    static const float3x3 XYZ_To_BT2020 =
      float3x3
      (
         1.71665120f,   -0.355670779f,  -0.253366291f,
        -0.666684329f,   1.61648118f,    0.0157685466f,
         0.0176398567f, -0.0427706129f,  0.942103147f
      );

    //XYZ -> AP1 D65
    static const float3x3 XYZ_To_AP1D65 =
      float3x3
      (
         1.67890453f,   -0.332301020f,   -0.241882294f,
        -0.661811172f,   1.61082458f,     0.0167095959f,
         0.0108608892f, -0.00767592666f,  0.915794551f
      );

    //XYZ -> AP0 D65
    static const float3x3 XYZ_To_AP0D65 =
      float3x3
      (
         1.05223858f,  0.f,         -0.0000977099625f,
        -0.491495221f, 1.36110639f,  0.0973668321f,
         0.f,          0.f,          0.918224930f
      );


    namespace BT709_To
    {
      float3 XYZ(const float3 RGB)
      {
        return mul(BT709_To_XYZ, RGB);
      }

      float3 DCIP3(const float3 RGB)
      {
        return mul(BT709_To_DCIP3, RGB);
      }

      float3 BT2020(const float3 RGB)
      {
        return mul(BT709_To_BT2020, RGB);
      }

      float3 AP1D65(const float3 RGB)
      {
        return mul(BT709_To_AP1D65, RGB);
      }

      float3 AP0D65(const float3 RGB)
      {
        return mul(BT709_To_AP0D65, RGB);
      }

      float3 AP1D60(const float3 RGB)
      {
        return mul(BT709_To_AP1D60, RGB);
      }

      float3 AP0D60(const float3 RGB)
      {
        return mul(BT709_To_AP0D60, RGB);
      }
    } //BT709_To

    namespace scRGB_To
    {
      float3 BT2020_normalised(const float3 RGB)
      {
        return mul(scRGB_To_BT2020_normalised, RGB);
      }
    } //scRGB_To

    namespace DCIP3_To
    {
      float3 XYZ(const float3 RGB)
      {
        return mul(DCIP3_To_XYZ, RGB);
      }

      float3 BT709(const float3 RGB)
      {
        return mul(DCIP3_To_BT709, RGB);
      }

      float3 BT2020(const float3 RGB)
      {
        return mul(DCIP3_To_BT2020, RGB);
      }
    } //DCIP3_To

    namespace DCIP3_80_To
    {
      float3 XYZ_normalised(const float3 RGB)
      {
        return mul(DCIP3_80_To_XYZ_normalised, RGB);
      }

      float3 BT2020_normalised(const float3 RGB)
      {
        return mul(DCIP3_80_To_BT2020_normalised, RGB);
      }
    } //DCIP3_80_To

    namespace BT2020_To
    {
      float3 XYZ(const float3 RGB)
      {
        return mul(BT2020_To_XYZ, RGB);
      }

      float3 BT709(const float3 RGB)
      {
        return mul(BT2020_To_BT709, RGB);
      }

      float3 DCIP3(const float3 RGB)
      {
        return mul(BT2020_To_DCIP3, RGB);
      }

      float3 AP1D65(const float3 RGB)
      {
        return mul(BT2020_To_AP1D65, RGB);
      }

      float3 AP0D65(const float3 RGB)
      {
        return mul(BT2020_To_AP0D65, RGB);
      }

      float3 AP1D60(const float3 RGB)
      {
        return mul(BT2020_To_AP1D60, RGB);
      }

      float3 AP0D60(const float3 RGB)
      {
        return mul(BT2020_To_AP0D60, RGB);
      }
    } //BT2020_To

    namespace BT2020_normalised_To
    {
      float3 scRGB(const float3 RGB)
      {
        return mul(BT2020_normalised_To_scRGB, RGB);
      }
    } //BT2020_normalised_To

    namespace BT2020_80_To
    {
      float3 XYZ_normalised(const float3 RGB)
      {
        return mul(BT2020_80_To_XYZ_normalised, RGB);
      }
    } //BT2020_normalised_To

    namespace AP1D65_To
    {
      float3 XYZ(const float3 RGB)
      {
        return mul(AP1D65_To_XYZ, RGB);
      }
    } //AP1D65_To

    namespace AP0D65_To
    {
      float3 XYZ(const float3 RGB)
      {
        return mul(AP0D65_To_XYZ, RGB);
      }

      float3 BT709(const float3 RGB)
      {
        return mul(AP0D65_To_BT709, RGB);
      }

      float3 BT2020(const float3 RGB)
      {
        return mul(AP0D65_To_BT2020, RGB);
      }
    } //AP0D65_To

    namespace AP1D60_To
    {
      float3 BT709(const float3 RGB)
      {
        return mul(AP1D60_To_BT709, RGB);
      }

      float3 BT2020(const float3 RGB)
      {
        return mul(AP1D60_To_BT2020, RGB);
      }

      float3 AP0D60(const float3 RGB)
      {
        return mul(AP1D60_To_AP0D60, RGB);
      }
    } //AP1D60_To

    namespace AP0D60_To
    {
      float3 BT709(const float3 RGB)
      {
        return mul(AP0D60_To_BT709, RGB);
      }

      float3 BT2020(const float3 RGB)
      {
        return mul(AP0D60_To_BT2020, RGB);
      }

      float3 AP1D60(const float3 RGB)
      {
        return mul(AP0D60_To_AP1D60, RGB);
      }
    } //AP0D60_To

    namespace XYZ_To
    {
      float3 BT709(const float3 XYZ)
      {
        return mul(XYZ_To_BT709, XYZ);
      }

      float3 DCIP3(const float3 XYZ)
      {
        return mul(XYZ_To_DCIP3, XYZ);
      }

      float3 BT2020(const float3 XYZ)
      {
        return mul(XYZ_To_BT2020, XYZ);
      }

      float3 AP1D65(const float3 XYZ)
      {
        return mul(XYZ_To_AP1D65, XYZ);
      }

      float3 AP0D65(const float3 XYZ)
      {
        return mul(XYZ_To_AP0D65, XYZ);
      }
    } //XYZ_To

  } //Mat

} //Csp
