
namespace Csp
{

  namespace Darktable_UCS
  {
    // Copyright 2022 - Aurélien PIERRE / darktable project
    // URL : https://eng.aurelienpierre.com/2022/02/color-saturation-control-for-the-21th-century/

    // - U*'V*' is pure colour coordinates
    // - JCH    is lightness (J), chroma     (C) and hue        (H)  [Helmholtz-Kohlrausch effect is corrected]
    // - HSB    is hue       (H), saturation (S) and brightness (B)  [derived from JCH so HKE is corrected]
    // - HCB    is hue       (H), chroma     (C) and brightness (B)  [derived from JCH so HKE is corrected]
    // - HPW    is hue       (H), purity     (P) and whiteness  (W)  [derived from HSB/HCB so HKE is corrected]
    //
    // - U*'V*' is perceptually uniform colour coordinates
    // - JCH    is a LCh like colour space
    //
    // - U*'V*' is for perceptually uniform gamut mapping
    // - JCH    gets you chroma amount and hue angles and is adjusted for the Helmholtz-Kohlrausch effect
    // - HSB    and HCB are intermediate colour spaces to get to specifc HPW versions
    // - HPW    is for purity artistic grading (painter's saturation)
    //
    // there are 3 implementations of HPW, each with different numerical issues:
    // - method 1: when (W^2 - S^2) is smaller than 0 it needs to be clipped
    // - method 2: slightly improves the issues of method 1
    // - method 3: adjusting Purity or Whiteness can have issues when converting back if I understand it correctly?


    namespace Y_To
    {
      //Y -> L*
      float L(const float Y)
      {
        //Y^
        float y_hat = pow(Y, 0.631651345306265);

        //L*
        float l = 2.098883786377 * y_hat
                / (y_hat + 1.12426773749357);

        return l;
      }
    } //Y_To

    namespace L_To
    {
      //L* -> Y
      float Y(const float L)
      {
        float y_num = -1.12426773749357 * L;

        float y_den = L - 2.098883786377;

        //Y
        float y = pow(y_num / y_den, 1.5831518565279648);

        return y;
      }
    } //L_To

//#define TTTTTT

    namespace xy_To
    {
#ifndef TTTTTT
      static const float3x3 xy_To_UVD =
        float3x3
        (
          -0.783941002840055,  0.277512987809202,  0.153836578598858,
           0.745273540913283, -0.205375866083878, -0.165478376301988,
           0.318707282433486,  2.16743692732158,   0.291320554395942
        );
#else
      static const float3x2 xy_To_UVD =
        float3x2
        (
          -0.783941002840055,  0.277512987809202,
           0.745273540913283, -0.205375866083878,
           0.318707282433486,  2.16743692732158
        );
#endif

      //U*V* -> U*'V*'
      static const float2x2 UV_Star_To_UV_Star_Prime =
        float2x2
        (
          -1.124983854323892, -0.980483721769325,
           1.86323315098672,   1.971853092390862
        );

      //CIE xy -> U*'V*'
      float2 UV(const float2 xy)
      {
#ifndef TTTTTT
        float3 uvd = mul(xy_To_UVD, float3(xy, 1.f));
#else
        float3 uvd = mul(xy_To_UVD, xy) + float3(0.153836578598858, -0.165478376301988, 0.291320554395942);
#endif

        float2 uv = uvd.xy / uvd.z;

        float2 uv_star = float2(1.39656225667, 1.4513954287) * uv
                       / (abs(uv) + float2(1.49217352929, 1.52488637914));

        float2 uv_star_prime = mul(UV_Star_To_UV_Star_Prime, uv_star);

        return uv_star_prime;
      }
    } //xy_To

    namespace UV_To
    {
#ifndef TTTTTT
      static const float3x3 UV_To_xyD =
        float3x3
        (
           0.167171472114775,  0.141299802443708, -0.00801531300850582,
          -0.150959086409163, -0.155185060382272, -0.00843312433578007,
           0.940254742367256,  1.0,               -0.0256325967652889
        );
#else
      static const float2x2 UV_To_xyD =
        float2x2
        (
           0.167171472114775,  0.141299802443708,
          -0.150959086409163, -0.155185060382272
        );
#endif

      //U*'V*' -> U*V*
      static const float2x2 UV_Star_Prime_To_UV_Star =
        float2x2
        (
          -5.037522385190711, -2.504856328185843,
           4.760029407436461,  2.874012963239247
        );

