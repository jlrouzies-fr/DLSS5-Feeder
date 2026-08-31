
namespace Csp
{

  namespace OKLab
  {

//  Copyright (c) 2021 Björn Ottosson
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of
//  this software and associated documentation files (the "Software"), to deal in
//  the Software without restriction, including without limitation the rights to
//  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
//  of the Software, and to permit persons to whom the Software is furnished to do
//  so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//  SOFTWARE.

    //RGB BT.709 -> OKLMS
    static const float3x3 BT709_To_OKLMS =
      float3x3
      (
        0.412176460f,  0.536273956f, 0.0514403730f,
        0.211909204f,  0.680717885f, 0.107399843f,
        0.0883448123f, 0.281853973f, 0.630280852f
      );

    //RGB BT.2020 -> OKLMS
    static const float3x3 BT2020_To_OKLMS =
      float3x3
      (
        0.616688430f, 0.360159069f, 0.0230432935f,
        0.265140205f, 0.635856509f, 0.0990302339f,
        0.100150644f, 0.204004317f, 0.696324706f
      );

    //OKL'M'S' -> OKLab
    static const float3x3 G3_OKLMS_To_OKLab =
      float3x3
      (
        0.2104542553f,  0.7936177850f, -0.0040720468f,
        1.9779984951f, -2.4285922050f,  0.4505937099f,
        0.0259040371f,  0.7827717662f, -0.8086757660f
      );

    //OKLab -> OKL'M'S'
    static const float3x3 OKLab_To_G3_OKLMS =
      float3x3
      (
        1.f,  0.3963377774f,  0.2158037573f,
        1.f, -0.1055613458f, -0.0638541728f,
        1.f, -0.0894841775f, -1.2914855480f
      );

    //OKLMS -> RGB BT.709
    static const float3x3 OKLMS_To_BT709 =
      float3x3
      (
         4.07718706f,    -3.30762243f,   0.230859190f,
        -1.26857650f,     2.60968708f,  -0.341155737f,
        -0.00419654231f, -0.703399658f,  1.70679605f
      );

    //OKLMS -> RGB BT.2020
    static const float3x3 OKLMS_To_BT2020 =
      float3x3
      (
         2.14014029f,   -1.24635589f,   0.106431722f,
        -0.884832441f,   2.16317272f,  -0.278361588f,
        -0.0485790595f, -0.454490900f,  1.50235629f
      );


// provided matrices
//
//    //RGB BT.709 -> OKLMS
//    static const float3x3 BT709_To_OKLMS =
//      float3x3
//      (
//        0.4122214708f, 0.5363325363f, 0.0514459929f,
//        0.2119034982f, 0.6806995451f, 0.1073969566f,
//        0.0883024619f, 0.2817188376f, 0.6299787005f
//      );
//
//    //OKLMS -> RGB BT.709
//    static const float3x3 OKLMS_To_BT709 =
//      float3x3
//      (
//         4.0767416621f, -3.3077115913f,  0.2309699292f,
//        -1.2684380046f,  2.6097574011f, -0.3413193965f,
//        -0.0041960863f, -0.7034186147f,  1.7076147010f
//      );

    namespace OKLab_To
    {

      //OKLab -> OKLCh°
      float3 OKLCh(const float3 Lab)
      {
        const float L = Lab[0];
        const float a = Lab[1];
        const float b = Lab[2];

        float c = sqrt(a * a
                     + b * b);

        float h = atan2(b, a);

        return float3(L, c, h);
      }

    } //OKLab_To

    namespace OKLCh_To
    {

      //OKLCh° -> OKLab
      float3 OKLab(const float3 LCh)
      {
        const float L = LCh[0];
        const float C = LCh[1];
        const float h = LCh[2];

        float2 ab;
        sincos(h, ab[1], ab[0]);

        ab *= C;

        return float3(L, ab);
      }

    } //OKLCh_To

    namespace OKLMS_To
    {

