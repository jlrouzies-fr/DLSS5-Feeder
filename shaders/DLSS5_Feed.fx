/*
    DLSS5_Feed.fx - companion effect for the "DLSS 5 Feed" ReShade add-on (dlss5-feed.addon64/32).

    It turns what ReShade already has into the guide textures DLSS needs, in the exact layout
    the add-on expects:

      DLSS5_MV     RG16F   motion vectors in PIXELS, pointing from the current pixel to where it was
                           in the previous frame (DLSS convention). Vectors that fail validation
                           (below) are zeroed.
      DLSS5_Depth  R32F    the game's raw hardware depth (not linearised), sampled at backbuffer size,
                           with ReShade's RESHADE_DEPTH_INPUT_* orientation fixes applied.
      DLSS5_Mask   R8      "bias current colour" mask for DLSS: 1 where the motion vector could not
                           be trusted, so DLSS leans on the current frame there instead of warping
                           history in. Optional -- an add-on that does not know it ignores it.

    MOTION VECTOR PROVIDER -- set the DLSS5_MV_PROVIDER preprocessor definition (ReShade overlay:
    this effect's "Preprocessor definitions", or the global list) and enable that provider's
    technique ABOVE this one in the effect list:

      0  texMotionVectors    the community-standard shared texture: qUINT_motionvectors,
                             dh_uber_motion, ReshadeMotionEstimation (DRME -- NOTE: DRME does not
                             compile on ReShade 6.8, "cannot sample from texture that is also used
                             as render target"; it then silently writes nothing)         [default]
      1  Launchpad           iMMERSE Launchpad (MartysMods_LAUNCHPAD.fx): Deferred::MotionVectorsTex.
                             Launchpad only runs its optical flow when asked to, so this mode also
                             files that per-frame request (Launchpad's IPC buffer, see below).
      2  VORT                vort_Motion.fx (MIT): MotVectTexVort -- the recommended provider
      3  LumeniteFX Kernel   lumenite_Kernel.fx ("LUMENITE: Kernel"): Kernel::tFlow -- pyramidal
                             optical flow with per-level median + a-trous filtering and previous-
                             frame seeding. 1/8 resolution, upsampled here. Needs no depth buffer.
      4  LumeniteFX QuantMotion
                             lumenite_QuantMotion.fx: QuantMotion::tFlow -- the light cut of 3.

    This is the same mechanism dh_uber_rt (USE_MARTY_LAUNCHPAD_MOTION / USE_VORT_MOTION) and
    vort (V_MV_MODE) use: the selected provider's OUTPUT texture is declared here exactly as the
    provider declares it, so ReShade binds the same resource, and only that one is allocated.
    Every provider above hands out delta UV with prev_uv = uv + mv. Nothing of any provider is
    included or bundled: this file contains no third-party code and includes no third-party
    files beyond ReShade's own headers.

    VALIDATION -- why it exists. A game's motion vectors are geometric: a static wall under a
    flickering light has vectors of exactly zero. Every provider above is OPTICAL FLOW: it
    matches pixels, so a lighting change (flicker, flames, particles) is answered with a vector
    that points at whatever happened to match -- confidently wrong, and DLSS then warps its
    history in from there. That is the "warping around flames" and the "bad dither when the
    light flickers". The fix is the one every production TAA uses: reproject and CHECK.
    For each pixel, three tests against the previous frame at uv + mv:
      - luma:        the previous luma must fall inside the current 3x3 neighbourhood's range
                     (flicker moves the whole range, so a stale match falls outside);
      - depth:       the previous linear depth must match the current one (disocclusions);
      - consistency: the previous frame's vector at that spot must resemble this one
                     (real motion is smooth frame to frame; flow on fire is erratic).
    A vector failing any test is zeroed (the surface is treated as static -- the right answer
    for a lit wall) and the pixel is flagged in DLSS5_Mask so DLSS trusts the current frame there.

    The add-on runs DLSS + DLSS 5 neural rendering right after the "DLSS5_Feed" technique has
    rendered, so anything placed below it in the list is applied on top of the neural output.
*/

#include "ReShade.fxh"

// Expose ReShade's completed frame to the add-on as an SRV. The 64-bit D3D11 path
// uses this only when its work-resolution control is below 100%; no extra pass or
// copy is introduced by this declaration.
texture DLSS5_ColorInput : COLOR;
sampler sDLSS5_ColorInput { Texture = DLSS5_ColorInput; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point; MagFilter = Point; };

#ifndef DLSS5_MV_PROVIDER
    #define DLSS5_MV_PROVIDER 0
#endif

// ---------------------------------------------------------------------------------------------
// The selected provider's output, declared byte for byte like the provider itself does.
// ---------------------------------------------------------------------------------------------

#if DLSS5_MV_PROVIDER == 1
    // iMMERSE Launchpad (MartysMods/mmx_deferred.fxh)
    namespace Deferred {
        texture MotionVectorsTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
        // Launchpad's request buffer. Launchpad only computes optical flow when a consumer asked
        // for it during the previous frame (it reads this 1x1 RGBA8 at the top of its technique
        // and clears it at the bottom; bit 4 = optical flow, written through the render-target
        // write mask). Being below Launchpad in the list, our request lands for the next frame.
        // Declared like Launchpad declares it; the two shaders that write it below are ours.
        namespace IPC {
            texture2D PredicationBuffer { Format = RGBA8; };
        }
    }
    sampler sDLSS5_ProviderMV { Texture = Deferred::MotionVectorsTex; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point; MagFilter = Point; };
    float4 DLSS5_IpcRequestVS(in uint id : SV_VertexID) : SV_Position { return float4(0.0, 0.0, 0.0, 1.0); }
    float4 DLSS5_IpcRequestPS(in float4 vpos : SV_Position) : SV_Target0 { return 1.0; }
    #define DLSS5_MV_PROVIDER_NAME "Launchpad (Deferred::MotionVectorsTex)"
    #define DLSS5_MV_REQUEST_PASS pass IpcRequestOpticalFlow { PrimitiveTopology = POINTLIST; VertexCount = 1; VertexShader = DLSS5_IpcRequestVS; PixelShader = DLSS5_IpcRequestPS; RenderTarget = Deferred::IPC::PredicationBuffer; RenderTargetWriteMask = 4; }
