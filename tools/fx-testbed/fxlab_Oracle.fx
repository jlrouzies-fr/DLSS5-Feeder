/*
    fxlab_Oracle.fx -- EXACT motion vectors for fxlab's 3D scene. A measuring stick, not a provider.

    How good could the picture be if the vectors were perfect? No estimator can answer that, so
    this one cheats: fxlab (--oracle) stamps the number of the scene frame it is presenting, and
    of the one before, into the first two pixels of the frame, and this effect rebuilds the
    camera of both from fxlab's own camera path (Camera3D in fxlab3d.h -- keep the two in step)
    and reprojects every pixel through the depth buffer. What it writes is what a game engine
    would hand DLSS for everything that does not move by itself: exact, smooth, right at every
    outline. (The spinning cube and the sliding sphere get the camera's motion only, which is
    wrong for them; they are a few percent of the frame.)

    It writes the community-standard texMotionVectors, so DLSS5_Feed.fx reads it as provider 0:

        Run-FxLab.ps1 -Scene 3d -Oracle -Provider 0 -Techniques 'fxlab_Oracle@fxlab_Oracle.fx,DLSS5_Feed@DLSS5_Feed.fx'

    Needs the 10-bit swap chain fxlab uses by default and its reversed depth.
*/

#include "ReShade.fxh"

texture fxlab_Color : COLOR;
sampler sfxlab_Color { Texture = fxlab_Color; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

texture texMotionVectors < pooled = false; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };

// fxlab3d.h, Camera3D: 120 frames still, a yaw pan, a dolly, a strafe; 720 frames in all.
void Camera(float frame, out float3 pos, out float3 fwd)
{
    const float f = frame - 720.0 * floor(frame / 720.0);
    const float tau = 6.2831853;
    float x = 0.0, z = -8.0, a = 0.0;
    if (f >= 120.0 && f < 300.0)      a = 0.45 * sin(tau * (f - 120.0) / 180.0);
    else if (f >= 300.0 && f < 480.0) z = -8.0 + 2.0 * (1.0 - cos(tau * (f - 300.0) / 180.0));
    else if (f >= 480.0)              { const float s = sin(tau * (f - 480.0) / 240.0); x = 3.0 * s; a = -0.3 * s; }
    pos = float3(x, 1.7, z);
    fwd = normalize(float3(sin(a), -0.06, cos(a)));
}

float FrameNumber(int x)
{
    const float3 c = tex2Dfetch(sfxlab_Color, int2(x, 0)).rgb;
    return round(c.r * 1023.0) + round(c.g * 1023.0) * 1024.0;
}

float2 PS_Oracle(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float now = FrameNumber(0), before = FrameNumber(1);
    if (now == before) return float2(0.0, 0.0);              // the same frame again (a 30 fps game at 60)

    // The depth buffer as fxlab wrote it: XMMatrixPerspectiveFovLH(60 deg, aspect, 400, 0.1), i.e.
    // reversed, z_ndc = m33 + m43 / z_view.
    const float m33 = 0.1 / (0.1 - 400.0), m43 = -400.0 * 0.1 / (0.1 - 400.0);
    const float zn  = tex2Dlod(ReShade::DepthBuffer, float4(uv, 0.0, 0.0)).x;
    const float zv  = m43 / max(zn - m33, 1e-7);
    const float ty  = tan(radians(30.0)), tx = ty * BUFFER_WIDTH / BUFFER_HEIGHT;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 pv = float3(ndc.x * tx * zv, ndc.y * ty * zv, zv);

    float3 p0, f0, p1, f1;
    Camera(now, p0, f0);
    Camera(before, p1, f1);
    const float3 up = float3(0.0, 1.0, 0.0);
    const float3 r0 = normalize(cross(up, f0)), u0 = cross(f0, r0);
    const float3 r1 = normalize(cross(up, f1)), u1 = cross(f1, r1);
    const float3 world = p0 + r0 * pv.x + u0 * pv.y + f0 * pv.z;
    const float3 d  = world - p1;
    const float3 q  = float3(dot(d, r1), dot(d, u1), dot(d, f1));
    if (q.z <= 0.01) return float2(0.0, 0.0);
    const float2 ndc1 = float2(q.x / (tx * q.z), q.y / (ty * q.z));
    const float2 uv1  = float2(ndc1.x * 0.5 + 0.5, 0.5 - ndc1.y * 0.5);
    return uv1 - uv;                                         // delta UV, previous = uv + mv
}

technique fxlab_Oracle < ui_tooltip = "Exact camera vectors for tools/fx-testbed's 3D scene. Test use only."; >
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_Oracle; RenderTarget = texMotionVectors; }
}
