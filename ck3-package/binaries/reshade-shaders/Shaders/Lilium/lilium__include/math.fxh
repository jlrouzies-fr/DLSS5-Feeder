
#if (__VENDOR__ == 0x1002)
  #define TIMES_100 100.0001f
  #define   DIV_100 99.99999f
#else
  #define TIMES_100 100.f
  #define   DIV_100 100.f
#endif


#define PI   3.1415927410125732421875f
#define PI_2 6.283185482025146484375f

#define _1_DIV_PI 0.3183098733425140380859375f

#define FP32_MIN asfloat(0x00800000)
#define FP32_MAX asfloat(0x7F7FFFFF)

#define UINT_MAX 4294967295u
#define  INT_MAX 2147483647

#define MIN3(A, B, C) min(A, min(B, C))

#define MIN4(A, B, C, D) min(A, min(B, min(C, D)))

#define MIN3_TEMPLATE(T) \
  T min                  \
  (                      \
    const T A,           \
    const T B,           \
    const T C            \
  )                      \
  {                      \
    T ret;               \
                         \
    ret = min(B, C);     \
    ret = min(A, ret);   \
                         \
    return ret;          \
  }

#define MIN4_TEMPLATE(T) \
  T min                  \
  (                      \
    const T A,           \
    const T B,           \
    const T C,           \
    const T D            \
  )                      \
  {                      \
    T ret;               \
                         \
    ret = min(C, D);     \
    ret = min(B, ret);   \
    ret = min(A, ret);   \
                         \
    return ret;          \
  }

#define MIN5_TEMPLATE(T) \
  T min                  \
  (                      \
    const T A,           \
    const T B,           \
    const T C,           \
    const T D,           \
    const T E            \
  )                      \
  {                      \
    T ret;               \
                         \
    ret = min(D, E);     \
    ret = min(C, ret);   \
    ret = min(B, ret);   \
    ret = min(A, ret);   \
                         \
    return ret;          \
  }

#define MIN6_TEMPLATE(T) \
  T min                  \
  (                      \
    const T A,           \
    const T B,           \
    const T C,           \
    const T D,           \
    const T E,           \
    const T F            \
  )                      \
  {                      \
    T ret;               \
                         \
    ret = min(E, F);     \
    ret = min(D, ret);   \
    ret = min(C, ret);   \
    ret = min(B, ret);   \
    ret = min(A, ret);   \
                         \
    return ret;          \
  }

MIN3_TEMPLATE(float)
MIN3_TEMPLATE(float2)
MIN3_TEMPLATE(float3)
MIN3_TEMPLATE(float4)
MIN3_TEMPLATE(uint)
MIN3_TEMPLATE(uint2)
MIN3_TEMPLATE(uint3)
MIN3_TEMPLATE(uint4)
MIN3_TEMPLATE(int)
MIN3_TEMPLATE(int2)
MIN3_TEMPLATE(int3)
MIN3_TEMPLATE(int4)

MIN4_TEMPLATE(float)
MIN4_TEMPLATE(float2)
MIN4_TEMPLATE(float3)
MIN4_TEMPLATE(float4)
MIN4_TEMPLATE(uint)
MIN4_TEMPLATE(uint2)
MIN4_TEMPLATE(uint3)
MIN4_TEMPLATE(uint4)
MIN4_TEMPLATE(int)
MIN4_TEMPLATE(int2)
MIN4_TEMPLATE(int3)
MIN4_TEMPLATE(int4)

MIN5_TEMPLATE(float)
MIN5_TEMPLATE(float2)
MIN5_TEMPLATE(float3)
MIN5_TEMPLATE(float4)
MIN5_TEMPLATE(uint)
MIN5_TEMPLATE(uint2)
MIN5_TEMPLATE(uint3)
MIN5_TEMPLATE(uint4)
MIN5_TEMPLATE(int)
MIN5_TEMPLATE(int2)
MIN5_TEMPLATE(int3)
MIN5_TEMPLATE(int4)

