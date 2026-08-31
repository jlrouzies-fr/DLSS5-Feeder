
namespace Csp
{

  namespace YCbCr
  {

    //#define K_BT709  float3(0.2126f, 0.7152f, 0.0722f)
    //#define K_BT2020 float3(0.2627f, 0.6780f, 0.0593f)

    //#define KB_BT709_HELPER 1.8556f //2 - 2 * 0.0722
    //#define KR_BT709_HELPER 1.5748f //2 - 2 * 0.2126
    //#define KG_BT709_HELPER float2(0.187324272930648, 0.468124272930648)
    //(0.0722/0.7152)*(2-2*0.0722), (0.2126/0.7152)*(2-2*0.2126)

    //#define KB_BT2020_HELPER 1.8814f //2 - 2 * 0.0593
    //#define KR_BT2020_HELPER 1.4746f //2 - 2 * 0.2627
    //#define KG_BT2020_HELPER float2(0.164553126843658, 0.571353126843658)
    //(0.0593/0.6780)*(2-2*0.0593), (0.2627/0.6780)*(2-2*0.2627)

    //Y'Cb'Cr non constant luminance
    //BT.709
    static const float3 K_BT709 = float3(0.212639003f,
                                         0.715168654f,
                                         0.0721923187f);
    static const float  KB_enc_BT709 = 0.538904786f;
    static const float  KR_enc_BT709 = 0.635032713f;
    static const float  KB_dec_BT709 = 1.85561537f;
    static const float  KR_dec_BT709 = 1.57472193f;
    static const float2 KG_dec_BT709 = float2(0.187314093f,
                                              0.468207478f);
    //BT.2020
    static const float3 K_BT2020 = float3(0.262700200f,
                                          0.677998065f,
                                          0.0593017153f);
    static const float  KB_enc_BT2020 = 0.531520068f;
    static const float  KR_enc_BT2020 = 0.678150177f;
    static const float  KB_dec_BT2020 = 1.88139653f;
    static const float  KR_dec_BT2020 = 1.47459959f;
    static const float2 KG_dec_BT2020 = float2(0.164558053f,
                                               0.571355044f);

    //Y'cCbc'Crc constant luminance for gamma 2
    //BT.709
    static const float2 PB_NB_BT709_g2_enc = float2(0.683701097f,
                                                    0.519088029f);
    static const float2 PR_NR_BT709_g2_enc = float2(0.927864074f,
                                                    0.563485920f);
    static const float2 PB_NB_BT709_g2_dec = float2(1.46262741f,
                                                    1.92645549f);
    static const float2 PR_NR_BT709_g2_dec = float2(1.07774400f,
                                                    1.77466726f);
    static const float  K_BT709G_inverse   = 1.39827156f;
    //BT.2020
    static const float2 PB_NB_BT2020_g2_enc = float2(0.660955488f,
                                                     0.515519201f);
    static const float2 PR_NR_BT2020_g2_enc = float2(1.02573120f,
                                                     0.582301557f);
    static const float2 PB_NB_BT2020_g2_dec = float2(1.51296114f,
                                                     1.93979203f);
    static const float2 PR_NR_BT2020_g2_dec = float2(0.974914252f,
                                                     1.71732330f);
    static const float  K_BT2020G_inverse   = 1.47493040f;


    namespace YCbCr_To
    {

      float3 RGB_BT709(const float3 YCbCr)
      {
        const float Y  = YCbCr[0];
        const float Cb = YCbCr[1];
        const float Cr = YCbCr[2];

        float3 rgb;

        rgb.r = Y + KR_dec_BT709    * Cr;
        rgb.g = Y - KG_dec_BT709[0] * Cb - KG_dec_BT709[1] * Cr;
        rgb.b = Y + KB_dec_BT709    * Cb;

        return rgb;
      }

      float3 RGB_BT2020(const float3 YCbCr)
      {
        const float Y  = YCbCr[0];
        const float Cb = YCbCr[1];
        const float Cr = YCbCr[2];

        float3 rgb;

        rgb.r = Y + KR_dec_BT2020    * Cr;
        rgb.g = Y - KG_dec_BT2020[0] * Cb - KG_dec_BT2020[1] * Cr;
        rgb.b = Y + KB_dec_BT2020    * Cb;

        return rgb;
      }

    } //YCbCr_To


    namespace RGB_To
    {

      float3 YCbCr_BT709(const float3 RGB)
      {
        float y = dot(RGB, K_BT709);

        float cb = (RGB.b - y) * KB_enc_BT709;
        float cr = (RGB.r - y) * KR_enc_BT709;

        return float3(y, cb, cr);
      }

      float3 YCbCr_BT2020(const float3 RGB)
      {
        float y = dot(RGB, K_BT2020);

        float cb = (RGB.b - y) * KB_enc_BT2020;
        float cr = (RGB.r - y) * KR_enc_BT2020;

        return float3(y, cb, cr);
      }

    } //RGB_To

  } //YCbCr

} //Csp
