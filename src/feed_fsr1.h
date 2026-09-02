// AMD FidelityFX Super Resolution 1.0 -- EASU (edge-adaptive spatial upsampling) and
// RCAS (robust contrast-adaptive sharpening) -- transcribed to plain HLSL so the same
// source compiles at ps_4_0 (the 32-bit add-on meets feature-level 10 devices) and ps_5_0.
// No Gather, no wave ops: twelve explicit Loads for EASU, five for RCAS.
//
// This is the expand-back for `work_upscale=1`: the DLSS/neural output at the work
// extent is spatially upsampled to the backbuffer instead of bilinearly stretched.
// It is a better filter, not super resolution: nothing here can exceed the native frame.
//
// Derived from FidelityFX-FSR (ffx_fsr1.h), https://github.com/GPUOpen-Effects/FidelityFX-FSR
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this
// software and associated documentation files (the "Software"), to deal in the Software
// without restriction, including without limitation the rights to use, copy, modify,
// merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following
// conditions: The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
// OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#pragma once

#include <math.h>

// Mirrors `cbuffer FsrConstants` below: 48 bytes, three float4s.
struct FsrConstants
{
    float con0[4];   // input-per-output scale (x,y), and the half-texel offset that maps an
                     // output pixel index to an input texel index (z,w)
    float con1[4];   // input size (x,y), 1/input size (z,w)
    float rcas[4];   // x = RCAS sharpness as a linear factor exp2(-stops); y,z,w unused
};

// sharpness01: 0 = RCAS off (the caller skips the pass), 1 = the sharpest FSR allows.
// FSR expresses sharpness in "stops" where 0 is sharpest and 2 is barely there.
static inline void FsrFillConstants(FsrConstants *c, unsigned in_w, unsigned in_h,
                                    unsigned out_w, unsigned out_h, float sharpness01)
{
    const float iw = static_cast<float>(in_w),  ih = static_cast<float>(in_h);
    const float ow = static_cast<float>(out_w), oh = static_cast<float>(out_h);
    c->con0[0] = iw / ow;
    c->con0[1] = ih / oh;
    c->con0[2] = 0.5f * iw / ow - 0.5f;
    c->con0[3] = 0.5f * ih / oh - 0.5f;
    c->con1[0] = iw;
    c->con1[1] = ih;
    c->con1[2] = 1.0f / iw;
    c->con1[3] = 1.0f / ih;
    if (sharpness01 < 0.0f) sharpness01 = 0.0f;
    if (sharpness01 > 1.0f) sharpness01 = 1.0f;
    const float stops = (1.0f - sharpness01) * 2.0f;
    c->rcas[0] = exp2f(-stops);
    c->rcas[1] = c->rcas[2] = c->rcas[3] = 0.0f;
}