MIN6_TEMPLATE(float)
MIN6_TEMPLATE(float2)
MIN6_TEMPLATE(float3)
MIN6_TEMPLATE(float4)
MIN6_TEMPLATE(uint)
MIN6_TEMPLATE(uint2)
MIN6_TEMPLATE(uint3)
MIN6_TEMPLATE(uint4)
MIN6_TEMPLATE(int)
MIN6_TEMPLATE(int2)
MIN6_TEMPLATE(int3)
MIN6_TEMPLATE(int4)


#define MAX3(A, B, C) max(A, max(B, C))

#define MAX4(A, B, C, D) max(A, max(B, max(C, D)))

#define MAX3_TEMPLATE(T) \
  T max                  \
  (                      \
    const T A,           \
    const T B,           \
    const T C            \
  )                      \
  {                      \
    T ret;               \
                         \
    ret = max(B, C);     \
    ret = max(A, ret);   \
                         \
    return ret;          \
  }

#define MAX4_TEMPLATE(T) \
  T max                  \
  (                      \
    const T A,           \
    const T B,           \
    const T C,           \
    const T D            \
  )                      \
  {                      \
    T ret;               \
                         \
    ret = max(C, D);     \
    ret = max(B, ret);   \
    ret = max(A, ret);   \
                         \
    return ret;          \
  }

#define MAX5_TEMPLATE(T) \
  T max                  \
  (                      \
    const T A,           \
    const T B,           \
    const T C,           \
    const T D,           \
    const T E            \
  )                      \
  {                      \
    T ret;               \
                         \
    ret = max(D, E);     \
    ret = max(C, ret);   \
    ret = max(B, ret);   \
    ret = max(A, ret);   \
                         \
    return ret;          \
  }

#define MAX6_TEMPLATE(T) \
  T max                  \
  (                      \
    const T A,           \
    const T B,           \
    const T C,           \
    const T D,           \
    const T E,           \
    const T F            \
  )                      \
  {                      \
    T ret;               \
                         \
    ret = max(E, F);     \
    ret = max(D, ret);   \
    ret = max(C, ret);   \
    ret = max(B, ret);   \
    ret = max(A, ret);   \
                         \
    return ret;          \
  }

MAX3_TEMPLATE(float)
MAX3_TEMPLATE(float2)
MAX3_TEMPLATE(float3)
MAX3_TEMPLATE(float4)
MAX3_TEMPLATE(uint)
MAX3_TEMPLATE(uint2)
MAX3_TEMPLATE(uint3)
MAX3_TEMPLATE(uint4)
MAX3_TEMPLATE(int)
MAX3_TEMPLATE(int2)
MAX3_TEMPLATE(int3)
MAX3_TEMPLATE(int4)

MAX4_TEMPLATE(float)
MAX4_TEMPLATE(float2)
MAX4_TEMPLATE(float3)
MAX4_TEMPLATE(float4)
MAX4_TEMPLATE(uint)
MAX4_TEMPLATE(uint2)
MAX4_TEMPLATE(uint3)
MAX4_TEMPLATE(uint4)
MAX4_TEMPLATE(int)
MAX4_TEMPLATE(int2)
MAX4_TEMPLATE(int3)
MAX4_TEMPLATE(int4)

MAX5_TEMPLATE(float)
MAX5_TEMPLATE(float2)
MAX5_TEMPLATE(float3)
MAX5_TEMPLATE(float4)
MAX5_TEMPLATE(uint)
MAX5_TEMPLATE(uint2)
MAX5_TEMPLATE(uint3)
MAX5_TEMPLATE(uint4)
MAX5_TEMPLATE(int)
MAX5_TEMPLATE(int2)
MAX5_TEMPLATE(int3)
MAX5_TEMPLATE(int4)

MAX6_TEMPLATE(float)
MAX6_TEMPLATE(float2)
MAX6_TEMPLATE(float3)
MAX6_TEMPLATE(float4)
MAX6_TEMPLATE(uint)
MAX6_TEMPLATE(uint2)
MAX6_TEMPLATE(uint3)
MAX6_TEMPLATE(uint4)
MAX6_TEMPLATE(int)
MAX6_TEMPLATE(int2)
MAX6_TEMPLATE(int3)
MAX6_TEMPLATE(int4)


#define MAXRGB(Rgb) max(Rgb.r, max(Rgb.g, Rgb.b))
#define MINRGB(Rgb) min(Rgb.r, min(Rgb.g, Rgb.b))