      //U*'V*' -> CIE xy
      float2 xy
      (
        const float2 UV_Star_Prime
      )
      {
        float2 uv_star = mul(UV_Star_Prime_To_UV_Star, UV_Star_Prime);

        float2 uv = float2(-1.49217352929, -1.52488637914) * uv_star
                  / (abs(uv_star) - float2(1.39656225667, 1.4513954287));

#ifndef TTTTTT
        float3 xyd = mul(UV_To_xyD, float3(uv, 1.f));
#else
        float2 xy = mul(UV_To_xyD, uv);

        float3 xyd = float3(xy, uv[0] * 0.940254742367256 + uv[1])
                   + float3(-0.00801531300850582, -0.00843312433578007, -0.0256325967652889);
#endif

        float2 xy = xyd.xy / xyd.z;

        return xy;
      }
    } //UV_To

    namespace xyY_To
    {
      //CIE xyY -> L*U*'V*'
      float3 LUV
      (
        const s_xyY xyY
      )
      {
        float l_star = Y_To::L(xyY.Y);

        float2 uv_star_prime = xy_To::UV(xyY.xy);

        return float3(l_star, uv_star_prime);
      }

      //CIE xyY -> L*CH
      float3 LCH
      (
        const s_xyY xyY
      )
      {
        float l_star = Y_To::L(xyY.Y);

        float2 uv_star_prime = xy_To::UV(xyY.xy);

        float c = sqrt(uv_star_prime.x * uv_star_prime.x
                     + uv_star_prime.y * uv_star_prime.y);

        float h = atan2(uv_star_prime.y, uv_star_prime.x);

        return float3(l_star, c, h);
      }

      //CIE xyY -> JCH
      float3 JCH
      (
        const s_xyY xyY,
        const float Y_White,
        const float cz
      )
      {
        //input:
        //  * xyY in normalized CIE XYZ for the 2° 1931 observer adapted for D65
        //  * L_White the lightness of white as dt UCS L* lightness. [is this the max white you want to display? like 10000 nits?]
        //  * cz: c * z
        //    * n = ratio of background luminance and the luminance of white,
        //    * z = 1 + sqrt(n)
        //    * c = 0.69 for average surround lighting (sRGB standard)
        //          0.59 for dim surround lighting
        //          0.525 for dark surround lighting
        //    * cz = 1 for standard pre-print proofing conditions with average surround and n = 20 %
        //           (background = middle grey, white = perfect diffuse white)
        //range:
        //  * xy in [0; 1]
        //  * Y normalized for perfect diffuse white = 1


        float l_star  = Y_To::L(xyY.Y);
        float l_white = Y_To::L(Y_White);

        float2 uv_star_prime = xy_To::UV(xyY.xy);

        float m2 = uv_star_prime.x * uv_star_prime.x
                 + uv_star_prime.y * uv_star_prime.y;

        float c = 15.932993652962535
                * pow(l_star, 0.6523997524738018)
                * pow(m2,    0.6007557017508491)
                / l_white;

        float j = pow(l_star / l_white, cz);

        float h = atan2(uv_star_prime.y, uv_star_prime.x);

        return float3(j, c, h);
      }
    } //xyY_To

    namespace LUV_To
    {
      //L*U*'V*' -> CIE xyY
      s_xyY xyY
      (
        const float3 LUV
      )
      {
        s_xyY xyY;

        xyY.xy = UV_To::xy(LUV.yz);

        xyY.Y = L_To::Y(LUV[0]);

        return xyY;
      }
    } //LUV_To

    namespace LCH_To
    {
      //L*CH -> CIE xyY
      s_xyY xyY
      (
        const float3 LCH
      )
      {
        float2 uv_star_prime;
        sincos(LCH[2], uv_star_prime[1], uv_star_prime[0]);

        uv_star_prime *= LCH[1];

        s_xyY xyY;

        xyY.xy = UV_To::xy(uv_star_prime);

        xyY.Y = L_To::Y(LCH[0]);

        return xyY;
      }
    } //LCH_To

