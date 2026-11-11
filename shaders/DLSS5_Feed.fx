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
      2  VORT                vort_Motion.fx: MotVectTexVort
      3  LumeniteFX Kernel   lumenite_Kernel.fx ("LUMENITE: Kernel"): Kernel::tFlow -- pyramidal
                             optical flow with per-level median + a-trous filtering and previous-
                             frame seeding. 1/8 resolution, upsampled here. Needs no depth buffer.
      4  LumeniteFX QuantMotion
                             lumenite_QuantMotion.fx: QuantMotion::tFlow -- the light cut of 3.
      5  Built in            this file's own estimator: hierarchical block matching on a luma
                             pyramid, 1/64 of the frame down to 1/2, written for finished 2D
                             pictures (UI over video, menus, scrolling lists). Nothing to install
                             and no technique to enable. Under 3D camera motion it is clearly
                             behind 3 (measured, tools/fx-testbed) -- use 3 for a 3D game.

    For 0-4 this is the same mechanism dh_uber_rt (USE_MARTY_LAUNCHPAD_MOTION / USE_VORT_MOTION)
    and vort (V_MV_MODE) use: the selected provider's OUTPUT texture is declared here exactly as
    the provider declares it, so ReShade binds the same resource, and only that one is allocated.
    Every provider above hands out delta UV with prev_uv = uv + mv. Nothing of any provider is
    included or bundled: this file contains no third-party code (the built-in estimator is
    written from scratch) and includes no third-party files beyond ReShade's own headers.

    REFINEMENT (the default "Vector pipeline"). Whatever the provider, its field is coarse and
    a coarse field cannot follow the outline of a glyph. It gives the pixels of a still caption
    -- or of a game's HUD -- a fraction of the motion behind them, and a temporal model that is
    told "this text moved 0.3 px" resamples its history by 0.3 px every frame: soft, smeared
    text. So the provider's vector stands unless the pixel itself says otherwise: a pixel that
    did not change, or whose own structure is where it was, gets exactly zero; where the vector
    leaves the pixel unexplained, the provider's cells around it and last frame's vector are
    tried; and the nearest whole-pixel vector is sent wherever it reproduces the pixel, so
    history is fetched pixel for pixel. See RefineMV().
    tools/fx-testbed measures all of it through the real add-on and DLSS, in 2D and in 3D,
    with and without a neural consumer.

    VALIDATION (the "Classic" pipeline, 1.19 and earlier) -- why it exists. A game's motion vectors are geometric: a static wall under a
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

    One such thing is in this file: the technique DLSS5_Feed_Restore (optional; enable it below
    DLSS5_Feed). It puts the fine detail of the frame as it came in back on captions and HUD
    elements -- pixels seen standing still while the picture around them moved -- which DLSS
    softens a little whatever the vectors and a neural pass redraws. See PS_Restore.
*/

#include "ReShade.fxh"

// D3D9 is not a target. The add-on attaches to D3D10/11/12, OpenGL and Vulkan runtimes only,
// so if ReShade is on its DirectX 9 backend the effect could never be fed anyway -- and the
// geometric-fit solver below cannot compile there (SM3 has no tex2Dfetch and must unroll the
// [loop]s over dynamically indexed arrays, which is the "error X3531: can't unroll loops
// marked with loop attribute" of issue #56). Say which of those two facts the user is looking
// at, because the compiler error alone sends people hunting through their shader list.
//
// It used to be an #error, and issue #129 still reported the X3531 it was meant to replace --
// from an older copy of this file, or a ReShade build that shows the compiler's error rather
// than the preprocessor's; the report does not say which. Either way, on D3D9 the effect now
// compiles to a stub (a pass-through technique, the explanation as its overlay text) that
// cannot fail, so the reason is the one thing on screen.
#if __RENDERER__ < 0xA000

#warning "DLSS5_Feed needs D3D10 or newer, and ReShade has loaded its DirectX 9 backend. For a D3D9 game the dgVoodoo2 wrapper must be in effect first (check DisableAndPassThru=false in dgVoodoo.conf); see the README's 'Install for a DirectX 9 game' section. A 64-bit D3D9 game does not need this add-on at all -- renodx-dlss handles those on its own."

uniform int DLSS5_D3D9_Notice <
    ui_type = "radio"; ui_label = " ";
    ui_text = "DLSS5_Feed does nothing here: ReShade is running on DirectX 9, and the add-on only attaches to\n"
              "D3D10/11/12, OpenGL and Vulkan. For a 32-bit D3D9 game, put the dgVoodoo2 wrapper in front first\n"
              "(DisableAndPassThru=false in dgVoodoo.conf) -- see the README's 'Install for a DirectX 9 game'.\n"
              "A 64-bit D3D9 game does not need this add-on at all: renodx-dlss handles those on its own.";
> = 0;

float4 PS_DLSS5_D3D9(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(ReShade::BackBuffer, uv);
}

technique DLSS5_Feed < ui_tooltip = "Not available on DirectX 9 -- see the text in this effect's settings."; >
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_DLSS5_D3D9; }
}

#else   // D3D10 and newer: the real effect, to the end of the file

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
#elif DLSS5_MV_PROVIDER == 5
    // Built in: the pyramid estimator further down in this file. Nothing to install or enable.
    // Its finest level is half resolution, vectors in full-resolution PIXELS (.xy), the match
    // cost in .z and the patch contrast in .w.
    texture DLSS5_Flow1 { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };
    sampler sDLSS5_ProviderMV      { Texture = DLSS5_Flow1; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Linear; MagFilter = Linear; };
    sampler sDLSS5_ProviderMVPoint { Texture = DLSS5_Flow1; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point;  MagFilter = Point;  };
    #define DLSS5_MV_PROVIDER_NAME "Built in (DLSS5_Feed's own estimator)"
    #define DLSS5_MV_BUILTIN 1
    #define DLSS5_MV_CELL 2.0
#else
    // The community-standard shared texture (ReshadeMotionEstimation, qUINT, dh_uber_motion, ...)
    texture texMotionVectors < pooled = false; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
    sampler sDLSS5_ProviderMV { Texture = texMotionVectors; AddressU = Clamp; AddressV = Clamp; MipFilter = Point; MinFilter = Point; MagFilter = Point; };
    #define DLSS5_MV_PROVIDER_NAME "texMotionVectors (DRME, qUINT, dh_uber_motion, ...)"
#endif

#ifndef DLSS5_MV_LOWRES
    #define DLSS5_MV_LOWRES 0
#endif
#ifndef DLSS5_MV_BUILTIN
    #define DLSS5_MV_BUILTIN 0
#endif
// How far apart, in pixels, two neighbouring vectors of the provider are -- the spacing at which
// the refinement stage looks for a better candidate than the one under the pixel.
#ifndef DLSS5_MV_CELL
    #if DLSS5_MV_LOWRES
        #define DLSS5_MV_CELL 8.0
    #else
        #define DLSS5_MV_CELL 3.0
    #endif
#endif
#ifndef BUFFER_COLOR_SPACE
    #define BUFFER_COLOR_SPACE 1
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
              "  3 LumeniteFX Kernel   4 LumeniteFX QuantMotion   5 built in (nothing to enable)\n"
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

uniform int MV_PIPELINE <
    ui_category = "Vector refinement (per pixel)";
    ui_type = "combo";
    ui_items = "Refine: pick the best vector per pixel\0Classic validation (1.19 and earlier)\0";
    ui_label = "Vector pipeline";
    ui_tooltip = "Refine: the provider's vector stands unless the pixel itself says otherwise. A pixel that\n"
                 "did not change, or whose own structure is where it was, gets exactly zero -- so a caption\n"
                 "or a HUD over a moving picture keeps a clean zero right up to the glyph edge. Where the\n"
                 "vector leaves the pixel unexplained (the border between two motions), the provider's\n"
                 "cells around it and last frame's vector are tried. Whole-pixel vectors are preferred where\n"
                 "they reproduce the pixel.\n"
                 "Classic: the provider's vector with the validation tests below.";
> = 0;

uniform float STATIC_NOISE <
    ui_category = "Vector refinement (per pixel)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.05; ui_step = 0.0005;
    ui_label = "Static lock: noise tolerance";
    ui_tooltip = "A pixel whose 3x3 neighbourhood changed by no more than this since the last frame (luma,\n"
                 "0..1) is static: vector exactly zero, full history. Raise it for a noisy source (analogue\n"
                 "capture, film grain or an animated dither ahead of this effect); 0 turns the lock off.";
> = 0.003;

uniform float ZERO_BIAS <
    ui_category = "Vector refinement (per pixel)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.01;
    ui_label = "Preference for 'did not move'";
    ui_tooltip = "Head start (relative) that the zero vector gets over every other candidate, in the built-in\n"
                 "estimator and in the per-pixel pick. Higher = steadier still images and UI, at the price\n"
                 "of very slow motion being ignored a little longer.";
> = 0.15;

uniform float FLOW_NOISE <
    ui_category = "Vector refinement (per pixel)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.02; ui_step = 0.0002;
    ui_label = "Match noise floor";
    ui_tooltip = "Differences in match quality below this (luma, 0..1) are treated as noise: they cannot\n"
                 "make a pixel move. Raise it with the static tolerance for a noisy source.";