      //OKLMS -> OKL'M'S'
      float3 G3_OKLMS(const float3 LMS)
      {
        //apply gamma 3
        return sign(LMS) * pow(abs(LMS), 1.f / 3.f);
      }

    } //OKLMS_To

    namespace G3_OKLMS_To
    {

      //OKL'M'S' -> OKLMS
      float3 OKLMS(const float3 G3_LMS)
      {
        //remove gamma 3
        return G3_LMS * G3_LMS * G3_LMS;
      }

      //OKL'M'S' -> OKLab
      float3 OKLab(const float3 G3_LMS)
      {
        return mul(G3_OKLMS_To_OKLab, G3_LMS);
      }

    } //G3_OKLMS_To

    namespace OKLMS_To
    {

      //OKLMS -> OKLab
      float3 OKLab(const float3 LMS)
      {
        float3 g3_lms = OKLMS_To::G3_OKLMS(LMS);

        return G3_OKLMS_To::OKLab(g3_lms);
      }

    } //OKLMS_To

    namespace OKLab_To
    {

      //OKLab -> OKL'M'S'
      float3 G3_OKLMS(const float3 Lab)
      {
        return mul(OKLab_To_G3_OKLMS, Lab);
      }

      //OKLab -> OKLMS
      float3 OKLMS(const float3 Lab)
      {
        float3 g3_lms = OKLab_To::G3_OKLMS(Lab);

        return G3_OKLMS_To::OKLMS(g3_lms);
      }

    } //OKLab_To

    namespace BT709_To
    {

      //RGB BT.709 -> OKLab
      float3 OKLab(const float3 RGB)
      {
        float3 lms = mul(BT709_To_OKLMS, RGB);

        return OKLMS_To::OKLab(lms);
      }

    } //BT709_To

    namespace BT2020_To
    {

      //RGB BT.2020 -> OKLab
      float3 OKLab(const float3 RGB)
      {
        float3 lms = mul(BT2020_To_OKLMS, RGB);

        return OKLMS_To::OKLab(lms);
      }

    } //BT2020_To

    namespace OKLab_To
    {

      //OKLab -> RGB BT.709
      float3 BT709(const float3 Lab)
      {
        float3 lms = OKLab_To::OKLMS(Lab);

        return mul(OKLMS_To_BT709, lms);
      }

      //OKLab -> RGB BT.2020
      float3 BT2020(const float3 Lab)
      {
        float3 lms = OKLab_To::OKLMS(Lab);

        return mul(OKLMS_To_BT2020, lms);
      }

    } //OKLab_To


    // Finds the maximum saturation possible for a given hue that fits in sRGB
    // Saturation here is defined as S = C/L
    // a and b must be normalized so a^2 + b^2 == 1
    float Compute_Max_Saturation(const float2 ab)
    {
      // Max saturation will be when one of r, g or b goes below zero.

      // Select different coefficients depending on which component goes below zero first
      float k0, k1, k2, k3, k4;

      float3 w_lms;

      if (-1.88170328f * ab.x - 0.80936493f * ab.y > 1)
      {
        // Red component
        k0 = 1.19086277f;
        k1 = 1.76576728f;
        k2 = 0.59662641f;
        k3 = 0.75515197f;
        k4 = 0.56771245f;

        w_lms.rgb = OKLMS_To_BT709[0].rgb;
      }
      else if (1.81444104f * ab.x - 1.19445276f * ab.y > 1)
      {
        // Green component
        k0 =  0.73956515f;
        k1 = -0.45954404f;
        k2 =  0.08285427f;
        k3 =  0.12541070f;
        k4 =  0.14503204f;

        w_lms.rgb = OKLMS_To_BT709[1].rgb;
      }
      else
      {
        // Blue component
        k0 =  1.35733652f;
        k1 = -0.00915799f;
        k2 = -1.15130210f;
        k3 = -0.50559606f;
        k4 =  0.00692167f;

        w_lms.rgb = OKLMS_To_BT709[2].rgb;
      }

      // Approximate max saturation using a polynomial:
      float s = k0
              + k1 * ab.x
              + k2 * ab.y
              + k3 * ab.x * ab.x
              + k4 * ab.x * ab.y;

      // Do one step Halley's method to get closer
      // this gives an error less than 10e6, except for some blue hues where the dS/dh is close to infinite
      // this should be sufficient for most applications, otherwise do two/three steps

      float3 k_lms = mul(OKLab_To_G3_OKLMS, float3(0.f, ab));

      {
        float3 g3_lms = 1.f + s * k_lms;

        float3 intermediate_lms = g3_lms * g3_lms;

        float3 lms = intermediate_lms * g3_lms;

        float3 g3_lms_ds  = 3.f * k_lms * intermediate_lms;
        float3 g3_lms_ds2 = 6.f * k_lms * k_lms * g3_lms;

        float3 f = mul(float3x3(lms,
                                g3_lms_ds,
                                g3_lms_ds2), w_lms);

        s = s
          - f.x * f.y
          / (f.y * f.y - 0.5f * f.x * f.z);
      }

      return s;
    }