#elif DLSS5_MV_PROVIDER == 2
    // VORT (Includes/vort_MotionUtils.fxh, V_MV_MODE 1)
    texture2D MotVectTexVort { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
    sampler sDLSS5_ProviderMV { Texture = MotVectTexVort; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point; MagFilter = Point; };
    #define DLSS5_MV_PROVIDER_NAME "VORT (MotVectTexVort)"
#elif DLSS5_MV_PROVIDER == 3
    // LumeniteFX Kernel (lumenite_Kernel.fx), as lumenite_RTAO/TRAA re-declare it. 1/8 resolution.
    namespace Kernel {
        texture2D tFlow { Width = BUFFER_WIDTH/8; Height = BUFFER_HEIGHT/8; Format = RG16F; };
        texture2D tConfidence { Width = BUFFER_WIDTH/8; Height = BUFFER_HEIGHT/8; Format = R16F; };
    }
    sampler sDLSS5_ProviderMV        { Texture = Kernel::tFlow;       AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Linear; MagFilter = Linear; };
    sampler sDLSS5_ProviderMVPoint   { Texture = Kernel::tFlow;       AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point;  MagFilter = Point;  };
    sampler sDLSS5_ProviderConfidence{ Texture = Kernel::tConfidence; AddressU = Clamp; AddressV = Clamp; };
    #define DLSS5_MV_PROVIDER_NAME "LumeniteFX Kernel (Kernel::tFlow, 1/8 res)"
    #define DLSS5_MV_LOWRES 1
#elif DLSS5_MV_PROVIDER == 4
    // LumeniteFX QuantMotion (lumenite_QuantMotion.fx), as lumenite_QuantAO re-declares it. 1/8 resolution.
    namespace QuantMotion {
        texture2D tFlow { Width = BUFFER_WIDTH/8; Height = BUFFER_HEIGHT/8; Format = RG16F; };
        texture2D tConfidence { Width = BUFFER_WIDTH/8; Height = BUFFER_HEIGHT/8; Format = R16F; };
    }
    sampler sDLSS5_ProviderMV        { Texture = QuantMotion::tFlow;       AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Linear; MagFilter = Linear; };
    sampler sDLSS5_ProviderMVPoint   { Texture = QuantMotion::tFlow;       AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point;  MagFilter = Point;  };
    sampler sDLSS5_ProviderConfidence{ Texture = QuantMotion::tConfidence; AddressU = Clamp; AddressV = Clamp; };
    #define DLSS5_MV_PROVIDER_NAME "LumeniteFX QuantMotion (QuantMotion::tFlow, 1/8 res)"
    #define DLSS5_MV_LOWRES 1
#else
    // The community-standard shared texture (ReshadeMotionEstimation, qUINT, dh_uber_motion, ...)
    texture texMotionVectors < pooled = false; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
    sampler sDLSS5_ProviderMV { Texture = texMotionVectors; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point; MagFilter = Point; };
    #define DLSS5_MV_PROVIDER_NAME "texMotionVectors (DRME, qUINT, dh_uber_motion, ...)"
#endif

#ifndef DLSS5_MV_LOWRES
    #define DLSS5_MV_LOWRES 0
#endif
#ifndef DLSS5_MV_REQUEST_PASS
    #define DLSS5_MV_REQUEST_PASS
#endif

// ---------------------------------------------------------------------------------------------

uniform int MV_PROVIDER_INFO <
    ui_type = "radio";
    ui_label = " ";
    ui_text = "Motion vector provider: " DLSS5_MV_PROVIDER_NAME "\n"
              "Change it with the DLSS5_MV_PROVIDER preprocessor definition:\n"
              "  0 texMotionVectors (DRME, qUINT, dh_uber_motion)   1 Launchpad   2 VORT\n"
              "  3 LumeniteFX Kernel   4 LumeniteFX QuantMotion\n"
              "Enable that provider's technique ABOVE DLSS 5 Feed.";
>;

#if DLSS5_MV_LOWRES
uniform int MV_LOWRES_FILTER <
    ui_type = "combo";
    ui_items = "Bilinear\0Point (nearest)\0";
    ui_label = "Low-res provider filter";
    ui_tooltip = "How the provider's 1/8-resolution flow is brought up to full resolution.\n"
                 "Bilinear smooths across flow cells; point keeps each 8x8 cell's vector as-is.";
> = 0;
#endif

// ---------------------------------------------------------------------------------------------
// Geometry vectors. A game's motion vectors for static geometry come from camera motion and
// depth, not from pixels. We have depth; the camera motion is fitted each frame from the
// provider's flow over a sparse grid (robust two-pass least squares on a 9-term screen-space
// model: affine + quadratic rotation terms + inverse-depth parallax terms), and every pixel
// then gets the vector that model predicts from its depth -- correct under flicker, correct
// while moving. The provider's flow is only used where it disagrees with the model AND wins a
// structure test: a genuinely moving object. Flames and flicker lose that test and keep the
// geometric vector, so nothing warps.
// ---------------------------------------------------------------------------------------------

uniform bool GEOM_ENABLE <
    ui_category = "Geometry vectors (camera model + depth) -- EXPERIMENTAL";
    ui_label = "Use geometry vectors (experimental, off by default)";
    ui_tooltip = "Fit the camera motion from the provider's flow + depth each frame and derive every static\n"
                 "pixel's vector from it. The provider is then only consulted for moving objects.\n"
                 "EXPERIMENTAL: the per-frame fit is still noisy, and anything not part of the 3D world\n"
                 "(the HUD) gets camera vectors it should not have -- expect jitter there.\n"
                 "Off = the per-pixel validation below is applied to the provider's flow directly.";
> = false;

uniform float GEOM_PARALLAX <
    ui_category = "Geometry vectors (camera model + depth)";
    ui_type = "drag"; ui_min = 0.001; ui_max = 0.5; ui_step = 0.001;
    ui_label = "Parallax depth scale";
    ui_tooltip = "The model's inverse-depth term is s / (depth + s) with linear depth in 0..1. Smaller = more\n"
                 "parallax resolution near the camera. Usually fine as is.";
> = 0.02;

uniform float GEOM_OUTLIER_PX <
    ui_category = "Geometry vectors (camera model + depth)";
    ui_type = "drag"; ui_min = 0.5; ui_max = 32.0; ui_step = 0.5;
    ui_label = "Fit: outlier rejection (px)";
    ui_tooltip = "Second fitting pass ignores samples whose flow is further than this from the first pass's\n"
                 "prediction -- moving objects, flames, the first-person weapon.";
> = 4.0;

uniform float GEOM_AGREE_PX <
    ui_category = "Geometry vectors (camera model + depth)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 16.0; ui_step = 0.1;
    ui_label = "Agreement (px)";
    ui_tooltip = "If the provider's flow is within this many pixels (+10% of the vector) of the model, the\n"
                 "model's vector is used as-is. Beyond it, the structure test decides moving object vs junk.";
> = 1.5;

uniform float GEOM_DYNAMIC_MARGIN <
    ui_category = "Geometry vectors (camera model + depth)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.9; ui_step = 0.01;
    ui_label = "Moving-object margin";
    ui_tooltip = "For the provider's flow to override the model on a disagreeing pixel, its reprojection must\n"
                 "explain the pixel's structure at least this much (relative) better than the model's does.\n"
                 "Higher = more conservative (fewer things count as moving objects).";
> = 0.25;

uniform float GEOM_MASK_REJECTED <
    ui_category = "Geometry vectors (camera model + depth)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.05;
    ui_label = "Mask strength on rejected flow";
    ui_tooltip = "Where the provider disagreed with the model but did not win the structure test (fire, smoke,\n"
                 "flicker), the geometric vector is used; this is how strongly DLSS is additionally asked to\n"
                 "favour the current frame there. 0 = pure history (smoothest), 1 = mostly current frame.";
> = 0.35;

uniform bool MV_VALIDATE <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_label = "Validate motion vectors against the previous frame";
    ui_tooltip = "Optical-flow providers answer a lighting change (flicker, flames) with a vector that\n"
                 "points at whatever happened to match. Reprojecting and checking catches those:\n"
                 "the vector is zeroed and DLSS is told to trust the current frame there (DLSS5_Mask).";
> = true;

uniform bool VALIDATE_STATIC <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_label = "Static-hypothesis test (zeroes the vector, keeps history)";
    ui_tooltip = "For each pixel, asks which explains it better: 'did not move' or the provider's vector.\n"
                 "Both are scored on illumination-normalised 3x3 structure (local mean removed), so a\n"
                 "flickering light does not count as motion. When 'did not move' wins, the vector is zeroed\n"
                 "and the pixel is NOT masked -- a static wall wants its full history, which is what smooths\n"
                 "the flicker. This is the test for the flickering-wall case.";
> = true;

uniform float STATIC_BIAS <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Static bias";
    ui_tooltip = "How much worse (relative) the static explanation may score than the vector's and still win.\n"
                 "0 = the vector must strictly beat 'did not move'. Higher favours zero vectors.";
> = 0.15;

uniform float STATIC_MIN_CONTRAST <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.1; ui_step = 0.001;
    ui_label = "Static test: minimum patch contrast";
    ui_tooltip = "Below this 3x3 contrast (mean absolute deviation of luma) a patch has no structure to judge\n"
                 "motion by, and the test abstains -- the provider's vector stands. Raise it if flat surfaces\n"
                 "trail while moving (yellow on plain motion in the debug view); lower it if the\n"
                 "flickering wall stops being caught.";
> = 0.012;

uniform bool VALIDATE_LUMA <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_label = "Luma test (mask only)";
    ui_tooltip = "The reprojected previous luma must fall inside the current 3x3 neighbourhood's luma range.\n"
                 "A failure only raises the mask (DLSS leans on the current frame); it never zeroes the vector,\n"
                 "because a lighting change does not prove the surface did not move. Off by default: on a\n"
                 "flickering surface it asks DLSS to drop exactly the history that would smooth the flicker.";
> = false;

uniform float LUMA_TOLERANCE <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Luma tolerance";
    ui_tooltip = "How far outside the current 3x3 neighbourhood's luma range the reprojected previous luma\n"
                 "may fall (relative to that range's maximum). Lower = stricter.";
> = 0.25;

uniform bool VALIDATE_DEPTH <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_label = "Depth test (zeroes the vector)";
    ui_tooltip = "The reprojected previous linear depth must match the current one: a mismatch means the vector\n"
                 "points at a different surface (disocclusion), so it is zeroed and masked. Sky is exempt.";
> = true;

uniform float DEPTH_TOLERANCE <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.005;
    ui_label = "Depth tolerance";
    ui_tooltip = "Allowed relative difference between the reprojected previous linear depth and the current one.";
> = 0.10;

uniform bool VALIDATE_MV <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_label = "Consistency test (zeroes the vector)";
    ui_tooltip = "This frame's vector must resemble the previous frame's vector at the spot it points to.\n"
                 "Real motion is smooth frame to frame; optical flow on fire, smoke or a flickering wall is not.\n"
                 "A failure zeroes the vector and masks the pixel.";
> = true;

uniform float MV_CONSISTENCY <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 16.0; ui_step = 0.1;
    ui_label = "Vector consistency (px)";
    ui_tooltip = "Allowed change, in pixels, between this frame's vector and the previous frame's vector at\n"
                 "the reprojected spot, plus 50% of the vector length. Raise it if plain camera motion\n"
                 "shows blue in the 'Validation tests' debug view.";
> = 1.4;

uniform float MASK_STRENGTH <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.05;
    ui_label = "Bias-current-colour mask strength";
    ui_tooltip = "How strongly a distrusted pixel asks DLSS to favour the current frame (DLSS5_Mask).\n"
                 "1 = fully; 0 = only zero the vector, do not mask.";
> = 1.0;

uniform float2 MV_SIGN <
    ui_type = "drag";
    ui_min = -1.0; ui_max = 1.0; ui_step = 2.0;
    ui_label = "Motion vector sign (x, y)";
    ui_tooltip = "Flip a component if the DLAA output doubles/smears in that direction while moving.\n"
                 "Default (1, 1) matches the convention every supported provider uses (prev_uv = uv + mv).";
> = float2(1.0, 1.0);

uniform float MV_SCALE <
    ui_type = "drag";
    ui_min = 0.0; ui_max = 4.0; ui_step = 0.01;
    ui_label = "Motion vector scale";
    ui_tooltip = "1.0 = the provider's estimate as-is. Diagnostic only.";
> = 1.0;

uniform int DEBUG_VIEW <
    ui_type = "combo";
    ui_items = "Motion vectors (colour = direction, brightness = speed)\0"
               "Raw depth\0"
               "Provider confidence (LumeniteFX only; white = confident)\0"
               "Validation mask (white = vector distrusted, DLSS uses current frame)\0"
               "Validation mask over the image\0"
               "Validation tests over the image (red = luma, green = depth, blue = consistency, yellow = static wins)\0"
               "Geometry model vectors (colour = direction, brightness = speed)\0"
               "Geometry decision over the image (green = model, red = provider won as moving object, blue = provider rejected)\0"
               "Geometry fit quality (grey = inlier share; top strip = fit error, black 0 px .. white 8 px)\0";
    ui_label = "Debug view (DLSS5_Feed_Debug technique)";
> = 0;

// Outputs for the add-on
texture DLSS5_MV    { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
texture DLSS5_Depth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F;  };
texture DLSS5_Mask  { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8;    };
sampler sDLSS5_MV    { Texture = DLSS5_MV;    MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Depth { Texture = DLSS5_Depth; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Mask  { Texture = DLSS5_Mask;  MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// Previous-frame history for validation (written at the end of the technique)
texture DLSS5_PrevLuma  { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F;  };
texture DLSS5_PrevDepth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F;  };
texture DLSS5_PrevMV    { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
// Luma may be interpolated (a smooth quantity); depth and vectors must NOT be -- bilinear
// across an object edge mixes two surfaces' values and fails the test on every edge in motion.
sampler sDLSS5_PrevLuma  { Texture = DLSS5_PrevLuma;  AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler sDLSS5_PrevDepth { Texture = DLSS5_PrevDepth; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT;  MagFilter = POINT;  MipFilter = POINT; };
sampler sDLSS5_PrevMV    { Texture = DLSS5_PrevMV;    AddressU = Clamp; AddressV = Clamp; MinFilter = POINT;  MagFilter = POINT;  MipFilter = POINT; };

// Camera-model fit: a sparse sample grid of (x, y, w, valid | u, v), and the solved model as
// six 1x1 RGBA32F texels (18 parameters + fit statistics).
#define DLSS5_FIT_W 40
#define DLSS5_FIT_H 23
texture DLSS5_FitA { Width = DLSS5_FIT_W; Height = DLSS5_FIT_H; Format = RGBA32F; };
texture DLSS5_FitB { Width = DLSS5_FIT_W; Height = DLSS5_FIT_H; Format = RGBA32F; };
sampler sDLSS5_FitA { Texture = DLSS5_FitA; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_FitB { Texture = DLSS5_FitB; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_Cam0 { Width = 1; Height = 1; Format = RGBA32F; };
texture DLSS5_Cam1 { Width = 1; Height = 1; Format = RGBA32F; };
texture DLSS5_Cam2 { Width = 1; Height = 1; Format = RGBA32F; };
texture DLSS5_Cam3 { Width = 1; Height = 1; Format = RGBA32F; };
texture DLSS5_Cam4 { Width = 1; Height = 1; Format = RGBA32F; };
texture DLSS5_Cam5 { Width = 1; Height = 1; Format = RGBA32F; };
sampler sDLSS5_Cam0 { Texture = DLSS5_Cam0; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Cam1 { Texture = DLSS5_Cam1; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Cam2 { Texture = DLSS5_Cam2; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Cam3 { Texture = DLSS5_Cam3; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Cam4 { Texture = DLSS5_Cam4; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Cam5 { Texture = DLSS5_Cam5; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// ---------------------------------------------------------------------------------------------

// The selected provider's vector at uv, as delta UV (prev_uv = uv + mv).
float2 ProviderMV(float2 uv)
{
    float4 c = float4(uv, 0.0, 0.0);
#if DLSS5_MV_LOWRES
    return MV_LOWRES_FILTER == 0 ? tex2Dlod(sDLSS5_ProviderMV, c).xy : tex2Dlod(sDLSS5_ProviderMVPoint, c).xy;
#else
    return tex2Dlod(sDLSS5_ProviderMV, c).xy;
#endif
}

float Luma(float2 uv)
{
    return dot(tex2Dlod(sDLSS5_ColorInput, float4(uv, 0.0, 0.0)).rgb, float3(0.299, 0.587, 0.114));
}

// Illumination-normalised 3x3 structure difference between the current frame at uv_cur and
// the previous frame at uv_prev: each patch has its own mean removed first, so a brightness
// change (flicker) contributes nothing and only the pattern is compared.
// Also returns the current patch's contrast (mean absolute deviation): a patch with no
// structure cannot decide anything, and the caller must not pretend it can.
float PatchError(float2 uv_cur, float2 uv_prev, out float contrast)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    float c[9], p[9];
    float mc = 0.0, mp = 0.0;
    [unroll] for (int i = 0; i < 9; ++i)
    {
        const float2 o = float2(i % 3 - 1, i / 3 - 1) * px;
        c[i] = Luma(uv_cur + o);
        p[i] = tex2Dlod(sDLSS5_PrevLuma, float4(uv_prev + o, 0.0, 0.0)).x;
        mc += c[i]; mp += p[i];
    }
    mc /= 9.0; mp /= 9.0;
    float err = 0.0;
    contrast = 0.0;
    [unroll] for (int j = 0; j < 9; ++j)
    {
        err += abs((c[j] - mc) - (p[j] - mp));
        contrast += abs(c[j] - mc);
    }
    contrast /= 9.0;
    return err / 9.0;
}

// Per-test failure (0 = fine, 1 = failed, soft in between): x = luma, y = depth, z = consistency,
// w = the static hypothesis won. Luma failing says "this pixel's appearance changed"; depth or
// consistency failing says "this vector points at the wrong thing"; static winning says "no
// vector explains this pixel better than zero". Only y, z and w justify zeroing the vector, and
// only x, y, z justify asking DLSS to distrust history.
float4 ValidateTests(float2 uv, float2 mv)
{
    const float2 puv = uv + mv;
    float4 bad = 0.0;
    // Reprojecting off-screen: nothing to compare against. Keep the vector (DLSS handles
    // it) and let the mask lean on the current frame.
    if (any(puv < 0.0) || any(puv > 1.0)) return float4(1.0, 0.0, 0.0, 0.0);

    // 0. Static hypothesis: does "did not move" explain this pixel at least as well as the
    //    vector does? Scored on mean-removed structure, so flicker is not motion. Skipped for
    //    vectors under half a pixel (nothing to decide).
    if (VALIDATE_STATIC && length(mv * BUFFER_SCREEN_SIZE) > 0.5)
    {
        float sc, unused;
        const float es = PatchError(uv, uv, sc);
        const float ef = PatchError(uv, puv, unused);
        // Only a patch with structure can tell the two apart. Below the contrast floor the
        // scores tie for lack of evidence, and a tie must go to the provider (its flow is
        // propagated from textured neighbours -- the right guess for a moving flat wall).
        // With structure, static wins only if it beats the vector by a share of that contrast.
        if (sc >= STATIC_MIN_CONTRAST)
            bad.w = es + 0.25 * sc <= ef * (1.0 + STATIC_BIAS) ? 1.0 : 0.0;
    }

    // 1. Luma: current 3x3 range vs the previous luma at the reprojected spot.
    if (VALIDATE_LUMA)
    {
        const float2 px = BUFFER_PIXEL_SIZE;
        float lc = Luma(uv), lmin = lc, lmax = lc;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float l = Luma(uv + float2(x, y) * px);
            lmin = min(lmin, l); lmax = max(lmax, l);
        }
        const float lp     = tex2Dlod(sDLSS5_PrevLuma, float4(puv, 0.0, 0.0)).x;
        const float margin = LUMA_TOLERANCE * max(lmax, 0.05) + 2.0 / 255.0;
        bad.x = saturate(max(lmin - lp, lp - lmax) / margin);
    }

    // 2. Depth: previous linear depth at the reprojected spot vs the current one (sky exempt).
    const float dc = ReShade::GetLinearizedDepth(uv);
    if (VALIDATE_DEPTH && dc < 0.999)
    {
        const float dp  = tex2Dlod(sDLSS5_PrevDepth, float4(puv, 0.0, 0.0)).x;
        const float tol = DEPTH_TOLERANCE * max(dc, 1e-3);
        bad.y = saturate((abs(dp - dc) - tol) / (tol + 1e-5));
    }

    // 3. Consistency: the previous frame's vector where this pixel came from vs this one.
    if (VALIDATE_MV && MV_CONSISTENCY > 0.0)
    {
        const float2 pmv   = tex2Dlod(sDLSS5_PrevMV, float4(puv, 0.0, 0.0)).xy;
        const float  diff  = length((mv - pmv) * BUFFER_SCREEN_SIZE);
        const float  allow = MV_CONSISTENCY + 0.5 * length(mv * BUFFER_SCREEN_SIZE);
        bad.z = saturate((diff - allow) / allow);
    }
    return bad;
}

// ---------------------------------------------------------------------------------------------
// Camera model. Screen position x, y in -0.5..0.5, inverse-depth term w = s / (depth + s).
// Basis (9 terms):  1, x, y, x^2, xy, y^2, w, xw, yw  -- the small-rotation flow field of a
// pinhole camera is quadratic in the image position, and translation adds terms in 1/Z.
// Both flow components share the basis; the fit solves them together (two right-hand sides).
// ---------------------------------------------------------------------------------------------

#define DLSS5_BASIS(B, x, y, w) \
    B[0] = 1.0; B[1] = x; B[2] = y; B[3] = x * x; B[4] = x * y; B[5] = y * y; B[6] = w; B[7] = x * w; B[8] = y * w;

float ParallaxW(float d) { return GEOM_PARALLAX / (d + GEOM_PARALLAX); }

// The model's predicted delta-UV at uv for linear depth d.
float2 PredictMV(float2 uv, float d)
{
    const float4 c = float4(0.5, 0.5, 0.0, 0.0);
    const float4 p0 = tex2Dlod(sDLSS5_Cam0, c), p1 = tex2Dlod(sDLSS5_Cam1, c), p2 = tex2Dlod(sDLSS5_Cam2, c);
    const float4 p3 = tex2Dlod(sDLSS5_Cam3, c), p4 = tex2Dlod(sDLSS5_Cam4, c);
    const float x = uv.x - 0.5, y = uv.y - 0.5, w = ParallaxW(d);
    float B[9]; DLSS5_BASIS(B, x, y, w)
    // u: p0.xyzw p1.xyzw p2.x     v: p2.yzw p3.xyzw p4.xy
    const float u = p0.x * B[0] + p0.y * B[1] + p0.z * B[2] + p0.w * B[3] + p1.x * B[4] + p1.y * B[5] + p1.z * B[6] + p1.w * B[7] + p2.x * B[8];
    const float v = p2.y * B[0] + p2.z * B[1] + p2.w * B[2] + p3.x * B[3] + p3.y * B[4] + p3.z * B[5] + p3.w * B[6] + p4.x * B[7] + p4.y * B[8];
    return float2(u, v);
}

bool FitIsUsable()
{
    const float4 s = tex2Dlod(sDLSS5_Cam5, float4(0.5, 0.5, 0.0, 0.0));   // x = inlier share, y = rms px, z = samples used
    return s.z >= 40.0 && s.x >= 0.25;
}

// Pass 1: sample the provider's flow and the depth on a sparse grid.
void PS_FitSamples(float4 vpos : SV_Position, float2 uv : TEXCOORD, out float4 A : SV_Target0, out float4 B : SV_Target1)
{
    const float2 suv = (floor(vpos.xy) + 0.5) / float2(DLSS5_FIT_W, DLSS5_FIT_H);
    const float  d   = ReShade::GetLinearizedDepth(suv);
    const float2 mv  = ProviderMV(suv);
    const bool valid = d > 0.001 && all(abs(mv * BUFFER_SCREEN_SIZE) < 512.0);
    A = float4(suv.x - 0.5, suv.y - 0.5, ParallaxW(d), valid ? 1.0 : 0.0);
    B = float4(mv, 0.0, 0.0);
}

// Pass 2 (one pixel): robust least squares. Pass one fits everything; pass two refits on the
// samples the first fit explains to within GEOM_OUTLIER_PX, which drops moving objects,
// flames and the weapon from the camera estimate.
void PS_FitSolve(float4 vpos : SV_Position, float2 uv : TEXCOORD,
                 out float4 P0 : SV_Target0, out float4 P1 : SV_Target1, out float4 P2 : SV_Target2,
                 out float4 P3 : SV_Target3, out float4 P4 : SV_Target4, out float4 P5 : SV_Target5)
{
    float p[18];
    [unroll] for (int z = 0; z < 18; ++z) p[z] = 0.0;
    float inlier = 0.0, rms = 0.0, used = 0.0;
    const int total = DLSS5_FIT_W * DLSS5_FIT_H;

    [loop] for (int it = 0; it < 2; ++it)
    {
        float M[45];   // upper triangle of the 9x9 normal matrix
        float ru[9], rv[9];
        [unroll] for (int z0 = 0; z0 < 45; ++z0) M[z0] = 0.0;
        [unroll] for (int z1 = 0; z1 < 9;  ++z1) { ru[z1] = 0.0; rv[z1] = 0.0; }
        int   n  = 0;
        float se = 0.0;

        [loop] for (int s = 0; s < total; ++s)
        {
            const int2   cell = int2(s % DLSS5_FIT_W, s / DLSS5_FIT_W);
            const float4 a = tex2Dfetch(sDLSS5_FitA, cell);
            const float4 b = tex2Dfetch(sDLSS5_FitB, cell);
            if (a.w < 0.5) continue;
            float B[9]; DLSS5_BASIS(B, a.x, a.y, a.z)
            if (it > 0)
            {
                float pu = 0.0, pv = 0.0;
                [unroll] for (int i0 = 0; i0 < 9; ++i0) { pu += p[i0] * B[i0]; pv += p[9 + i0] * B[i0]; }
                const float r = length((float2(pu, pv) - b.xy) * BUFFER_SCREEN_SIZE);
                if (r > GEOM_OUTLIER_PX) continue;
                se += r * r;
            }
            ++n;
            int k = 0;
            [unroll] for (int i = 0; i < 9; ++i)
            {
                ru[i] += B[i] * b.x;
                rv[i] += B[i] * b.y;
                [unroll] for (int j = i; j < 9; ++j) { M[k] += B[i] * B[j]; ++k; }
            }
        }
        if (n < 40) break;   // not enough evidence: keep whatever the previous pass produced

        // Augmented 9 x (9 + 2) system, Gauss-Jordan with partial pivoting, tiny ridge for
        // the degenerate cases (flat depth makes w collinear with 1; a still camera makes
        // everything zero).
        float G[99];
        {
            int k2 = 0;
            [unroll] for (int i = 0; i < 9; ++i)
            {
                [unroll] for (int j = i; j < 9; ++j) { G[i * 11 + j] = M[k2]; G[j * 11 + i] = M[k2]; ++k2; }
                G[i * 11 + i] += 1e-5 * n;
                G[i * 11 + 9]  = ru[i];
                G[i * 11 + 10] = rv[i];
            }
        }
        bool singular = false;
        [loop] for (int col = 0; col < 9; ++col)
        {
            int   piv  = col;
            float best = abs(G[col * 11 + col]);
            [loop] for (int r0 = col + 1; r0 < 9; ++r0)
            {
                const float v0 = abs(G[r0 * 11 + col]);
                if (v0 > best) { best = v0; piv = r0; }
            }
            if (best < 1e-12) { singular = true; break; }
            if (piv != col)
                [unroll] for (int c0 = 0; c0 < 11; ++c0) { const float t = G[col * 11 + c0]; G[col * 11 + c0] = G[piv * 11 + c0]; G[piv * 11 + c0] = t; }
            const float inv = 1.0 / G[col * 11 + col];
            [unroll] for (int c1 = 0; c1 < 11; ++c1) G[col * 11 + c1] *= inv;
            [loop] for (int r1 = 0; r1 < 9; ++r1)
            {
                if (r1 == col) continue;
                const float f = G[r1 * 11 + col];
                if (f == 0.0) continue;
                [unroll] for (int c2 = 0; c2 < 11; ++c2) G[r1 * 11 + c2] -= f * G[col * 11 + c2];
            }
        }
        if (singular) break;
        [unroll] for (int i2 = 0; i2 < 9; ++i2) { p[i2] = G[i2 * 11 + 9]; p[9 + i2] = G[i2 * 11 + 10]; }
        used   = n;
        inlier = float(n) / float(total);
        if (it > 0) rms = sqrt(se / max(n, 1));
    }

    P0 = float4(p[0],  p[1],  p[2],  p[3]);
    P1 = float4(p[4],  p[5],  p[6],  p[7]);
    P2 = float4(p[8],  p[9],  p[10], p[11]);
    P3 = float4(p[12], p[13], p[14], p[15]);
    P4 = float4(p[16], p[17], 0.0, 0.0);
    P5 = float4(inlier, rms, used, 0.0);
}

// Per-pixel decision: x = final delta-UV vector, .z = 0 model / 1 provider (moving object) /
// 2 provider rejected, .w = mask contribution from that decision.
float4 GeometryDecide(float2 uv, float d, float2 flow)
{
    const float2 pred = PredictMV(uv, d);
    const float  r     = length((flow - pred) * BUFFER_SCREEN_SIZE);
    const float  agree = GEOM_AGREE_PX + 0.1 * length(pred * BUFFER_SCREEN_SIZE);
    if (r <= agree) return float4(pred, 0.0, 0.0);

    float cp, cf;
    const float ep = PatchError(uv, uv + pred, cp);
    const float ef = PatchError(uv, uv + flow, cf);
    const bool  dynamic = cp >= STATIC_MIN_CONTRAST && ef <= ep * (1.0 - GEOM_DYNAMIC_MARGIN) - 1.0 / 255.0;
    if (dynamic) return float4(flow, 1.0, 0.0);
    return float4(pred, 2.0, GEOM_MASK_REJECTED * saturate((r - agree) / (4.0 * agree)));
}

void PS_MotionVectors(float4 vpos : SV_Position, float2 uv : TEXCOORD,
                      out float2 mv_out : SV_Target0, out float mask : SV_Target1)
{
    // Providers hand out "delta UV": previous position = uv + mv. DLSS wants the same
    // direction, in pixels.
    const float2 flow = ProviderMV(uv);
    float2 mv = flow;
    float  distrust = 0.0;

    if (GEOM_ENABLE && FitIsUsable())
    {
        const float  d = ReShade::GetLinearizedDepth(uv);
        const float4 g = GeometryDecide(uv, d, flow);
        mv = g.xy;
        distrust = g.w;
        // Disocclusion: the geometric vector on a newly revealed pixel points into the
        // occluder's old position; the depth test catches that and asks for the current frame.
        if (VALIDATE_DEPTH && d < 0.999)
        {
            const float2 puv = uv + mv;
            if (all(puv >= 0.0) && all(puv <= 1.0))
            {
                const float dp  = tex2Dlod(sDLSS5_PrevDepth, float4(puv, 0.0, 0.0)).x;
                const float tol = DEPTH_TOLERANCE * max(d, 1e-3);
                distrust = max(distrust, saturate((abs(dp - d) - tol) / (tol + 1e-5)));
            }
        }
    }
    else if (MV_VALIDATE)
    {
        const float4 bad = ValidateTests(uv, flow);
        const float  zero_vector = max(bad.y, max(bad.z, bad.w));   // wrong target, or static explains it: treat as static
        distrust = max(bad.x, max(bad.y, bad.z));                    // appearance changed / wrong target: favour the current frame
        mv = flow * (1.0 - zero_vector);
    }

    mv_out = mv * float2(BUFFER_WIDTH, BUFFER_HEIGHT) * MV_SIGN * MV_SCALE;
    mask   = distrust * MASK_STRENGTH;
}

float PS_Depth(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // Raw hardware depth, exactly as the game wrote it -- the same orientation/offset
    // corrections ReShade.fxh applies in GetLinearizedDepth(), minus the linearisation
    // (DLSS must receive the raw values; the add-on tells it whether the range is reversed).
    float2 t = uv;
#if RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
    t.y = 1.0 - t.y;
#endif
    t.x /= RESHADE_DEPTH_INPUT_X_SCALE;
    t.y /= RESHADE_DEPTH_INPUT_Y_SCALE;
#if RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET
    t.x -= RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET * BUFFER_RCP_WIDTH;
#else
    t.x -= RESHADE_DEPTH_INPUT_X_OFFSET / 2.000000001;
#endif
#if RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET
    t.y += RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET * BUFFER_RCP_HEIGHT;
#else
    t.y += RESHADE_DEPTH_INPUT_Y_OFFSET / 2.000000001;
#endif
    return tex2Dlod(ReShade::DepthBuffer, float4(t, 0.0, 0.0)).x;
}

// End of the technique: this frame becomes next frame's history. The raw provider vector is
// stored (not the validated one), so one distrusted frame does not poison the next test.
void PS_StoreHistory(float4 vpos : SV_Position, float2 uv : TEXCOORD,
                     out float luma : SV_Target0, out float depth : SV_Target1, out float2 mv : SV_Target2)
{
    luma  = Luma(uv);
    depth = ReShade::GetLinearizedDepth(uv);
    mv    = ProviderMV(uv);
}

float3 PS_Debug(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    if (DEBUG_VIEW == 1)
    {
        float d = tex2Dlod(sDLSS5_Depth, float4(uv, 0.0, 0.0)).x;
        return d.xxx;
    }
    if (DEBUG_VIEW == 2)
    {
#if DLSS5_MV_LOWRES
        return saturate(tex2Dlod(sDLSS5_ProviderConfidence, float4(uv, 0.0, 0.0)).x).xxx;
#else
        return (0.25).xxx; // this provider publishes no confidence map
#endif
    }
    if (DEBUG_VIEW == 3)
        return tex2Dlod(sDLSS5_Mask, float4(uv, 0.0, 0.0)).xxx;
    if (DEBUG_VIEW == 4)
    {
        const float  m   = tex2Dlod(sDLSS5_Mask, float4(uv, 0.0, 0.0)).x;
        const float3 img = tex2Dlod(sDLSS5_ColorInput, float4(uv, 0.0, 0.0)).rgb;
        return lerp(img, float3(1.0, 0.2, 0.1), m * 0.75);
    }
    if (DEBUG_VIEW == 5)
    {
        // Recomputed here against the same history the feed pass used this frame.
        const float4 bad = ValidateTests(uv, ProviderMV(uv));
        const float3 img = tex2Dlod(sDLSS5_ColorInput, float4(uv, 0.0, 0.0)).rgb * 0.5;
        return saturate(img + bad.xyz * 0.9 + bad.w * float3(0.6, 0.6, 0.0));
    }
    if (DEBUG_VIEW == 6)
    {
        const float2 pv = PredictMV(uv, ReShade::GetLinearizedDepth(uv)) * BUFFER_SCREEN_SIZE;
        const float angle = atan2(pv.y, pv.x), speed = length(pv);
        const float3 rgb = saturate(3.0 * abs(2.0 * frac(angle / 6.283185 + float3(0.0, -1.0 / 3.0, 1.0 / 3.0)) - 1.0) - 1.0);
        return lerp(0.5, rgb, saturate(speed / 16.0));
    }
    if (DEBUG_VIEW == 7)
    {
        const float3 img = tex2Dlod(sDLSS5_ColorInput, float4(uv, 0.0, 0.0)).rgb * 0.5;
        if (!FitIsUsable()) return img;   // no usable fit this frame: nothing to show
        const float4 g = GeometryDecide(uv, ReShade::GetLinearizedDepth(uv), ProviderMV(uv));
        const float3 tint = g.z < 0.5 ? float3(0.0, 0.5, 0.0) : g.z < 1.5 ? float3(0.9, 0.0, 0.0) : float3(0.0, 0.2, 0.9);
        return saturate(img + tint);
    }
    if (DEBUG_VIEW == 8)
    {
        const float4 s = tex2Dlod(sDLSS5_Cam5, float4(0.5, 0.5, 0.0, 0.0));
        if (uv.y < 0.05) return saturate(s.y / 8.0).xxx;   // fit error strip
        return s.x.xxx;                                    // inlier share
    }
    float2 mv = tex2Dlod(sDLSS5_MV, float4(uv, 0.0, 0.0)).xy; // pixels
    float angle = atan2(mv.y, mv.x);
    float speed = length(mv);
    float3 rgb = saturate(3.0 * abs(2.0 * frac(angle / 6.283185 + float3(0.0, -1.0 / 3.0, 1.0 / 3.0)) - 1.0) - 1.0);
    return lerp(0.5, rgb, saturate(speed / 16.0)); // 16 px/frame saturates the colour
}

// ---------------------------------------------------------------------------------------------

technique DLSS5_Feed
<
    ui_label   = "DLSS 5 Feed (place below your motion-vector provider)";
    ui_tooltip = "Prepares motion vectors + depth (+ a trust mask) for the DLSS 5 Feed add-on.\n\n"
                 "Provider: " DLSS5_MV_PROVIDER_NAME "\n"
                 "Change it with the DLSS5_MV_PROVIDER preprocessor definition (0 texMotionVectors,\n"
                 "1 Launchpad, 2 VORT, 3 LumeniteFX Kernel, 4 LumeniteFX QuantMotion) and enable\n"
                 "that provider's technique ABOVE this one.";
>
{
    pass FitSamples    { VertexShader = PostProcessVS; PixelShader = PS_FitSamples;    RenderTarget0 = DLSS5_FitA; RenderTarget1 = DLSS5_FitB; }
    pass FitSolve      { VertexShader = PostProcessVS; PixelShader = PS_FitSolve;      RenderTarget0 = DLSS5_Cam0; RenderTarget1 = DLSS5_Cam1; RenderTarget2 = DLSS5_Cam2; RenderTarget3 = DLSS5_Cam3; RenderTarget4 = DLSS5_Cam4; RenderTarget5 = DLSS5_Cam5; }
    pass MotionVectors { VertexShader = PostProcessVS; PixelShader = PS_MotionVectors; RenderTarget0 = DLSS5_MV; RenderTarget1 = DLSS5_Mask; }
    pass Depth         { VertexShader = PostProcessVS; PixelShader = PS_Depth;         RenderTarget  = DLSS5_Depth; }
    pass History       { VertexShader = PostProcessVS; PixelShader = PS_StoreHistory;  RenderTarget0 = DLSS5_PrevLuma; RenderTarget1 = DLSS5_PrevDepth; RenderTarget2 = DLSS5_PrevMV; }
    DLSS5_MV_REQUEST_PASS   // Launchpad only: ask it to compute optical flow again next frame
}

technique DLSS5_Feed_Debug
<
    ui_label   = "DLSS 5 Feed - debug view";
    ui_tooltip = "Shows the motion vectors / depth / mask the add-on will send to DLSS. Enable only for checking.";
>
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_Debug; }
}