> = 0.0015;

uniform int MV_SNAP <
    ui_category = "Vector refinement (per pixel)";
    ui_type = "combo";
    ui_items = "Where the whole-pixel vector reproduces the pixel\0Always (every vector rounded)\0Never (sub-pixel vectors)\0";
    ui_label = "Snap vectors to whole pixels";
    ui_tooltip = "A fractional vector makes a temporal model resample its history between pixels on every\n"
                 "frame. A game renders with sub-pixel jitter, which gives the model new detail to rebuild\n"
                 "that from; a finished picture has none, so the resampling only ever softens. A whole-pixel\n"
                 "vector fetches history pixel for pixel, and most 2D motion (menus, lists, tickers) IS in\n"
                 "whole pixels. The default takes the whole-pixel vector wherever last frame, fetched at\n"
                 "it, IS this pixel; genuinely fractional motion keeps its fraction.";
> = 0;

uniform int MOVING_HISTORY <
    ui_category = "Vector refinement (per pixel)";
    ui_type = "combo";
    ui_items = "Keep (send the vector)\0Drop where it cannot be reused: fine detail only\0Drop where it cannot be reused: everything\0";
    ui_label = "History of moving pixels (experimental)";
    ui_tooltip = "History can only be reused as it is where a whole-pixel vector (zero included) reproduces\n"
                 "the pixel. Anywhere else -- fractional motion, something creeping along at a fraction of\n"
                 "a pixel a frame, a fade -- it has to be resampled or is simply out of date, and on a\n"
                 "finished picture that only softens. 'Drop' sends a vector that points far outside the\n"
                 "frame there: the model then has no history for that pixel and answers from the current\n"
                 "frame alone. Measured with plain DLAA this is what keeps scrolling text crisp.\n"
                 "A neural consumer receives the same vectors, and what IT does with a moving pixel that\n"
                 "has no history is untested -- compare by eye before keeping it on.\n"
                 "'Fine detail' limits it to pixels with pixel-scale structure (text, lines, edges), so\n"
                 "smooth surfaces keep their real vectors.";
> = 0;

uniform float DETAIL_CONTRAST <
    ui_category = "Vector refinement (per pixel)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.2; ui_step = 0.001;
    ui_label = "Fine detail: minimum contrast";
    ui_tooltip = "3x3 contrast (mean absolute deviation of luma) from which a pixel counts as fine detail\n"
                 "for the option above.";
> = 0.02;

uniform bool REFINE_SUBPIXEL <
    ui_category = "Vector refinement (per pixel)";
    ui_label = "Sub-pixel polish at full resolution";
    ui_tooltip = "After the pick, the vector of a moving pixel is fitted to a fraction of a pixel against the\n"
                 "full-resolution frames. Slow scrolling text and fine patterns need it; costs a few fetches.";
> = true;

uniform int RESTORE_SCOPE <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_type = "combo";
    ui_items = "Text and HUD: what stands still while the picture around it moves\0Everything that stands still\0";
    ui_label = "What to restore";
    ui_tooltip = "DLSS softens a finished picture a little even with perfect vectors, and a neural pass\n"
                 "redraws lettering it has no business with. The DLSS5_Feed_Restore technique (enable it\n"
                 "BELOW DLSS 5 Feed) puts the fine detail of the frame as it came in back on pixels that\n"
                 "stand still, and leaves everything broader -- the neural pass's lighting, its colour --\n"
                 "as it comes out.\n"
                 "Text and HUD: only pixels that have been seen standing still while the picture around\n"
                 "them moved -- a caption over video, a HUD over the game. They stay marked for as long as\n"
                 "they do not change. The world itself is never touched, so a neural pass keeps the detail\n"
                 "it adds to it, moving or not.\n"
                 "Everything: any pixel that stands still. Sharpest for a menu with nothing moving behind\n"
                 "it; with a neural pass, detail it adds to a still scene is taken out again.";
> = 0;

uniform int RESTORE_WHAT <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_type = "combo";
    ui_items = "Fine detail only\0The whole pixel\0";
    ui_label = "How much of it";
    ui_tooltip = "Fine detail only: what is finer than a few pixels -- the edges of a glyph -- comes from the\n"
                 "frame as it came in; broader changes (a neural pass's lighting, a tint) stay.\n"
                 "The whole pixel: a marked pixel is simply the input again. Exact, but a pixel marked by\n"
                 "mistake then shows as a patch the neural pass did not reach.";
> = 0;

uniform float RESTORE_WIDTH <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_type = "drag"; ui_min = 1.0; ui_max = 4.0; ui_step = 0.25;
    ui_label = "Fine detail: up to (px)";
    ui_tooltip = "What counts as fine detail: structure up to about twice this many pixels across.";
> = 1.5;

uniform float RESTORE_MIN_CONTRAST <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.3; ui_step = 0.005;
    ui_label = "Text and HUD: minimum contrast";
    ui_tooltip = "How much local contrast a still pixel needs to be taken for lettering or HUD line work.\n"
                 "Raise it if distant scenery gets marked while the camera travels (debug view 'Restore');\n"
                 "lower it if faint HUD text is missed.";
> = 0.06;

uniform float RESTORE_STRENGTH <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.05;
    ui_label = "Strength";
    ui_tooltip = "How much of the difference in fine detail is put back. 1 = the input's own.";
> = 1.0;

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
                 "favour the current frame there. 0 = pure history (smoothest), 1 = mostly current frame.\n\n"
                 "Also used by the static test's first frame when hysteresis holds its vector back.";
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