    // finds L_cusp and C_cusp for a given hue
    // a and b must be normalized so a^2 + b^2 == 1
    float2 Find_Cusp(const float2 ab)
    {
      // First, find the maximum saturation (saturation S = C/L)
      float s_cusp = Compute_Max_Saturation(ab);

      // Convert to linear sRGB to find the first point where at least one of r, g or b >= 1:
      float3 rgb_at_max = OKLab_To::BT709(float3(1.f, s_cusp * ab));

      float2 lc_cusp;

      lc_cusp.x = pow(1.f / max(max(rgb_at_max.r, rgb_at_max.g), rgb_at_max.b), 1.f / 3.f);
      lc_cusp.y = lc_cusp.x * s_cusp;

      return lc_cusp;
    }

    float2 To_St(const float2 LC)
    {
      return LC.y / float2(LC.x,
                           1.f - LC.x);
    }

    static const float Toe_K1 = 0.206f;
    static const float Toe_K2 = 0.03f;
    static const float Toe_K3 = (1.f + Toe_K1) / (1.f + Toe_K2);

    // toe function for L_r
    float Toe(const float X)
    {
      float i0 = Toe_K3 * X - Toe_K1;

      return 0.5f * (i0 + sqrt(i0 * i0 + 4.f * Toe_K2 * Toe_K3 * X));
    }

    // inverse toe function for L_r
    float Toe_Inv(const float X)
    {
      return (X * X      + Toe_K1 * X)
           / (X * Toe_K3 + Toe_K2 * Toe_K3);
    }

    float2 Get_St_Mid(const float2 ab)
    {

      float s = 0.11516993f
              + 1.f / (7.44778970f
                     + 4.15901240f * ab.y
                     + ab.x * (-2.19557347f
                              + 1.75198401f * ab.y
                              + ab.x * (-2.13704948f
                                      - 10.02301043f * ab.y
                                      + ab.x * (-4.24894561f
                                               + 5.38770819f * ab.y
                                               + 4.69891013f * ab.x))));

      float t = 0.11239642f
              + 1.f / (1.61320320f
                     - 0.68124379f * ab.y
                     + ab.x * (0.40370612f
                             + 0.90148123f * ab.y
                             + ab.x * (-0.27087943f
                                      + 0.61223990f * ab.y
                                      + ab.x * (0.00299215f
                                              - 0.45399568f * ab.y
                                              - 0.14661872f * ab.x))));

      return float2(s, t);
    }