// Two entry points, `ps_easu` and `ps_rcas`, both driven by the add-on's existing
// fullscreen-triangle vertex shader (SV_Position + TEXCOORD0). Source texture at t0,
// no sampler: every tap is an integer Load, so the result is identical at either
// shader model. The pixel index comes from SV_Position, which is what FSR's
// dispatch-thread id would be in the compute original.
static const char kFsr1Src[] = R"HLSL(
Texture2D<float4> fsr_src : register(t0);
cbuffer FsrConstants : register(b0) { float4 con0; float4 con1; float4 rcas; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float3 FsrLoad(int2 p)
{
    p = clamp(p, int2(0, 0), int2(con1.xy) - int2(1, 1));
    return fsr_src.Load(int3(p, 0)).rgb;
}
float FsrLuma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

// Accumulate the direction and length estimate from one of the four bilinear corners.
void FsrEasuSet(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV,
                float lA, float lB, float lC, float lD, float lE)
{
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w =        pp.x  * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) *        pp.y;
    if (biV) w =        pp.x  *        pp.y;
    float dc = lD - lC, cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = rcp(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = rcp(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

// One of the twelve taps of the rotated, stretched, windowed lanczos-like kernel.
void FsrEasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c)
{
    float2 v;
    v.x = off.x *  dir.x + off.y * dir.y;
    v.y = off.x * -dir.y + off.y * dir.x;
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

float4 ps_easu(VSOut i) : SV_Target
{
    int2 ip = int2(i.pos.xy);
    float2 pp = float2(ip) * con0.xy + con0.zw;
    float2 fp = floor(pp);
    pp -= fp;
    int2 f0 = int2(fp);
    //  b c
    // e f g h
    // i j k l
    //  n o
    float3 b = FsrLoad(f0 + int2( 0, -1));
    float3 c = FsrLoad(f0 + int2( 1, -1));
    float3 e = FsrLoad(f0 + int2(-1,  0));
    float3 f = FsrLoad(f0 + int2( 0,  0));
    float3 g = FsrLoad(f0 + int2( 1,  0));
    float3 h = FsrLoad(f0 + int2( 2,  0));
    float3 ii = FsrLoad(f0 + int2(-1, 1));
    float3 j = FsrLoad(f0 + int2( 0,  1));
    float3 k = FsrLoad(f0 + int2( 1,  1));
    float3 l = FsrLoad(f0 + int2( 2,  1));
    float3 n = FsrLoad(f0 + int2( 0,  2));
    float3 o = FsrLoad(f0 + int2( 1,  2));
    float bL = FsrLuma(b), cL = FsrLuma(c), eL = FsrLuma(e), fL = FsrLuma(f), gL = FsrLuma(g), hL = FsrLuma(h);
    float iL = FsrLuma(ii), jL = FsrLuma(j), kL = FsrLuma(k), lL = FsrLuma(l), nL = FsrLuma(n), oL = FsrLuma(o);

    float2 dir = float2(0.0, 0.0);
    float  len = 0.0;
    FsrEasuSet(dir, len, pp, true,  false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSet(dir, len, pp, false, true,  false, false, cL, fL, gL, hL, kL);
    FsrEasuSet(dir, len, pp, false, false, true,  false, fL, iL, jL, kL, nL);
    FsrEasuSet(dir, len, pp, false, false, false, true,  gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = rsqrt(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * rcp(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = rcp(lob);

    float3 min4 = min(min(f, g), min(j, k));
    float3 max4 = max(max(f, g), max(j, k));
    float3 aC = float3(0.0, 0.0, 0.0);
    float  aW = 0.0;
    FsrEasuTap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
    FsrEasuTap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
    FsrEasuTap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, ii);
    FsrEasuTap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    FsrEasuTap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    FsrEasuTap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
    FsrEasuTap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    FsrEasuTap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
    FsrEasuTap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
    FsrEasuTap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    FsrEasuTap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
    FsrEasuTap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);
    float3 pix = min(max4, max(min4, aC * rcp(aW)));
    return float4(pix, 1.0);
}

float4 ps_rcas(VSOut i) : SV_Target
{
    int2 ip = int2(i.pos.xy);
    //   b
    // d e f
    //   h
    float3 b = FsrLoad(ip + int2( 0, -1));
    float3 d = FsrLoad(ip + int2(-1,  0));
    float3 e = FsrLoad(ip);
    float3 f = FsrLoad(ip + int2( 1,  0));
    float3 h = FsrLoad(ip + int2( 0,  1));
    float bL = FsrLuma(b), dL = FsrLuma(d), eL = FsrLuma(e), fL = FsrLuma(f), hL = FsrLuma(h);
    // Noise detection: back the sharpening off where the neighbourhood is already busy.
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    nz = saturate(abs(nz) * rcp(max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL)));
    nz = -0.5 * nz + 1.0;
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float2 peakC = float2(1.0, -4.0);
    float3 hitMin = mn4 * rcp(4.0 * mx4);
    float3 hitMax = (peakC.x - mx4) * rcp(4.0 * mn4 + peakC.y);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0)) * rcas.x;
    lobe *= nz;
    float rcpL = rcp(4.0 * lobe + 1.0);
    float3 pix = ((b + d + f + h) * lobe + e) * rcpL;
    return float4(pix, 1.0);
}
)HLSL";