uniform bool STATIC_HYSTERESIS <
    ui_category = "Validation (flicker / flames / disocclusion)";
    ui_label = "Static test: require two frames in a row";
    ui_tooltip = "The static test has no memory: on a low-contrast surface under a slow pan it can win on\n"
                 "one frame and lose on the next, so the vector alternates between the provider's and zero\n"
                 "and DLSS alternately reprojects and does not -- a flicker/judder that comes and goes.\n"
                 "With this on, the vector is only zeroed where the test won on this frame AND the last;\n"
                 "on the first frame the provider's vector is kept and the pixel is masked instead, so\n"
                 "DLSS leans on the current frame rather than reprojecting from nowhere.\n"
                 "Turn it off to compare against the old (per-frame) behaviour.";
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
               "Validation tests over the image (red = luma, green = depth, blue = consistency, yellow = vector zeroed, orange = static held back)\0"
               "Geometry model vectors (colour = direction, brightness = speed)\0"
               "Geometry decision over the image (green = model, red = provider won as moving object, blue = provider rejected)\0"
               "Geometry fit quality (grey = inlier share; top strip = fit error, black 0 px .. white 8 px)\0"
               "Refine: what each pixel took (dark = static lock, green = zero, blue = the provider's vector, cyan = a neighbouring cell, magenta = last frame's vector, yellow = snapped to whole pixels)\0"
               "Vector dump for tools/fx-testbed (10-bit codes: R = 512 + 16 x, G = 512 + 16 y, B = mask)\0"
               "Restore: where DLSS5_Feed_Restore puts the input's fine detail back (white)\0";
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
// The matching feature (luma) of this frame and the last, with a mip chain: the built-in
// estimator matches coarse to fine on it, and everything that compares frames reads these two
// rather than converting the colour buffer again.
texture DLSS5_CurLuma   { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F; MipLevels = 7; };
sampler sDLSS5_CurLuma  { Texture = DLSS5_CurLuma; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_PrevLuma  { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F; MipLevels = 7; };
texture DLSS5_PrevDepth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F;  };
texture DLSS5_PrevMV    { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
// Luma may be interpolated (a smooth quantity); depth and vectors must NOT be -- bilinear
// across an object edge mixes two surfaces' values and fails the test on every edge in motion.
sampler sDLSS5_PrevLuma  { Texture = DLSS5_PrevLuma;  AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler sDLSS5_PrevDepth { Texture = DLSS5_PrevDepth; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT;  MagFilter = POINT;  MipFilter = POINT; };
sampler sDLSS5_PrevMV    { Texture = DLSS5_PrevMV;    AddressU = Clamp; AddressV = Clamp; MinFilter = POINT;  MagFilter = POINT;  MipFilter = POINT; };
// For DLSS5_Feed_Restore. Hud: how sure it is that a pixel is a still element over a moving
// picture (0..1, built up over frames, see the feed pass); PrevHud: last frame's. Before: the
// frame as it came in, kept in the back buffer's own format so that nothing is lost in it.
// x: the share of the frame the provider reports as moving (a 16x9 probe).
texture DLSS5_Global  { Width = 1; Height = 1; Format = R16F; };
sampler sDLSS5_Global { Texture = DLSS5_Global; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_Hud     { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
texture DLSS5_PrevHud { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
sampler sDLSS5_Hud     { Texture = DLSS5_Hud;     AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_PrevHud { Texture = DLSS5_PrevHud; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
#ifndef BUFFER_COLOR_BIT_DEPTH
    #define BUFFER_COLOR_BIT_DEPTH 8
#endif
#if BUFFER_COLOR_BIT_DEPTH == 16
    texture DLSS5_Before { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
#elif BUFFER_COLOR_BIT_DEPTH == 10
    texture DLSS5_Before { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGB10A2; };
#else
    texture DLSS5_Before { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
#endif
sampler sDLSS5_Before { Texture = DLSS5_Before; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };

// This frame's vector as decided, in delta UV -- DLSS5_MV cannot be read back for that: it is
// in the add-on's units and sign, and may deliberately carry an off-screen vector.
texture DLSS5_VecNow  { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
sampler sDLSS5_VecNow { Texture = DLSS5_VecNow; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// The static-hypothesis decision, this frame and last. The test has no memory of its own,
// so on a low-contrast surface under a slow pan it can win on one frame and lose on the
// next; the vector then alternates between the provider's and zero, and DLSS alternately
// reprojects and does not. That is a flicker/judder that no consumer can smooth out, and
// it comes and goes with the surface (The Surge 2, 2026-09-02). Guides writes StaticNow,
// History copies it into PrevStatic, and the next frame's Guides reads that -- a texture
// cannot be sampled and written in the same pass, which is why it takes two of them.
texture DLSS5_StaticNow  { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
texture DLSS5_PrevStatic { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
sampler sDLSS5_StaticNow  { Texture = DLSS5_StaticNow;  AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_PrevStatic { Texture = DLSS5_PrevStatic; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

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

#if DLSS5_MV_BUILTIN
// The built-in estimator's coarser levels (level n is 1/2^n of the frame). Same layout as Flow1.
// The finest level as matched, before the median that makes DLSS5_Flow1 out of it.
texture DLSS5_Flow1Raw { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };
sampler sDLSS5_Flow1Raw { Texture = DLSS5_Flow1Raw; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_Flow2 { Width = BUFFER_WIDTH / 4;  Height = BUFFER_HEIGHT / 4;  Format = RGBA16F; };
texture DLSS5_Flow3 { Width = BUFFER_WIDTH / 8;  Height = BUFFER_HEIGHT / 8;  Format = RGBA16F; };
texture DLSS5_Flow4 { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = RGBA16F; };
texture DLSS5_Flow5 { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = RGBA16F; };
texture DLSS5_Flow6 { Width = BUFFER_WIDTH / 64; Height = BUFFER_HEIGHT / 64; Format = RGBA16F; };
sampler sDLSS5_Flow2 { Texture = DLSS5_Flow2; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Flow3 { Texture = DLSS5_Flow3; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Flow4 { Texture = DLSS5_Flow4; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Flow5 { Texture = DLSS5_Flow5; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Flow6 { Texture = DLSS5_Flow6; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
#endif

// ---------------------------------------------------------------------------------------------

// The selected provider's vector at uv, as delta UV (prev_uv = uv + mv).
float2 ProviderMV(float2 uv)
{
    float4 c = float4(uv, 0.0, 0.0);
#if DLSS5_MV_BUILTIN
    return tex2Dlod(sDLSS5_ProviderMV, c).xy * BUFFER_PIXEL_SIZE;
#elif DLSS5_MV_LOWRES
    return MV_LOWRES_FILTER == 0 ? tex2Dlod(sDLSS5_ProviderMV, c).xy : tex2Dlod(sDLSS5_ProviderMVPoint, c).xy;
#else
    return tex2Dlod(sDLSS5_ProviderMV, c).xy;
#endif
}

// One whole vector of the provider -- the cell under uv, NOT an interpolation between cells --
// in pixels. Half of one object's motion plus half of its neighbour's is the motion of nothing.
float2 ProviderCellPx(float2 uv)
{
    float4 c = float4(uv, 0.0, 0.0);
#if DLSS5_MV_BUILTIN
    return tex2Dlod(sDLSS5_ProviderMVPoint, c).xy;
#elif DLSS5_MV_LOWRES
    return tex2Dlod(sDLSS5_ProviderMVPoint, c).xy * BUFFER_SCREEN_SIZE;
#else
    return tex2Dlod(sDLSS5_ProviderMV, c).xy * BUFFER_SCREEN_SIZE;
#endif
}

// The quantity frames are compared in. Display-referred input (SDR, PQ, HLG) is already
// perceptually spaced; linear scRGB is not and has no upper bound, so it is compressed first.
float FeatureLuma(float3 c)
{
#if BUFFER_COLOR_SPACE == 2
    const float l = dot(max(c, 0.0), float3(0.299, 0.587, 0.114));
    return sqrt(l / (1.0 + l));
#else
    return dot(c, float3(0.299, 0.587, 0.114));
#endif
}

float Luma(float2 uv)
{
    return FeatureLuma(tex2Dlod(sDLSS5_ColorInput, float4(uv, 0.0, 0.0)).rgb);
}

float PS_Luma(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return Luma(uv);
}

// ---------------------------------------------------------------------------------------------
// 3x3 patches. Every decision below -- in the built-in estimator and in the per-pixel pick --
// is made by comparing a 3x3 patch of this frame with a 3x3 patch of the last one.
// ---------------------------------------------------------------------------------------------

struct P9 { float3 r0, r1, r2; };

// t = tap spacing in UV, lod = mip level sampled.
P9 Patch9(sampler2D s, float2 uv, float2 t, float lod)
{
    P9 p;
    p.r0 = float3(tex2Dlod(s, float4(uv + float2(-t.x, -t.y), 0.0, lod)).x, tex2Dlod(s, float4(uv + float2(0.0, -t.y), 0.0, lod)).x, tex2Dlod(s, float4(uv + float2(t.x, -t.y), 0.0, lod)).x);
    p.r1 = float3(tex2Dlod(s, float4(uv + float2(-t.x,  0.0), 0.0, lod)).x, tex2Dlod(s, float4(uv,                       0.0, lod)).x, tex2Dlod(s, float4(uv + float2(t.x,  0.0), 0.0, lod)).x);
    p.r2 = float3(tex2Dlod(s, float4(uv + float2(-t.x,  t.y), 0.0, lod)).x, tex2Dlod(s, float4(uv + float2(0.0,  t.y), 0.0, lod)).x, tex2Dlod(s, float4(uv + float2(t.x,  t.y), 0.0, lod)).x);
    return p;
}

float Sum3(float3 v) { return v.x + v.y + v.z; }
float P9Mean(P9 p) { return (Sum3(p.r0) + Sum3(p.r1) + Sum3(p.r2)) / 9.0; }
// Mean absolute deviation: how much structure the patch has to judge a match by.
float P9Contrast(P9 p, float m) { return (Sum3(abs(p.r0 - m)) + Sum3(abs(p.r1 - m)) + Sum3(abs(p.r2 - m))) / 9.0; }
// Largest change of any of the nine samples.
float P9MaxDiff(P9 a, P9 b)
{
    const float3 d = max(abs(a.r0 - b.r0), max(abs(a.r1 - b.r1), abs(a.r2 - b.r2)));
    return max(d.x, max(d.y, d.z));
}
// Mean change of the nine samples, as they are. This is what "the same pixels" means; the match
// cost below is not it (see there).
float P9RawDiff(P9 a, P9 b)
{
    return (Sum3(abs(a.r0 - b.r0)) + Sum3(abs(a.r1 - b.r1)) + Sum3(abs(a.r2 - b.r2))) / 9.0;
}
// Match cost: the patterns compared with each patch's own mean removed (so a brightness change --
// flicker, a fade -- is not motion), plus a quarter of the brightness difference itself (so a
// smooth gradient, which has no pattern, still prefers the place it came from).
float P9Cost(P9 c, float mc, P9 p)
{
    const float mp = P9Mean(p);
    const float d  = mc - mp;
    return (Sum3(abs(c.r0 - p.r0 - d)) + Sum3(abs(c.r1 - p.r1 - d)) + Sum3(abs(c.r2 - p.r2 - d))) / 9.0 + 0.25 * abs(d);
}

// The cost of vector v (pixels) for the patch at uv, on mip `lod` with taps `cell` pixels apart,
// sampled SYMMETRICALLY: the fractional part of v (in cells) is split between the two frames,
// this one read a quarter cell one way at most and the last one the same the other way.
// Reading only the last frame off-grid compares a sharp patch with a bilinearly blurred one,
// and that blur costs about as much as being half a cell out of place -- so a correct half
// pixel vector lost to "did not move" every time, and a camera turning at two thirds of a pixel
// a frame came out as a still one. Split, both sides carry the same blur and it cancels.
// A vector on the grid needs no second read: c and mc are the patch at uv as it is.
float SymCost(P9 c, float mc, float2 uv, float2 v, float cell, float lod)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    const float2 vi = round(v / cell) * cell;
    const float2 vf = v - vi;
    if (dot(vf, vf) < 1e-4 * cell * cell) return P9Cost(c, mc, Patch9(sDLSS5_PrevLuma, uv + vi * px, px * cell, lod));
    const P9 cs = Patch9(sDLSS5_CurLuma, uv - 0.5 * vf * px, px * cell, lod);
    return P9Cost(cs, P9Mean(cs), Patch9(sDLSS5_PrevLuma, uv + (vi + 0.5 * vf) * px, px * cell, lod));
}

// Where between three equally spaced costs (left, centre, right) the minimum lies, in steps:
// -1..1, from the parabola through them. If a neighbour is already lower than the centre the
// minimum is at or past it: step there. (Intersecting the two flanks as straight lines -- the
// textbook fit for a sum of absolute differences -- measured WORSE on tools/fx-testbed's
// fractional scrolls: 44% of text pixels within a quarter pixel against 52% for the parabola.)
float SubCell(float l, float c, float r)
{
    if (min(l, r) < c) return l < r ? -1.0 : 1.0;
    const float a = l - 2.0 * c + r;
    return a > 1e-6 ? clamp(0.5 * (l - r) / a, -0.5, 0.5) : 0.0;
}

// ---------------------------------------------------------------------------------------------
// The built-in estimator (DLSS5_MV_PROVIDER = 5). Hierarchical block matching on the luma mip
// chain, 1/64 of the frame down to 1/2, written for what this effect is fed with -- finished
// pictures, often 2D ones: UI over video, menus, scrolling lists. What it does differently from
// a general-purpose optical flow:
//   - "did not move" is a candidate at EVERY level, with a head start. A still element inside a
//     moving scene is found still at the first level fine enough to see it, instead of
//     inheriting the motion around it and having to be argued out of it later;
//   - candidates are whole vectors (the coarser level's cells around the pixel, last frame's
//     vector, zero), compared by what they do to the picture. Nothing is averaged, so there is
//     no half-way vector on the border between two motions;
//   - each level then searches one cell around the winner and fits the last half cell, so the
//     half-resolution result is already good to a fraction of a pixel.
// ---------------------------------------------------------------------------------------------

#if DLSS5_MV_BUILTIN

// A candidate is only worth nine fetches if it differs from the vector already in hand: inside
// anything that moves as one piece -- and in everything that does not move -- the cells around
// a pixel all say the same thing.
// Of two candidates that match about equally well, the one closer to what the coarser level
// found is taken (at the coarsest level: the shorter one): each cell of distance costs 4% and a
// quarter of the noise floor. Without that, anything that repeats -- the rows of a list, a
// fence, brickwork -- matches the NEXT repeat as well as itself, and a level that happened to
// pick it sends every finer level looking in the wrong place (measured: a 32 px-pitch list
// scrolling at 1.25 px a frame, 1080p, read as -30.75). Measured from the coarser level's
// answer and not from zero, because from zero it is a tax on speed: a 38 px pan is 19 cells
// long at the finest level, and was read as 25 px.
#define DLSS5_FLOW_TRY(V) \
    { const float2 v_ = (V); \
      if (any(abs(v_ - best) > 0.125 * cell)) { \
          const float e_ = P9Cost(c, mc, Patch9(sDLSS5_PrevLuma, uv + v_ * BUFFER_PIXEL_SIZE, t, lod)); \
          const float n_ = length(v_ - ref) / cell; \
          const float w_ = e_ * (1.0 + 0.04 * n_) + 0.25 * FLOW_NOISE * n_; \
          if (w_ < best_cost) { best_cost = w_; best_raw = e_; best = v_; } } }

float4 FlowLevel(float2 uv, float lod, sampler2D coarse, bool has_coarse)
{
    const float  cell = exp2(lod);                 // one texel of this level, in pixels
    const float2 t    = BUFFER_PIXEL_SIZE * cell;
    const P9     c    = Patch9(sDLSS5_CurLuma, uv, t, lod);
    const float  mc   = P9Mean(c);
    const float  contrast = P9Contrast(c, mc);
    float2 ref = 0.0;                              // the coarser level's answer for this spot
    if (has_coarse) ref = tex2Dlod(coarse, float4(uv, 0.0, 0.0)).xy;

    const float zero_raw = P9Cost(c, mc, Patch9(sDLSS5_PrevLuma, uv, t, lod));
    // The patch is where it was, to within the noise. (This is most of a menu, and all of a
    // still picture -- the estimator costs next to nothing there.) That is only evidence of
    // standing still if there was enough in the patch to see it move: with a contrast of 16
    // times the noise floor, anything over a sixteenth of a cell would have shown. A patch
    // with less than that -- the middle of a plain wall, of a sky, a soft gradient -- looks the
    // same wherever it goes, so it takes the coarser level's word for it: the inside of a
    // moving surface moves with its outline. (A flat patch that really is still, next to
    // something that moves, inherits that too; the per-pixel stage sees that its pixels did
    // not change and gives them zero.)
    if (zero_raw <= FLOW_NOISE)
        return float4(contrast >= 16.0 * FLOW_NOISE ? float2(0.0, 0.0) : ref, zero_raw, contrast);

    // Zero first, with its head start: everything else has to beat it by a margin.
    const float n0 = length(ref) / cell;
    float2 best      = 0.0;
    float  best_raw  = zero_raw;
    float  best_cost = (zero_raw * (1.0 + 0.04 * n0) + 0.25 * FLOW_NOISE * n0) * (1.0 - ZERO_BIAS) - FLOW_NOISE;

    // Where this pixel went last frame (the previous frame's final vector, in delta UV).
    DLSS5_FLOW_TRY(tex2Dlod(sDLSS5_PrevMV, float4(uv, 0.0, 0.0)).xy * BUFFER_SCREEN_SIZE)

    if (has_coarse)
    {
        // The coarser level's 3x3 cells around this pixel.
        const float2 ct = t * 2.0;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            DLSS5_FLOW_TRY(tex2Dlod(coarse, float4(uv + float2(x, y) * ct, 0.0, 0.0)).xy)
    }
    else
    {
        // Coarsest level: nothing to inherit, so look two cells in every direction.
        [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
            if (x != 0 || y != 0)
                DLSS5_FLOW_TRY(float2(x, y) * cell)
    }

    // One cell around the winner...
    const float2 b0 = best;
    [unroll] for (int y1 = -1; y1 <= 1; ++y1)
    [unroll] for (int x1 = -1; x1 <= 1; ++x1)
        if (x1 != 0 || y1 != 0)
            DLSS5_FLOW_TRY(b0 + float2(x1, y1) * cell)

    // ...then half a cell either side on each axis and the fitted minimum.
    const float2 b1 = best;
    const float  h  = 0.5 * cell;
    const float  c0 = SymCost(c, mc, uv, b1, cell, lod);
    const float  cl = SymCost(c, mc, uv, b1 + float2(-h, 0.0), cell, lod);
    const float  cr = SymCost(c, mc, uv, b1 + float2( h, 0.0), cell, lod);
    const float  cu = SymCost(c, mc, uv, b1 + float2(0.0, -h), cell, lod);
    const float  cd = SymCost(c, mc, uv, b1 + float2(0.0,  h), cell, lod);
    const float2 f  = b1 + h * float2(SubCell(cl, c0, cr), SubCell(cu, c0, cd));
    if (dot(b1, b1) > 0.0)
    {
        best = f;
        best_raw = min(c0, min(min(cl, cr), min(cu, cd)));
    }
    else if (any(abs(f) > 0.0))
    {
        // The winner is zero, yet the patch changed. Motion of less than half a cell lands
        // here -- at the finest level that is anything under a pixel a frame, i.e. every slow
        // pan -- and leaving it at zero is how a slowly turning camera came out as a still one.
        // But away from zero only on evidence: the fitted fraction has to beat "did not move"
        // by the margin everything else had to.
        const float ef = SymCost(c, mc, uv, f, cell, lod);
        if (ef < c0 * (1.0 - ZERO_BIAS) - FLOW_NOISE) { best = f; best_raw = ef; }
    }
    return float4(best, best_raw, contrast);
}

float4 PS_Flow6(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 6.0, sDLSS5_PrevMV, false); }   // no coarser level: that sampler is never read
float4 PS_Flow5(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 5.0, sDLSS5_Flow6, true); }
float4 PS_Flow4(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 4.0, sDLSS5_Flow5, true); }
float4 PS_Flow3(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 3.0, sDLSS5_Flow4, true); }
float4 PS_Flow2(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 2.0, sDLSS5_Flow3, true); }
float4 PS_Flow1(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 1.0, sDLSS5_Flow2, true); }

// Every cell above chose for itself, and on brickwork, tiles, planks -- anything that matches in
// more than one place -- neighbours do not always choose alike. Taken as it is, that field made
// the picture shimmer: on fx-testbed's 3D scene the output changed 1.39 times as much from one
// frame to the next as the input did (1.04 for LumeniteFX, which filters at every level). A
// 3x3 median of the vectors, each component for itself, removes the odd one out and leaves an
// edge between two motions where it is -- which a blur would not. What it does take away is a
// structure thinner than two cells, a glyph stroke on a moving background for one; that is the
// per-pixel stage's to put back, and it does.
#define DLSS5_SORT2(A, B) { const float2 lo_ = min(A, B); B = max(A, B); A = lo_; }
float4 PS_FlowMedian(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float2 t = BUFFER_PIXEL_SIZE * 2.0;
    const float4 c = tex2Dlod(sDLSS5_Flow1Raw, float4(uv, 0.0, 0.0));
    float2 p0 = tex2Dlod(sDLSS5_Flow1Raw, float4(uv + float2(-t.x, -t.y), 0.0, 0.0)).xy;
    float2 p1 = tex2Dlod(sDLSS5_Flow1Raw, float4(uv + float2( 0.0, -t.y), 0.0, 0.0)).xy;
    float2 p2 = tex2Dlod(sDLSS5_Flow1Raw, float4(uv + float2( t.x, -t.y), 0.0, 0.0)).xy;
    float2 p3 = tex2Dlod(sDLSS5_Flow1Raw, float4(uv + float2(-t.x,  0.0), 0.0, 0.0)).xy;
    float2 p4 = c.xy;
    float2 p5 = tex2Dlod(sDLSS5_Flow1Raw, float4(uv + float2( t.x,  0.0), 0.0, 0.0)).xy;
    float2 p6 = tex2Dlod(sDLSS5_Flow1Raw, float4(uv + float2(-t.x,  t.y), 0.0, 0.0)).xy;
    float2 p7 = tex2Dlod(sDLSS5_Flow1Raw, float4(uv + float2( 0.0,  t.y), 0.0, 0.0)).xy;
    float2 p8 = tex2Dlod(sDLSS5_Flow1Raw, float4(uv + float2( t.x,  t.y), 0.0, 0.0)).xy;
    // the 19-exchange median-of-nine network
    DLSS5_SORT2(p1, p2) DLSS5_SORT2(p4, p5) DLSS5_SORT2(p7, p8) DLSS5_SORT2(p0, p1) DLSS5_SORT2(p3, p4) DLSS5_SORT2(p6, p7)
    DLSS5_SORT2(p1, p2) DLSS5_SORT2(p4, p5) DLSS5_SORT2(p7, p8) DLSS5_SORT2(p0, p3) DLSS5_SORT2(p5, p8) DLSS5_SORT2(p4, p7)
    DLSS5_SORT2(p3, p6) DLSS5_SORT2(p1, p4) DLSS5_SORT2(p2, p5) DLSS5_SORT2(p4, p7) DLSS5_SORT2(p4, p2) DLSS5_SORT2(p6, p4)
    DLSS5_SORT2(p4, p2)
    return float4(p4, c.zw);
}

#endif   // DLSS5_MV_BUILTIN

// ---------------------------------------------------------------------------------------------
// Per-pixel refinement (MV_PIPELINE = 0), for every provider. Returns the vector in PIXELS.
//
// A provider's field is coarse -- 1/8 of the frame for LumeniteFX, half for the built-in one,
// smoothed for the rest -- and a coarse field cannot follow the outline of a glyph. It hands the
// pixels of a still caption a fraction of the motion behind it, and a temporal model that is
// told "this text moved 0.3 px" resamples its history by 0.3 px every single frame: soft,
// smeared text. A game's HUD over a moving world is the same thing.
//
// What the field IS good at is being smooth: neighbouring pixels of one surface get nearly the
// same vector, frame after frame. A first cut of this stage let every pixel pick freely among
// candidates and lost that -- on a 3D scene with known vectors (tools/fx-testbed, --scene 3d)
// the output changed 1.21 times as much from frame to frame as the input did, against 1.04 for
// the untouched field: brickwork and tiles match in more than one place, and independent
// pixels disagree about which. So the provider's vector STANDS unless the pixel itself says
// otherwise:
//   1. a pixel whose neighbourhood did not change is static: vector exactly zero;
//   2. so is one with structure of its own that last frame reproduces in place, and one that
//      "did not move" explains clearly better than the provider does -- that is the caption,
//      the HUD, the still wall under a flickering lamp;
//   3. only where the vector in hand leaves a real part of the pixel unexplained are the
//      provider's whole cells around it and last frame's vector tried, and each has to be a
//      clear improvement to be taken -- the pixel on the border between two motions;
//   4. the result is polished to sub-pixel accuracy where that is a clear gain too, and the
//      nearest whole-pixel vector is taken where it reproduces the pixel (history is then
//      fetched pixel for pixel).
// code: 0 static lock, 1 zero, 2 the provider's vector, 3 a neighbouring cell, 4 last frame's
// vector, 5 snapped to whole pixels.
// ---------------------------------------------------------------------------------------------

// Scored at two scales at once: the 3x3 pixels around the pixel, and a 3x3 of 4 px cells around
// that (mip 2). One scale is not enough either way round -- the pixels alone cannot tell a
// vertical stroke that slides along itself from one that stands still (both look unchanged
// through a 3x3 hole), and the wide patch alone cannot find a glyph edge.
float PickCost(P9 c0, float m0, P9 c2, float m2, float2 uv, float2 v_px)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    return SymCost(c0, m0, uv, v_px, 1.0, 0.0)
         + P9Cost(c2, m2, Patch9(sDLSS5_PrevLuma, uv + v_px * px, px * 4.0, 2.0));
}

// One step of the sub-pixel polish around f (cost ef): half a pixel either side on each axis
// and the minimum of the curve through the three costs. Moves f only if that is an improvement;
// returns true if it moved AND ran into the half pixel limit on an axis, i.e. the minimum may
// lie further out and a second step from the new place is worth taking.
bool PolishStep(P9 c0, float m0, float2 uv, inout float2 f, inout float ef)
{
    const float  cl = SymCost(c0, m0, uv, f + float2(-0.5, 0.0), 1.0, 0.0);
    const float  cr = SymCost(c0, m0, uv, f + float2( 0.5, 0.0), 1.0, 0.0);
    const float  cu = SymCost(c0, m0, uv, f + float2(0.0, -0.5), 1.0, 0.0);
    const float  cd = SymCost(c0, m0, uv, f + float2(0.0,  0.5), 1.0, 0.0);
    const float2 o  = float2(SubCell(cl, ef, cr), SubCell(cu, ef, cd));
    const float2 g  = f + 0.5 * o;
    const float  eg = SymCost(c0, m0, uv, g, 1.0, 0.0);
    if (eg >= ef) return false;
    f = g; ef = eg;
    return any(abs(o) > 0.999);
}

// A challenger is taken only if it explains the pixel a fifth better than what is in hand.
#define DLSS5_PICK_TRY(V, CODE) \
    { const float2 v_ = (V); \
      if (any(abs(v_ - best) > 0.125)) { \
          const float e_ = PickCost(c0, m0, c2, m2, uv, v_); \
          if (e_ < t_best * 0.8) { t_best = e_; best = v_; code = CODE; } } }

// detail: the pixel-scale contrast of the patch (how much fine structure there is to lose).
// stale:  1 if history cannot simply be reused for this pixel -- the vector is fractional, or
//         it is a whole-pixel one (zero included) that does not reproduce the pixel: something
//         creeping along at a fraction of a pixel a frame, a fade, an animation in place.
// rich:   pixel-scale plus wide contrast -- lettering and line work have a lot of it, a far
//         hillside in haze has little.
float2 RefineMV(float2 uv, out float distrust, out float code, out float detail, out float stale, out float rich)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    const P9 c0 = Patch9(sDLSS5_CurLuma, uv, px, 0.0);
    const P9 c2 = Patch9(sDLSS5_CurLuma, uv, px * 4.0, 2.0);
    const P9 p0 = Patch9(sDLSS5_PrevLuma, uv, px, 0.0);
    const P9 p2 = Patch9(sDLSS5_PrevLuma, uv, px * 4.0, 2.0);
    distrust = 0.0;
    code     = 0.0;
    stale    = 0.0;
    const float m0 = P9Mean(c0);
    const float m2 = P9Mean(c2);
    detail = P9Contrast(c0, m0);
    const float wide     = P9Contrast(c2, m2);
    const float contrast = detail + wide;
    rich = contrast;
    // Static lock: the same picture, here and around here. The wide patch averages 16 pixels a
    // sample, which makes it the sensitive one: a surface drifting a fraction of a pixel a
    // frame changes few of its pixels by a whole step, but every one of those averages a
    // little. It gets a quarter of the tolerance, so a slow pan does not lock.
    if (P9MaxDiff(c0, p0) <= STATIC_NOISE && P9MaxDiff(c2, p2) <= 0.25 * STATIC_NOISE) return float2(0.0, 0.0);

    // The provider's own answer for this pixel.
    float2 best = ProviderMV(uv) * BUFFER_SCREEN_SIZE;
    code = 2.0;
    // Nothing here to tell one place from another (flat colour, a soft gradient, plain noise):
    // nothing can be shown to be better than the provider's vector, whose estimate comes from
    // the structure around the pixel -- the inside of a moving surface moves with its outline.
    if (contrast < 4.0 * FLOW_NOISE) { stale = 1.0; return best; }

    // "Reproduces": last frame, fetched PIXEL FOR PIXEL at a whole-pixel vector, is this pixel
    // -- its nine samples, as they are, to within the noise and a tenth of the patch's own
    // contrast. On the raw values, not on the match cost: the cost removes each patch's mean
    // (that is what makes a flickering lamp not count as motion), and with the mean goes any
    // plain slope -- a gentle gradient that has slid two thirds of a pixel costs next to
    // nothing, and "reproduces" by cost turned every slow pan over a smooth surface into a
    // still picture. And at pixel scale only: the wide patch is read from a mip, which a shift
    // that is not a multiple of its 4 px cells never reproduces, however right the vector.
    const float exact  = 2.0 * FLOW_NOISE + 0.1 * detail;
    const float r_zero = P9RawDiff(c0, p0);
    const float e_zero = P9Cost(c0, m0, p0);
    const float t_zero = e_zero + P9Cost(c2, m2, p2);
    float t_best = PickCost(c0, m0, c2, m2, uv, best);

    // Still: it has pixel-scale structure and those very pixels are where they were, or "did
    // not move" explains it at least twice as well as the provider does (by match cost, so the
    // wall under the flickering lamp goes to zero by this one). Twice, not "somewhat better":
    // under a slow pan a provider that is a third of a pixel off and "did not move" are about
    // equally wrong, and a small head start for zero was enough to freeze every edge in the
    // picture (the tile joints and mortar lines of fx-testbed's courtyard, at 0.6 px a frame).
    // "In place" has to hold around the pixel as well as on it: a horizontal mortar line under
    // a horizontal pan slides along itself, and its own nine samples do not change at all.
    const bool in_place = detail >= 4.0 * FLOW_NOISE && r_zero <= exact && P9RawDiff(c2, p2) <= FLOW_NOISE + 0.1 * wide;
    if (in_place || t_zero < 0.5 * t_best - FLOW_NOISE)
    {
        best = 0.0; t_best = t_zero; code = 1.0;
        if (in_place) return float2(0.0, 0.0);
    }

    // The vector in hand leaves a real part of the pixel unexplained: the border between two
    // motions, or a provider that is simply off here. The provider's whole cells -- the one
    // under the pixel and the four around it, not interpolations of them -- and last frame's
    // vector get a hearing.
    if (t_best > 0.3 * contrast + 2.0 * FLOW_NOISE)
    {
        const float2 cell = px * DLSS5_MV_CELL;
        DLSS5_PICK_TRY(ProviderCellPx(uv), 3.0)
        DLSS5_PICK_TRY(ProviderCellPx(uv + float2(-cell.x, 0.0)), 3.0)
        DLSS5_PICK_TRY(ProviderCellPx(uv + float2( cell.x, 0.0)), 3.0)
        DLSS5_PICK_TRY(ProviderCellPx(uv + float2(0.0, -cell.y)), 3.0)
        DLSS5_PICK_TRY(ProviderCellPx(uv + float2(0.0,  cell.y)), 3.0)
        DLSS5_PICK_TRY(tex2Dlod(sDLSS5_PrevMV, float4(uv, 0.0, 0.0)).xy * BUFFER_SCREEN_SIZE, 4.0)
    }

    // Nothing explains this pixel well (disocclusion, something translucent, a cut): say so.
    distrust = saturate((t_best - 2.0 * FLOW_NOISE) / (0.5 * contrast + 4.0 * FLOW_NOISE) - 0.5);

    // Sub-pixel polish: half a pixel either side on each axis (pixel scale only: this is about
    // the last fraction of a pixel) and the minimum of the curve through those three costs.
    // If that runs into its half pixel limit, once more from there: up to a pixel in all, which
    // is exactly the range a half-resolution estimate cannot see -- every slow pan lives in it.
    const bool zero_now = dot(best, best) == 0.0;
    float e_best    = zero_now ? e_zero : SymCost(c0, m0, uv, best, 1.0, 0.0);
    bool  from_zero = false;
    if (REFINE_SUBPIXEL && MV_SNAP != 1)
    {
        float2 f  = best;
        float  ef = e_best;
        if (PolishStep(c0, m0, uv, f, ef)) PolishStep(c0, m0, uv, f, ef);
        if (zero_now)
        {
            // Away from zero only on evidence: the fraction has to beat "did not move" by a
            // margin. (A glyph edge with something passing behind it must not pick up a tenth
            // of a pixel from this.)
            if (ef < e_zero * (1.0 - ZERO_BIAS) - FLOW_NOISE) { best = f; e_best = ef; code = 2.0; from_zero = true; }
        }
        // Elsewhere only for a clear gain: on a surface the provider already has right to a
        // fraction of a pixel, a fit that moves every pixel a little differently takes the
        // field's smoothness and gives nothing back.
        else if (ef < e_best * 0.9 - 0.5 * FLOW_NOISE) { best = f; e_best = ef; }
    }

    // A whole-pixel vector fetches history pixel for pixel: nothing is resampled, so nothing
    // softens. Where the nearest one reproduces the pixel it is the vector to send -- anything
    // that really moves in whole pixels, which is most 2D motion (a list scrolling 4 px a frame
    // came back from DLAA with an error of 0.12 against 1.4 for the same list at 2.5 px).
    // "Reproduces" is meant strictly here, stricter than for standing still: the noise floor
    // and a few percent of the patch's contrast, on the pixel and around it. In a 3D scene
    // nothing moves in whole pixels, and with a looser test (or with "about as good as the
    // fractional vector", which was tried for fine repeating patterns) vectors hopped between
    // whole values from pixel to pixel and from frame to frame: fx-testbed's courtyard came out
    // 1.12 times as restless as it went in, against 1.06 without any snapping.
    // Motion that is genuinely fractional keeps its fraction: rounding 1.25 px a frame to 1
    // would have the picture slip a quarter pixel against its history on every frame.
    // (Half-way cases go AWAY from zero: a polish that started at zero and ran into its limit
    // is pointing at the next pixel over, not back at the one it left.)
    // "Around it" is eight pixels on a ring three pixels out, read at full resolution: a
    // whole-pixel shift lands on pixel centres there, so real whole-pixel motion reproduces
    // them exactly -- which the mip the wide patch comes from never does, and a tolerance wide
    // enough for the mip is wide enough for an edge sliding along itself.
    const float2 whole = round(best + sign(best) * 0.01);
    float rw = r_zero;
    if (dot(whole, whole) > 0.0) rw = P9RawDiff(c0, Patch9(sDLSS5_PrevLuma, uv + whole * px, px, 0.0));
    float ring = 0.0, ring_lo = 1e3, ring_hi = -1e3;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        const float2 o  = float2(k == 0 || k == 3 || k == 5 ? -3.0 : (k == 1 || k == 6 ? 0.0 : 3.0), k < 3 ? -3.0 : (k < 5 ? 0.0 : 3.0)) * px;
        const float  a_ = tex2Dlod(sDLSS5_CurLuma,  float4(uv + o, 0.0, 0.0)).x;
        const float  b_ = tex2Dlod(sDLSS5_PrevLuma, float4(uv + o + whole * px, 0.0, 0.0)).x;
        ring += abs(a_ - b_); ring_lo = min(ring_lo, a_); ring_hi = max(ring_hi, a_);
    }
    ring /= 8.0;
    const float ring_range = ring_hi - ring_lo;
    const bool fits  = rw <= exact && ring <= 2.0 * FLOW_NOISE + 0.05 * ring_range;          // history can be reused as it is
    const bool tight = rw <= FLOW_NOISE + 0.03 * detail && ring <= FLOW_NOISE + 0.015 * ring_range;
    if (MV_SNAP == 1 || (MV_SNAP == 0 && tight)) { best = whole; code = dot(whole, whole) > 0.0 ? 5.0 : 1.0; }
    // A polish that left zero and did not at least halve the mismatch found no motion, only a
    // slope to slide down (fine repeating patterns have plenty), and a fraction of a pixel of
    // nothing in particular is worse than the zero it left.
    else if (from_zero && e_best > 0.5 * e_zero) { best = float2(0.0, 0.0); code = 1.0; }
    stale = (fits && all(best == whole)) ? 0.0 : 1.0;
    return best;
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
//
// ReShade cannot skip a whole pass on a uniform, so both fit passes start by checking the
// one uniform that decides whether anything downstream will ever read them. It matters:
// PS_FitSolve is a ONE-pixel shader that runs 2 x 920 iterations with two fetches each and
// a 9x9 Gauss-Jordan solve -- a long serial latency chain on a single lane, every frame,
// for a result only GeometryDecide consumes. GEOM_ENABLE is off by default.
void PS_FitSamples(float4 vpos : SV_Position, float2 uv : TEXCOORD, out float4 A : SV_Target0, out float4 B : SV_Target1)
{
    if (!GEOM_ENABLE) { A = 0.0; B = 0.0; return; }
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
    // See PS_FitSamples. P5.z is the sample count FitIsUsable() tests against 40, so a zeroed
    // P5 also reads as "no usable fit" for anything that looks at it anyway.
    if (!GEOM_ENABLE) { P0 = 0.0; P1 = 0.0; P2 = 0.0; P3 = 0.0; P4 = 0.0; P5 = 0.0; return; }
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

// The share of the frame the provider reports as moving (more than half a pixel), on a 16x9
// probe. One pixel; it only tells "the picture as a whole is in motion" from "something in it is".
float PS_Global(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float n = 0.0;
    [unroll] for (int y = 0; y < 9; ++y)
    [unroll] for (int x = 0; x < 16; ++x)
        n += any(abs(ProviderMV((float2(x, y) + 0.5) / float2(16.0, 9.0))) > 0.5 * BUFFER_PIXEL_SIZE) ? 1.0 : 0.0;
    return n / 144.0;
}

float RawDepth(float2 uv)
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

void PS_MotionVectors(float4 vpos : SV_Position, float2 uv : TEXCOORD,
                      out float2 mv_out : SV_Target0, out float mask : SV_Target1,
                      out float depth : SV_Target2, out float static_now : SV_Target3, out float2 vec_now : SV_Target4,
                      out float hud : SV_Target5)
{
    bool drop_history = false;
    hud = 0.0;
    // Providers hand out "delta UV": previous position = uv + mv. DLSS wants the same
    // direction, in pixels.
    const float2 flow = ProviderMV(uv);
    float2 mv = flow;
    float  distrust = 0.0;
    static_now = 0.0;   // the geometry path does not run the static test

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
    else if (MV_PIPELINE == 0)
    {
        float code, detail, stale, rich;
        mv = RefineMV(uv, distrust, code, detail, stale, rich) * BUFFER_PIXEL_SIZE;
        static_now = code / 8.0;   // what was decided, for the debug view

        // Disocclusion, where there is a depth buffer to see it with: the vector points at a
        // different surface, so it is dropped and the pixel flagged.
        const float d = ReShade::GetLinearizedDepth(uv);
        if (VALIDATE_DEPTH && d < 0.999 && code > 1.5)
        {
            const float2 puv = uv + mv;
            if (all(puv >= 0.0) && all(puv <= 1.0))
            {
                const float dp  = tex2Dlod(sDLSS5_PrevDepth, float4(puv, 0.0, 0.0)).x;
                const float tol = DEPTH_TOLERANCE * max(d, 1e-3);
                const float bad_depth = saturate((abs(dp - d) - tol) / (tol + 1e-5));
                distrust = max(distrust, bad_depth);
                if (bad_depth > 0.5) mv = float2(0.0, 0.0);
            }
        }
        drop_history = MOVING_HISTORY > 0 && stale > 0.5 && (MOVING_HISTORY == 2 || detail >= DETAIL_CONTRAST);

        // For DLSS5_Feed_Restore: is this a still element over a moving picture -- a caption, a
        // HUD? A pixel that stands still says nothing by itself (a paused game stands still
        // too, and a neural pass is welcome to its detail). One that stands still while the
        // picture as a whole moves -- at least a quarter of the frame, AND right around it --
        // does. (Around it alone is not enough: the wall behind a walking figure stands still
        // next to motion too, and is no HUD.) And it has to look the part: lettering and line
        // work are full of contrast, whereas the other thing that stands still while a camera
        // travels -- scenery too far away to shift -- mostly is not. That evidence is gathered
        // over a few frames, kept for as long as the pixel goes on standing still -- so a HUD
        // stays marked when the camera stops -- and lost quickly once the pixel itself changes.
        const float was = tex2Dlod(sDLSS5_PrevHud, float4(uv, 0.0, 0.0)).x;
        if (dot(mv, mv) == 0.0)
        {
            bool around = RESTORE_SCOPE == 1;
            if (!around && rich >= RESTORE_MIN_CONTRAST && tex2Dlod(sDLSS5_Global, float4(0.5, 0.5, 0.0, 0.0)).x >= 0.25)
            {
                const float2 r = BUFFER_PIXEL_SIZE * 24.0;
                const float2 lim = 0.5 * BUFFER_PIXEL_SIZE;
                around = any(abs(flow) > lim)
                      || any(abs(ProviderMV(uv + float2( r.x, 0.0))) > lim) || any(abs(ProviderMV(uv + float2(-r.x, 0.0))) > lim)
                      || any(abs(ProviderMV(uv + float2(0.0,  r.y))) > lim) || any(abs(ProviderMV(uv + float2(0.0, -r.y))) > lim);
            }
            hud = around ? min(was + 0.125, 1.0) : was;
        }
        else hud = max(was - 0.25, 0.0);
    }
    else if (MV_VALIDATE)
    {
        const float4 bad = ValidateTests(uv, flow);
        static_now = bad.w;   // the raw decision, for next frame's hysteresis

        // The static test may only zero the vector once it has won twice in a row. On the
        // first win the provider's vector is kept and the pixel is masked instead: "prefer
        // the current frame here" is a safe answer either way, where a zeroed vector on a
        // pixel that really is moving smears and a full vector on a pixel that really is
        // static flickers.
        float static_zero = bad.w;
        if (STATIC_HYSTERESIS && bad.w > 0.5)
        {
            const float won_before = tex2Dlod(sDLSS5_PrevStatic, float4(uv, 0.0, 0.0)).x;
            if (won_before <= 0.5) { static_zero = 0.0; distrust = max(distrust, GEOM_MASK_REJECTED); }
        }

        // Hard decision, not a blend: half a vector points at neither where the pixel came
        // from nor where it is, so DLSS would warp history in from a place that means
        // nothing. The soft scores stay in the mask, which IS a continuous quantity.
        const bool zero_vector = max(bad.y, max(bad.z, static_zero)) > 0.5;
        distrust = max(distrust, max(bad.x, max(bad.y, bad.z)));   // appearance changed / wrong target
        mv = zero_vector ? float2(0.0, 0.0) : flow;
    }

    float2 mv_px = mv * float2(BUFFER_WIDTH, BUFFER_HEIGHT);
    if (MV_SNAP == 1) mv_px = round(mv_px);
    vec_now = mv_px * BUFFER_PIXEL_SIZE;   // the real vector, for next frame -- whatever is sent below
    if (drop_history) mv_px = float2(BUFFER_WIDTH, BUFFER_HEIGHT) * 4.0;   // off screen from anywhere
    mv_out = mv_px * MV_SIGN * MV_SCALE;
    mask   = saturate(distrust) * MASK_STRENGTH;
    depth  = RawDepth(uv);
}

// End of the technique: this frame becomes next frame's history. The raw provider vector is
// stored (not the validated one), so one distrusted frame does not poison the next test.
// prev_static carries this frame's static decision over to the next (the Guides pass cannot
// both sample and write one texture, so it lands here).
void PS_StoreHistory(float4 vpos : SV_Position, float2 uv : TEXCOORD,
                     out float luma : SV_Target0, out float depth : SV_Target1, out float2 mv : SV_Target2,
                     out float prev_static : SV_Target3, out float prev_hud : SV_Target4, out float4 before : SV_Target5)
{
    prev_hud = tex2Dfetch(sDLSS5_Hud, int2(vpos.xy)).x;
    before   = tex2Dfetch(sDLSS5_ColorInput, int2(vpos.xy));   // the frame as it came in, for DLSS5_Feed_Restore
    luma  = tex2Dfetch(sDLSS5_CurLuma, int2(vpos.xy)).x;
    depth = ReShade::GetLinearizedDepth(uv);
    // Refine: the vector this pixel was given (it is next frame's "where this pixel went last
    // time" candidate). Classic: the raw provider vector, as before.
    mv    = MV_PIPELINE == 0 ? tex2Dfetch(sDLSS5_VecNow, int2(vpos.xy)).xy : ProviderMV(uv);
    prev_static = tex2Dfetch(sDLSS5_StaticNow, int2(vpos.xy)).x;
}

float3 PS_Debug(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    if (DEBUG_VIEW == 1)
    {
        const float raw_depth = tex2Dlod(sDLSS5_Depth, float4(uv, 0.0, 0.0)).x;
#if RESHADE_DEPTH_INPUT_IS_REVERSED
        const float proximity = raw_depth;
#else
        const float proximity = 1.0 - raw_depth;
#endif
        // Display-only contrast curve: DLSS5_Depth itself remains raw and untouched.
        return pow(saturate(proximity), 0.125).xxx;
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
        const float2 flow = ProviderMV(uv);
        const float4 bad  = ValidateTests(uv, flow);
        const float3 img  = tex2Dlod(sDLSS5_ColorInput, float4(uv, 0.0, 0.0)).rgb * 0.5;
        // Yellow is what the feed pass ACTUALLY did, not what the static test proposed: with
        // hysteresis on, a test that won only this frame keeps its vector. Watch a slow pan
        // over a flat surface -- yellow that blinks on and off frame by frame is the flicker.
        const bool  moved  = length(flow * BUFFER_SCREEN_SIZE) > 0.5;
        const bool  zeroed = moved && length(tex2Dlod(sDLSS5_MV, float4(uv, 0.0, 0.0)).xy) < 1e-4;
        // Dim orange: the static test won this frame but hysteresis kept the vector (masked instead).
        const bool  held   = moved && !zeroed && tex2Dlod(sDLSS5_StaticNow, float4(uv, 0.0, 0.0)).x > 0.5;
        return saturate(img + bad.xyz * 0.9 + (zeroed ? float3(0.6, 0.6, 0.0) : float3(0.0, 0.0, 0.0))
                                            + (held   ? float3(0.35, 0.18, 0.0) : float3(0.0, 0.0, 0.0)));
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
    if (DEBUG_VIEW == 9)
    {
        const float3 img  = tex2Dlod(sDLSS5_ColorInput, float4(uv, 0.0, 0.0)).rgb * 0.35;
        const float  code = tex2Dlod(sDLSS5_StaticNow, float4(uv, 0.0, 0.0)).x * 8.0;
        const float3 tint = code < 0.5 ? float3(0.0, 0.0, 0.0) : code < 1.5 ? float3(0.0, 0.55, 0.0) : code < 2.5 ? float3(0.1, 0.25, 0.9)
                          : code < 3.5 ? float3(0.0, 0.6, 0.6) : code < 4.5 ? float3(0.8, 0.0, 0.7) : float3(0.85, 0.75, 0.0);
        return saturate(img + tint);
    }
    if (DEBUG_VIEW == 10)
    {
        const float2 v = tex2Dlod(sDLSS5_MV, float4(uv, 0.0, 0.0)).xy;
        // Whole 10-bit codes, 1/16 px a step, so a zero vector reads back as exactly 512.
        return float3((round(clamp(v, -31.0, 31.0) * 16.0) + 512.0) / 1023.0, tex2Dlod(sDLSS5_Mask, float4(uv, 0.0, 0.0)).x);
    }
    if (DEBUG_VIEW == 11)
    {
        const float3 img = tex2Dlod(sDLSS5_ColorInput, float4(uv, 0.0, 0.0)).rgb * 0.3;
        return saturate(img + smoothstep(0.35, 0.85, tex2Dlod(sDLSS5_Hud, float4(uv, 0.0, 0.0)).x) * 0.7);
    }
    float2 mv = tex2Dlod(sDLSS5_MV, float4(uv, 0.0, 0.0)).xy; // pixels
    float angle = atan2(mv.y, mv.x);
    float speed = length(mv);
    float3 rgb = saturate(3.0 * abs(2.0 * frac(angle / 6.283185 + float3(0.0, -1.0 / 3.0, 1.0 / 3.0)) - 1.0) - 1.0);
    return lerp(0.5, rgb, saturate(speed / 16.0)); // 16 px/frame saturates the colour
}

// ---------------------------------------------------------------------------------------------
// DLSS5_Feed_Restore. Runs after the add-on has put DLSS's (and the neural consumer's) picture
// into the frame, and puts the FINE DETAIL of the frame as it came in back on the pixels marked
// above: output + (input - blur(input)) - (output - blur(output)). What is broader than a few
// pixels -- a relit surface, a colour shift -- stays as it came out; only what DLSS softened
// or a neural pass redrew at the scale of a glyph stroke is replaced by the original.
// On a still element with no neural pass that is, to within the blur, the input itself.
// ---------------------------------------------------------------------------------------------

// Nine bilinear taps RESTORE_WIDTH pixels apart: at 1.5, about a 5x5 binomial.
float3 Blur5(sampler2D s, float2 uv)
{
    const float2 o = BUFFER_PIXEL_SIZE * RESTORE_WIDTH;
    float3 c = tex2Dlod(s, float4(uv, 0.0, 0.0)).rgb * 0.25;
    c += (tex2Dlod(s, float4(uv + float2( o.x, 0.0), 0.0, 0.0)).rgb + tex2Dlod(s, float4(uv + float2(-o.x, 0.0), 0.0, 0.0)).rgb
        + tex2Dlod(s, float4(uv + float2(0.0,  o.y), 0.0, 0.0)).rgb + tex2Dlod(s, float4(uv + float2(0.0, -o.y), 0.0, 0.0)).rgb) * 0.125;
    c += (tex2Dlod(s, float4(uv + float2( o.x,  o.y), 0.0, 0.0)).rgb + tex2Dlod(s, float4(uv + float2(-o.x,  o.y), 0.0, 0.0)).rgb
        + tex2Dlod(s, float4(uv + float2( o.x, -o.y), 0.0, 0.0)).rgb + tex2Dlod(s, float4(uv + float2(-o.x, -o.y), 0.0, 0.0)).rgb) * 0.0625;
    return c;
}

float4 PS_Restore(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float4 now = tex2Dlod(ReShade::BackBuffer, float4(uv, 0.0, 0.0));
    // The mark, grown by a pixel: the antialiased rim of a glyph belongs to it.
    const float2 px = BUFFER_PIXEL_SIZE;
    float h = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        h = max(h, tex2Dlod(sDLSS5_Hud, float4(uv + float2(x, y) * px, 0.0, 0.0)).x);
    const float w = smoothstep(0.35, 0.85, h) * RESTORE_STRENGTH;
    if (w <= 0.0) return now;
    const float3 was    = tex2Dlod(sDLSS5_Before, float4(uv, 0.0, 0.0)).rgb;
    if (RESTORE_WHAT == 1) return float4(lerp(now.rgb, was, w), now.a);
    const float3 hi_was = was - Blur5(sDLSS5_Before, uv);
    const float3 hi_now = now.rgb - Blur5(ReShade::BackBuffer, uv);
    return float4(now.rgb + w * (hi_was - hi_now), now.a);
}

// ---------------------------------------------------------------------------------------------

technique DLSS5_Feed
<
    ui_label   = "DLSS 5 Feed (place below your motion-vector provider)";
    ui_tooltip = "Prepares motion vectors + depth (+ a trust mask) for the DLSS 5 Feed add-on.\n\n"
                 "Provider: " DLSS5_MV_PROVIDER_NAME "\n"
                 "Change it with the DLSS5_MV_PROVIDER preprocessor definition (0 texMotionVectors,\n"
                 "1 Launchpad, 2 VORT, 3 LumeniteFX Kernel, 4 LumeniteFX QuantMotion) and enable\n"
                 "that provider's technique ABOVE this one. 5 = built in, nothing to enable.";
>
{
    pass Luma          { VertexShader = PostProcessVS; PixelShader = PS_Luma;          RenderTarget0 = DLSS5_CurLuma; }
#if DLSS5_MV_BUILTIN
    pass Flow6         { VertexShader = PostProcessVS; PixelShader = PS_Flow6;         RenderTarget0 = DLSS5_Flow6; }
    pass Flow5         { VertexShader = PostProcessVS; PixelShader = PS_Flow5;         RenderTarget0 = DLSS5_Flow5; }
    pass Flow4         { VertexShader = PostProcessVS; PixelShader = PS_Flow4;         RenderTarget0 = DLSS5_Flow4; }
    pass Flow3         { VertexShader = PostProcessVS; PixelShader = PS_Flow3;         RenderTarget0 = DLSS5_Flow3; }
    pass Flow2         { VertexShader = PostProcessVS; PixelShader = PS_Flow2;         RenderTarget0 = DLSS5_Flow2; }
    pass Flow1         { VertexShader = PostProcessVS; PixelShader = PS_Flow1;         RenderTarget0 = DLSS5_Flow1Raw; }
    pass FlowMedian    { VertexShader = PostProcessVS; PixelShader = PS_FlowMedian;    RenderTarget0 = DLSS5_Flow1; }
#endif
    pass FitSamples    { VertexShader = PostProcessVS; PixelShader = PS_FitSamples;    RenderTarget0 = DLSS5_FitA; RenderTarget1 = DLSS5_FitB; }
    pass FitSolve      { VertexShader = PostProcessVS; PixelShader = PS_FitSolve;      RenderTarget0 = DLSS5_Cam0; RenderTarget1 = DLSS5_Cam1; RenderTarget2 = DLSS5_Cam2; RenderTarget3 = DLSS5_Cam3; RenderTarget4 = DLSS5_Cam4; RenderTarget5 = DLSS5_Cam5; }
    pass Global        { VertexShader = PostProcessVS; PixelShader = PS_Global;        RenderTarget0 = DLSS5_Global; }
    pass Guides        { VertexShader = PostProcessVS; PixelShader = PS_MotionVectors; RenderTarget0 = DLSS5_MV; RenderTarget1 = DLSS5_Mask; RenderTarget2 = DLSS5_Depth; RenderTarget3 = DLSS5_StaticNow; RenderTarget4 = DLSS5_VecNow; RenderTarget5 = DLSS5_Hud; }
    pass History       { VertexShader = PostProcessVS; PixelShader = PS_StoreHistory;  RenderTarget0 = DLSS5_PrevLuma; RenderTarget1 = DLSS5_PrevDepth; RenderTarget2 = DLSS5_PrevMV; RenderTarget3 = DLSS5_PrevStatic; RenderTarget4 = DLSS5_PrevHud; RenderTarget5 = DLSS5_Before; }
    DLSS5_MV_REQUEST_PASS   // Launchpad only: ask it to compute optical flow again next frame
}

technique DLSS5_Feed_Restore
<
    ui_label   = "DLSS 5 Feed - restore text and HUD (place BELOW DLSS 5 Feed)";
    ui_tooltip = "Puts the fine detail of the frame as it came in back on captions and HUD elements, after\n"
                 "DLSS and the neural pass have had it. Needs the Refine vector pipeline. Settings are in\n"
                 "DLSS5_Feed's own list, under Restore.";
>
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_Restore; }
}

technique DLSS5_Feed_Debug
<
    ui_label   = "DLSS 5 Feed - debug view";
    ui_tooltip = "Shows the motion vectors / depth / mask the add-on will send to DLSS. Enable only for checking.";
>
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_Debug; }
}

#endif   // __RENDERER__ >= 0xA000