    float Find_Gamut_Intersection
    (
      float2 ab,
      float  L1,
      float  C1,
      float  L0,
      float2 Cusp
    )
    {
      // Find the intersection for upper and lower half seprately
      float t;

      const float check = (L1     - L0) * Cusp.y
                        - (Cusp.x - L0) * C1;

      if (check <= 0.f)
      {
        // Lower half
        t = Cusp.y * L0
          / (C1 * Cusp.x + Cusp.y * (L0 - L1));
      }
      else
      {
        // Upper half
        // First intersect with triangle
        t = Cusp.y * (L0 - 1.f)
          / (C1 * (Cusp.x - 1.f) + Cusp.y * (L0 - L1));

        // Then one step Halley's method
        {
          float d_l = L1 - L0;
          float d_c = C1;

          float3 k_lms = mul(OKLab_To_G3_OKLMS, float3(0.f, ab));

          float3 g3_lms_dt = d_l + d_c * k_lms;

          // If higher accuracy is required, 2 or 3 iterations of the following block can be used:
          {
            float L = L0 * (1.f - t) + t * L1;
            float C = t * C1;

            float3 g3_lms = L + C * k_lms;

            float3 intermediate_lms = g3_lms * g3_lms;

            float3 lms = g3_lms * intermediate_lms;

            float3 lms_dt  = 3.f * g3_lms_dt * intermediate_lms;
            float3 lms_dt2 = 6.f * g3_lms_dt * g3_lms_dt * g3_lms;

            const float3x3 i_lms = float3x3(lms.xyz,
                                            lms_dt.xyz,
                                            lms_dt2.xyz);

            const float3 i_r = OKLMS_To_BT709[0].rgb;

            const float3 i_g = OKLMS_To_BT709[1].rgb;

            const float3 i_b = OKLMS_To_BT709[2].rgb;

            float3 r = mul(i_lms, i_r);

            r.x -= 1.f;

            float u_r = r.y / (r.y * r.y - 0.5f * r.x * r.z);
            float t_r = -r.x * u_r;

            float3 g = mul(i_lms, i_g);

            g.x -= 1.f;

            float u_g = g.y / (g.y * g.y - 0.5f * g.x * g.z);
            float t_g = -g.x * u_g;

            float3 b = mul(i_lms, i_b);

            b.x -= 1.f;

            float u_b = b.y / (b.y * b.y - 0.5f * b.x * b.z);
            float t_b = -b.x * u_b;

            t_r = u_r >= 0.f ? t_r : FP32_MAX;
            t_g = u_g >= 0.f ? t_g : FP32_MAX;
            t_b = u_b >= 0.f ? t_b : FP32_MAX;

            t += min(t_r, min(t_g, t_b));
          }
        }
      }

      return t;
    }

    float3 Get_Cs(const float3 Lab)
    {
      const float L = Lab[0];
      const float a = Lab[1];
      const float b = Lab[2];

      const float2 ab = float2(a, b);

      float2 cusp = Find_Cusp(ab);

      float   c_max = Find_Gamut_Intersection(ab, L, 1.f, L, cusp);
      float2 st_max = To_St(cusp);

      // Scale factor to compensate for the curved part of gamut shape:
      float k = c_max / min((L * st_max.x),
                            (1.f - L) * st_max.y);

      float c_mid;
      {
        float2 st_mid = Get_St_Mid(ab);

        // Use a soft minimum function, instead of a sharp triangle shape to get a smooth value for chroma.
        float2 c_ab = L * st_mid;

        c_ab.y = st_mid.y - c_ab.y;

        c_mid = 0.9f * k * sqrt(sqrt(1.f
                                   / (1.f / (c_ab.x * c_ab.x * c_ab.x * c_ab.x)
                                    + 1.f / (c_ab.y * c_ab.y * c_ab.y * c_ab.y))));
      }

      float c_0;
      {
        // for c_0, the shape is independent of hue, so ST are constant. Values picked to roughly be the average values of ST.
        float c_a = L * 0.4f;
        float c_b = (1.f - L) * 0.8f;

        // Use a soft minimum function, instead of a sharp triangle shape to get a smooth value for chroma.
        c_0 = sqrt(1.f
                 / (1.f / (c_a * c_a)
                  + 1.f / (c_b * c_b)));
      }

      return float3(c_0, c_mid, c_max);
    }

    namespace OKLab_To
    {

