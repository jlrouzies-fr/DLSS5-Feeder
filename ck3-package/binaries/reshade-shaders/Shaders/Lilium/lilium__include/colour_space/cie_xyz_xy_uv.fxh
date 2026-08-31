
//CIE 1931 xy
struct s_xyY
{
  float2 xy;
  float  Y;
};

//CIE 1976 u'v'
struct s_uvY
{
  float2 uv;
  float  Y;
};

namespace Csp
{

  namespace CIE
  {

    namespace XYZ_To
    {
      s_xyY xyY(const float3 XYZ)
      {
        const float xyz = XYZ.x + XYZ.y + XYZ.z;

        s_xyY xyY;

        // for pure black (RGB(0,0,0) = XYZ(0,0,0)) there is a division by 0
        xyY.xy = xyz != 0.f ? XYZ.xy / xyz
                            : 0.f;

        xyY.Y = XYZ.y;

        return xyY;
      }

      s_uvY uvY(const float3 XYZ)
      {
        const float X15Y3Z = XYZ.x
                           + 15.f * XYZ.y
                           +  3.f * XYZ.z;

        s_uvY uvY;

        // for pure black (RGB(0,0,0) = XYZ(0,0,0)) there is a division by 0
        uvY.uv = X15Y3Z != 0.f ? float2(4.f, 9.f) * XYZ.xy / X15Y3Z
                               : 0.f;

        uvY.Y = XYZ.y;

        return uvY;
      }

      float2 xy(const float3 XYZ)
      {
        const float xyz = XYZ.x + XYZ.y + XYZ.z;

        // for pure black (RGB(0,0,0) = XYZ(0,0,0)) there is a division by 0
        float2 xy = xyz != 0.f ? XYZ.xy / xyz
                               : 0.f;

        return xy;
      }

      float2 uv(const float3 XYZ)
      {
        const float X15Y3Z = XYZ.x
                           + 15.f * XYZ.y
                           +  3.f * XYZ.z;

        // for pure black (RGB(0,0,0) = XYZ(0,0,0)) there is a division by 0
        float2 uv = X15Y3Z != 0.f ? float2(4.f, 9.f) * XYZ.xy / X15Y3Z
                                  : 0.f;

        return uv;
      }
    } //XYZ_To

    namespace xyY_To
    {
      float3 XYZ(const s_xyY xyY)
      {
        float3 XYZ;

        XYZ.xz = float2(xyY.xy.x, (1.f - xyY.xy.x - xyY.xy.y))
               / xyY.xy.y
               * xyY.Y;

        XYZ.y = xyY.Y;

        return XYZ;
      }
    } //xyY_To

    namespace uvY_To
    {
      float3 XYZ(const s_uvY uvY)
      {
        float3 XYZ;

        XYZ.x = 9.f * uvY.uv[0];

        XYZ.z = 12.f
              - (3.f * uvY.uv[0])
              - (20.f * uvY.uv[1]);

        XYZ.xz = XYZ.xz
               / (4.f * uvY.uv[1])
               * uvY.Y;

        XYZ.y = uvY.Y;

        return XYZ;
      }
    } //uvY_To

    namespace xy_To
    {
      float3 XYZ(const float2 xy)
      {
        float3 XYZ;

        XYZ.xz = float2(xy.x, (1.f - xy.x - xy.y))
               / xy.y;

        XYZ.y = 1.f;

        return XYZ;
      }

      float2 uv(const float2 xy)
      {
        float m2x12y3 = -2.f * xy.x
                      + 12.f * xy.y
                      +  3.f;

        float2 uv = float2(4.f, 9.f)
                  * xy
                  / m2x12y3;

        return uv;
      }
    } //xy_To

    namespace uv_To
    {
      float3 XYZ(const float2 uv)
      {
        float3 XYZ;

        XYZ.x = 9.f * uv[0];

        XYZ.z = 12.f
              - ( 3.f * uv[0])
              - (20.f * uv[1]);

        XYZ.xz /= 4.f * uv[1];

        XYZ.y = 1.f;

        return XYZ;
      }
    } //uv_To

  } //CIE

} //Csp
