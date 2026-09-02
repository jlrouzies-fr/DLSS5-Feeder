// SmoothMotionTest.fx -- does ReShade's output reach a frame pacer's GENERATED frames?
//
// Run this with DLSS5-Feeder DISABLED (enabled=0 in dlss5-feed.cfg) and Smooth
// Motion ON. It has nothing to do with the feeder: it only asks whether ReShade
// itself survives an in-driver frame pacer on Vulkan.
//
// Three independent signals, so the answer is unambiguous:
//
//   1. BLUE TINT over the whole frame. Present = this frame went through ReShade.
//      Missing on some frames = those frames bypassed the effect chain entirely.
//   2. RED BAR sweeping left to right, once per second, driven by wall-clock time.
//      Smooth = every displayed frame carries fresh ReShade output.
//      Jumping back and forth = displayed frames carry STALE ReShade output.
//   3. CORNER BLOCK (top right) flipping black/white every frame.
//      Steady flicker = frames advance. Holding = frames are being repeated.
//
// Reading the result:
//   * All three clean            -> ReShade + Smooth Motion is fine; the fault is
//                                   in the feeder's transport.
//   * Tint flickers on/off       -> generated frames bypass ReShade completely.
//                                   Nothing an add-on does can appear on them.
//   * Tint steady, bar jumps back-> generated frames use a stale snapshot of
//                                   ReShade's output. Same conclusion, different
//                                   mechanism: we cannot control when it snapshots.

#include "ReShade.fxh"

uniform float timer      < source = "timer"; >;
uniform int   framecount < source = "framecount"; >;

float3 PS_SmoothMotionTest(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float3 c = tex2D(ReShade::BackBuffer, uv).rgb;

    // 1. Did this frame go through the effect chain at all?
    // A hard blend, NOT a saturating add: adding to an already-bright centre is
    // invisible and reads as a vignette, which is exactly how this misled its first
    // run. Blending is unmissable everywhere regardless of scene brightness.
    c = lerp(c, float3(0.0, 0.35, 1.0), 0.40);

    // 2. Is the ReShade output on this frame fresh, or an old one?
    const float bar = frac(timer / 1000.0);
    if (abs(uv.x - bar) < 0.006)
        c = float3(1.0, 0.0, 0.0);

    // 3. Are frames advancing at all?
    if (uv.x > 0.95 && uv.y < 0.05)
        c = ((framecount % 2) == 1) ? float3(1.0, 1.0, 1.0) : float3(0.0, 0.0, 0.0);

    return c;
}

technique SmoothMotionTest <
    ui_tooltip = "Diagnostic: does ReShade output reach a frame pacer's generated frames?";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_SmoothMotionTest;
    }
}