      //OKLab -> OKHSV
      float3 OKHSV(const float3 Lab)
      {
        const float L = Lab[0];
        const float a = Lab[1];
        const float b = Lab[2];

        float2 lc;

        lc.x = L;
        lc.y = sqrt(a * a
                  + b * b);

        float2 ab = float2(a, b) / lc.y;

        float3 hsv;

        hsv[0] = 0.5f
               + 0.5f * atan2(-b, -a) * _1_DIV_PI;

        float2 cusp = Find_Cusp(ab);

        float2 st_max = To_St(cusp);

        float s0 = 0.5f;
        float k  = 1.f - s0 / st_max.x;

        // first we find l_v, c_v, l_vt and c_vt

        float t = st_max.y
                / (lc.y + lc.x * st_max.y);

        float2 lc_v = lc * t;

        float l_vt = Toe_Inv(lc_v.x);
        float c_vt = lc_v.y * l_vt / lc_v.x;

        // we can then use these to invert the step that compensates for the toe and the curved top part of the triangle:
        float3 rgb_scale = OKLab_To::BT709(float3(l_vt,
                                                  ab * c_vt));

        float scale_l = pow(1.f / max(max(rgb_scale.r, rgb_scale.g), max(rgb_scale.b, 0.f)), 1.f / 3.f);

        lc /= scale_l;

        float toe_l = Toe(lc.x);

        lc.y = lc.y * toe_l / lc.x;
        lc.x = toe_l;

        // we can now compute v and s:

        hsv[2] = lc.x / lc_v.x;
        hsv[1] = (s0 + st_max.y) * lc_v.y
               / ((st_max.y * s0) + st_max.y * k * lc_v.y);

        return hsv;
      }

      //OKLab -> OKHSL
      float3 OKHSL(const float3 Lab)
      {
        const float L = Lab[0];
        const float a = Lab[1];
        const float b = Lab[2];

        float c = sqrt(a * a
                     + b * b);

        float2 ab = float2(a, b)
                  / c;

        float3 hsl;

        hsl[0] = 0.5f
               + 0.5f * atan2(-b, -a) * _1_DIV_PI;

        float3 cs = Get_Cs(float3(L, ab));

        float c_0   = cs.x;
        float c_mid = cs.y;
        float c_max = cs.z;

        // Inverse of the interpolation in okhsl_to_srgb:
        float mid     = 0.8f;
        float mid_inv = 1.25f;

        if (c < c_mid)
        {
          float k1 = mid * c_0;

          float k2 = 1.f
                   - k1 / c_mid;

          float t = c
                  / (k1 + k2 * c);

          hsl[1] = t * mid;
        }
        else
        {
          float k0 = c_mid;

          float k1 = (1.f - mid)
                   * c_mid * c_mid
                   * mid_inv * mid_inv / c_0;

          float k2 = 1.f
                   - k1 / (c_max - c_mid);

          float t = (c - k0)
                  / (k1 + k2 * (c - k0));

          hsl[1] = mid
                 + (1.f - mid) * t;
        }

        hsl[2] = Toe(L);

        return hsl;
      }

    }

    namespace BT709_To
    {

      //RGB BT.709 -> OKHSV
      float3 OKHSV(const float3 RGB)
      {
        float3 lab = BT709_To::OKLab(RGB);

        return OKLab_To::OKHSV(lab);
      }

      //RGB BT.709 -> OKHSL
      float3 OKHSL(const float3 RGB)
      {
        float3 lab = BT709_To::OKLab(RGB);

        return OKLab_To::OKHSL(lab);
      }

    } //BT709_To

    namespace BT2020_To
    {

      //RGB BT.2020 -> OKHSV
      float3 OKHSV(const float3 RGB)
      {
        float3 lab = BT2020_To::OKLab(RGB);

        return OKLab_To::OKHSV(lab);
      }

      //RGB BT.2020 -> OKHSL
      float3 OKHSL(const float3 RGB)
      {
        float3 lab = BT2020_To::OKLab(RGB);

        return OKLab_To::OKHSL(lab);
      }

    } //BT2020_To