    namespace JCH_To
    {
      //JCH -> CIE xyY
      s_xyY xyY
      (
        const float3 JCH,
        const float  Y_White,
        const float  cz
      )
      {
        //output: xyY in normalized CIE XYZ for the 2° 1931 observer adapted for D65
        //range:
        //  * xy in [0; 1]
        //  * Y normalized for perfect diffuse white = 1

        float J = JCH[0];
        float C = JCH[1];
        float H = JCH[2];

        float l_white = Y_To::L(Y_White);

        float l_star = pow(J, (1.f / cz)) * l_white;

        float m_num = C * l_white;
        float m_den = 15.932993652962535 * pow(l_star, 0.6523997524738018);

        float m = pow(m_num / m_den, 0.8322850678616855);

        float2 uv_star_prime;
        sincos(H, uv_star_prime[1], uv_star_prime[0]);

        uv_star_prime *= m;

        s_xyY xyY;

        xyY.xy = UV_To::xy(uv_star_prime);

        xyY.Y = L_To::Y(l_star);

        return xyY;
      }

      //JCH -> HCB
      float3 HCB(const float3 JCH)
      {
        float J = JCH[0];
        float C = JCH[1];
        float H = JCH[2];

        float B = J
                * (pow(C, 1.33654221029386) + 1.f);

        return float3(H, C, B);
      }

      //JCH -> HSB
      float3 HSB(const float3 JCH)
      {
        float J = JCH[0];
        float C = JCH[1];
        float H = JCH[2];

        float B = J
                * (pow(C, 1.33654221029386) + 1.f);

        float S = C / B;

        return float3(H, S, B);
      }
    } //JCH_To

    namespace HCB_To
    {
      //HCB -> JCH
      float3 JCH(const float3 HCB)
      {
        float H = HCB[0];
        float C = HCB[1];
        float B = HCB[2];

        float J = B
                / (pow(C, 1.33654221029386) + 1.f);

        return float3(J, C, H);
      }
    } //HCB_To

    namespace HSB_To
    {
      //HSB -> JCH
      float3 JCH(const float3 HSB)
      {
        float H = HSB[0];
        float S = HSB[1];
        float B = HSB[2];

        float C = S * B;
        float J = B
                / (pow(C, 1.33654221029386) + 1.f);

        return float3(J, C, H);
      }
    } //HSB_To

    // has numerical issues:
    // when (W^2 - S^2) is smaller than 0 it needs to be clipped
    namespace Method1
    {

      namespace HSB_To
      {
        //HSB -> HPW
        float3 HPW(const float3 HSB)
        {
          float H = HSB[0];
          float S = HSB[1];
          float B = HSB[2];

          float W = sqrt(S * S + B * B);
          float P = S / W;

          return float3(H, P, W);
        }
      } //HSB_To

      namespace HPW_To
      {
        //HPW -> HSB
        float3 HSB(const float3 HPW)
        {
          float H = HPW[0];
          float P = HPW[1];
          float W = HPW[2];

          float S = P;
          float B = sqrt(W * W - S * S);

          return float3(H, S, B);
        }
      } //HPW_To

    } //Method1

    namespace Method2
    {

      namespace HSB_To
      {
        //HSB -> HPW
        float3 HPW(const float3 HSB)
        {
          float H = HSB[0];
          float S = HSB[1];
          float B = HSB[2];

          float W = B;
          float P = W / B;

          return float3(H, P, W);
        }
      } //HSB_To

      namespace HPW_To
      {
        //HPW -> HSB
        float3 HSB(const float3 HPW)
        {
          float H = HPW[0];
          float P = HPW[1];
          float W = HPW[2];

          float S = W * P;
          float B = W;

          return float3(H, S, B);
        }
      } //HPW_To

    } //Method2

    namespace Method3
    {

      namespace HCB_To
      {
        //HCB -> HPWcc
        float3 HPWcc(const float3 HCB)
        {
          float H = HCB[0];
          float C = HCB[1];
          float B = HCB[2];

          float W = sqrt(2.f * (C * C) + (B * B));
          float P = 2.f * C / W;

          return float3(H, P, W);
        }
      } //HCB_To

      namespace HPWcc_To
      {
        //HPWcc -> HCB
        float3 HCB(const float3 HPWcc)
        {
          float H = HPWcc[0];
          float P = HPWcc[1];
          float W = HPWcc[2];

          float C = P * W / 2.f;
          float B = sqrt((W * W) - 2.f * (C * C));

          return float3(H, C, B);
        }
      } //HPWcc_To

    } //Method3

  } //Darktable_UCS

} //Csp