    namespace OKHSV_To
    {
      //OKHSV -> OKLab
      float3 OKLab(const float3 HSV)
      {
        const float H = HSV[0];
        const float S = HSV[1];
        const float V = HSV[2];

        float i_ab = PI_2 * H;

        float2 ab;
        sincos(i_ab, ab[1], ab[0]);

        float2 cusp = Find_Cusp(ab);

        float2 st_max = To_St(cusp);

        float s0 = 0.5f;
        float k  = 1.f - s0 / st_max.x;

        // first we compute L and V as if the gamut is a perfect triangle:

        // L, c when v==1:
        float i_num = S * s0;
        float i_den = s0 + st_max.y
                    - st_max.y * k * S;

        float i_div = i_num / i_den;

        float l_v =      1.f - i_div;
        float c_v = st_max.y * i_div;

        float2 lc = float2(l_v, c_v) * V;

        // then we compensate for both toe and the curved top part of the triangle:
        float l_vt = Toe_Inv(l_v);
        float c_vt = c_v * l_vt / l_v;

        float l_new = Toe_Inv(lc.x);

        lc.y = lc.y * l_new / lc.x;
        lc.x = l_new;

        float3 rgb_scale = OKLab_To::BT709(float3(l_vt,
                                                  ab * c_vt));

        float scale_l = pow(1.f / max(max(rgb_scale.r, rgb_scale.g), max(rgb_scale.b, 0.f)), 1.f / 3.f);

        lc *= scale_l;

        return float3(lc.x,
                      ab * lc.y);
      }

      //OKHSV -> BT.709
      float3 BT709(const float3 HSV)
      {
        float3 lab = OKHSV_To::OKLab(HSV);

        return OKLab_To::BT709(lab);
      }

      //OKHSV -> BT.2020
      float3 BT2020(const float3 HSV)
      {
        float3 lab = OKHSV_To::OKLab(HSV);

        return OKLab_To::BT2020(lab);
      }
    } //OKHSV_To

    namespace OKHSL_To
    {
      //OKHSL -> OKLab
      float3 OKLab(const float3 HSL)
      {
        const float H = HSL[0];
        const float S = HSL[1];
        const float L = HSL[2];

        if (L == 1.f)
        {
          return (float3)1.f;
        }
        else if (L == 0.f)
        {
          return (float3)0.f;
        }

        float  l;
        float2 ab;

        float i_ab = PI_2 * H;

        l = Toe_Inv(L);

        sincos(i_ab, ab[1], ab[0]);

        float3 cs = Get_Cs(float3(l, ab));

        float c_0   = cs.x;
        float c_mid = cs.y;
        float c_max = cs.z;

        // Interpolate the three values for c so that:
        // At s=0: d_c/ds = c_0, c=0
        // At s=0.8: c=c_mid
        // At s=1.0: c=c_max

        float mid    = 0.8f;
        float mid_inv = 1.25f;

        float c,
              t,
              k0,
              k1,
              k2;

        if (S < mid)
        {
          t = mid_inv * S;

          k1 = mid * c_0;
          k2 = 1.f
             - k1 / c_mid;

          c = t * k1 / (1.f - k2 * t);
        }
        else
        {
          float i0 = 1.f - mid;

          t = (S - mid) / i0;

          k0 = c_mid;

          k1 = i0
             * c_mid * c_mid
             * mid_inv * mid_inv
             / c_0;

          k2 = 1.f
             - k1 / (c_max - c_mid);

          c = k0
            + t * k1
            / (1.f - k2 * t);
        }

        return float3(l,
                      ab * c);
      }

      //OKHSL -> BT.709
      float3 BT709(const float3 HSL)
      {
        float3 lab = OKHSL_To::OKLab(HSL);

        return OKLab_To::BT709(lab);
      }

      //OKHSL -> BT.2020
      float3 BT2020(const float3 HSL)
      {
        float3 lab = OKHSL_To::OKLab(HSL);

        return OKLab_To::BT2020(lab);
      }
    } //OKHSL_To

  } //OKLab

} //Csp
